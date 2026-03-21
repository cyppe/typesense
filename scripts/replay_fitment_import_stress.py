#!/usr/bin/env python3

import argparse
import http.client
import json
import math
import os
import shlex
import shutil
import signal
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
DEFAULT_PROBE_WORKERS = 1
DEFAULT_SEARCH_WORKERS = 1
DEFAULT_PROBE_PROFILE = "standard"
DEFAULT_SCHEMA_DIR = Path(__file__).resolve().parent.parent / "benchmark" / "data" / "fitment_replay_schemas"
STANDARD_PROBE_ROUTES = [
    "/health",
    "/metrics.json",
    "/stats.json",
]
DASHBOARD_PROBE_ROUTES = [
    "/health",
    "/metrics.json",
    "/stats.json",
    "/collections",
    "/aliases",
    "/analytics/rules",
    "/keys",
    "/presets",
    "/stemming/dictionaries",
    "/stopwords",
    "/debug",
]


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


def build_probe_routes(profile: str, extra_routes: list[str]) -> list[str]:
    if profile == "standard":
        base_routes = STANDARD_PROBE_ROUTES
    elif profile == "dashboard":
        base_routes = DASHBOARD_PROBE_ROUTES
    else:
        raise RuntimeError(f"Unknown probe profile: {profile}")

    deduped: list[str] = []
    seen: set[str] = set()
    for route in [*base_routes, *extra_routes]:
        normalized = route if route.startswith("/") else f"/{route}"
        if normalized not in seen:
            deduped.append(normalized)
            seen.add(normalized)
    return deduped


def http_request(
    method: str,
    url: str,
    api_key: str,
    body: bytes | None = None,
    timeout: float = DEFAULT_TIMEOUT,
    extra_headers: dict[str, str] | None = None,
    retries: int = 0,
    stream_chunk_bytes: int | None = None,
    stream_chunk_delay_ms: float = 0.0,
) -> tuple[int, bytes]:
    headers = {"x-typesense-api-key": api_key}
    if extra_headers:
        headers.update(extra_headers)
    attempt = 0
    while True:
        try:
            if body is not None and stream_chunk_bytes is not None and stream_chunk_bytes > 0:
                parsed = urllib.parse.urlsplit(url)
                path = parsed.path or "/"
                if parsed.query:
                    path = f"{path}?{parsed.query}"
                connection_cls = http.client.HTTPSConnection if parsed.scheme == "https" else http.client.HTTPConnection
                connection = connection_cls(parsed.hostname, parsed.port, timeout=timeout)
                try:
                    connection.putrequest(method, path)
                    for key, value in headers.items():
                        connection.putheader(key, value)
                    connection.putheader("content-length", str(len(body)))
                    connection.endheaders()
                    for start in range(0, len(body), stream_chunk_bytes):
                        connection.send(body[start:start + stream_chunk_bytes])
                        if stream_chunk_delay_ms > 0 and start + stream_chunk_bytes < len(body):
                            time.sleep(stream_chunk_delay_ms / 1000.0)
                    response = connection.getresponse()
                    try:
                        return response.status, response.read()
                    finally:
                        response.close()
                finally:
                    connection.close()

            request = urllib.request.Request(url, data=body, headers=headers, method=method)
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
    phase: str
    values: dict[str, Any]


@dataclass
class ScenarioResult:
    label: str
    create_timings_ms: dict[str, float]
    import_stats: dict[str, Any]
    probe_stats: dict[str, dict[str, float | int]]
    probe_phase_stats: dict[str, dict[str, dict[str, float | int]]]
    search_stats: dict[str, float | int]
    search_phase_stats: dict[str, dict[str, float | int]]
    metrics_timeline_summary: dict[str, dict[str, float | int]]
    metrics_timeline_summary_by_phase: dict[str, dict[str, dict[str, float | int]]]
    metrics_label_summary_by_phase: dict[str, dict[str, dict[str, int]]]
    final_metrics: dict[str, Any]
    profiling_outputs: dict[str, Any]
    profile_command: str | None
    profile_exit_code: int | None
    profile_stdout_log: str | None
    profile_stderr_log: str | None
    perf_data_path: str | None
    perf_exit_code: int | None
    perf_stdout_log: str | None
    perf_stderr_log: str | None
    stdout_log: str
    stderr_log: str


@dataclass
class ProbeSample:
    route: str
    phase: str
    latency_ms: float
    success: bool


@dataclass
class SearchSample:
    phase: str
    latency_ms: float
    success: bool


