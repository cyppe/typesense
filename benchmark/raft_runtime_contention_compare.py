#!/usr/bin/env python3

import argparse
import json
import os
import random
import shutil
import socket
import statistics
import subprocess
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare focused mixed read/write contention on the live braft server and the NuRaft runtime.",
    )
    parser.add_argument("--repo-root", required=True, help="Repository root")
    parser.add_argument("--work-dir", required=True, help="Benchmark work directory")
    parser.add_argument("--braft-binary", required=True, help="Path to the current braft server binary")
    parser.add_argument("--nuraft-binary", required=True, help="Path to the NuRaft runtime binary")
    parser.add_argument("--duration-seconds", type=float, default=10.0, help="Benchmark duration per runtime")
    parser.add_argument("--preload-docs", type=int, default=200, help="Number of docs to pre-create")
    parser.add_argument("--reader-threads", type=int, default=2, help="Concurrent document-read threads")
    parser.add_argument("--repeats", type=int, default=1, help="Number of repeated contention runs to aggregate")
    parser.add_argument("--output", required=True, help="Path to write JSON results")
    return parser.parse_args()


def find_free_ports(count: int) -> list[int]:
    sockets: list[socket.socket] = []
    ports: list[int] = []
    try:
        for _ in range(count):
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.bind(("127.0.0.1", 0))
            sock.listen(1)
            sockets.append(sock)
            ports.append(sock.getsockname()[1])
        return ports
    finally:
        for sock in sockets:
            sock.close()


def proc_snapshot(pid: int) -> dict[str, float]:
    stat_path = Path(f"/proc/{pid}/stat")
    status_path = Path(f"/proc/{pid}/status")
    if not stat_path.exists():
        return {"cpu_ms": 0.0, "rss_kb": 0.0}

    stat_parts = stat_path.read_text().split()
    clock_ticks = os.sysconf("SC_CLK_TCK")
    cpu_ms = ((int(stat_parts[13]) + int(stat_parts[14])) * 1000.0) / float(clock_ticks)

    rss_kb = 0.0
    if status_path.exists():
        for line in status_path.read_text().splitlines():
            if line.startswith("VmRSS:"):
                rss_kb = float(line.split()[1])
                break

    return {"cpu_ms": cpu_ms, "rss_kb": rss_kb}


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round((pct / 100.0) * (len(ordered) - 1)))))
    return ordered[index]


def request_json(url: str, method: str = "GET", body: dict[str, Any] | None = None, timeout: float = 5.0) -> Any:
    headers = {"X-TYPESENSE-API-KEY": "xyz"}
    data = None
    if body is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(body).encode("utf-8")

    request = urllib.request.Request(url, method=method, headers=headers, data=data)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = response.read().decode("utf-8")
            return json.loads(payload) if payload else {}
    except urllib.error.HTTPError as exc:
        payload = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code} for {method} {url}: {payload}") from exc


def wait_for_health(port: int, process: subprocess.Popen[str], timeout_seconds: float = 30.0) -> None:
    deadline = time.monotonic() + timeout_seconds
    url = f"http://127.0.0.1:{port}/health"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited early with code {process.returncode}")
        try:
            request = urllib.request.Request(url, headers={"X-TYPESENSE-API-KEY": "xyz"})
            with urllib.request.urlopen(request, timeout=2.0):
                return
        except Exception:
            time.sleep(0.1)
    raise RuntimeError(f"timed out waiting for /health on port {port}")


def make_collection(port: int) -> None:
    request_json(
        f"http://127.0.0.1:{port}/collections",
        method="POST",
        body={
            "name": "contention_docs",
            "fields": [
                {"name": "id", "type": "string"},
                {"name": "company_name", "type": "string"},
                {"name": "num_employees", "type": "int32"},
                {"name": "country", "type": "string", "facet": True},
            ],
        },
    )


