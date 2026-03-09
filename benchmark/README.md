# Typesense Benchmark CLI

This directory contains the benchmark CLI and dashboard assets used for fork-vs-baseline performance comparisons.

For most repo work, the supported entrypoint is the root wrapper script:

```bash
scripts/benchmark_vs_upstream.sh --build --profile standard
```

Use the CLI in `benchmark/` directly only when you are developing the benchmark tool itself or need custom invocations beyond the wrapper.

`benchmark/BENCHMARK_RESULTS.md` owns benchmark policy and accepted tuning decisions. Keep this README focused on benchmark-tool usage.

## Prerequisites

- Docker
- Node.js 20+
- Bun 1.3+
- A built `typesense-server` binary when you are not using `scripts/benchmark_vs_upstream.sh --build`

## Default Workflow

From the repo root:

```bash
# Standard comparison against upstream release
scripts/benchmark_vs_upstream.sh --build --profile standard

# Faster feedback loop
scripts/benchmark_vs_upstream.sh --build --profile quick

# Mixed read/write stress validation
scripts/benchmark_vs_upstream.sh --build --profile write-stress
```

See `benchmark/BENCHMARK_RESULTS.md` for the meaning of each profile and the accepted default tuning posture.

## CLI Development

Install dependencies and build the CLI:

```bash
cd benchmark
bun install
bun run build
```

## Usage

The CLI currently exposes two commands: `install` and `benchmark`.

### Installing Typesense

```bash
./dist/index.js install [options]

Options:
  -n, --container-name <name>     Name for the Docker container (default: "bazel-build")
  -i, --image-name <image>        Name for the Docker image (default: "ubuntu-build")
  -g, --typesense-git-url <url>   Git URL for the Typesense repo
  -d, --working-directory <dir>   Working directory for installation
  -c, --commitHash <hash>         Specific commit to install
  -y, --yes                       Answer yes to all prompts
  -v, --verbose                   Enable verbose output
```

### Running Benchmarks

```bash
./dist/index.js benchmark [options]

Options:
  --commit-hashes <hashes...>    Commits to compare
  --binaries <paths...>         Paths to pre-built binaries to compare
  --batch-size <num>            Batch size for indexing (default: 100)
  --duration <time>             Duration for search tests (e.g., "30s", "1m")
  --port <port>                 Base port for benchmark runs
  --server-args <args...>       Extra server arguments passed through to Typesense
  --fail <percentage>           Regression threshold percentage (default: 50)
  --api-key <key>              API key for Typesense
  -v, --verbose                Enable verbose output
```

## Configuration

The tool can be configured through command-line options or environment variables:

- `OPENAI_API_KEY`: Your OpenAI API key for embedding tests

Key files:

- `tsconfig.json`: TypeScript configuration
- `tsup.config.ts`: Build configuration
- `eslint.config.mjs`: Linting rules
- `src/commands/benchmark.ts`: benchmark command implementation
- `src/benchmarks/metrics-collector.ts`: metrics extraction during runs
- `dashboards/typesense-benchmark.json`: Grafana dashboard

## Development

### Local validation

```bash
bun test
```

## NuRaft Prototype Benchmark

The NuRaft feasibility sprint uses a separate Bazel target instead of this HTTP benchmark CLI:

```bash
scripts/bazel_in_docker.sh build //:nuraft-prototype-benchmark
scripts/bazel_in_docker.sh run //:nuraft-prototype-benchmark -- --mode=all --docs=1000 --post-snapshot-docs=100
scripts/bazel_in_docker.sh run //:nuraft-prototype-benchmark -- --mode=snapshot-pressure --docs=1000 --post-snapshot-docs=200 --snapshot-rounds=3
scripts/bazel_in_docker.sh run //:nuraft-prototype-benchmark -- --mode=snapshot-policy-compare --docs=1000 --post-snapshot-docs=200 --snapshot-rounds=3
```

This target measures the isolated prototype's append/apply path, snapshot-install/recovery path, repeated timed-snapshot pressure during follower outage, and direct comparison between leader-only and `require-healthy-peers` timed-snapshot policy. It is intentionally separate from `scripts/benchmark_vs_upstream.sh`, which still benchmarks the normal HTTP server binaries.

## Raft Recovery Comparison

The root wrapper now also owns the canonical recovery comparison between the live `braft` runtime path and the isolated NuRaft prototype:

```bash
scripts/benchmark_vs_upstream.sh --build --profile raft-recovery
```

This profile does not benchmark upstream releases. Instead it:
- stages a runtime bundle for the current fork's `typesense-server`,
- runs a simple steady single-node write case against `braft`,
- runs `//:nuraft-prototype-benchmark --mode=append-apply` with the same initial write count,
- runs a real 3-node follower-outage/rejoin scenario against `braft`,
- runs `//:nuraft-prototype-benchmark --mode=snapshot-policy-compare` with the same write/outage counts,
- runs a late-third-node join scenario against `braft`,
- runs `//:nuraft-prototype-benchmark --mode=delayed-join` with the same pre-join and post-snapshot write counts,
- prints a simple side-by-side summary, and
- writes the full JSON payload to `~/.cache/typesense/benchmark/raft-recovery-summary.json`.

The JSON also carries explicit recovery-path classification (`snapshot-install-only`, `snapshot-install-plus-log-replay`, or `log-replay-only`) plus process-level CPU and RSS samples for the live `braft` leader/follower and the NuRaft benchmark process. Treat the CPU/RSS rows as directional overhead signals only: the `braft` numbers come from the real HTTP server runtime, while the NuRaft numbers still come from the isolated prototype binary.

Useful knobs:

```bash
scripts/benchmark_vs_upstream.sh --profile raft-recovery --docs 200 --post-snapshot-docs 50 --snapshot-rounds 3 --repeats 2
```

## Raft API Replay Comparison

The root wrapper also owns a focused runtime-vs-runtime API replay profile for the currently implemented NuRaft HTTP surface:

```bash
scripts/benchmark_vs_upstream.sh --build --profile raft-api-replay
```

This profile does not use the normal benchmark CLI. Instead it:
- runs the real `scripts/run_api_tests.sh` wrapper against `//:typesense-server`,
- runs the same replay files against `//:typesense-server-nuraft-runtime`,
- currently measures `tests/collections.test.ts` and `tests/documents.test.ts`,
- prints a simple side-by-side summary, and
- writes the full JSON payload to `~/.cache/typesense/benchmark/raft-api-replay-summary.json`.

Treat this as focused runtime-parity timing on implemented surfaces, not as a replacement for the full benchmark dashboard profiles.