class PhaseTracker:
    def __init__(self, initial_phase: str) -> None:
        self._phase = initial_phase
        self._lock = threading.Lock()

    def get(self) -> str:
        with self._lock:
            return self._phase

    def set(self, phase: str) -> None:
        with self._lock:
            self._phase = phase


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

    @property
    def pid(self) -> int:
        if self.process is None or self.process.pid is None:
            raise RuntimeError("typesense-server process is not running")
        return self.process.pid

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
    client_chunk_bytes: int | None,
    client_chunk_delay_ms: float,
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
        stream_chunk_bytes=client_chunk_bytes,
        stream_chunk_delay_ms=client_chunk_delay_ms,
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
    client_chunk_bytes: int | None,
    client_chunk_delay_ms: float,
) -> None:
    start = 1
    while start <= total_docs:
        end = min(total_docs, start + batch_docs - 1)
        lines = []
        for value in range(start, end + 1):
            lines.append(json.dumps({"id": f"{collection}-{value}", id_field: value, "runId": 1}, separators=(",", ":")))
        body = ("\n".join(lines) + "\n").encode("utf-8")
        status, payload, _ = import_ndjson(
            base_url,
            api_key,
            collection,
            body,
            timeout,
            server_batch_size,
            client_chunk_bytes,
            client_chunk_delay_ms,
        )
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
    routes: list[str],
    stats: dict[str, ProbeStats],
    phase_tracker: PhaseTracker,
    samples: list[ProbeSample],
    lock: threading.Lock,
) -> None:
    while not stop_event.is_set():
        for route in routes:
            started = now_ms()
            phase = phase_tracker.get()
            try:
                status, _ = http_request("GET", base_url + route, api_key, timeout=timeout)
                elapsed = now_ms() - started
                with lock:
                    probe = stats[route]
                    if 200 <= status < 300:
                        probe.latencies_ms.append(elapsed)
                        samples.append(ProbeSample(route=route, phase=phase, latency_ms=elapsed, success=True))
                    else:
                        probe.failures += 1
                        samples.append(ProbeSample(route=route, phase=phase, latency_ms=elapsed, success=False))
            except Exception:
                with lock:
                    stats[route].failures += 1
                    samples.append(ProbeSample(route=route, phase=phase, latency_ms=0.0, success=False))
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
    phase_tracker: PhaseTracker,
    samples: list[SearchSample],
    lock: threading.Lock,
) -> None:
    params = urllib.parse.urlencode({"q": "*", "filter_by": "variant_pid:>0", "per_page": 10})
    url = f"{base_url}/collections/{collection}/documents/search?{params}"
    while not stop_event.is_set():
        started = now_ms()
        phase = phase_tracker.get()
        try:
            status, _ = http_request("GET", url, api_key, timeout=timeout)
            elapsed = now_ms() - started
            with lock:
                if 200 <= status < 300:
                    stats.latencies_ms.append(elapsed)
                    samples.append(SearchSample(phase=phase, latency_ms=elapsed, success=True))
                else:
                    stats.failures += 1
                    samples.append(SearchSample(phase=phase, latency_ms=elapsed, success=False))
        except Exception:
            with lock:
                stats.failures += 1
                samples.append(SearchSample(phase=phase, latency_ms=0.0, success=False))
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
                "nuraft_commit_lag",
                "nuraft_live_apply_lag",
                "nuraft_state_machine_apply_lag",
                "nuraft_read_caught_up",
                "nuraft_write_caught_up",
                "import_handler_last_split_ms",
                "import_handler_last_add_many_ms",
                "import_handler_last_total_ms",
                "collection_import_last_total_ms",
                "collection_import_last_collection_name",
                "collection_import_last_docs",
                "collection_import_last_num_indexed",
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
                "collection_search_last_init_lock_wait_ms",
                "collection_search_last_run_lock_wait_ms",
                "collection_write_last_memory_lock_wait_ms",
                "collection_write_last_memory_lock_hold_ms",
                "search_route_last_total_ms",
                "search_route_last_nl_query_ms",
                "search_route_last_do_search_ms",
                "search_route_last_results_parse_ms",
                "search_route_last_results_dump_ms",
                "collections_route_last_total_ms",
                "collections_route_last_api_key_collections_ms",
                "collections_route_last_get_summaries_ms",
                "collections_route_last_dump_ms",
                "collections_route_last_collection_count",
                "stats_route_last_total_ms",
                "stats_route_last_app_metrics_ms",
                "stats_route_last_dump_ms",
                "http_route_health_last_total_ms",
                "http_route_health_last_auth_ms",
                "http_route_health_last_handler_wait_ms",
                "http_route_health_last_handler_ms",
                "http_route_health_last_response_queue_ms",
                "http_route_health_avg_total_ms",
                "http_route_health_avg_auth_ms",
                "http_route_health_avg_handler_wait_ms",
                "http_route_health_avg_handler_ms",
                "http_route_health_avg_response_queue_ms",
                "http_route_collections_last_total_ms",
                "http_route_collections_last_auth_ms",
                "http_route_collections_last_handler_wait_ms",
                "http_route_collections_last_handler_ms",
                "http_route_collections_last_response_queue_ms",
                "http_route_collections_avg_total_ms",
                "http_route_collections_avg_auth_ms",
                "http_route_collections_avg_handler_wait_ms",
                "http_route_collections_avg_handler_ms",
                "http_route_collections_avg_response_queue_ms",
                "http_route_stats_json_last_total_ms",
                "http_route_stats_json_last_auth_ms",
                "http_route_stats_json_last_handler_wait_ms",
                "http_route_stats_json_last_handler_ms",
                "http_route_stats_json_last_response_queue_ms",
                "http_route_stats_json_avg_total_ms",
                "http_route_stats_json_avg_auth_ms",
                "http_route_stats_json_avg_handler_wait_ms",
                "http_route_stats_json_avg_handler_ms",
                "http_route_stats_json_avg_response_queue_ms",
                "http_route_metrics_json_last_total_ms",
                "http_route_metrics_json_last_auth_ms",
                "http_route_metrics_json_last_handler_wait_ms",
                "http_route_metrics_json_last_handler_ms",
                "http_route_metrics_json_last_response_queue_ms",
                "http_route_metrics_json_avg_total_ms",
                "http_route_metrics_json_avg_auth_ms",
                "http_route_metrics_json_avg_handler_wait_ms",
                "http_route_metrics_json_avg_handler_ms",
                "http_route_metrics_json_avg_response_queue_ms",
                "http_route_search_last_total_ms",
                "http_route_search_last_auth_ms",
                "http_route_search_last_handler_wait_ms",
                "http_route_search_last_handler_ms",
                "http_route_search_last_response_queue_ms",
                "http_route_search_avg_total_ms",
                "http_route_search_avg_auth_ms",
                "http_route_search_avg_handler_wait_ms",
                "http_route_search_avg_handler_ms",
                "http_route_search_avg_response_queue_ms",
                "config_import_batch_size",
                "http_request_last_total_ms",
                "http_request_last_response_queue_ms",
                "http_request_last_route",
                "http_import_last_total_ms",
                "http_import_last_auth_ms",
                "http_import_last_handler_wait_ms",
                "http_import_last_handler_ms",
                "http_import_last_unattributed_ms",
                "http_import_last_conn_to_start_ms",
                "http_import_last_response_dispatch_ms",
                "http_import_last_response_pre_dispatch_wait_ms",
                "http_import_last_response_queue_ms",
                "http_import_last_response_progress_ms",
                "http_import_avg_total_ms",
                "http_import_avg_auth_ms",
                "http_import_avg_handler_wait_ms",
                "http_import_avg_handler_ms",
                "http_import_avg_unattributed_ms",
                "http_import_avg_response_pre_dispatch_wait_ms",
                "http_import_avg_response_queue_ms",
                "thread_pool_last_wait_ms",
                "thread_pool_queued_tasks",
                "meta_thread_pool_last_wait_ms",
                "meta_thread_pool_queued_tasks",
                "response_flow_active_deferred_requests",
                "message_dispatch_stream_response_last_queue_ms",
                "message_dispatch_stream_response_max_queue_ms",
                "queued_writes",
                "pending_write_batches",
                "system_cpu_active_percentage",
                "system_memory_used_bytes",
                "system_memory_used_swap_bytes",
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
    phase_tracker: PhaseTracker,
    samples: list[MetricsSample],
    lock: threading.Lock,
) -> None:
    while not stop_event.is_set():
        values = collect_metrics_sample(base_url, api_key, timeout)
        if values is not None:
            with lock:
                samples.append(MetricsSample(ts_ms=now_ms(), phase=phase_tracker.get(), values=values))
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


