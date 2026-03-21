#!/usr/bin/env python3

import argparse
import json
import os
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.client import HTTPException
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


DEFAULT_API_KEY = "xyz"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_SOURCE_COLLECTION = "product_vehicle_fitments_se"
DEFAULT_PRODUCT_COLLECTION = "products_se"
DEFAULT_VEHICLE_COLLECTION = "vehicles_se"
DEFAULT_TOTAL_FITMENT_DOCS = 150_000
DEFAULT_BATCH_DOCS = 5_000
DEFAULT_IMPORT_WORKERS = 3
DEFAULT_PROBE_INTERVAL = 0.5
DEFAULT_TIMEOUT = 120.0
DEFAULT_PRODUCT_DOCS = 30_000
DEFAULT_VEHICLE_DOCS = 30_000
DEFAULT_SCHEMA_DIR = Path(__file__).resolve().parent.parent / "benchmark" / "data" / "fitment_replay_schemas"


def pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((DEFAULT_HOST, 0))
        return sock.getsockname()[1]


def percentile(samples: list[float], pct: float) -> float:
    if not samples:
        return 0.0
    if len(samples) == 1:
        return samples[0]
    ordered = sorted(samples)
    rank = (len(ordered) - 1) * pct
    lower = int(rank)
    upper = min(lower + 1, len(ordered) - 1)
    weight = rank - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def now_ms() -> float:
    return time.perf_counter() * 1000.0


def http_request(
    method: str,
    url: str,
    api_key: str,
    body: bytes | None = None,
    timeout: float = DEFAULT_TIMEOUT,
    extra_headers: dict[str, str] | None = None,
    retries: int = 0,
) -> tuple[int, bytes]:
    headers = {"x-typesense-api-key": api_key}
    if extra_headers:
        headers.update(extra_headers)
    request = urllib.request.Request(url, data=body, headers=headers, method=method)
    attempt = 0
    while True:
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read()
        except (ConnectionResetError, TimeoutError, urllib.error.URLError, HTTPException) as exc:
            if attempt >= retries:
                raise RuntimeError(f"{method} {url} failed after {attempt + 1} attempt(s): {exc}") from exc
            attempt += 1
            time.sleep(min(0.25 * attempt, 1.0))


def json_request(
    method: str,
    url: str,
    api_key: str,
    payload: dict[str, Any] | None = None,
    timeout: float = DEFAULT_TIMEOUT,
) -> tuple[int, dict[str, Any]]:
    body = None
    headers = {}
    if payload is not None:
        body = json.dumps(payload).encode("utf-8")
        headers["content-type"] = "application/json"
    retries = 3 if method == "GET" else 0
    status, raw = http_request(method, url, api_key, body=body, timeout=timeout, extra_headers=headers, retries=retries)
    decoded = json.loads(raw.decode("utf-8")) if raw else {}
    return status, decoded


def ensure_success(status: int, payload: Any, context: str) -> None:
    if 200 <= status < 300:
        return
    raise RuntimeError(f"{context} failed with status {status}: {payload}")


