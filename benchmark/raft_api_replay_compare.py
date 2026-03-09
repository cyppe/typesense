#!/usr/bin/env python3

import argparse
import json
import shutil
import subprocess
import tempfile
import time
from pathlib import Path


SUITES = (
    ("collections", "tests/collections.test.ts"),
    ("documents", "tests/documents.test.ts"),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare focused API replay runtime costs between the live braft server and the NuRaft runtime.",
    )
    parser.add_argument("--repo-root", required=True, help="Repository root")
    parser.add_argument("--work-dir", required=True, help="Benchmark work directory")
    parser.add_argument("--braft-binary", required=True, help="Path to the current braft server binary")
    parser.add_argument("--nuraft-binary", required=True, help="Path to the NuRaft runtime binary")
    parser.add_argument("--output", required=True, help="Path to write JSON results")
    return parser.parse_args()


def run_suite(repo_root: Path, work_dir: Path, binary: str, label: str, suite_name: str, test_file: str) -> dict:
    bundle_root = repo_root / "tmp" / "raft-api-replay-bundles"
    bundle_root.mkdir(parents=True, exist_ok=True)
    bundle_dir = Path(tempfile.mkdtemp(prefix=f"{label}-{suite_name}-", dir=bundle_root))
    command = [
        "scripts/run_api_tests.sh",
        "--skip-install",
        "--runtime-bundle-dir",
        str(bundle_dir),
        "--server-binary",
        binary,
        "--",
        "--no-secrets",
        test_file,
    ]

    start = time.perf_counter()
    process = subprocess.run(
        command,
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    elapsed_ms = (time.perf_counter() - start) * 1000.0

    output_lines = process.stdout.strip().splitlines()
    result = {
        "suite": suite_name,
        "binary_label": label,
        "binary_path": binary,
        "test_file": test_file,
        "elapsed_ms": elapsed_ms,
        "returncode": process.returncode,
        "output_tail": "\n".join(output_lines[-20:]),
    }

    if process.returncode != 0:
        result["output"] = process.stdout

    shutil.rmtree(bundle_dir, ignore_errors=True)
    return result


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    work_dir = Path(args.work_dir).resolve()
    work_dir.mkdir(parents=True, exist_ok=True)

    runs: list[dict] = []
    for suite_name, test_file in SUITES:
        runs.append(run_suite(repo_root, work_dir, args.braft_binary, "braft", suite_name, test_file))
        runs.append(run_suite(repo_root, work_dir, args.nuraft_binary, "nuraft-runtime", suite_name, test_file))

    for run in runs:
        if run["returncode"] != 0:
            print(run.get("output", run["output_tail"]))
            raise SystemExit(run["returncode"])

    summary = {}
    for suite_name, _ in SUITES:
        braft = next(run for run in runs if run["suite"] == suite_name and run["binary_label"] == "braft")
        nuraft = next(run for run in runs if run["suite"] == suite_name and run["binary_label"] == "nuraft-runtime")
        summary[suite_name] = {
            "braft_elapsed_ms": braft["elapsed_ms"],
            "nuraft_elapsed_ms": nuraft["elapsed_ms"],
            "delta_ms": braft["elapsed_ms"] - nuraft["elapsed_ms"],
            "speedup_ratio": braft["elapsed_ms"] / nuraft["elapsed_ms"] if nuraft["elapsed_ms"] else 0.0,
        }

    payload = {
        "runs": runs,
        "summary": summary,
    }

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(json.dumps(payload, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