def summarize_metrics_timeline_by_phase(
    samples: list[MetricsSample],
) -> dict[str, dict[str, dict[str, float | int]]]:
    grouped: dict[str, list[MetricsSample]] = {}
    for sample in samples:
        grouped.setdefault(sample.phase, []).append(sample)
    return {phase: summarize_metrics_timeline(phase_samples) for phase, phase_samples in grouped.items()}


def summarize_metric_labels_by_phase(
    samples: list[MetricsSample],
) -> dict[str, dict[str, dict[str, int]]]:
    grouped: dict[str, dict[str, dict[str, int]]] = {}
    for sample in samples:
        phase_summary = grouped.setdefault(sample.phase, {})
        for key, value in sample.values.items():
            if isinstance(value, str) and value:
                key_summary = phase_summary.setdefault(key, {})
                key_summary[value] = key_summary.get(value, 0) + 1
    return grouped


def summarize_probe_samples_by_phase(
    samples: list[ProbeSample],
) -> dict[str, dict[str, dict[str, float | int]]]:
    grouped: dict[str, dict[str, ProbeStats]] = {}
    for sample in samples:
        route_stats = grouped.setdefault(sample.phase, {})
        probe = route_stats.setdefault(sample.route, ProbeStats(sample.route))
        if sample.success:
            probe.latencies_ms.append(sample.latency_ms)
        else:
            probe.failures += 1
    return {
        phase: {
            route: {**summarize_latencies(probe.latencies_ms), "failures": probe.failures}
            for route, probe in route_stats.items()
        }
        for phase, route_stats in grouped.items()
    }