def load_json_file(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def compact_schema_for_output(schema: dict[str, Any]) -> dict[str, Any]:
    # Keep original payload stable when writing fetched schemas, but avoid num_documents noise.
    result = dict(schema)
    result.pop("num_documents", None)
    return result


def field_type_to_sortable(field_type: str) -> bool:
    return field_type in {"int32", "int64", "float"}


def build_minimal_target_schema(collection_name: str, field_name: str, field_type: str) -> dict[str, Any]:
    schema = {
        "name": collection_name,
        "enable_nested_fields": False,
        "fields": [
            {"name": field_name, "type": field_type, "index": True, "facet": True},
            {"name": "runId", "type": "int64", "index": True},
        ],
    }
    if field_type_to_sortable(field_type):
        schema["default_sorting_field"] = field_name
    return schema


def load_schema_fixtures(schema_dir: Path) -> dict[str, dict[str, Any]]:
    expected = [
        DEFAULT_PRODUCT_COLLECTION,
        DEFAULT_VEHICLE_COLLECTION,
        DEFAULT_SOURCE_COLLECTION,
    ]
    schemas: dict[str, dict[str, Any]] = {}
    for name in expected:
        path = schema_dir / f"{name}.json"
        if not path.is_file():
            raise RuntimeError(f"Missing schema fixture: {path}")
        schema = load_json_file(path)
        if schema.get("name") != name:
            raise RuntimeError(f"Schema fixture {path} has name={schema.get('name')}, expected {name}")
        schemas[name] = schema
    return schemas


def order_schemas_for_creation(schemas: dict[str, dict[str, Any]]) -> list[str]:
    pending = dict(schemas)
    ordered: list[str] = []

    while pending:
        ready = []
        for name, schema in pending.items():
            refs = [
                field["reference"].split(".", 1)[0]
                for field in schema.get("fields", [])
                if isinstance(field.get("reference"), str) and "." in field["reference"]
            ]
            if all(ref not in pending for ref in refs):
                ready.append(name)

        if not ready:
            raise RuntimeError(f"Could not resolve schema creation order for collections: {', '.join(sorted(pending.keys()))}")

        for name in sorted(ready):
            ordered.append(name)
            pending.pop(name, None)

    return ordered


def fetch_live_schemas(
    source_url: str,
    api_key: str,
    source_collection: str,
    output_dir: Path | None,
) -> dict[str, dict[str, Any]]:
    source_collection_url = f"{source_url.rstrip('/')}/collections/{source_collection}"
    status, fitment_schema = json_request("GET", source_collection_url, api_key)
    ensure_success(status, fitment_schema, f"fetch schema {source_collection}")
    fitment_schema = compact_schema_for_output(fitment_schema)
    refs: list[tuple[str, str]] = []
    for field in fitment_schema.get("fields", []):
        reference = field.get("reference")
        if isinstance(reference, str) and "." in reference:
            collection_name, field_name = reference.split(".", 1)
            refs.append((collection_name, field_name))

    schemas: dict[str, dict[str, Any]] = {source_collection: fitment_schema}

    for collection_name, field_name in refs:
        collection_url = f"{source_url.rstrip('/')}/collections/{collection_name}"
        status, schema = json_request("GET", collection_url, api_key)
        ensure_success(status, schema, f"fetch schema {collection_name}")
        schema = compact_schema_for_output(schema)
        target_field = next((f for f in schema.get("fields", []) if f.get("name") == field_name), None)
        if target_field is None:
            raise RuntimeError(f"Could not find referenced field {field_name} in source collection {collection_name}")
        minimal_schema = build_minimal_target_schema(collection_name, field_name, target_field["type"])
        schemas[collection_name] = minimal_schema

    if output_dir is not None:
        output_dir.mkdir(parents=True, exist_ok=True)
        for name, schema in schemas.items():
            (output_dir / f"{name}.json").write_text(json.dumps(schema, indent=2) + "\n", encoding="utf-8")

    return schemas


def format_ms(value: float) -> str:
    return f"{value:.1f} ms"


def summarize_latencies(samples: list[float]) -> dict[str, float]:
    if not samples:
        return {"count": 0, "avg_ms": 0.0, "p95_ms": 0.0, "max_ms": 0.0}
    return {
        "count": len(samples),
        "avg_ms": statistics.mean(samples),
        "p95_ms": percentile(samples, 0.95),
        "max_ms": max(samples),
    }


@dataclass
class ProbeStats:
    route: str
    latencies_ms: list[float] = field(default_factory=list)
    failures: int = 0


@dataclass
class ImportStats:
    batch_latencies_ms: list[float] = field(default_factory=list)
    imported_docs: int = 0
    failed_batches: int = 0
    failed_docs: int = 0


@dataclass
class SearchStats:
    latencies_ms: list[float] = field(default_factory=list)
    failures: int = 0


@dataclass
class MetricsSample:
    ts_ms: float
    values: dict[str, Any]


@dataclass
class ScenarioResult:
    label: str
    create_timings_ms: dict[str, float]
    import_stats: dict[str, Any]
    probe_stats: dict[str, dict[str, float | int]]
    search_stats: dict[str, float | int]
    metrics_timeline_summary: dict[str, dict[str, float | int]]
    final_metrics: dict[str, Any]
    stdout_log: str
    stderr_log: str


class TypesenseProcess:
    def __init__(self, binary: Path, api_key: str, extra_args: list[str]) -> None:
        self.binary = binary
        self.api_key = api_key
        self.extra_args = extra_args
        self.temp_dir = Path(tempfile.mkdtemp(prefix="typesense-fitment-stress-"))
        self.stdout_log = self.temp_dir / "stdout.log"
        self.stderr_log = self.temp_dir / "stderr.log"
        self.listen_port = pick_free_port()
        self.peer_port = pick_free_port()
        self.base_url = f"http://{DEFAULT_HOST}:{self.listen_port}"
        self.process: subprocess.Popen[str] | None = None
        self._help_text: str | None = None

    def supported_flags(self) -> set[str]:
        if self._help_text is None:
            result = subprocess.run(
                [str(self.binary), "--help"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )
            self._help_text = result.stdout
        flags = set()
        for token in self._help_text.split():
            if token.startswith("--"):
                flags.add(token.rstrip(","))
        return flags

    def start(self) -> None:
        data_dir = self.temp_dir / "data"
        data_dir.mkdir(parents=True, exist_ok=True)
        stdout_file = self.stdout_log.open("w", encoding="utf-8")
        stderr_file = self.stderr_log.open("w", encoding="utf-8")
        supported = self.supported_flags()
        args = [str(self.binary), "--data-dir", str(data_dir), "--api-key", self.api_key]

        if "--node-host" in supported:
            args.extend(["--node-host", DEFAULT_HOST])
        if "--listen-address" in supported:
            args.extend(["--listen-address", DEFAULT_HOST])
        if "--listen-port" in supported:
            args.extend(["--listen-port", str(self.listen_port)])
        if "--api-address" in supported:
            args.extend(["--api-address", DEFAULT_HOST])
        if "--api-port" in supported:
            args.extend(["--api-port", str(self.listen_port)])
        if "--peering-port" in supported:
            args.extend(["--peering-port", str(self.peer_port)])
        args.extend(self.extra_args)
        self.process = subprocess.Popen(args, stdout=stdout_file, stderr=stderr_file, text=True)
        deadline = time.time() + 30.0
        while time.time() < deadline:
            try:
                status, _ = http_request("GET", self.base_url + "/health", self.api_key, timeout=2.0)
                if status == 200:
                    return
            except Exception:
                pass
            if self.process.poll() is not None:
                raise RuntimeError(
                    f"typesense-server exited early with code {self.process.returncode}. "
                    f"stdout={self.stdout_log} stderr={self.stderr_log}"
                )
            time.sleep(0.05)
        raise RuntimeError(f"Timed out waiting for server health. stdout={self.stdout_log} stderr={self.stderr_log}")

    def stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5.0)
        self.process = None

    def cleanup(self, keep_temp: bool) -> None:
        if keep_temp:
            return
        shutil.rmtree(self.temp_dir, ignore_errors=True)


def create_collection(base_url: str, api_key: str, schema: dict[str, Any]) -> float:
    started = now_ms()
    status, payload = json_request("POST", base_url + "/collections", api_key, schema)
    ensure_success(status, payload, f"create collection {schema['name']}")
    return now_ms() - started


def import_ndjson(
    base_url: str,
    api_key: str,
    collection: str,
    body: bytes,
    timeout: float,
    server_batch_size: int | None,
) -> tuple[int, bytes, float]:
    query_params: dict[str, str | int] = {"action": "upsert", "collection": collection}
    if server_batch_size is not None:
        query_params["batch_size"] = server_batch_size
    query = urllib.parse.urlencode(query_params)
    url = f"{base_url}/collections/{collection}/documents/import?{query}"
    started = now_ms()
    status, payload = http_request(
        "POST",
        url,
        api_key,
        body=body,
        timeout=timeout,
        extra_headers={"content-type": "text/plain"},
    )
    return status, payload, now_ms() - started


def seed_target_collection(
    base_url: str,
    api_key: str,
    collection: str,
    id_field: str,
    total_docs: int,
    batch_docs: int,
    timeout: float,
    server_batch_size: int | None,
) -> None:
    start = 1
    while start <= total_docs:
        end = min(total_docs, start + batch_docs - 1)
        lines = []
        for value in range(start, end + 1):
            lines.append(json.dumps({"id": f"{collection}-{value}", id_field: value, "runId": 1}, separators=(",", ":")))
        body = ("\n".join(lines) + "\n").encode("utf-8")
        status, payload, _ = import_ndjson(base_url, api_key, collection, body, timeout, server_batch_size)
        ensure_success(status, payload.decode("utf-8", errors="replace"), f"seed {collection}")
        start = end + 1


def build_fitment_batch(start_index: int, count: int, product_docs: int, vehicle_docs: int) -> bytes:
    lines = []
    for offset in range(count):
        doc_id = start_index + offset
        variant_pid = (doc_id % product_docs) + 1
        vehicle_id = (doc_id % vehicle_docs) + 1
        lines.append(
            json.dumps(
                {
                    "id": f"fitment-{doc_id}",
                    "variant_pid": variant_pid,
                    "vehicle_id": vehicle_id,
                    "runId": 1,
                },
                separators=(",", ":"),
            )
        )
    return ("\n".join(lines) + "\n").encode("utf-8")


def parse_import_response(raw: bytes) -> tuple[int, int]:
    success = 0
    failed = 0
    text = raw.decode("utf-8", errors="replace").strip()
    if not text:
        return success, failed
    for line in text.splitlines():
        parsed = json.loads(line)
        if parsed.get("success") is True:
            success += 1
        else:
            failed += 1
    return success, failed


def run_probes(
    base_url: str,
    api_key: str,
    timeout: float,
    interval_s: float,
    stop_event: threading.Event,
    stats: dict[str, ProbeStats],
) -> None:
    routes = ["/health", "/metrics.json", "/stats.json"]
    while not stop_event.is_set():
        for route in routes:
            started = now_ms()
            try:
                status, _ = http_request("GET", base_url + route, api_key, timeout=timeout)
                elapsed = now_ms() - started
                probe = stats[route]
                if 200 <= status < 300:
                    probe.latencies_ms.append(elapsed)
                else:
                    probe.failures += 1
            except Exception:
                stats[route].failures += 1
            if stop_event.wait(interval_s):
                return


def run_search_probe(
    base_url: str,
    api_key: str,
    collection: str,
    timeout: float,
    interval_s: float,
    stop_event: threading.Event,
    stats: SearchStats,
) -> None:
    params = urllib.parse.urlencode({"q": "*", "filter_by": "variant_pid:>0", "per_page": 10})
    url = f"{base_url}/collections/{collection}/documents/search?{params}"
    while not stop_event.is_set():
        started = now_ms()
        try:
            status, _ = http_request("GET", url, api_key, timeout=timeout)
            elapsed = now_ms() - started
            if 200 <= status < 300:
                stats.latencies_ms.append(elapsed)
            else:
                stats.failures += 1
        except Exception:
            stats.failures += 1
        if stop_event.wait(interval_s):
            return


def collect_metrics_sample(base_url: str, api_key: str, timeout: float) -> dict[str, Any] | None:
    try:
        status, payload = json_request("GET", base_url + "/metrics.json", api_key, timeout=timeout)
        if 200 <= status < 300:
            interesting = [
                "nuraft_last_import_total_ms",
                "nuraft_last_import_replay_ms",
                "nuraft_last_import_docs_per_sec",
                "nuraft_last_import_bytes_per_sec",
                "nuraft_last_import_replay_chunks",
                "import_handler_last_split_ms",
                "import_handler_last_add_many_ms",
                "import_handler_last_total_ms",
                "collection_import_last_total_ms",
                "collection_import_last_doc_parse_ms",
                "collection_import_last_schema_update_ms",
                "collection_import_last_batch_calls",
                "collection_import_last_effective_index_batch_size",
                "collection_import_last_batch_index_ms",
                "collection_import_last_batch_total_ms",
                "collection_import_last_batch_store_prep_ms",
                "collection_import_last_batch_write_ms",
                "collection_import_last_batch_response_ms",
                "collection_import_last_batch_async_reference_ms",
                "config_import_batch_size",
                "http_request_last_total_ms",
                "http_request_last_response_queue_ms",
                "http_import_last_total_ms",
                "http_import_last_auth_ms",
                "http_import_last_handler_wait_ms",
                "http_import_last_handler_ms",
                "http_import_last_unattributed_ms",
                "http_import_last_conn_to_start_ms",
                "http_import_last_response_dispatch_ms",
                "http_import_last_response_queue_ms",
                "http_import_last_response_progress_ms",
                "thread_pool_last_wait_ms",
                "meta_thread_pool_last_wait_ms",
                "response_flow_active_deferred_requests",
            ]
            return {key: payload.get(key) for key in interesting if key in payload}
    except Exception:
        return None
    return None


def run_metrics_sampler(
    base_url: str,
    api_key: str,
    timeout: float,
    interval_s: float,
    stop_event: threading.Event,
    samples: list[MetricsSample],
    lock: threading.Lock,
) -> None:
    while not stop_event.is_set():
        values = collect_metrics_sample(base_url, api_key, timeout)
        if values is not None:
            with lock:
                samples.append(MetricsSample(ts_ms=now_ms(), values=values))
        if stop_event.wait(interval_s):
            return


def summarize_metrics_timeline(samples: list[MetricsSample]) -> dict[str, dict[str, float | int]]:
    if not samples:
        return {}

    series: dict[str, list[float]] = {}
    for sample in samples:
        for key, value in sample.values.items():
            if isinstance(value, (int, float)):
                series.setdefault(key, []).append(float(value))

    summary: dict[str, dict[str, float | int]] = {}
    for key, values in series.items():
        summary[key] = {
            "count": len(values),
            "avg": statistics.mean(values),
            "p95": percentile(values, 0.95),
            "max": max(values),
        }
    return summary


def run_imports(
    base_url: str,
    api_key: str,
    collection: str,
    total_docs: int,
    batch_docs: int,
    product_docs: int,
    vehicle_docs: int,
    import_workers: int,
    timeout: float,
    server_batch_size: int | None,
) -> ImportStats:
    stats = ImportStats()
    lock = threading.Lock()
    next_start = 1

    def worker() -> None:
        nonlocal next_start
        while True:
            with lock:
                if next_start > total_docs:
                    return
                start_index = next_start
                count = min(batch_docs, total_docs - next_start + 1)
                next_start += count

            body = build_fitment_batch(start_index, count, product_docs, vehicle_docs)
            try:
                status, payload, elapsed = import_ndjson(
                    base_url, api_key, collection, body, timeout, server_batch_size
                )
                with lock:
                    stats.batch_latencies_ms.append(elapsed)
                if not (200 <= status < 300):
                    with lock:
                        stats.failed_batches += 1
                        stats.failed_docs += count
                    continue
                success, failed = parse_import_response(payload)
                with lock:
                    stats.imported_docs += success
                    stats.failed_docs += failed
                    if failed > 0:
                        stats.failed_batches += 1
            except Exception:
                with lock:
                    stats.failed_batches += 1
                    stats.failed_docs += count

    threads = [threading.Thread(target=worker, daemon=True) for _ in range(import_workers)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    return stats


def collect_final_metrics(base_url: str, api_key: str, timeout: float) -> dict[str, Any]:
    status, payload = json_request("GET", base_url + "/metrics.json", api_key, timeout=timeout)
    ensure_success(status, payload, "fetch final metrics")
    interesting = [
        "http_request_last_route",
        "http_request_last_total_ms",
        "http_request_last_response_queue_ms",
        "http_request_last_response_send_window_ms",
        "http_request_last_response_defer_count",
        "response_flow_active_deferred_requests",
        "response_flow_cumulative_defer_schedules",
        "response_flow_cumulative_defer_callbacks",
        "response_flow_cumulative_response_proceeds",
        "response_flow_cumulative_response_send_calls",
        "response_flow_last_defer_actual_ms",
        "response_flow_max_defer_actual_ms",
        "message_dispatch_stream_response_last_queue_ms",
        "message_dispatch_request_proceed_last_queue_ms",
        "message_dispatch_defer_processing_last_queue_ms",
        "thread_pool_last_wait_ms",
        "meta_thread_pool_last_wait_ms",
        "nuraft_last_import_total_ms",
        "nuraft_last_import_replay_ms",
        "nuraft_last_import_request_bytes",
        "nuraft_last_import_logical_chunks",
        "nuraft_last_import_replay_chunks",
        "nuraft_last_import_docs_estimate",
        "nuraft_last_import_docs_per_sec",
        "nuraft_last_import_bytes_per_sec",
        "collection_import_last_total_ms",
        "collection_import_last_reference_helper_ms",
        "collection_create_last_total_ms",
        "collection_drop_last_total_ms",
    ]
    return {key: payload.get(key) for key in interesting if key in payload}


def run_scenario(
    label: str,
    binary: Path,
    schemas: dict[str, dict[str, Any]],
    args: argparse.Namespace,
) -> ScenarioResult:
    process = TypesenseProcess(binary, args.api_key, args.server_arg)
    try:
        process.start()
        create_timings: dict[str, float] = {}
        for name in order_schemas_for_creation(schemas):
            create_timings[name] = create_collection(process.base_url, args.api_key, schemas[name])

        source_schema = schemas[args.source_collection]
        references = [
            field["reference"].split(".", 1)
            for field in source_schema["fields"]
            if isinstance(field.get("reference"), str) and "." in field["reference"]
        ]
        reference_map = {collection: field_name for collection, field_name in references}

        product_collection = next((name for name, field_name in reference_map.items() if field_name == "variant_pid"), DEFAULT_PRODUCT_COLLECTION)
        vehicle_collection = next((name for name, field_name in reference_map.items() if field_name == "vehicle_id"), DEFAULT_VEHICLE_COLLECTION)

        if args.seed_target_order == "before":
            seed_target_collection(
                process.base_url,
                args.api_key,
                product_collection,
                "variant_pid",
                args.product_docs,
                max(1, min(args.batch_docs, 5_000)),
                args.timeout,
                args.server_batch_size,
            )
            seed_target_collection(
                process.base_url,
                args.api_key,
                vehicle_collection,
                "vehicle_id",
                args.vehicle_docs,
                max(1, min(args.batch_docs, 5_000)),
                args.timeout,
                args.server_batch_size,
            )

        stop_event = threading.Event()
        probe_stats = {route: ProbeStats(route) for route in ["/health", "/metrics.json", "/stats.json"]}
        search_stats = SearchStats()
        metrics_samples: list[MetricsSample] = []
        metrics_lock = threading.Lock()
        probe_thread = threading.Thread(
            target=run_probes,
            args=(process.base_url, args.api_key, args.timeout, args.probe_interval, stop_event, probe_stats),
            daemon=True,
        )
        search_thread = threading.Thread(
            target=run_search_probe,
            args=(
                process.base_url,
                args.api_key,
                args.source_collection,
                args.timeout,
                args.probe_interval,
                stop_event,
                search_stats,
            ),
            daemon=True,
        )
        metrics_thread = threading.Thread(
            target=run_metrics_sampler,
            args=(
                process.base_url,
                args.api_key,
                args.timeout,
                args.probe_interval,
                stop_event,
                metrics_samples,
                metrics_lock,
            ),
            daemon=True,
        )
        probe_thread.start()
        search_thread.start()
        metrics_thread.start()

        started = now_ms()
        import_stats = run_imports(
            process.base_url,
            args.api_key,
            args.source_collection,
            args.total_fitment_docs,
            args.batch_docs,
            args.product_docs,
            args.vehicle_docs,
            args.import_workers,
            args.timeout,
            args.server_batch_size,
        )
        elapsed_ms = now_ms() - started

        if args.seed_target_order == "after":
            seed_target_collection(
                process.base_url,
                args.api_key,
                product_collection,
                "variant_pid",
                args.product_docs,
                max(1, min(args.batch_docs, 5_000)),
                args.timeout,
                args.server_batch_size,
            )
            seed_target_collection(
                process.base_url,
                args.api_key,
                vehicle_collection,
                "vehicle_id",
                args.vehicle_docs,
                max(1, min(args.batch_docs, 5_000)),
                args.timeout,
                args.server_batch_size,
            )
        elif args.seed_target_order == "never":
            pass

        stop_event.set()
        probe_thread.join(timeout=2.0)
        search_thread.join(timeout=2.0)
        metrics_thread.join(timeout=2.0)

        final_metrics = collect_final_metrics(process.base_url, args.api_key, args.timeout)
        import_summary = summarize_latencies(import_stats.batch_latencies_ms)
        import_summary.update(
            {
                "docs": import_stats.imported_docs,
                "failed_docs": import_stats.failed_docs,
                "failed_batches": import_stats.failed_batches,
                "elapsed_ms": elapsed_ms,
                "docs_per_sec": (import_stats.imported_docs / (elapsed_ms / 1000.0)) if elapsed_ms > 0 else 0.0,
                "batches_per_sec": (len(import_stats.batch_latencies_ms) / (elapsed_ms / 1000.0)) if elapsed_ms > 0 else 0.0,
            }
        )
        probe_summary = {
            route: {**summarize_latencies(data.latencies_ms), "failures": data.failures}
            for route, data in probe_stats.items()
        }
        search_summary = {**summarize_latencies(search_stats.latencies_ms), "failures": search_stats.failures}
        with metrics_lock:
            metrics_timeline_summary = summarize_metrics_timeline(metrics_samples)
        return ScenarioResult(
            label=label,
            create_timings_ms=create_timings,
            import_stats=import_summary,
            probe_stats=probe_summary,
            search_stats=search_summary,
            metrics_timeline_summary=metrics_timeline_summary,
            final_metrics=final_metrics,
            stdout_log=str(process.stdout_log),
            stderr_log=str(process.stderr_log),
        )
    finally:
        process.stop()
        process.cleanup(args.keep_temp)


def print_result(result: ScenarioResult) -> None:
    print(f"\n== {result.label} ==")
    print("Collection create timings:")
    for name, elapsed in result.create_timings_ms.items():
        print(f"  {name}: {format_ms(elapsed)}")

    import_stats = result.import_stats
    print("Import summary:")
    print(
        "  docs={docs} failed_docs={failed_docs} failed_batches={failed_batches} "
        "avg={avg_ms:.1f}ms p95={p95_ms:.1f}ms max={max_ms:.1f}ms docs/s={docs_per_sec:.1f} batches/s={batches_per_sec:.2f}".format(
            **import_stats
        )
    )

    print("Probe summary:")
    for route, summary in result.probe_stats.items():
        print(
            f"  {route}: count={summary['count']} failures={summary['failures']} "
            f"avg={summary['avg_ms']:.1f}ms p95={summary['p95_ms']:.1f}ms max={summary['max_ms']:.1f}ms"
        )

    search_summary = result.search_stats
    print(
        "Search summary:\n"
        f"  count={search_summary['count']} failures={search_summary['failures']} "
        f"avg={search_summary['avg_ms']:.1f}ms p95={search_summary['p95_ms']:.1f}ms max={search_summary['max_ms']:.1f}ms"
    )

    print("Selected server metrics:")
    for key in sorted(result.final_metrics.keys()):
        print(f"  {key}: {result.final_metrics[key]}")

    if result.metrics_timeline_summary:
        print("Metrics timeline summary:")
        for key in sorted(result.metrics_timeline_summary.keys()):
            summary = result.metrics_timeline_summary[key]
            print(
                f"  {key}: count={summary['count']} avg={summary['avg']:.1f} "
                f"p95={summary['p95']:.1f} max={summary['max']:.1f}"
            )
    print(f"Logs: stdout={result.stdout_log} stderr={result.stderr_log}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Replay a reference-heavy product_vehicle_fitments import locally without DDEV."
    )
    parser.add_argument("--binary", type=Path, help="Single typesense-server binary to test.")
    parser.add_argument("--baseline-binary", type=Path, help="Optional baseline binary to compare first.")
    parser.add_argument("--candidate-binary", type=Path, help="Optional candidate binary to compare second.")
    parser.add_argument("--baseline-label", default="baseline")
    parser.add_argument("--candidate-label", default="candidate")
    parser.add_argument("--api-key", default=DEFAULT_API_KEY)
    parser.add_argument("--source-url", help="Optional running Typesense URL to fetch live schemas from.")
    parser.add_argument("--source-api-key", help="API key for --source-url. Defaults to --api-key.")
    parser.add_argument("--source-collection", default=DEFAULT_SOURCE_COLLECTION)
    parser.add_argument(
        "--schema-dir",
        type=Path,
        default=DEFAULT_SCHEMA_DIR,
        help="Directory containing repo-owned schema fixtures used when --source-url is not set.",
    )
    parser.add_argument("--schema-output-dir", type=Path, help="Optional directory to dump fetched/minimal schemas.")
    parser.add_argument("--total-fitment-docs", type=int, default=DEFAULT_TOTAL_FITMENT_DOCS)
    parser.add_argument("--batch-docs", type=int, default=DEFAULT_BATCH_DOCS)
    parser.add_argument("--import-workers", type=int, default=DEFAULT_IMPORT_WORKERS)
    parser.add_argument("--product-docs", type=int, default=DEFAULT_PRODUCT_DOCS)
    parser.add_argument("--vehicle-docs", type=int, default=DEFAULT_VEHICLE_DOCS)
    parser.add_argument("--probe-interval", type=float, default=DEFAULT_PROBE_INTERVAL)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument(
        "--seed-target-order",
        choices=["before", "after", "never"],
        default="after",
        help=(
            "Seed referenced products/vehicles before fitments import, after fitments import "
            "to exercise async_reference resolution, or never to keep references unresolved."
        ),
    )
    parser.add_argument(
        "--server-batch-size",
        type=int,
        help="Optional import API batch_size query parameter to mimic a specific server-side batching setup.",
    )
    parser.add_argument("--keep-temp", action="store_true", help="Keep temporary data/log directories for inspection.")
    parser.add_argument("--server-arg", action="append", default=[], help="Extra arg passed through to typesense-server.")
    parser.add_argument("--json-output", type=Path, help="Optional path to write the final summary JSON.")
    args = parser.parse_args()

    if args.binary and (args.baseline_binary or args.candidate_binary):
        parser.error("--binary cannot be combined with --baseline-binary/--candidate-binary")
    if args.binary is None and not (args.baseline_binary and args.candidate_binary):
        parser.error("Provide either --binary or both --baseline-binary and --candidate-binary")
    return args


def main() -> int:
    args = parse_args()

    if args.source_url:
        schemas = fetch_live_schemas(
            args.source_url,
            args.source_api_key or args.api_key,
            args.source_collection,
            args.schema_output_dir,
        )
    else:
        schemas = load_schema_fixtures(args.schema_dir)
        if args.schema_output_dir is not None:
            args.schema_output_dir.mkdir(parents=True, exist_ok=True)
            for name, schema in schemas.items():
                (args.schema_output_dir / f"{name}.json").write_text(json.dumps(schema, indent=2) + "\n", encoding="utf-8")

    results: list[ScenarioResult] = []

    if args.binary:
        results.append(run_scenario(args.binary.name, args.binary, schemas, args))
    else:
        results.append(run_scenario(args.baseline_label, args.baseline_binary, schemas, args))
        results.append(run_scenario(args.candidate_label, args.candidate_binary, schemas, args))

    for result in results:
        print_result(result)

    if args.json_output is not None:
        payload = [
            {
                "label": result.label,
                "create_timings_ms": result.create_timings_ms,
                "import_stats": result.import_stats,
                "probe_stats": result.probe_stats,
                "search_stats": result.search_stats,
                "metrics_timeline_summary": result.metrics_timeline_summary,
                "final_metrics": result.final_metrics,
                "stdout_log": result.stdout_log,
                "stderr_log": result.stderr_log,
            }
            for result in results
        ]
        args.json_output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    return 0


if __name__ == "__main__":
    sys.exit(main())
