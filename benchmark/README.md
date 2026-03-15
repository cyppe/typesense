# Typesense Benchmark CLI

This directory contains the benchmark CLI and dashboard assets used for fork-vs-baseline performance comparisons.

For most repo work, the supported entrypoint is the root wrapper script:

```bash
TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile standard --scope core
```

The wrapper builds and runs the benchmark CLI in Docker by default, so the host only needs Docker. Use `--host-bun` only when you intentionally want to iterate on the benchmark CLI outside its container.

Use the CLI in `benchmark/` directly only when you are developing the benchmark tool itself or need custom invocations beyond the wrapper.

`benchmark/BENCHMARK_RESULTS.md` owns benchmark policy and accepted tuning decisions. Keep this README focused on benchmark-tool usage.

## Prerequisites

Default wrapper path:
- Docker
- A built `typesense-server` binary when you are not using `scripts/benchmark_vs_upstream.sh --build`

Direct CLI development:
- Node.js 24 LTS
- Bun 1.3.10

## Default Workflow

From the repo root:

```bash
# Standard comparison against upstream release
TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile standard --scope core

# Faster feedback loop
TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile quick --scope core

# Mixed read/write stress validation
TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile write-stress --scope extended

# Reproduce the benchmark lane locally against the same freshly built binary
TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --self-compare --profile quick --scope core
```

See `benchmark/BENCHMARK_RESULTS.md` for the meaning of each profile and the accepted default tuning posture.

Live benchmark output still forwards server stdout/stderr, but the harness now collapses repeated
`Threadpool exhaustion detected` stderr bursts into periodic summaries so CI logs stay readable.

## CLI Development

Install dependencies and build the CLI:

```bash
cd benchmark
bun install
bun run build
```

If you want the root wrapper to use host Bun instead of the default Dockerized CLI, pass:

```bash
TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --host-bun --build --profile quick --scope core
```

## Usage

The CLI currently exposes two commands: `install` and `benchmark`.

### Installing Typesense

```bash
bun dist/index.js install [options]

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
bun dist/index.js benchmark [options]

Options:
  --commit-hashes <hashes...>    Commits to compare
  --binaries <paths...>         Paths to pre-built binaries to compare
  --batch-size <num>            Batch size for indexing (default: 100)
  --duration <time>             Duration for search tests (e.g., "30s", "1m")
  --scope <scope>               core (index + search) or extended (core + stress/concurrent/metrics)
  --port <port>                 Base port for benchmark runs
  --server-args <args...>       Extra server arguments passed through to Typesense
  --fail <percentage>           Regression threshold percentage (default: 50)
  --api-key <key>              API key for Typesense
  -v, --verbose                Enable verbose output
```

## Configuration

The tool can be configured through command-line options or environment variables:

- `OPENAI_API_KEY`: Your OpenAI API key for embedding tests
- `TYPESENSE_REQUEST_TIMEOUT_MS`: forwarded by the wrapper/launcher into benchmark server containers when one-shot imports need more than the default `60000ms`

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

## Historical Raft Comparison Notes

The old `//:nuraft-prototype-benchmark` target and the `raft-recovery`, `raft-api-replay`, and `raft-runtime-contention` wrapper profiles were retired when this branch removed the in-tree `braft`/`brpc` runtime. Historical side-by-side results still live in `benchmark/BENCHMARK_RESULTS.md` as archival decision evidence for the cutover.

On `v32`, Run 27 in `benchmark/BENCHMARK_RESULTS.md` remains the canonical post-cutover benchmark baseline. Do not recreate the deleted prototype target just to refresh dates; rerun `scripts/benchmark_vs_upstream.sh` only when benchmark-sensitive import/search/runtime changes need fresh evidence.