def summarize_search_samples_by_phase(samples: list[SearchSample]) -> dict[str, dict[str, float | int]]:
    grouped: dict[str, SearchStats] = {}
    for sample in samples:
        stats = grouped.setdefault(sample.phase, SearchStats())
        if sample.success:
            stats.latencies_ms.append(sample.latency_ms)
        else:
            stats.failures += 1
    return {
        phase: {**summarize_latencies(stats.latencies_ms), "failures": stats.failures}
        for phase, stats in grouped.items()
    }


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
    client_chunk_bytes: int | None,
    client_chunk_delay_ms: float,
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
                    base_url,
                    api_key,
                    collection,
                    body,
                    timeout,
                    server_batch_size,
                    client_chunk_bytes,
                    client_chunk_delay_ms,
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
        "http_request_last_response_pre_dispatch_wait_ms",
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
        "message_dispatch_stream_response_max_queue_ms",
        "message_dispatch_request_proceed_last_queue_ms",
        "message_dispatch_defer_processing_last_queue_ms",
        "thread_pool_last_wait_ms",
        "thread_pool_queued_tasks",
        "meta_thread_pool_last_wait_ms",
        "meta_thread_pool_queued_tasks",
        "nuraft_last_import_total_ms",
        "nuraft_last_import_replay_ms",
        "nuraft_last_import_request_bytes",
        "nuraft_last_import_logical_chunks",
        "nuraft_last_import_replay_chunks",
        "nuraft_last_import_docs_estimate",
        "nuraft_last_import_docs_per_sec",
        "nuraft_last_import_bytes_per_sec",
        "http_import_cumulative_requests",
        "http_import_avg_total_ms",
        "http_import_avg_auth_ms",
        "http_import_avg_handler_wait_ms",
        "http_import_avg_handler_ms",
        "http_import_avg_unattributed_ms",
        "http_import_avg_response_pre_dispatch_wait_ms",
        "http_import_avg_response_queue_ms",
        "http_import_max_total_ms",
        "config_import_batch_size",
        "queued_writes",
        "pending_write_batches",
        "nuraft_commit_lag",
        "nuraft_live_apply_lag",
        "nuraft_state_machine_apply_lag",
        "system_cpu_active_percentage",
        "system_memory_used_bytes",
        "system_memory_used_swap_bytes",
        "collection_import_last_total_ms",
        "collection_import_last_collection_name",
        "collection_import_last_docs",
        "collection_import_last_num_indexed",
        "collection_import_last_batch_calls",
        "collection_import_last_effective_index_batch_size",
        "collection_import_last_reference_helper_ms",
        "collection_search_last_init_lock_wait_ms",
        "collection_search_last_run_lock_wait_ms",
        "collection_write_last_memory_lock_wait_ms",
        "collection_write_last_memory_lock_hold_ms",
        "search_route_last_collection_name",
        "search_route_last_total_ms",
        "search_route_last_nl_query_ms",
        "search_route_last_do_search_ms",
        "search_route_last_results_parse_ms",
        "search_route_last_results_dump_ms",
        "collections_route_last_total_ms",
        "collections_route_last_api_key_collections_ms",
        "collections_route_last_get_summaries_ms",
        "collections_route_last_dump_ms",
        "collections_route_last_collection_count",
        "stats_route_last_total_ms",
        "stats_route_last_app_metrics_ms",
        "stats_route_last_dump_ms",
        "http_route_health_last_total_ms",
        "http_route_health_last_auth_ms",
        "http_route_health_last_handler_wait_ms",
        "http_route_health_last_handler_ms",
        "http_route_health_last_response_queue_ms",
        "http_route_health_avg_total_ms",
        "http_route_health_avg_auth_ms",
        "http_route_health_avg_handler_wait_ms",
        "http_route_health_avg_handler_ms",
        "http_route_health_avg_response_queue_ms",
        "http_route_collections_last_total_ms",
        "http_route_collections_last_auth_ms",
        "http_route_collections_last_handler_wait_ms",
        "http_route_collections_last_handler_ms",
        "http_route_collections_last_response_queue_ms",
        "http_route_collections_avg_total_ms",
        "http_route_collections_avg_auth_ms",
        "http_route_collections_avg_handler_wait_ms",
        "http_route_collections_avg_handler_ms",
        "http_route_collections_avg_response_queue_ms",
        "http_route_stats_json_last_total_ms",
        "http_route_stats_json_last_auth_ms",
        "http_route_stats_json_last_handler_wait_ms",
        "http_route_stats_json_last_handler_ms",
        "http_route_stats_json_last_response_queue_ms",
        "http_route_stats_json_avg_total_ms",
        "http_route_stats_json_avg_auth_ms",
        "http_route_stats_json_avg_handler_wait_ms",
        "http_route_stats_json_avg_handler_ms",
        "http_route_stats_json_avg_response_queue_ms",
        "http_route_metrics_json_last_total_ms",
        "http_route_metrics_json_last_auth_ms",
        "http_route_metrics_json_last_handler_wait_ms",
        "http_route_metrics_json_last_handler_ms",
        "http_route_metrics_json_last_response_queue_ms",
        "http_route_metrics_json_avg_total_ms",
        "http_route_metrics_json_avg_auth_ms",
        "http_route_metrics_json_avg_handler_wait_ms",
        "http_route_metrics_json_avg_handler_ms",
        "http_route_metrics_json_avg_response_queue_ms",
        "http_route_search_last_total_ms",
        "http_route_search_last_auth_ms",
        "http_route_search_last_handler_wait_ms",
        "http_route_search_last_handler_ms",
        "http_route_search_last_response_queue_ms",
        "http_route_search_avg_total_ms",
        "http_route_search_avg_auth_ms",
        "http_route_search_avg_handler_wait_ms",
        "http_route_search_avg_handler_ms",
        "http_route_search_avg_response_queue_ms",
        "collection_create_last_total_ms",
        "collection_drop_last_total_ms",
    ]
    return {key: payload.get(key) for key in interesting if key in payload}


def start_profile_command(
    command: str,
    process: TypesenseProcess,
    api_key: str,
    label: str,
) -> tuple[subprocess.Popen[str], Path, Path]:
    stdout_log = process.temp_dir / f"profile-{label}-stdout.log"
    stderr_log = process.temp_dir / f"profile-{label}-stderr.log"
    env = os.environ.copy()
    env.update(
        {
            "TYPESENSE_PID": str(process.pid),
            "TYPESENSE_BASE_URL": process.base_url,
            "TYPESENSE_API_KEY": api_key,
            "TYPESENSE_TEMP_DIR": str(process.temp_dir),
            "TYPESENSE_BINARY": str(process.binary),
            "TYPESENSE_PROFILE_LABEL": label,
        }
    )
    stdout_file = stdout_log.open("w", encoding="utf-8")
    stderr_file = stderr_log.open("w", encoding="utf-8")
    proc = subprocess.Popen(
        command,
        shell=True,
        executable="/bin/bash",
        env=env,
        stdout=stdout_file,
        stderr=stderr_file,
        text=True,
        start_new_session=True,
    )
    return proc, stdout_log, stderr_log


def start_perf_record(
    process: TypesenseProcess,
    label: str,
    seconds: float,
    frequency: int,
    call_graph: str,
    offcpu: bool = False,
) -> tuple[subprocess.Popen[str], Path, Path, Path]:
    profile_label = f"{label}-offcpu" if offcpu else label
    data_path = process.temp_dir / f"{profile_label}.perf"
    stdout_log = process.temp_dir / f"perf-{profile_label}-stdout.log"
    stderr_log = process.temp_dir / f"perf-{profile_label}-stderr.log"
    stdout_file = stdout_log.open("w", encoding="utf-8")
    stderr_file = stderr_log.open("w", encoding="utf-8")
    command = [
        "sudo",
        "perf",
        "record",
        "-B",
        "-N",
        "-o",
        str(data_path),
        "-p",
        str(process.pid),
    ]
    if offcpu:
        command.append("--off-cpu")
    else:
        command.extend(["-F", str(frequency)])
    command.extend(["--call-graph", call_graph, "--", "sleep", str(seconds)])
    proc = subprocess.Popen(
        command,
        stdout=stdout_file,
        stderr=stderr_file,
        text=True,
        start_new_session=True,
    )
    return proc, data_path, stdout_log, stderr_log


