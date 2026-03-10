#!/usr/bin/env python3

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare NuRaft prototype recovery behavior against the live braft runtime path.",
    )
    parser.add_argument("--repo-root", required=True, help="Repository root")
    parser.add_argument("--work-dir", required=True, help="Benchmark work directory")
    parser.add_argument("--runtime-bundle", required=True, help="Prepared runtime bundle directory")
    parser.add_argument("--docs", type=int, default=200, help="Initial writes before follower outage")
    parser.add_argument(
        "--post-snapshot-docs",
        type=int,
        default=50,
        help="Writes per outage round after follower shutdown",
    )
    parser.add_argument(
        "--snapshot-rounds",
        type=int,
        default=3,
        help="Outage rounds used for both NuRaft and braft scenarios",
    )
    parser.add_argument(
        "--outage-sleep-seconds",
        type=float,
        default=22.0,
        help="Sleep duration between outage write rounds for the braft scenario",
    )
    parser.add_argument("--repeats", type=int, default=2, help="Number of comparison repeats")
    parser.add_argument(
        "--health-timeout-seconds",
        type=float,
        default=120.0,
        help="Timeout for server /health and cluster startup checks",
    )
    parser.add_argument(
        "--recovery-timeout-seconds",
        type=float,
        default=60.0,
        help="Timeout for follower catch-up after restart",
    )
    parser.add_argument("--api-key", default="xyz", help="API key used for local benchmark servers")
    parser.add_argument(
        "--server-arg",
        action="append",
        default=[],
        help="Additional typesense-server arg for the braft recovery run",
    )
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


