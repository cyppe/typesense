#!/usr/bin/env python3

import argparse
import json
import shutil
import socket
import statistics
import subprocess
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from pathlib import Path


API_KEY = "xyz"


def pick_port() -> int:
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = int(sock.getsockname()[1])
    sock.close()
    return port


def percentile(values: list[float], pct: float) -> float | None:
    if not values:
        return None
    if len(values) == 1:
        return values[0]
    ordered = sorted(values)
    rank = (len(ordered) - 1) * pct
    lower = int(rank)
    upper = min(lower + 1, len(ordered) - 1)
    weight = rank - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def request(url: str,
            method: str = "GET",
            body: str | bytes | None = None,
            headers: dict[str, str] | None = None,
            timeout: float = 10.0) -> tuple[int, bytes]:
    data = None
    if body is not None:
        data = body.encode("utf-8") if isinstance(body, str) else body

    req = urllib.request.Request(url, data=data, method=method)
    for key, value in (headers or {}).items():
        req.add_header(key, value)

    with urllib.request.urlopen(req, timeout=timeout) as response:
        return response.getcode(), response.read()


def wait_for_json(url: str,
                  predicate,
                  headers: dict[str, str] | None = None,
                  timeout: float = 20.0,
                  interval: float = 0.05) -> dict:
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            code, body = request(url, headers=headers, timeout=3.0)
            if code == 200:
                parsed = json.loads(body)
                last = parsed
                if predicate(parsed):
                    return parsed
        except Exception:
            pass
        time.sleep(interval)
    raise RuntimeError(f"Timed out waiting for {url}; last={last}")


def build_import_batch(start_doc_id: int,
                       count: int,
                       payload: str,
                       extra_fields: int) -> bytes:
    lines = []
    for doc_id in range(start_doc_id, start_doc_id + count):
        document = {
            "id": str(doc_id),
            "title": f"title {doc_id}",
        }
        for field_index in range(1, extra_fields + 1):
            document[f"f{field_index}"] = payload
        lines.append(json.dumps(document, separators=(",", ":")))
    return ("\n".join(lines) + "\n").encode("utf-8")


def make_schema(extra_fields: int) -> dict:
    return {
        "name": "books",
        "fields": [{"name": "title", "type": "string"}] + [
            {"name": f"f{field_index}", "type": "string", "optional": True}
            for field_index in range(1, extra_fields + 1)
        ],
    }