def start_perf_stat(
    process: TypesenseProcess,
    label: str,
    seconds: float,
    events: str,
) -> tuple[subprocess.Popen[str], Path]:
    output_path = process.temp_dir / f"{label}.perf-stat.txt"
    output_file = output_path.open("w", encoding="utf-8")
    proc = subprocess.Popen(
        [
            "sudo",
            "perf",
            "stat",
            "-e",
            events,
            "-p",
            str(process.pid),
            "--",
            "sleep",
            str(seconds),
        ],
        stdout=subprocess.DEVNULL,
        stderr=output_file,
        text=True,
        start_new_session=True,
    )
    return proc, output_path


def start_runqlat(
    process: TypesenseProcess,
    label: str,
    seconds: float,
) -> tuple[subprocess.Popen[str], Path, Path]:
    output_path = process.temp_dir / f"{label}.runqlat.txt"
    stderr_log = process.temp_dir / f"{label}.runqlat.stderr.log"
    output_file = output_path.open("w", encoding="utf-8")
    stderr_file = stderr_log.open("w", encoding="utf-8")
    count = max(1, math.ceil(seconds))
    proc = subprocess.Popen(
        [
            "sudo",
            "runqlat",
            "-m",
            "-P",
            "-p",
            str(process.pid),
            "1",
            str(count),
        ],
        stdout=output_file,
        stderr=stderr_file,
        text=True,
        start_new_session=True,
    )
    return proc, output_path, stderr_log


def wait_for_capture_process(proc: subprocess.Popen[str], timeout: float) -> int:
    try:
        return proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGINT)
        except ProcessLookupError:
            pass
        try:
            return proc.wait(timeout=10.0)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                return proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                return proc.wait(timeout=5.0)