def mean(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def json_request(url: str,
                 method: str = "GET",
                 api_key: str | None = None,
                 body: dict[str, Any] | None = None,
                 timeout: float = 5.0) -> Any:
    headers = {}
    data = None
    if api_key:
        headers["X-TYPESENSE-API-KEY"] = api_key
    if body is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(body).encode("utf-8")

    request = urllib.request.Request(url, method=method, headers=headers, data=data)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = response.read().decode("utf-8")
        return json.loads(payload) if payload else {}


def parse_int(value: Any) -> int:
    if value is None:
        return 0
    return int(str(value))


def read_proc_snapshot(pid: int) -> dict[str, float]:
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


def proc_delta(before: dict[str, float], after: dict[str, float]) -> dict[str, float]:
    return {
        "cpu_ms": after["cpu_ms"] - before["cpu_ms"],
        "rss_kb": after["rss_kb"],
    }


def classify_recovery_path(snapshot_install_seen: bool, replay_gap_on_rejoin: int) -> str:
    if snapshot_install_seen:
        if replay_gap_on_rejoin == 0:
            return "snapshot-install-only"
        return "snapshot-install-plus-log-replay"
    return "log-replay-only"


def summarize_recovery_paths(paths: list[str]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for path in paths:
        counts[path] = counts.get(path, 0) + 1
    return counts


def wait_for_health(port: int,
                    timeout_seconds: float,
                    process: subprocess.Popen[bytes] | None = None) -> None:
    deadline = time.monotonic() + timeout_seconds
    url = f"http://127.0.0.1:{port}/health"
    last_error = "health check not attempted"
    while time.monotonic() < deadline:
        if process is not None and process.poll() is not None:
            raise RuntimeError(f"server on port {port} exited early with code {process.returncode}")
        try:
            json_request(url, timeout=2.0)
            return
        except Exception as exc:  # noqa: BLE001
            last_error = str(exc)
            time.sleep(0.25)
    raise RuntimeError(f"timed out waiting for /health on port {port}: {last_error}")


def wait_for_committed_index(port: int,
                             api_key: str,
                             target_index: int,
                             timeout_seconds: float,
                             process: subprocess.Popen[bytes] | None = None,
                             observer: Any | None = None) -> tuple[dict[str, Any], float]:
    deadline = time.monotonic() + timeout_seconds
    url = f"http://127.0.0.1:{port}/status"
    last_status: dict[str, Any] = {}
    while time.monotonic() < deadline:
        if process is not None and process.poll() is not None:
            raise RuntimeError(f"server on port {port} exited early with code {process.returncode}")
        try:
            status = json_request(url, api_key=api_key, timeout=2.0)
            last_status = status
            if observer is not None:
                observer(status)
            if int(status.get("committed_index", 0)) >= target_index and int(status.get("queued_writes", 0)) == 0:
                return status, time.monotonic()
        except Exception:
            pass
        time.sleep(0.25)
    raise RuntimeError(f"timed out waiting for follower to reach committed index {target_index}: {last_status}")


def read_text_from_offset(path: Path, offset: int) -> str:
    if not path.exists():
        return ""
    with path.open("rb") as handle:
        handle.seek(offset)
        return handle.read().decode("utf-8", errors="replace")


def parse_last_index(log_text: str) -> int:
    matches = re.findall(r"Node last_index:\s*(\d+)", log_text)
    return int(matches[-1]) if matches else 0


def parse_first_index(log_text: str) -> int:
    matches = re.findall(r"Node last_index:\s*(\d+)", log_text)
    return int(matches[0]) if matches else 0


def parse_snapshot_install_index(log_text: str) -> int:
    latest = 0
    for pattern in (
        r"last_included_log_index=(\d+)",
        r"last_included_index[:=]\s*(\d+)",
    ):
        matches = re.findall(pattern, log_text)
        if matches:
            latest = max(latest, int(matches[-1]))
    return latest


def effective_replay_gap(final_committed_index: int,
                         restart_last_index: int,
                         snapshot_install_index: int) -> int:
    base_index = snapshot_install_index if snapshot_install_index > 0 else restart_last_index
    return max(0, final_committed_index - base_index)


def latest_snapshot_index(snapshot_root: Path) -> int:
    if not snapshot_root.exists():
        return 0
    latest = 0
    for child in snapshot_root.iterdir():
        if not child.is_dir():
            continue
        match = re.match(r"snapshot_(\d+)$", child.name)
        if match:
            latest = max(latest, int(match.group(1)))
    return latest


def wait_for_snapshot_index(snapshot_root: Path,
                            minimum_index: int,
                            timeout_seconds: float) -> int:
    deadline = time.monotonic() + timeout_seconds
    latest = 0
    while time.monotonic() < deadline:
        latest = latest_snapshot_index(snapshot_root)
        if latest >= minimum_index:
            return latest
        time.sleep(0.25)
    raise RuntimeError(
        f"timed out waiting for snapshot index >= {minimum_index} under {snapshot_root}, latest={latest}"
    )


def wait_for_log_pattern(path: Path,
                         offset: int,
                         pattern: str,
                         timeout_seconds: float,
                         process: subprocess.Popen[bytes] | None = None) -> str:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if process is not None and process.poll() is not None:
            raise RuntimeError(f"server exited early with code {process.returncode} while waiting for log pattern")
        text = read_text_from_offset(path, offset)
        if pattern in text:
            return text
        time.sleep(0.25)
    raise RuntimeError(f"timed out waiting for log pattern '{pattern}' in {path}")


def create_snapshot(api_port: int, api_key: str, snapshot_path: Path) -> Any:
    encoded_path = urllib.parse.quote(str(snapshot_path))
    return json_request(
        f"http://127.0.0.1:{api_port}/operations/snapshot?snapshot_path={encoded_path}",
        method="POST",
        api_key=api_key,
        timeout=120.0,
    )


def run_nuraft_policy_compare(repo_root: Path,
                              docs: int,
                              post_snapshot_docs: int,
                              snapshot_rounds: int) -> dict[str, Any]:
    benchmark_binary = repo_root / "bazel-bin" / "nuraft-prototype-benchmark"
    command = [
        str(benchmark_binary),
        "--mode=snapshot-policy-compare",
        f"--docs={docs}",
        f"--post-snapshot-docs={post_snapshot_docs}",
        f"--snapshot-rounds={snapshot_rounds}",
    ]
    completed = subprocess.run(command, capture_output=True, text=True, check=True, cwd=repo_root)
    return json.loads(completed.stdout)


def run_nuraft_append_apply(repo_root: Path, docs: int) -> dict[str, Any]:
    benchmark_binary = repo_root / "bazel-bin" / "nuraft-prototype-benchmark"
    command = [
        str(benchmark_binary),
        "--mode=append-apply",
        f"--docs={docs}",
    ]
    completed = subprocess.run(command, capture_output=True, text=True, check=True, cwd=repo_root)
    return json.loads(completed.stdout)


def run_nuraft_delayed_join(repo_root: Path, docs: int, post_snapshot_docs: int) -> dict[str, Any]:
    benchmark_binary = repo_root / "bazel-bin" / "nuraft-prototype-benchmark"
    command = [
        str(benchmark_binary),
        "--mode=delayed-join",
        f"--docs={docs}",
        f"--post-snapshot-docs={post_snapshot_docs}",
    ]
    completed = subprocess.run(command, capture_output=True, text=True, check=True, cwd=repo_root)
    return json.loads(completed.stdout)


@dataclass
class ManagedNode:
    name: str
    api_port: int
    peer_port: int
    data_dir: Path
    log_dir: Path
    analytics_dir: Path
    log_path: Path
    process: subprocess.Popen[bytes] | None = None
    log_handle: Any = None

    @property
    def file_log_path(self) -> Path:
        return self.log_dir / "typesense.log"

    def start(self,
              binary_path: Path,
              nodes_file: Path | None,
              api_key: str,
              runtime_lib_dir: Path,
              extra_args: list[str]) -> None:
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self.analytics_dir.mkdir(parents=True, exist_ok=True)
        self.log_path.parent.mkdir(parents=True, exist_ok=True)

        env = os.environ.copy()
        ld_library_path = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = str(runtime_lib_dir) if not ld_library_path else f"{runtime_lib_dir}:{ld_library_path}"

        args = [
            str(binary_path),
            "--peering-address=127.0.0.1",
            f"--data-dir={self.data_dir}",
            f"--api-key={api_key}",
            f"--api-port={self.api_port}",
            "--api-address=127.0.0.1",
            f"--peering-port={self.peer_port}",
            f"--log-dir={self.log_dir}",
            f"--analytics-dir={self.analytics_dir}",
            "--snapshot-interval-seconds=1",
            "--healthy-read-lag=20",
            "--healthy-write-lag=10",
            *extra_args,
        ]
        if nodes_file is not None:
            args.insert(1, f"--nodes={nodes_file}")

        self.log_handle = self.log_path.open("ab")
        self.process = subprocess.Popen(
            args,
            cwd=self.data_dir.parent,
            stdout=self.log_handle,
            stderr=subprocess.STDOUT,
            env=env,
        )

    def stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
        if self.log_handle is not None:
            self.log_handle.close()
            self.log_handle = None
        self.process = None


def create_collection(api_port: int, api_key: str) -> None:
    body = {
        "name": "books",
        "fields": [
            {"name": "title", "type": "string"},
        ],
    }
    json_request(f"http://127.0.0.1:{api_port}/collections", method="POST", api_key=api_key, body=body, timeout=10.0)


def write_documents(api_port: int, api_key: str, start_id: int, count: int) -> float:
    start = time.monotonic()
    for doc_id in range(start_id, start_id + count):
        body = {"id": f"doc-{doc_id}", "title": f"Book {doc_id}"}
        json_request(
            f"http://127.0.0.1:{api_port}/collections/books/documents",
            method="POST",
            api_key=api_key,
            body=body,
            timeout=10.0,
        )
    end = time.monotonic()
    return (end - start) * 1000.0


def run_braft_single_node_write_scenario(binary_path: Path,
                                         runtime_lib_dir: Path,
                                         run_dir: Path,
                                         docs: int,
                                         api_key: str,
                                         health_timeout_seconds: float,
                                         extra_server_args: list[str]) -> dict[str, Any]:
    api_port, peer_port = find_free_ports(2)
    node = ManagedNode(
        name="typesense-single",
        api_port=api_port,
        peer_port=peer_port,
        data_dir=run_dir / "typesense-data",
        log_dir=run_dir / "logs" / "typesense-single",
        analytics_dir=run_dir / "analytics-db",
        log_path=run_dir / "logs" / "typesense-single.log",
    )

    try:
        node.start(binary_path, None, api_key, runtime_lib_dir, extra_server_args)
        wait_for_health(node.api_port, health_timeout_seconds, node.process)
        create_collection(node.api_port, api_key)

        before_proc = read_proc_snapshot(node.process.pid)
        before_metrics = json_request(
            f"http://127.0.0.1:{node.api_port}/metrics.json",
            api_key=api_key,
            timeout=5.0,
        )
        write_ms = write_documents(node.api_port, api_key, 1, docs)
        after_proc = read_proc_snapshot(node.process.pid)
        after_metrics = json_request(
            f"http://127.0.0.1:{node.api_port}/metrics.json",
            api_key=api_key,
            timeout=5.0,
        )

        return {
            "mode": "braft-single-node-write",
            "docs": docs,
            "write_ms": write_ms,
            "writes_per_sec": (1000.0 * float(docs) / write_ms) if write_ms > 0 else 0.0,
            "process": {
                "before": before_proc,
                "after": after_proc,
                "cpu_ms": proc_delta(before_proc, after_proc)["cpu_ms"],
                "rss_kb_before": before_proc["rss_kb"],
                "rss_kb_after": after_proc["rss_kb"],
                "peak_rss_kb": max(before_proc["rss_kb"], after_proc["rss_kb"]),
            },
            "metrics": {
                "typesense_memory_active_bytes_before": parse_int(before_metrics.get("typesense_memory_active_bytes")),
                "typesense_memory_active_bytes_after": parse_int(after_metrics.get("typesense_memory_active_bytes")),
                "typesense_memory_resident_bytes_before": parse_int(before_metrics.get("typesense_memory_resident_bytes")),
                "typesense_memory_resident_bytes_after": parse_int(after_metrics.get("typesense_memory_resident_bytes")),
            },
            "data_dir": str(run_dir),
        }
    finally:
        node.stop()


def run_braft_recovery_scenario(binary_path: Path,
                                runtime_lib_dir: Path,
                                run_dir: Path,
                                docs: int,
                                post_snapshot_docs: int,
                                snapshot_rounds: int,
                                outage_sleep_seconds: float,
                                api_key: str,
                                health_timeout_seconds: float,
                                recovery_timeout_seconds: float,
                                extra_server_args: list[str]) -> dict[str, Any]:
    ports = find_free_ports(6)
    nodes = [
        ManagedNode(
            name=f"typesense-{index + 1}",
            api_port=ports[index * 2],
            peer_port=ports[index * 2 + 1],
            data_dir=run_dir / f"typesense-data-{index + 1}",
            log_dir=run_dir / "logs" / f"typesense-{index + 1}",
            analytics_dir=run_dir / f"analytics-db-{index + 1}",
            log_path=run_dir / "logs" / f"typesense-{index + 1}.log",
        )
        for index in range(3)
    ]

    nodes_file = run_dir / "nodes"
    cluster_spec = ",".join(f"127.0.0.1:{node.peer_port}:{node.api_port}" for node in nodes)
    nodes_file.parent.mkdir(parents=True, exist_ok=True)
    nodes_file.write_text(cluster_spec)

    try:
        for node in nodes:
            node.start(binary_path, nodes_file, api_key, runtime_lib_dir, extra_server_args)
        for node in nodes:
            wait_for_health(node.api_port, health_timeout_seconds, node.process)

        json_request(
            f"http://127.0.0.1:{nodes[0].api_port}/operations/vote",
            method="POST",
            api_key=api_key,
            body={},
            timeout=10.0,
        )
        time.sleep(1.0)

        create_collection(nodes[0].api_port, api_key)
        initial_write_ms = write_documents(nodes[0].api_port, api_key, 1, docs)
        leader_before_outage_proc = read_proc_snapshot(nodes[0].process.pid)
        leader_before_outage_metrics = json_request(
            f"http://127.0.0.1:{nodes[0].api_port}/metrics.json",
            api_key=api_key,
            timeout=5.0,
        )

        leader_log_offset = nodes[0].file_log_path.stat().st_size if nodes[0].file_log_path.exists() else 0
        follower_log_offset = nodes[2].file_log_path.stat().st_size if nodes[2].file_log_path.exists() else 0
        nodes[2].stop()

        outage_write_ms = 0.0
        next_doc_id = docs + 1
        outage_round_committed_indexes: list[int] = []
        leader_outage_samples: list[dict[str, float]] = []
        for _ in range(snapshot_rounds):
            outage_write_ms += write_documents(nodes[0].api_port, api_key, next_doc_id, post_snapshot_docs)
            next_doc_id += post_snapshot_docs
            leader_status = json_request(f"http://127.0.0.1:{nodes[0].api_port}/status", api_key=api_key, timeout=5.0)
            outage_round_committed_indexes.append(int(leader_status.get("committed_index", 0)))
            leader_outage_samples.append(read_proc_snapshot(nodes[0].process.pid))
            time.sleep(outage_sleep_seconds)

        leader_final_status = json_request(f"http://127.0.0.1:{nodes[0].api_port}/status", api_key=api_key, timeout=5.0)
        leader_after_outage_proc = read_proc_snapshot(nodes[0].process.pid)
        leader_after_outage_metrics = json_request(
            f"http://127.0.0.1:{nodes[0].api_port}/metrics.json",
            api_key=api_key,
            timeout=5.0,
        )
        leader_final_committed_index = int(leader_final_status.get("committed_index", 0))
        leader_snapshot_index = latest_snapshot_index(nodes[0].data_dir / "state" / "snapshot")

        restart_started = time.monotonic()
        nodes[2].start(binary_path, nodes_file, api_key, runtime_lib_dir, extra_server_args)
        wait_for_health(nodes[2].api_port, health_timeout_seconds, nodes[2].process)
        follower_recovery_start_proc = read_proc_snapshot(nodes[2].process.pid)
        leader_recovery_peak_rss_kb = leader_after_outage_proc["rss_kb"]
        follower_recovery_peak_rss_kb = follower_recovery_start_proc["rss_kb"]

        def observe_recovery(_status: dict[str, Any]) -> None:
            nonlocal leader_recovery_peak_rss_kb
            nonlocal follower_recovery_peak_rss_kb
            leader_recovery_peak_rss_kb = max(leader_recovery_peak_rss_kb, read_proc_snapshot(nodes[0].process.pid)["rss_kb"])
            follower_recovery_peak_rss_kb = max(follower_recovery_peak_rss_kb, read_proc_snapshot(nodes[2].process.pid)["rss_kb"])

        follower_final_status, restart_completed = wait_for_committed_index(
            nodes[2].api_port,
            api_key,
            leader_final_committed_index,
            recovery_timeout_seconds,
            nodes[2].process,
            observe_recovery,
        )
        leader_after_recovery_proc = read_proc_snapshot(nodes[0].process.pid)
        follower_after_recovery_proc = read_proc_snapshot(nodes[2].process.pid)
        leader_after_recovery_metrics = json_request(
            f"http://127.0.0.1:{nodes[0].api_port}/metrics.json",
            api_key=api_key,
            timeout=5.0,
        )

        leader_log_delta = read_text_from_offset(nodes[0].file_log_path, leader_log_offset)
        follower_log_delta = read_text_from_offset(nodes[2].file_log_path, follower_log_offset)
        follower_restart_last_index = parse_last_index(follower_log_delta)
        follower_snapshot_install_index = parse_snapshot_install_index(follower_log_delta)
        total_docs = docs + (post_snapshot_docs * snapshot_rounds)
        follower_install_snapshot_seen = (
            "on_snapshot_load" in follower_log_delta or
            "InstallSnapshotRequest" in follower_log_delta or
            "snapshot_load_done" in follower_log_delta
        )
        replay_gap_on_rejoin = effective_replay_gap(
            leader_final_committed_index,
            follower_restart_last_index,
            follower_snapshot_install_index,
        )
        recovery_path = classify_recovery_path(follower_install_snapshot_seen, replay_gap_on_rejoin)

        return {
            "mode": "braft-runtime-recovery",
            "docs_before_outage": docs,
            "docs_during_outage": post_snapshot_docs * snapshot_rounds,
            "outage_round_size": post_snapshot_docs,
            "snapshot_rounds": snapshot_rounds,
            "outage_sleep_seconds": outage_sleep_seconds,
            "initial_write_ms": initial_write_ms,
            "outage_write_ms": outage_write_ms,
            "outage_round_committed_indexes": outage_round_committed_indexes,
            "leader_final_committed_index": leader_final_committed_index,
            "leader_snapshot_index_after_outage": leader_snapshot_index,
            "snapshot_gap_to_final": max(0, leader_final_committed_index - leader_snapshot_index),
            "follower_restart_last_index": follower_restart_last_index,
            "follower_snapshot_install_index": follower_snapshot_install_index,
            "replay_gap_on_rejoin": replay_gap_on_rejoin,
            "follower_final_committed_index": int(follower_final_status.get("committed_index", 0)),
            "follower_install_snapshot_seen": follower_install_snapshot_seen,
            "recovery_path": recovery_path,
            "leader_timed_snapshot_success_count": leader_log_delta.count("Timed snapshot succeeded!"),
            "leader_unhealthy_peer_warning_count": leader_log_delta.count("reported unhealthy during snapshot pre-check"),
            "leader_continue_unhealthy_snapshot_count": leader_log_delta.count("Continuing timed snapshot on leader despite"),
            "recovery_ms": (restart_completed - restart_started) * 1000.0,
            "total_docs": total_docs,
            "leader_process": {
                "before_outage": leader_before_outage_proc,
                "after_outage": leader_after_outage_proc,
                "after_recovery": leader_after_recovery_proc,
                "cpu_ms_during_outage": proc_delta(leader_before_outage_proc, leader_after_outage_proc)["cpu_ms"],
                "cpu_ms_during_recovery": proc_delta(leader_after_outage_proc, leader_after_recovery_proc)["cpu_ms"],
                "rss_kb_before_outage": leader_before_outage_proc["rss_kb"],
                "rss_kb_after_outage": leader_after_outage_proc["rss_kb"],
                "rss_kb_after_recovery": leader_after_recovery_proc["rss_kb"],
                "peak_rss_kb_during_outage": max(
                    [leader_before_outage_proc["rss_kb"], leader_after_outage_proc["rss_kb"]] +
                    [sample["rss_kb"] for sample in leader_outage_samples]
                ),
                "peak_rss_kb_during_recovery": leader_recovery_peak_rss_kb,
            },
            "follower_recovery_process": {
                "start": follower_recovery_start_proc,
                "after_recovery": follower_after_recovery_proc,
                "cpu_ms_during_recovery": proc_delta(follower_recovery_start_proc, follower_after_recovery_proc)["cpu_ms"],
                "rss_kb_after_recovery": follower_after_recovery_proc["rss_kb"],
                "peak_rss_kb_during_recovery": follower_recovery_peak_rss_kb,
            },
            "leader_metrics": {
                "typesense_memory_active_bytes_before_outage": parse_int(
                    leader_before_outage_metrics.get("typesense_memory_active_bytes")
                ),
                "typesense_memory_active_bytes_after_outage": parse_int(
                    leader_after_outage_metrics.get("typesense_memory_active_bytes")
                ),
                "typesense_memory_active_bytes_after_recovery": parse_int(
                    leader_after_recovery_metrics.get("typesense_memory_active_bytes")
                ),
                "typesense_memory_resident_bytes_before_outage": parse_int(
                    leader_before_outage_metrics.get("typesense_memory_resident_bytes")
                ),
                "typesense_memory_resident_bytes_after_outage": parse_int(
                    leader_after_outage_metrics.get("typesense_memory_resident_bytes")
                ),
                "typesense_memory_resident_bytes_after_recovery": parse_int(
                    leader_after_recovery_metrics.get("typesense_memory_resident_bytes")
                ),
            },
            "data_dir": str(run_dir),
        }
    finally:
        for node in nodes:
            node.stop()


def run_braft_delayed_join_scenario(binary_path: Path,
                                    runtime_lib_dir: Path,
                                    run_dir: Path,
                                    docs: int,
                                    post_snapshot_docs: int,
                                    snapshot_rounds: int,
                                    api_key: str,
                                    health_timeout_seconds: float,
                                    recovery_timeout_seconds: float,
                                    extra_server_args: list[str]) -> dict[str, Any]:
    ports = find_free_ports(6)
    nodes = [
        ManagedNode(
            name=f"typesense-delayed-{index + 1}",
            api_port=ports[index * 2],
            peer_port=ports[index * 2 + 1],
            data_dir=run_dir / f"typesense-data-{index + 1}",
            log_dir=run_dir / "logs" / f"typesense-delayed-{index + 1}",
            analytics_dir=run_dir / f"analytics-db-{index + 1}",
            log_path=run_dir / "logs" / f"typesense-delayed-{index + 1}.log",
        )
        for index in range(3)
    ]

    nodes_file = run_dir / "nodes"
    two_node_spec = ",".join(f"127.0.0.1:{node.peer_port}:{node.api_port}" for node in nodes[:2])
    three_node_spec = ",".join(f"127.0.0.1:{node.peer_port}:{node.api_port}" for node in nodes)
    nodes_file.parent.mkdir(parents=True, exist_ok=True)
    nodes_file.write_text(two_node_spec)

    total_tail_docs = post_snapshot_docs * snapshot_rounds

    try:
        for node in nodes[:2]:
            node.start(binary_path, nodes_file, api_key, runtime_lib_dir, extra_server_args)
        for node in nodes[:2]:
            wait_for_health(node.api_port, health_timeout_seconds, node.process)

        json_request(
            f"http://127.0.0.1:{nodes[0].api_port}/operations/vote",
            method="POST",
            api_key=api_key,
            body={},
            timeout=10.0,
        )
        time.sleep(1.0)

        create_collection(nodes[0].api_port, api_key)
        initial_write_ms = write_documents(nodes[0].api_port, api_key, 1, docs)
        leader_initial_status = json_request(f"http://127.0.0.1:{nodes[0].api_port}/status", api_key=api_key, timeout=5.0)
        leader_initial_committed_index = int(leader_initial_status.get("committed_index", 0))

        manual_snapshot_dir = run_dir / "manual-snapshot"
        create_snapshot(nodes[0].api_port, api_key, manual_snapshot_dir)
        leader_snapshot_index = wait_for_snapshot_index(
            nodes[0].data_dir / "state" / "snapshot",
            leader_initial_committed_index,
            recovery_timeout_seconds,
        )

        tail_write_ms = write_documents(nodes[0].api_port, api_key, docs + 1, total_tail_docs)
        leader_before_join_proc = read_proc_snapshot(nodes[0].process.pid)
        leader_before_join_metrics = json_request(
            f"http://127.0.0.1:{nodes[0].api_port}/metrics.json",
            api_key=api_key,
            timeout=5.0,
        )
        leader_final_status = json_request(f"http://127.0.0.1:{nodes[0].api_port}/status", api_key=api_key, timeout=5.0)
        leader_final_committed_index = int(leader_final_status.get("committed_index", 0))

        leader_log_offset = nodes[0].file_log_path.stat().st_size if nodes[0].file_log_path.exists() else 0
        follower_log_offset = nodes[2].file_log_path.stat().st_size if nodes[2].file_log_path.exists() else 0
        join_started = time.monotonic()
        nodes_file.write_text(three_node_spec)
        nodes[2].start(binary_path, nodes_file, api_key, runtime_lib_dir, extra_server_args)
        wait_for_health(nodes[2].api_port, health_timeout_seconds, nodes[2].process)
        joiner_recovery_start_proc = read_proc_snapshot(nodes[2].process.pid)
        leader_join_peak_rss_kb = leader_before_join_proc["rss_kb"]
        joiner_peak_rss_kb = joiner_recovery_start_proc["rss_kb"]

        def observe_join(_status: dict[str, Any]) -> None:
            nonlocal leader_join_peak_rss_kb
            nonlocal joiner_peak_rss_kb
            leader_join_peak_rss_kb = max(leader_join_peak_rss_kb, read_proc_snapshot(nodes[0].process.pid)["rss_kb"])
            joiner_peak_rss_kb = max(joiner_peak_rss_kb, read_proc_snapshot(nodes[2].process.pid)["rss_kb"])

        joiner_final_status, join_completed = wait_for_committed_index(
            nodes[2].api_port,
            api_key,
            leader_final_committed_index,
            recovery_timeout_seconds,
            nodes[2].process,
            observe_join,
        )
        leader_after_join_proc = read_proc_snapshot(nodes[0].process.pid)
        joiner_after_proc = read_proc_snapshot(nodes[2].process.pid)
        leader_after_join_metrics = json_request(
            f"http://127.0.0.1:{nodes[0].api_port}/metrics.json",
            api_key=api_key,
            timeout=5.0,
        )

        leader_log_delta = read_text_from_offset(nodes[0].file_log_path, leader_log_offset)
        joiner_log_delta = read_text_from_offset(nodes[2].file_log_path, follower_log_offset)
        joiner_start_last_index = parse_first_index(joiner_log_delta)
        joiner_snapshot_install_index = parse_snapshot_install_index(joiner_log_delta)
        joiner_install_snapshot_seen = (
            "on_snapshot_load" in joiner_log_delta or
            "InstallSnapshotRequest" in joiner_log_delta or
            "snapshot_load_done" in joiner_log_delta
        )
        join_replay_gap = effective_replay_gap(
            leader_final_committed_index,
            joiner_start_last_index,
            joiner_snapshot_install_index,
        )
        join_recovery_path = classify_recovery_path(joiner_install_snapshot_seen, join_replay_gap)

        return {
            "mode": "braft-delayed-join",
            "docs_before_join": docs,
            "docs_after_snapshot_before_join": total_tail_docs,
            "initial_write_ms": initial_write_ms,
            "manual_snapshot_index": leader_snapshot_index,
            "tail_write_ms": tail_write_ms,
            "leader_final_committed_index": leader_final_committed_index,
            "snapshot_gap_to_final": max(0, leader_final_committed_index - leader_snapshot_index),
            "join_recovery_ms": (join_completed - join_started) * 1000.0,
            "joiner_start_last_index": joiner_start_last_index,
            "joiner_snapshot_install_index": joiner_snapshot_install_index,
            "join_replay_gap": join_replay_gap,
            "joiner_final_committed_index": int(joiner_final_status.get("committed_index", 0)),
            "joiner_install_snapshot_seen": joiner_install_snapshot_seen,
            "join_recovery_path": join_recovery_path,
            "leader_peer_refresh_success_count": leader_log_delta.count("Peer refresh succeeded!"),
            "leader_continue_unhealthy_snapshot_count": leader_log_delta.count("Continuing timed snapshot on leader despite"),
            "leader_process": {
                "before_join": leader_before_join_proc,
                "after_join": leader_after_join_proc,
                "cpu_ms_during_join": proc_delta(leader_before_join_proc, leader_after_join_proc)["cpu_ms"],
                "rss_kb_before_join": leader_before_join_proc["rss_kb"],
                "rss_kb_after_join": leader_after_join_proc["rss_kb"],
                "peak_rss_kb_during_join": leader_join_peak_rss_kb,
            },
            "joiner_process": {
                "start": joiner_recovery_start_proc,
                "after_join": joiner_after_proc,
                "cpu_ms_during_join": proc_delta(joiner_recovery_start_proc, joiner_after_proc)["cpu_ms"],
                "rss_kb_after_join": joiner_after_proc["rss_kb"],
                "peak_rss_kb_during_join": joiner_peak_rss_kb,
            },
            "leader_metrics": {
                "typesense_memory_active_bytes_before_join": parse_int(
                    leader_before_join_metrics.get("typesense_memory_active_bytes")
                ),
                "typesense_memory_active_bytes_after_join": parse_int(
                    leader_after_join_metrics.get("typesense_memory_active_bytes")
                ),
                "typesense_memory_resident_bytes_before_join": parse_int(
                    leader_before_join_metrics.get("typesense_memory_resident_bytes")
                ),
                "typesense_memory_resident_bytes_after_join": parse_int(
                    leader_after_join_metrics.get("typesense_memory_resident_bytes")
                ),
            },
            "data_dir": str(run_dir),
        }
    finally:
        for node in nodes:
            node.stop()


def build_summary(results: list[dict[str, Any]]) -> dict[str, Any]:
    nuraft_append_ms = [float(run["nuraft_append_apply"]["append_apply"]["append_ms"]) for run in results]
    nuraft_apply_ms = [float(run["nuraft_append_apply"]["append_apply"]["apply_ms"]) for run in results]
    nuraft_append_eps = [float(run["nuraft_append_apply"]["append_apply"]["append_entries_per_sec"]) for run in results]
    nuraft_apply_eps = [float(run["nuraft_append_apply"]["append_apply"]["apply_entries_per_sec"]) for run in results]
    nuraft_append_cpu = [float(run["nuraft_append_apply"]["process"]["total_cpu_ms"]) for run in results]
    nuraft_append_peak_rss = [float(run["nuraft_append_apply"]["process"]["peak_max_rss_kb"]) for run in results]

    braft_single_write_ms = [float(run["braft_single_node"]["write_ms"]) for run in results]
    braft_single_write_eps = [float(run["braft_single_node"]["writes_per_sec"]) for run in results]
    braft_single_write_cpu = [float(run["braft_single_node"]["process"]["cpu_ms"]) for run in results]
    braft_single_peak_rss = [float(run["braft_single_node"]["process"]["peak_rss_kb"]) for run in results]

    nuraft_delayed_join_ms = [float(run["nuraft_delayed_join"]["delayed_join"]["join_recovery_ms"]) for run in results]
    nuraft_delayed_join_replay = [
        float(run["nuraft_delayed_join"]["delayed_join"]["replayed_entries_after_install"]) for run in results
    ]
    nuraft_delayed_join_cpu = [float(run["nuraft_delayed_join"]["process"]["total_cpu_ms"]) for run in results]
    nuraft_delayed_join_rss = [float(run["nuraft_delayed_join"]["process"]["peak_max_rss_kb"]) for run in results]
    nuraft_delayed_join_paths = [
        str(run["nuraft_delayed_join"]["delayed_join"]["recovery_path"]) for run in results
    ]

    braft_delayed_join_ms = [float(run["braft_delayed_join"]["join_recovery_ms"]) for run in results]
    braft_delayed_join_replay = [float(run["braft_delayed_join"]["join_replay_gap"]) for run in results]
    braft_delayed_join_snapshot_gap = [float(run["braft_delayed_join"]["snapshot_gap_to_final"]) for run in results]
    braft_delayed_join_cpu = [float(run["braft_delayed_join"]["leader_process"]["cpu_ms_during_join"]) for run in results]
    braft_delayed_join_joiner_cpu = [float(run["braft_delayed_join"]["joiner_process"]["cpu_ms_during_join"]) for run in results]
    braft_delayed_join_rss = [float(run["braft_delayed_join"]["leader_process"]["peak_rss_kb_during_join"]) for run in results]
    braft_delayed_join_joiner_rss = [
        float(run["braft_delayed_join"]["joiner_process"]["peak_rss_kb_during_join"]) for run in results
    ]
    braft_delayed_join_paths = [str(run["braft_delayed_join"]["join_recovery_path"]) for run in results]
    braft_delayed_join_snapshot_installs = [
        1.0 if run["braft_delayed_join"]["joiner_install_snapshot_seen"] else 0.0 for run in results
    ]

    nuraft_deltas = [float(run["nuraft"]["snapshot_policy_compare"]["delta_recovery_ms"]) for run in results]
    nuraft_replay_deltas = [
        float(run["nuraft"]["snapshot_policy_compare"]["delta_replayed_entries"]) for run in results
    ]
    nuraft_leader_only = [
        float(run["nuraft"]["snapshot_policy_compare"]["leader_only"]["recovery_total_ms"]) for run in results
    ]
    nuraft_require_healthy = [
        float(run["nuraft"]["snapshot_policy_compare"]["require_healthy_peers"]["recovery_total_ms"])
        for run in results
    ]
    braft_recovery = [float(run["braft"]["recovery_ms"]) for run in results]
    braft_replay_gap = [float(run["braft"]["replay_gap_on_rejoin"]) for run in results]
    braft_snapshot_gap = [float(run["braft"]["snapshot_gap_to_final"]) for run in results]
    braft_timed_snapshots = [float(run["braft"]["leader_timed_snapshot_success_count"]) for run in results]
    braft_snapshot_installs = [1.0 if run["braft"]["follower_install_snapshot_seen"] else 0.0 for run in results]
    braft_recovery_paths = [str(run["braft"]["recovery_path"]) for run in results]
    nuraft_leader_only_paths = [
        str(run["nuraft"]["snapshot_policy_compare"]["leader_only"]["recovery_path"]) for run in results
    ]
    nuraft_require_healthy_paths = [
        str(run["nuraft"]["snapshot_policy_compare"]["require_healthy_peers"]["recovery_path"]) for run in results
    ]
    braft_leader_outage_cpu = [float(run["braft"]["leader_process"]["cpu_ms_during_outage"]) for run in results]
    braft_leader_recovery_cpu = [float(run["braft"]["leader_process"]["cpu_ms_during_recovery"]) for run in results]
    braft_leader_peak_outage_rss = [float(run["braft"]["leader_process"]["peak_rss_kb_during_outage"]) for run in results]
    braft_leader_peak_recovery_rss = [float(run["braft"]["leader_process"]["peak_rss_kb_during_recovery"]) for run in results]
    braft_follower_recovery_cpu = [float(run["braft"]["follower_recovery_process"]["cpu_ms_during_recovery"]) for run in results]
    braft_follower_recovery_peak_rss = [
        float(run["braft"]["follower_recovery_process"]["peak_rss_kb_during_recovery"]) for run in results
    ]
    nuraft_process_cpu = [float(run["nuraft"]["process"]["total_cpu_ms"]) for run in results]
    nuraft_process_peak_rss = [float(run["nuraft"]["process"]["peak_max_rss_kb"]) for run in results]

    return {
        "steady_write": {
            "nuraft_append_ms": {
                "mean": mean(nuraft_append_ms),
                "min": min(nuraft_append_ms),
                "max": max(nuraft_append_ms),
            },
            "nuraft_apply_ms": {
                "mean": mean(nuraft_apply_ms),
                "min": min(nuraft_apply_ms),
                "max": max(nuraft_apply_ms),
            },
            "nuraft_append_entries_per_sec": {
                "mean": mean(nuraft_append_eps),
                "min": min(nuraft_append_eps),
                "max": max(nuraft_append_eps),
            },
            "nuraft_apply_entries_per_sec": {
                "mean": mean(nuraft_apply_eps),
                "min": min(nuraft_apply_eps),
                "max": max(nuraft_apply_eps),
            },
            "nuraft_process_total_cpu_ms": {
                "mean": mean(nuraft_append_cpu),
                "min": min(nuraft_append_cpu),
                "max": max(nuraft_append_cpu),
            },
            "nuraft_process_peak_rss_kb": {
                "mean": mean(nuraft_append_peak_rss),
                "min": min(nuraft_append_peak_rss),
                "max": max(nuraft_append_peak_rss),
            },
            "braft_write_ms": {
                "mean": mean(braft_single_write_ms),
                "min": min(braft_single_write_ms),
                "max": max(braft_single_write_ms),
            },
            "braft_writes_per_sec": {
                "mean": mean(braft_single_write_eps),
                "min": min(braft_single_write_eps),
                "max": max(braft_single_write_eps),
            },
            "braft_process_cpu_ms": {
                "mean": mean(braft_single_write_cpu),
                "min": min(braft_single_write_cpu),
                "max": max(braft_single_write_cpu),
            },
            "braft_process_peak_rss_kb": {
                "mean": mean(braft_single_peak_rss),
                "min": min(braft_single_peak_rss),
                "max": max(braft_single_peak_rss),
            },
        },
        "nuraft": {
            "leader_only_recovery_ms": {
                "mean": mean(nuraft_leader_only),
                "min": min(nuraft_leader_only),
                "max": max(nuraft_leader_only),
            },
            "require_healthy_recovery_ms": {
                "mean": mean(nuraft_require_healthy),
                "min": min(nuraft_require_healthy),
                "max": max(nuraft_require_healthy),
            },
            "delta_recovery_ms": {
                "mean": mean(nuraft_deltas),
                "min": min(nuraft_deltas),
                "max": max(nuraft_deltas),
            },
            "delta_replayed_entries": {
                "mean": mean(nuraft_replay_deltas),
                "min": min(nuraft_replay_deltas),
                "max": max(nuraft_replay_deltas),
            },
            "process_total_cpu_ms": {
                "mean": mean(nuraft_process_cpu),
                "min": min(nuraft_process_cpu),
                "max": max(nuraft_process_cpu),
            },
            "process_peak_max_rss_kb": {
                "mean": mean(nuraft_process_peak_rss),
                "min": min(nuraft_process_peak_rss),
                "max": max(nuraft_process_peak_rss),
            },
            "leader_only_recovery_paths": summarize_recovery_paths(nuraft_leader_only_paths),
            "require_healthy_recovery_paths": summarize_recovery_paths(nuraft_require_healthy_paths),
            "delayed_join_recovery_ms": {
                "mean": mean(nuraft_delayed_join_ms),
                "min": min(nuraft_delayed_join_ms),
                "max": max(nuraft_delayed_join_ms),
            },
            "delayed_join_replayed_entries": {
                "mean": mean(nuraft_delayed_join_replay),
                "min": min(nuraft_delayed_join_replay),
                "max": max(nuraft_delayed_join_replay),
            },
            "delayed_join_process_total_cpu_ms": {
                "mean": mean(nuraft_delayed_join_cpu),
                "min": min(nuraft_delayed_join_cpu),
                "max": max(nuraft_delayed_join_cpu),
            },
            "delayed_join_process_peak_rss_kb": {
                "mean": mean(nuraft_delayed_join_rss),
                "min": min(nuraft_delayed_join_rss),
                "max": max(nuraft_delayed_join_rss),
            },
            "delayed_join_recovery_paths": summarize_recovery_paths(nuraft_delayed_join_paths),
        },
        "braft": {
            "recovery_ms": {
                "mean": mean(braft_recovery),
                "min": min(braft_recovery),
                "max": max(braft_recovery),
            },
            "replay_gap_on_rejoin": {
                "mean": mean(braft_replay_gap),
                "min": min(braft_replay_gap),
                "max": max(braft_replay_gap),
            },
            "snapshot_gap_to_final": {
                "mean": mean(braft_snapshot_gap),
                "min": min(braft_snapshot_gap),
                "max": max(braft_snapshot_gap),
            },
            "leader_timed_snapshot_success_count": {
                "mean": mean(braft_timed_snapshots),
                "min": min(braft_timed_snapshots),
                "max": max(braft_timed_snapshots),
            },
            "leader_cpu_ms_during_outage": {
                "mean": mean(braft_leader_outage_cpu),
                "min": min(braft_leader_outage_cpu),
                "max": max(braft_leader_outage_cpu),
            },
            "leader_cpu_ms_during_recovery": {
                "mean": mean(braft_leader_recovery_cpu),
                "min": min(braft_leader_recovery_cpu),
                "max": max(braft_leader_recovery_cpu),
            },
            "leader_peak_rss_kb_during_outage": {
                "mean": mean(braft_leader_peak_outage_rss),
                "min": min(braft_leader_peak_outage_rss),
                "max": max(braft_leader_peak_outage_rss),
            },
            "leader_peak_rss_kb_during_recovery": {
                "mean": mean(braft_leader_peak_recovery_rss),
                "min": min(braft_leader_peak_recovery_rss),
                "max": max(braft_leader_peak_recovery_rss),
            },
            "follower_cpu_ms_during_recovery": {
                "mean": mean(braft_follower_recovery_cpu),
                "min": min(braft_follower_recovery_cpu),
                "max": max(braft_follower_recovery_cpu),
            },
            "follower_peak_rss_kb_during_recovery": {
                "mean": mean(braft_follower_recovery_peak_rss),
                "min": min(braft_follower_recovery_peak_rss),
                "max": max(braft_follower_recovery_peak_rss),
            },
            "recovery_paths": summarize_recovery_paths(braft_recovery_paths),
            "follower_install_snapshot_seen_runs": int(sum(braft_snapshot_installs)),
            "delayed_join_recovery_ms": {
                "mean": mean(braft_delayed_join_ms),
                "min": min(braft_delayed_join_ms),
                "max": max(braft_delayed_join_ms),
            },
            "delayed_join_replay_gap": {
                "mean": mean(braft_delayed_join_replay),
                "min": min(braft_delayed_join_replay),
                "max": max(braft_delayed_join_replay),
            },
            "delayed_join_snapshot_gap_to_final": {
                "mean": mean(braft_delayed_join_snapshot_gap),
                "min": min(braft_delayed_join_snapshot_gap),
                "max": max(braft_delayed_join_snapshot_gap),
            },
            "delayed_join_leader_cpu_ms": {
                "mean": mean(braft_delayed_join_cpu),
                "min": min(braft_delayed_join_cpu),
                "max": max(braft_delayed_join_cpu),
            },
            "delayed_join_joiner_cpu_ms": {
                "mean": mean(braft_delayed_join_joiner_cpu),
                "min": min(braft_delayed_join_joiner_cpu),
                "max": max(braft_delayed_join_joiner_cpu),
            },
            "delayed_join_leader_peak_rss_kb": {
                "mean": mean(braft_delayed_join_rss),
                "min": min(braft_delayed_join_rss),
                "max": max(braft_delayed_join_rss),
            },
            "delayed_join_joiner_peak_rss_kb": {
                "mean": mean(braft_delayed_join_joiner_rss),
                "min": min(braft_delayed_join_joiner_rss),
                "max": max(braft_delayed_join_joiner_rss),
            },
            "delayed_join_recovery_paths": summarize_recovery_paths(braft_delayed_join_paths),
            "delayed_join_snapshot_install_seen_runs": int(sum(braft_delayed_join_snapshot_installs)),
            "run_count": len(results),
        },
    }


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    work_dir = Path(args.work_dir).resolve()
    runtime_bundle = Path(args.runtime_bundle).resolve()
    output_path = Path(args.output).resolve()

    binary_path = runtime_bundle / "typesense-server"
    runtime_lib_dir = runtime_bundle / "lib"
    if not binary_path.exists():
        raise FileNotFoundError(f"typesense-server not found in runtime bundle: {binary_path}")

    run_root = work_dir / "raft-recovery-runs" / time.strftime("%Y%m%d-%H%M%S")
    if run_root.exists():
        shutil.rmtree(run_root)
    run_root.mkdir(parents=True, exist_ok=True)

    results: list[dict[str, Any]] = []
    for repeat in range(1, args.repeats + 1):
        repeat_dir = run_root / f"repeat-{repeat}"
        repeat_dir.mkdir(parents=True, exist_ok=True)
        nuraft_append_apply = run_nuraft_append_apply(repo_root, args.docs)
        nuraft_delayed_join = run_nuraft_delayed_join(repo_root, args.docs, args.post_snapshot_docs)
        braft_single_node = run_braft_single_node_write_scenario(
            binary_path=binary_path,
            runtime_lib_dir=runtime_lib_dir,
            run_dir=repeat_dir / "braft-single-node",
            docs=args.docs,
            api_key=args.api_key,
            health_timeout_seconds=args.health_timeout_seconds,
            extra_server_args=args.server_arg,
        )
        nuraft_result = run_nuraft_policy_compare(repo_root, args.docs, args.post_snapshot_docs, args.snapshot_rounds)
        braft_result = run_braft_recovery_scenario(
            binary_path=binary_path,
            runtime_lib_dir=runtime_lib_dir,
            run_dir=repeat_dir / "braft",
            docs=args.docs,
            post_snapshot_docs=args.post_snapshot_docs,
            snapshot_rounds=args.snapshot_rounds,
            outage_sleep_seconds=args.outage_sleep_seconds,
            api_key=args.api_key,
            health_timeout_seconds=args.health_timeout_seconds,
            recovery_timeout_seconds=args.recovery_timeout_seconds,
            extra_server_args=args.server_arg,
        )
        braft_delayed_join = run_braft_delayed_join_scenario(
            binary_path=binary_path,
            runtime_lib_dir=runtime_lib_dir,
            run_dir=repeat_dir / "braft-delayed-join",
            docs=args.docs,
            post_snapshot_docs=args.post_snapshot_docs,
            snapshot_rounds=args.snapshot_rounds,
            api_key=args.api_key,
            health_timeout_seconds=args.health_timeout_seconds,
            recovery_timeout_seconds=args.recovery_timeout_seconds,
            extra_server_args=args.server_arg,
        )
        results.append({
            "repeat": repeat,
            "nuraft_append_apply": nuraft_append_apply,
            "nuraft_delayed_join": nuraft_delayed_join,
            "braft_single_node": braft_single_node,
            "nuraft": nuraft_result,
            "braft": braft_result,
            "braft_delayed_join": braft_delayed_join,
        })

    payload = {
        "config": {
            "docs": args.docs,
            "post_snapshot_docs": args.post_snapshot_docs,
            "snapshot_rounds": args.snapshot_rounds,
            "outage_sleep_seconds": args.outage_sleep_seconds,
            "repeats": args.repeats,
            "health_timeout_seconds": args.health_timeout_seconds,
            "recovery_timeout_seconds": args.recovery_timeout_seconds,
            "server_args": args.server_arg,
        },
        "run_root": str(run_root),
        "results": results,
        "summary": build_summary(results),
    }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2) + "\n")
    json.dump(payload, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