def start_node(binary: Path,
               data_dir: Path,
               api_port: int,
               peer_port: int,
               nodes_config: str) -> tuple[subprocess.Popen, Path, object]:
    log_path = data_dir / "server.log"
    log_file = open(log_path, "wb")
    args = [
        str(binary),
        "--data-dir", str(data_dir),
        "--node-host", "127.0.0.1",
        "--listen-address", "127.0.0.1",
        "--listen-port", str(api_port),
        "--api-port", str(api_port),
        "--peering-port", str(peer_port),
        "--api-key", API_KEY,
        "--nodes", nodes_config,
    ]
    process = subprocess.Popen(args, stdout=log_file, stderr=subprocess.STDOUT)
    return process, log_path, log_file


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Replay NuRaft search latency during concurrent imports on a 3-node local cluster.",
    )
    parser.add_argument("--binary", default="./bazel-bin/typesense-server",
                        help="Path to the typesense-server binary.")
    parser.add_argument("--total-docs", type=int, default=30000,
                        help="Total number of imported documents across all workers.")
    parser.add_argument("--http-batch-docs", type=int, default=1000,
                        help="Documents per import HTTP request.")
    parser.add_argument("--server-batch-size", type=int, default=250,
                        help="Import API batch_size query parameter.")
    parser.add_argument("--import-workers", type=int, default=3,
                        help="Concurrent import request workers.")
    parser.add_argument("--extra-fields", type=int, default=30,
                        help="Optional string fields added to each imported document.")
    parser.add_argument("--payload-bytes", type=int, default=1024,
                        help="Payload size for each extra string field.")
    parser.add_argument("--search-interval-ms", type=float, default=10.0,
                        help="Delay between search probes.")
    parser.add_argument("--request-timeout", type=float, default=120.0,
                        help="HTTP timeout for import requests.")
    parser.add_argument("--json-output",
                        help="Write the summary JSON to this path as well as stdout.")
    parser.add_argument("--keep-temp", action="store_true",
                        help="Keep the temporary node directories and logs.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    binary = Path(args.binary).resolve()
    if not binary.exists():
        raise SystemExit(f"Binary not found: {binary}")

    root_tmp = Path(tempfile.mkdtemp(prefix="nuraft-import-search-latency-"))
    processes: list[subprocess.Popen] = []
    log_files = []
    headers = {"X-TYPESENSE-API-KEY": API_KEY}
    json_headers = {**headers, "Content-Type": "application/json"}
    import_headers = {**headers, "Content-Type": "text/plain"}

    try:
        ports = [(pick_port(), pick_port()) for _ in range(3)]
        nodes_config = ",".join(
            f"127.0.0.1:{peer_port}:{api_port}" for api_port, peer_port in ports
        )
        base_urls: list[str] = []
        log_paths: list[str] = []

        for node_index, (api_port, peer_port) in enumerate(ports, start=1):
            data_dir = root_tmp / f"node{node_index}"
            data_dir.mkdir(parents=True, exist_ok=True)
            process, log_path, log_file = start_node(binary, data_dir, api_port, peer_port, nodes_config)
            processes.append(process)
            log_files.append(log_file)
            log_paths.append(str(log_path))
            base_urls.append(f"http://127.0.0.1:{api_port}")

        for base_url in base_urls:
            wait_for_json(base_url + "/status", lambda _: True, headers=headers, timeout=20.0)

        leader_index = None
        deadline = time.time() + 20.0
        latest_statuses = []
        while time.time() < deadline:
            try:
                latest_statuses = [
                    wait_for_json(base_url + "/status", lambda _: True, headers=headers, timeout=5.0)
                    for base_url in base_urls
                ]
            except RuntimeError:
                time.sleep(0.05)
                continue
            leaders = [index for index, status in enumerate(latest_statuses) if status.get("is_leader")]
            if len(leaders) == 1:
                leader_index = leaders[0]
                break
            time.sleep(0.05)
        if leader_index is None:
            raise RuntimeError(f"Leader election failed: {latest_statuses}")

        follower_indices = [index for index in range(3) if index != leader_index]
        search_node_index = follower_indices[0]
        import_node_index = follower_indices[1]

        schema_body = json.dumps(make_schema(args.extra_fields))
        create_status, create_body = request(
            base_urls[leader_index] + "/collections",
            method="POST",
            body=schema_body,
            headers=json_headers,
            timeout=20.0,
        )
        if create_status != 201:
            raise RuntimeError(f"Create collection failed: status={create_status} body={create_body!r}")

        wait_for_json(
            base_urls[search_node_index] + "/collections/books",
            lambda body: body.get("name") == "books",
            headers=headers,
            timeout=20.0,
        )

        payload = "x" * args.payload_bytes
        search_url = base_urls[search_node_index] + "/collections/books/documents/search?" + urllib.parse.urlencode({
            "q": "title",
            "query_by": "title",
            "per_page": 1,
        })
        import_url = (
            base_urls[import_node_index]
            + "/collections/books/documents/import?action=upsert&batch_size="
            + str(args.server_batch_size)
        )

        stop_search = threading.Event()
        search_results: list[dict] = []
        search_errors: list[str] = []
        import_errors: list[str] = []
        search_lock = threading.Lock()
        import_lock = threading.Lock()

        def search_worker() -> None:
            while not stop_search.is_set():
                started = time.perf_counter()
                try:
                    status, body = request(search_url, headers=headers, timeout=10.0)
                    elapsed_ms = (time.perf_counter() - started) * 1000.0
                    parsed = json.loads(body)
                    with search_lock:
                        search_results.append({
                            "status": status,
                            "wall_ms": elapsed_ms,
                            "search_time_ms": parsed.get("search_time_ms"),
                            "found": parsed.get("found"),
                        })
                except Exception as exc:
                    with search_lock:
                        search_errors.append(str(exc))
                time.sleep(args.search_interval_ms / 1000.0)

        def import_worker(worker_index: int, start_doc_id: int) -> None:
            try:
                current = start_doc_id
                docs_per_worker = args.total_docs // args.import_workers
                extra_docs = args.total_docs % args.import_workers
                if worker_index == args.import_workers - 1:
                    docs_per_worker += extra_docs
                end_doc_id = start_doc_id + docs_per_worker
                while current < end_doc_id:
                    count = min(args.http_batch_docs, end_doc_id - current)
                    batch = build_import_batch(current, count, payload, args.extra_fields)
                    status, body = request(
                        import_url,
                        method="POST",
                        body=batch,
                        headers=import_headers,
                        timeout=args.request_timeout,
                    )
                    if status != 200:
                        raise RuntimeError(
                            f"Import worker {worker_index} failed: status={status} body={body[:200]!r}"
                        )
                    current += count
            except Exception as exc:
                with import_lock:
                    import_errors.append(str(exc))

        search_thread = threading.Thread(target=search_worker, daemon=True)
        search_thread.start()

        import_threads = []
        import_started_at = time.perf_counter()
        for worker_index in range(args.import_workers):
            base_docs_per_worker = args.total_docs // args.import_workers
            start_doc_id = worker_index * base_docs_per_worker + 1
            thread = threading.Thread(
                target=import_worker,
                args=(worker_index, start_doc_id),
                daemon=True,
            )
            import_threads.append(thread)
            thread.start()
        for thread in import_threads:
            thread.join()
        import_elapsed_ms = (time.perf_counter() - import_started_at) * 1000.0
        if import_errors:
            raise RuntimeError(f"Import workers failed: {import_errors}")

        stop_search.set()
        search_thread.join(timeout=5.0)

        latest_statuses = []
        deadline = time.time() + 30.0
        while time.time() < deadline:
            latest_statuses = [
                wait_for_json(base_url + "/status", lambda _: True, headers=headers, timeout=5.0)
                for base_url in base_urls
            ]
            if all(
                int(status["live_product_applied_index"]) >= int(status["committed_index"])
                for status in latest_statuses
            ):
                break
            time.sleep(0.05)
        else:
            raise RuntimeError(f"Cluster did not converge after import: {latest_statuses}")

        if not search_results:
            raise RuntimeError(f"No search samples captured. errors={search_errors[:5]}")
        bad_results = [result for result in search_results if result["status"] != 200]
        if bad_results:
            raise RuntimeError(f"Non-200 search samples captured: {bad_results[:5]}")

        wall_values = [float(result["wall_ms"]) for result in search_results]
        internal_values = [
            float(result["search_time_ms"])
            for result in search_results
            if result["search_time_ms"] is not None
        ]

        summary = {
            "import_elapsed_ms": round(import_elapsed_ms, 2),
            "search_samples": len(search_results),
            "search_wall_avg_ms": round(statistics.fmean(wall_values), 2),
            "search_wall_p95_ms": round(percentile(wall_values, 0.95) or 0.0, 2),
            "search_wall_p99_ms": round(percentile(wall_values, 0.99) or 0.0, 2),
            "search_wall_max_ms": round(max(wall_values), 2),
            "search_internal_avg_ms": round(statistics.fmean(internal_values), 2) if internal_values else None,
            "sync_cumulative_calls": [int(status["sync_cumulative_calls"]) for status in latest_statuses],
            "committed_index": [int(status["committed_index"]) for status in latest_statuses],
            "live_product_applied_index": [
                int(status["live_product_applied_index"]) for status in latest_statuses
            ],
            "leader_index": leader_index,
            "search_node_index": search_node_index,
            "import_node_index": import_node_index,
            "log_paths": log_paths,
            "search_error_count": len(search_errors),
            "config": {
                "total_docs": args.total_docs,
                "http_batch_docs": args.http_batch_docs,
                "server_batch_size": args.server_batch_size,
                "import_workers": args.import_workers,
                "extra_fields": args.extra_fields,
                "payload_bytes": args.payload_bytes,
                "search_interval_ms": args.search_interval_ms,
            },
        }

        encoded = json.dumps(summary, indent=2)
        print(encoded)
        if args.json_output:
            Path(args.json_output).write_text(encoded + "\n", encoding="utf-8")
        return 0
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
        for process in processes:
            try:
                process.wait(timeout=5.0)
            except Exception:
                pass
        for log_file in log_files:
            try:
                log_file.close()
            except Exception:
                pass
        if args.keep_temp:
            print(f"Kept temporary cluster data at {root_tmp}", flush=True)
        else:
            shutil.rmtree(root_tmp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