def maybe_chown_to_current_user(path: Path | None) -> None:
    if path is None or not path.exists():
        return
    subprocess.run(
        ["sudo", "chown", f"{os.getuid()}:{os.getgid()}", str(path)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )


def generate_perf_artifacts(data_path: Path, prefix: str, temp_dir: Path) -> dict[str, Any]:
    outputs: dict[str, Any] = {}
    if not data_path.exists():
        outputs[f"{prefix}_artifact_error"] = f"missing data file: {data_path}"
        return outputs

    maybe_chown_to_current_user(data_path)
    outputs[f"{prefix}_data_path"] = str(data_path)

    report_path = temp_dir / f"{data_path.stem}.report.txt"
    report_stderr_log = temp_dir / f"{data_path.stem}.report.stderr.log"
    with report_path.open("w", encoding="utf-8") as report_file, report_stderr_log.open("w", encoding="utf-8") as err_file:
        report_proc = subprocess.run(
            [
                "timeout",
                "60",
                "perf",
                "report",
                "-f",
                "--stdio",
                "--no-children",
                "--percent-limit",
                "0",
                "-n",
                "-i",
                str(data_path),
                "--sort",
                "comm,dso,symbol",
            ],
            stdout=report_file,
            stderr=err_file,
            text=True,
            check=False,
        )
    outputs[f"{prefix}_report_path"] = str(report_path)
    outputs[f"{prefix}_report_stderr_log"] = str(report_stderr_log)
    outputs[f"{prefix}_report_exit_code"] = report_proc.returncode

    if not (shutil.which("inferno-collapse-perf") and shutil.which("inferno-flamegraph")):
        outputs[f"{prefix}_flamegraph_skipped"] = "inferno tools not installed"
        return outputs
    folded_path = temp_dir / f"{data_path.stem}.folded"
    flamegraph_path = temp_dir / f"{data_path.stem}.svg"
    flamegraph_stderr_log = temp_dir / f"{data_path.stem}.flamegraph.stderr.log"
    shell_command = (
        f"timeout 120 perf script -i {shlex.quote(str(data_path))} "
        f"| inferno-collapse-perf > {shlex.quote(str(folded_path))} "
        f"&& inferno-flamegraph < {shlex.quote(str(folded_path))} > {shlex.quote(str(flamegraph_path))}"
    )
    with flamegraph_stderr_log.open("w", encoding="utf-8") as err_file:
        flamegraph_proc = subprocess.run(
            ["/bin/bash", "-lc", shell_command],
            stdout=subprocess.DEVNULL,
            stderr=err_file,
            text=True,
            check=False,
        )
    outputs[f"{prefix}_folded_path"] = str(folded_path)
    outputs[f"{prefix}_flamegraph_path"] = str(flamegraph_path)
    outputs[f"{prefix}_flamegraph_stderr_log"] = str(flamegraph_stderr_log)
    outputs[f"{prefix}_flamegraph_exit_code"] = flamegraph_proc.returncode
    if flamegraph_proc.returncode == 0 and folded_path.exists():
        top_path = temp_dir / f"{data_path.stem}.top-stacks.txt"
        rows: list[tuple[int, str]] = []
        with folded_path.open("r", encoding="utf-8", errors="replace") as folded_file:
            for line in folded_file:
                stripped = line.rstrip()
                if not stripped:
                    continue
                frames, sep, count_text = stripped.rpartition(" ")
                if not sep:
                    continue
                try:
                    count = int(count_text)
                except ValueError:
                    continue
                rows.append((count, frames))
        rows.sort(key=lambda item: item[0], reverse=True)
        with top_path.open("w", encoding="utf-8") as top_file:
            for count, frames in rows[:50]:
                top_file.write(f"{count:>12} {frames}\n")
        outputs[f"{prefix}_top_stacks_path"] = str(top_path)
    return outputs


def run_scenario(
    label: str,
    binary: Path,
    schemas: dict[str, dict[str, Any]],
    args: argparse.Namespace,
) -> ScenarioResult:
    process = TypesenseProcess(binary, args.api_key, args.server_arg)
    profile_proc: subprocess.Popen[str] | None = None
    profile_stdout_log: Path | None = None
    profile_stderr_log: Path | None = None
    profile_exit_code: int | None = None
    profiling_outputs: dict[str, Any] = {}
    perf_proc: subprocess.Popen[str] | None = None
    perf_data_path: Path | None = None
    perf_stdout_log: Path | None = None
    perf_stderr_log: Path | None = None
    perf_exit_code: int | None = None
    perf_offcpu_proc: subprocess.Popen[str] | None = None
    perf_offcpu_data_path: Path | None = None
    perf_offcpu_stdout_log: Path | None = None
    perf_offcpu_stderr_log: Path | None = None
    perf_offcpu_exit_code: int | None = None
    perf_stat_proc: subprocess.Popen[str] | None = None
    perf_stat_output_path: Path | None = None
    perf_stat_exit_code: int | None = None
    runqlat_proc: subprocess.Popen[str] | None = None
    runqlat_output_path: Path | None = None
    runqlat_stderr_log: Path | None = None
    runqlat_exit_code: int | None = None
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
                args.client_chunk_bytes,
                args.client_chunk_delay_ms,
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
                args.client_chunk_bytes,
                args.client_chunk_delay_ms,
            )

        stop_event = threading.Event()
        phase_tracker = PhaseTracker("source_import")
        probe_routes = build_probe_routes(args.probe_profile, args.probe_route)
        probe_stats = {route: ProbeStats(route) for route in probe_routes}
        probe_samples: list[ProbeSample] = []
        search_stats = SearchStats()
        search_samples: list[SearchSample] = []
        metrics_samples: list[MetricsSample] = []
        metrics_lock = threading.Lock()
        probe_lock = threading.Lock()
        search_lock = threading.Lock()
        probe_threads = [
            threading.Thread(
                target=run_probes,
                args=(
                    process.base_url,
                    args.api_key,
                    args.timeout,
                    args.probe_interval,
                    stop_event,
                    probe_routes,
                    probe_stats,
                    phase_tracker,
                    probe_samples,
                    probe_lock,
                ),
                daemon=True,
            )
            for _ in range(args.probe_workers)
        ]
        search_threads = [
            threading.Thread(
                target=run_search_probe,
                args=(
                    process.base_url,
                    args.api_key,
                    args.source_collection,
                    args.timeout,
                    args.probe_interval,
                    stop_event,
                    search_stats,
                    phase_tracker,
                    search_samples,
                    search_lock,
                ),
                daemon=True,
            )
            for _ in range(args.search_workers)
        ]
        metrics_thread = threading.Thread(
            target=run_metrics_sampler,
            args=(
                process.base_url,
                args.api_key,
                args.timeout,
                args.probe_interval,
                stop_event,
                phase_tracker,
                metrics_samples,
                metrics_lock,
            ),
            daemon=True,
        )
        for thread in probe_threads:
            thread.start()
        for thread in search_threads:
            thread.start()
        metrics_thread.start()

        if args.profile_cmd:
            profile_proc, profile_stdout_log, profile_stderr_log = start_profile_command(
                args.profile_cmd,
                process,
                args.api_key,
                label,
            )
        if args.perf_seconds > 0:
            perf_proc, perf_data_path, perf_stdout_log, perf_stderr_log = start_perf_record(
                process,
                label,
                args.perf_seconds,
                args.perf_frequency,
                args.perf_call_graph,
            )
        if args.perf_offcpu_seconds > 0:
            perf_offcpu_proc, perf_offcpu_data_path, perf_offcpu_stdout_log, perf_offcpu_stderr_log = start_perf_record(
                process,
                label,
                args.perf_offcpu_seconds,
                args.perf_frequency,
                args.perf_call_graph,
                offcpu=True,
            )
        if args.perf_stat_seconds > 0:
            perf_stat_proc, perf_stat_output_path = start_perf_stat(
                process,
                label,
                args.perf_stat_seconds,
                args.perf_stat_events,
            )
        if args.runqlat_seconds > 0:
            runqlat_proc, runqlat_output_path, runqlat_stderr_log = start_runqlat(
                process,
                label,
                args.runqlat_seconds,
            )

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
            args.client_chunk_bytes,
            args.client_chunk_delay_ms,
        )
        elapsed_ms = now_ms() - started

        if args.seed_target_order == "after":
            phase_tracker.set("reference_seed")
            seed_target_collection(
                process.base_url,
                args.api_key,
                product_collection,
                "variant_pid",
                args.product_docs,
                max(1, min(args.batch_docs, 5_000)),
                args.timeout,
                args.server_batch_size,
                args.client_chunk_bytes,
                args.client_chunk_delay_ms,
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
                args.client_chunk_bytes,
                args.client_chunk_delay_ms,
            )
        elif args.seed_target_order == "never":
            pass

        phase_tracker.set("cooldown")
        stop_event.set()
        for thread in probe_threads:
            thread.join(timeout=2.0)
        for thread in search_threads:
            thread.join(timeout=2.0)
        metrics_thread.join(timeout=2.0)

        if profile_proc is not None:
            profile_exit_code = wait_for_capture_process(profile_proc, args.profile_wait_timeout)

        if perf_proc is not None:
            perf_exit_code = wait_for_capture_process(perf_proc, args.profile_wait_timeout)
            if perf_data_path is not None:
                profiling_outputs.update(generate_perf_artifacts(perf_data_path, "perf_oncpu", process.temp_dir))
            if perf_stdout_log is not None:
                profiling_outputs["perf_oncpu_stdout_log"] = str(perf_stdout_log)
            if perf_stderr_log is not None:
                profiling_outputs["perf_oncpu_stderr_log"] = str(perf_stderr_log)
            profiling_outputs["perf_oncpu_exit_code"] = perf_exit_code

        if perf_offcpu_proc is not None:
            perf_offcpu_exit_code = wait_for_capture_process(perf_offcpu_proc, args.profile_wait_timeout)
            if perf_offcpu_data_path is not None:
                profiling_outputs.update(generate_perf_artifacts(perf_offcpu_data_path, "perf_offcpu", process.temp_dir))
            if perf_offcpu_stdout_log is not None:
                profiling_outputs["perf_offcpu_stdout_log"] = str(perf_offcpu_stdout_log)
            if perf_offcpu_stderr_log is not None:
                profiling_outputs["perf_offcpu_stderr_log"] = str(perf_offcpu_stderr_log)
            profiling_outputs["perf_offcpu_exit_code"] = perf_offcpu_exit_code

        if perf_stat_proc is not None:
            perf_stat_exit_code = wait_for_capture_process(perf_stat_proc, args.profile_wait_timeout)
            profiling_outputs["perf_stat_exit_code"] = perf_stat_exit_code
            if perf_stat_output_path is not None:
                profiling_outputs["perf_stat_output_path"] = str(perf_stat_output_path)

        if runqlat_proc is not None:
            runqlat_exit_code = wait_for_capture_process(runqlat_proc, args.profile_wait_timeout)
            profiling_outputs["runqlat_exit_code"] = runqlat_exit_code
            if runqlat_output_path is not None:
                profiling_outputs["runqlat_output_path"] = str(runqlat_output_path)
            if runqlat_stderr_log is not None:
                profiling_outputs["runqlat_stderr_log"] = str(runqlat_stderr_log)

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
            metrics_timeline_summary_by_phase = summarize_metrics_timeline_by_phase(metrics_samples)
            metrics_label_summary_by_phase = summarize_metric_labels_by_phase(metrics_samples)
        with probe_lock:
            probe_phase_summary = summarize_probe_samples_by_phase(probe_samples)
        with search_lock:
            search_phase_summary = summarize_search_samples_by_phase(search_samples)
        return ScenarioResult(
            label=label,
            create_timings_ms=create_timings,
            import_stats=import_summary,
            probe_stats=probe_summary,
            probe_phase_stats=probe_phase_summary,
            search_stats=search_summary,
            search_phase_stats=search_phase_summary,
            metrics_timeline_summary=metrics_timeline_summary,
            metrics_timeline_summary_by_phase=metrics_timeline_summary_by_phase,
            metrics_label_summary_by_phase=metrics_label_summary_by_phase,
            final_metrics=final_metrics,
            profiling_outputs=profiling_outputs,
            profile_command=args.profile_cmd,
            profile_exit_code=profile_exit_code,
            profile_stdout_log=str(profile_stdout_log) if profile_stdout_log is not None else None,
            profile_stderr_log=str(profile_stderr_log) if profile_stderr_log is not None else None,
            perf_data_path=str(perf_data_path) if perf_data_path is not None else None,
            perf_exit_code=perf_exit_code,
            perf_stdout_log=str(perf_stdout_log) if perf_stdout_log is not None else None,
            perf_stderr_log=str(perf_stderr_log) if perf_stderr_log is not None else None,
            stdout_log=str(process.stdout_log),
            stderr_log=str(process.stderr_log),
        )
    finally:
        if profile_proc is not None and profile_proc.poll() is None:
            wait_for_capture_process(profile_proc, 0.1)
        if perf_proc is not None and perf_proc.poll() is None:
            wait_for_capture_process(perf_proc, 0.1)
        if perf_offcpu_proc is not None and perf_offcpu_proc.poll() is None:
            wait_for_capture_process(perf_offcpu_proc, 0.1)
        if perf_stat_proc is not None and perf_stat_proc.poll() is None:
            wait_for_capture_process(perf_stat_proc, 0.1)
        if runqlat_proc is not None and runqlat_proc.poll() is None:
            wait_for_capture_process(runqlat_proc, 0.1)
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
    if result.probe_phase_stats:
        print("Probe summary by phase:")
        for phase in sorted(result.probe_phase_stats.keys()):
            print(f"  [{phase}]")
            for route, summary in sorted(result.probe_phase_stats[phase].items()):
                print(
                    f"    {route}: count={summary['count']} failures={summary['failures']} "
                    f"avg={summary['avg_ms']:.1f}ms p95={summary['p95_ms']:.1f}ms max={summary['max_ms']:.1f}ms"
                )

    search_summary = result.search_stats
    print(
        "Search summary:\n"
        f"  count={search_summary['count']} failures={search_summary['failures']} "
        f"avg={search_summary['avg_ms']:.1f}ms p95={search_summary['p95_ms']:.1f}ms max={search_summary['max_ms']:.1f}ms"
    )
    if result.search_phase_stats:
        print("Search summary by phase:")
        for phase in sorted(result.search_phase_stats.keys()):
            summary = result.search_phase_stats[phase]
            print(
                f"  [{phase}] count={summary['count']} failures={summary['failures']} "
                f"avg={summary['avg_ms']:.1f}ms p95={summary['p95_ms']:.1f}ms max={summary['max_ms']:.1f}ms"
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
    if result.metrics_timeline_summary_by_phase:
        print("Metrics timeline summary by phase:")
        for phase in sorted(result.metrics_timeline_summary_by_phase.keys()):
            print(f"  [{phase}]")
            for key in sorted(result.metrics_timeline_summary_by_phase[phase].keys()):
                summary = result.metrics_timeline_summary_by_phase[phase][key]
                print(
                    f"    {key}: count={summary['count']} avg={summary['avg']:.1f} "
                    f"p95={summary['p95']:.1f} max={summary['max']:.1f}"
                )
    if result.metrics_label_summary_by_phase:
        print("Metrics label summary by phase:")
        for phase in sorted(result.metrics_label_summary_by_phase.keys()):
            print(f"  [{phase}]")
            for key in sorted(result.metrics_label_summary_by_phase[phase].keys()):
                print(f"    {key}: {result.metrics_label_summary_by_phase[phase][key]}")
    if result.profiling_outputs:
        print("Profiling outputs:")
        for key in sorted(result.profiling_outputs.keys()):
            print(f"  {key}: {result.profiling_outputs[key]}")
    if result.profile_command:
        print(
            "Profiler:\n"
            f"  command={result.profile_command}\n"
            f"  exit_code={result.profile_exit_code}\n"
            f"  stdout={result.profile_stdout_log}\n"
            f"  stderr={result.profile_stderr_log}"
        )
    if result.perf_data_path:
        print(
            "Perf capture:\n"
            f"  data={result.perf_data_path}\n"
            f"  exit_code={result.perf_exit_code}\n"
            f"  stdout={result.perf_stdout_log}\n"
            f"  stderr={result.perf_stderr_log}"
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
    parser.add_argument("--probe-workers", type=int, default=DEFAULT_PROBE_WORKERS)
    parser.add_argument("--search-workers", type=int, default=DEFAULT_SEARCH_WORKERS)
    parser.add_argument(
        "--probe-profile",
        choices=["standard", "dashboard"],
        default=DEFAULT_PROBE_PROFILE,
        help="Which GET routes to probe during import. 'dashboard' mirrors the Typesense dashboard control-plane view.",
    )
    parser.add_argument(
        "--probe-route",
        action="append",
        default=[],
        help="Additional GET route to probe during import. May be passed multiple times.",
    )
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
    parser.add_argument(
        "--client-chunk-bytes",
        type=int,
        help="Optionally stream import requests in fixed-size chunks instead of sending the whole body at once.",
    )
    parser.add_argument(
        "--client-chunk-delay-ms",
        type=float,
        default=0.0,
        help="Optional delay between streamed client-side import chunks.",
    )
    parser.add_argument(
        "--profile-cmd",
        help=(
            "Optional shell command to run after the server starts and before imports begin. "
            "The command receives TYPESENSE_PID, TYPESENSE_BASE_URL, TYPESENSE_API_KEY, "
            "TYPESENSE_TEMP_DIR, TYPESENSE_BINARY, and TYPESENSE_PROFILE_LABEL in its environment."
        ),
    )
    parser.add_argument(
        "--profile-wait-timeout",
        type=float,
        default=30.0,
        help="How long to wait for --profile-cmd to exit after the workload finishes before terminating it.",
    )
    parser.add_argument(
        "--perf-seconds",
        type=float,
        default=0.0,
        help="Optionally capture a host-side perf record attached to the server PID for this many seconds.",
    )
    parser.add_argument(
        "--perf-offcpu-seconds",
        type=float,
        default=0.0,
        help="Optionally capture a host-side perf off-CPU profile attached to the server PID for this many seconds.",
    )
    parser.add_argument(
        "--perf-frequency",
        type=int,
        default=199,
        help="Sampling frequency used when --perf-seconds is enabled.",
    )
    parser.add_argument(
        "--perf-call-graph",
        default="dwarf,16384",
        help="Call graph mode passed to perf record when --perf-seconds/--perf-offcpu-seconds are enabled.",
    )
    parser.add_argument(
        "--perf-stat-seconds",
        type=float,
        default=0.0,
        help="Optionally capture a host-side perf stat summary for this many seconds.",
    )
    parser.add_argument(
        "--perf-stat-events",
        default="task-clock,context-switches,cpu-migrations,page-faults",
        help="Comma-separated perf stat events used when --perf-stat-seconds is enabled.",
    )
    parser.add_argument(
        "--runqlat-seconds",
        type=float,
        default=0.0,
        help="Optionally capture run queue latency histograms for this many seconds with runqlat.",
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
                "probe_phase_stats": result.probe_phase_stats,
                "search_stats": result.search_stats,
                "search_phase_stats": result.search_phase_stats,
                "metrics_timeline_summary": result.metrics_timeline_summary,
                "metrics_timeline_summary_by_phase": result.metrics_timeline_summary_by_phase,
                "metrics_label_summary_by_phase": result.metrics_label_summary_by_phase,
                "final_metrics": result.final_metrics,
                "profiling_outputs": result.profiling_outputs,
                "profile_command": result.profile_command,
                "profile_exit_code": result.profile_exit_code,
                "profile_stdout_log": result.profile_stdout_log,
                "profile_stderr_log": result.profile_stderr_log,
                "perf_data_path": result.perf_data_path,
                "perf_exit_code": result.perf_exit_code,
                "perf_stdout_log": result.perf_stdout_log,
                "perf_stderr_log": result.perf_stderr_log,
                "stdout_log": result.stdout_log,
                "stderr_log": result.stderr_log,
            }
            for result in results
        ]
        args.json_output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    return 0


if __name__ == "__main__":
    sys.exit(main())