def write_document(port: int, doc_id: int, company_name: str) -> None:
    request_json(
        f"http://127.0.0.1:{port}/collections/contention_docs/documents",
        method="POST",
        body={
            "id": str(doc_id),
            "company_name": company_name,
            "num_employees": 100 + doc_id,
            "country": "US",
        },
    )


def read_document(port: int, doc_id: int) -> None:
    request_json(f"http://127.0.0.1:{port}/collections/contention_docs/documents/{doc_id}", timeout=5.0)


def preload_documents(port: int, count: int) -> None:
    for doc_id in range(count):
        write_document(port, doc_id, f"alpha-company-{doc_id}")


def run_contention(label: str,
                   binary: str,
                   work_dir: Path,
                   duration_seconds: float,
                   preload_docs: int,
                   reader_threads: int) -> dict[str, Any]:
    data_dir = work_dir / f"{label}-data"
    shutil.rmtree(data_dir, ignore_errors=True)
    data_dir.mkdir(parents=True, exist_ok=True)
    log_path = work_dir / f"{label}.log"
    api_port, peer_port = find_free_ports(2)

    if label == "braft":
        command = [
            binary,
            f"--data-dir={data_dir}",
            "--api-address=127.0.0.1",
            f"--api-port={api_port}",
            "--peering-address=127.0.0.1",
            f"--peering-port={peer_port}",
            "--api-key=xyz",
        ]
    else:
        command = [
            binary,
            f"--data-dir={data_dir}",
            "--listen-address=127.0.0.1",
            f"--api-port={api_port}",
            "--node-host=127.0.0.1",
            f"--peering-port={peer_port}",
            "--api-key=xyz",
        ]

    with log_path.open("w", encoding="utf-8") as log_file:
        process = subprocess.Popen(command, stdout=log_file, stderr=subprocess.STDOUT, text=True)

    try:
        wait_for_health(api_port, process)
        make_collection(api_port)
        preload_documents(api_port, preload_docs)

        cpu_before = proc_snapshot(process.pid)
        peak_rss_kb = cpu_before["rss_kb"]
        writer_latencies: list[float] = []
        reader_latencies: list[float] = []
        errors: list[str] = []
        stop_event = threading.Event()
        metrics_lock = threading.Lock()
        next_doc_id = preload_docs

        def rss_sampler() -> None:
            nonlocal peak_rss_kb
            while not stop_event.is_set():
                snapshot = proc_snapshot(process.pid)
                peak_rss_kb = max(peak_rss_kb, snapshot["rss_kb"])
                time.sleep(0.1)

        def writer() -> None:
            nonlocal next_doc_id
            while not stop_event.is_set():
                start = time.perf_counter()
                try:
                    with metrics_lock:
                        doc_id = next_doc_id
                        next_doc_id += 1
                    write_document(api_port, doc_id, f"alpha-writer-{doc_id}")
                    with metrics_lock:
                        writer_latencies.append((time.perf_counter() - start) * 1000.0)
                except Exception as exc:  # noqa: BLE001
                    with metrics_lock:
                        errors.append(f"writer:{exc}")
                    stop_event.set()

        def reader() -> None:
            while not stop_event.is_set():
                start = time.perf_counter()
                try:
                    read_document(api_port, random.randint(0, max(preload_docs - 1, 0)))
                    with metrics_lock:
                        reader_latencies.append((time.perf_counter() - start) * 1000.0)
                except Exception as exc:  # noqa: BLE001
                    with metrics_lock:
                        errors.append(f"reader:{exc}")
                    stop_event.set()

        threads = [threading.Thread(target=rss_sampler, daemon=True), threading.Thread(target=writer, daemon=True)]
        threads.extend(threading.Thread(target=reader, daemon=True) for _ in range(reader_threads))
        for thread in threads:
            thread.start()

        time.sleep(duration_seconds)
        stop_event.set()
        for thread in threads:
            thread.join(timeout=2.0)

        cpu_after = proc_snapshot(process.pid)
        return {
            "label": label,
            "duration_seconds": duration_seconds,
            "preload_docs": preload_docs,
            "reader_threads": reader_threads,
            "writes_completed": len(writer_latencies),
            "reads_completed": len(reader_latencies),
            "write_p50_ms": percentile(writer_latencies, 50),
            "write_p95_ms": percentile(writer_latencies, 95),
            "read_p50_ms": percentile(reader_latencies, 50),
            "read_p95_ms": percentile(reader_latencies, 95),
            "process_cpu_ms": cpu_after["cpu_ms"] - cpu_before["cpu_ms"],
            "process_peak_rss_kb": peak_rss_kb,
            "errors": errors,
            "log_path": str(log_path),
        }
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def aggregate_results(label: str, runs: list[dict[str, Any]]) -> dict[str, Any]:
    if not runs:
        raise ValueError("aggregate_results requires at least one run")

    def med(key: str) -> float:
        return float(statistics.median(run[key] for run in runs))

    return {
        "label": label,
        "repeat_count": len(runs),
        "duration_seconds": runs[0]["duration_seconds"],
        "preload_docs": runs[0]["preload_docs"],
        "reader_threads": runs[0]["reader_threads"],
        "writes_completed": int(round(med("writes_completed"))),
        "reads_completed": int(round(med("reads_completed"))),
        "write_p50_ms": med("write_p50_ms"),
        "write_p95_ms": med("write_p95_ms"),
        "read_p50_ms": med("read_p50_ms"),
        "read_p95_ms": med("read_p95_ms"),
        "process_cpu_ms": med("process_cpu_ms"),
        "process_peak_rss_kb": med("process_peak_rss_kb"),
        "errors": [error for run in runs for error in run["errors"]],
        "log_paths": [run["log_path"] for run in runs],
    }


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    work_dir = Path(args.work_dir).resolve() / "raft-runtime-contention-runs"
    work_dir.mkdir(parents=True, exist_ok=True)
    run_root = work_dir / time.strftime("%Y%m%d-%H%M%S")
    run_root.mkdir(parents=True, exist_ok=True)

    repeats: list[dict[str, Any]] = []
    braft_runs: list[dict[str, Any]] = []
    nuraft_runs: list[dict[str, Any]] = []
    for repeat in range(args.repeats):
        repeat_root = run_root / f"repeat-{repeat + 1}"
        repeat_root.mkdir(parents=True, exist_ok=True)
        braft = run_contention("braft", args.braft_binary, repeat_root, args.duration_seconds, args.preload_docs, args.reader_threads)
        nuraft = run_contention("nuraft-runtime", args.nuraft_binary, repeat_root, args.duration_seconds, args.preload_docs, args.reader_threads)
        braft_runs.append(braft)
        nuraft_runs.append(nuraft)
        repeats.append(
            {
                "repeat": repeat + 1,
                "run_root": str(repeat_root),
                "braft": braft,
                "nuraft": nuraft,
                "summary": {
                    "write_speedup_ratio": (nuraft["writes_completed"] / braft["writes_completed"]) if braft["writes_completed"] else 0.0,
                    "read_speedup_ratio": (nuraft["reads_completed"] / braft["reads_completed"]) if braft["reads_completed"] else 0.0,
                },
            }
        )

    braft = aggregate_results("braft", braft_runs)
    nuraft = aggregate_results("nuraft-runtime", nuraft_runs)

    payload = {
        "run_root": str(run_root),
        "runs": repeats,
        "braft": braft,
        "nuraft": nuraft,
        "summary": {
            "write_speedup_ratio": (nuraft["writes_completed"] / braft["writes_completed"]) if braft["writes_completed"] else 0.0,
            "read_speedup_ratio": (nuraft["reads_completed"] / braft["reads_completed"]) if braft["reads_completed"] else 0.0,
        },
    }

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(json.dumps(payload, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
