# Typesense Test Runbook

This runbook is Docker-first for reproducibility. Local host toolchains are supported, but optional.

Docs ownership:
- This file owns build/test/replay/API command lines.
- `benchmark/README.md` owns benchmark CLI usage.
- `benchmark/BENCHMARK_RESULTS.md` owns benchmark decisions and tuning guidance.

## 1) Recommended default: Dockerized Bazel

Build the CI toolchain image once, then run Bazel commands inside it.

```bash
scripts/bazel_in_docker.sh --build-image-only
curl -L "https://dl.typesense.org/ci/tyrec/tyrec-1-models.tar.gz" -o "./test/resources/models.tar.gz"
scripts/bazel_in_docker.sh build //:typesense-server
scripts/bazel_in_docker.sh test --cache_test_results=no --test_output=all //:typesense-test --test_timeout=900
```

Notes:
- The image is defined in `docker/ci-bazel.Dockerfile`.
- Bazel output/cache is persisted on host at `$HOME/.cache/typesense/bazel-docker` by default.
- Bazel version is controlled by `.bazelversion` (via bazelisk inside the container).

## 2) Optional local-host flow

Use this only if you intentionally want to run outside Docker.

```bash
scripts/check_local_toolchain.sh
curl -L "https://dl.typesense.org/ci/tyrec/tyrec-1-models.tar.gz" -o "./test/resources/models.tar.gz"
bazel build //:typesense-server
bazel test --cache_test_results=no --test_output=all //:typesense-test --test_timeout=900
```

## 3) Why Docker-first here

- Avoids host-to-host compiler/runtime drift.
- Makes CI and local command lines identical in behavior.
- Centralizes compiler/Bazel/tooling versions in one Dockerfile.

## 4) Known gotcha: GCC 15 + rules_foreign_cc pkgconfig

`rules_foreign_cc` 0.15.1 has a known pkgconfig bootstrap failure with GCC 15 C23 defaults (`goption.c ... gboolean bool`).
To keep builds stable while staying modern, the repo pins C mode to gnu17 (`.bazelrc`) for C codepaths.

## 5) Useful debugging commands

```bash
scripts/bazel_in_docker.sh clean --expunge
scripts/bazel_in_docker.sh test --verbose_failures //:typesense-test
```

## 6) Sanitizer lanes

Use these for memory, UB, and race checks when you are touching lower-level C++ code, build plumbing, or concurrency-sensitive paths.

```bash
scripts/bazel_in_docker.sh test --config=asan --cache_test_results=no --test_output=errors //:typesense-test --test_timeout=1800
scripts/bazel_in_docker.sh test --config=tsan --cache_test_results=no --test_output=errors //:typesense-test --test_timeout=1800
```

## 7) API replay (one-command style)

When API tests fail in CI (especially startup/runtime linker issues), use the API wrapper. It prepares the runtime bundle automatically and runs the Bun harness in Docker by default, so the host does not need Bun installed:

```bash
scripts/bazel_in_docker.sh build //:typesense-server
scripts/run_api_tests.sh -- --no-secrets --download-migration-binary
```

To replay a single lane/file quickly, append the test path:

```bash
scripts/run_api_tests.sh -- --no-secrets tests/health.test.ts
```

To replay the API harness against an alternate built binary (for example the self-contained ONNX Runtime probe), point the wrapper at it directly:

```bash
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-static-one-protobuf-probe -- --no-secrets tests/health.test.ts
```

To replay the bounded NuRaft HTTP runtime lane through the same wrapper:

```bash
scripts/bazel_in_docker.sh build //:typesense-server-nuraft-runtime
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-nuraft-runtime -- --no-secrets tests/nuraft_runtime_smoke.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-nuraft-runtime -- --no-secrets tests/health.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-nuraft-runtime -- --no-secrets tests/nuraft_runtime_cluster.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-nuraft-runtime -- --no-secrets tests/collections.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-nuraft-runtime -- --no-secrets tests/documents.test.ts
```

If you intentionally want to reuse only the single-node phases of a file, the API runner now supports:

```bash
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-nuraft-runtime -- --single-node-only --no-secrets tests/health.test.ts
```

If you intentionally want to bypass the Dockerized Bun image and use host Bun:

```bash
scripts/run_api_tests.sh --host-bun -- --no-secrets tests/health.test.ts
```

## 8) C++ integration replay (one-command style)

When `//:typesense-test` fails in CI, replay the same lane locally with one command. The helper will prewarm the `ts/e5-small` model cache if needed and run the Dockerized Bazel test command with CI-like flags.

```bash
# Replay one failing gtest case quickly
test/scripts/replay_typesense_test.sh FilterTest.FilterTreeIteratorTimeout

# Replay full C++ suite (same helper)
test/scripts/replay_typesense_test.sh
```

You can still append extra Bazel test options after the filter when needed, for example:

```bash
test/scripts/replay_typesense_test.sh FilterTest.FilterTreeIteratorTimeout --runs_per_test=20
```

## 9) NuRaft prototype microbenchmark

Use this only for the isolated NuRaft feasibility sprint. It does not replace the normal HTTP benchmark wrapper in `scripts/benchmark_vs_upstream.sh`.

```bash
scripts/bazel_in_docker.sh build //:nuraft-prototype-benchmark
scripts/bazel_in_docker.sh run //:nuraft-prototype-benchmark -- --mode=all --docs=1000 --post-snapshot-docs=100
scripts/bazel_in_docker.sh run //:nuraft-prototype-benchmark -- --mode=snapshot-pressure --docs=1000 --post-snapshot-docs=200 --snapshot-rounds=3
scripts/bazel_in_docker.sh run //:nuraft-prototype-benchmark -- --mode=snapshot-policy-compare --docs=1000 --post-snapshot-docs=200 --snapshot-rounds=3
```

The binary emits JSON for append/apply throughput, snapshot-recovery timing, repeated timed-snapshot pressure while a follower stays unhealthy, and direct leader-only vs `require-healthy-peers` outage comparison, so Story E can measure the prototype without pretending the normal HTTP benchmark lane already covers NuRaft.

## 10) NuRaft HTTP runtime smoke

Use this when you need the bounded HTTP-facing NuRaft lane rather than the CLI-only prototype controller.

```bash
scripts/bazel_in_docker.sh build //:typesense-server-nuraft-runtime
scripts/bazel_in_docker.sh test //:nuraft-http-runtime-test
```

The current runtime smoke covers a real subprocess-backed HTTP binary with health/status, collection/document writes, restart persistence, snapshot export, and snapshot install into a fresh node. It is intentionally narrower than the full `api_tests` harness and currently keeps the NuRaft write/snapshot response path on a simplified synchronous model until broader async/import/streaming parity is revisited.

The same bounded runtime now also passes the real `api_tests` wrapper for the focused single-node smoke lane:

```bash
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-nuraft-runtime -- --no-secrets tests/nuraft_runtime_smoke.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-nuraft-runtime -- --no-secrets tests/health.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-nuraft-runtime -- --no-secrets tests/nuraft_runtime_cluster.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-nuraft-runtime -- --no-secrets tests/collections.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-nuraft-runtime -- --no-secrets tests/documents.test.ts
```

## 11) Raft recovery comparison

Use this when you need one canonical command that compares the current fork's live `braft` runtime recovery path against the isolated NuRaft prototype on the same follower-outage shape.

```bash
scripts/benchmark_vs_upstream.sh --build --profile raft-recovery
```

Optional knobs:

```bash
scripts/benchmark_vs_upstream.sh --profile raft-recovery --docs 200 --post-snapshot-docs 50 --snapshot-rounds 3 --repeats 2
```

This mode stages a runtime bundle for `//:typesense-server`, runs a simple steady single-node write case plus a real 3-node follower-outage/rejoin scenario and a late-third-node join scenario on the current `braft` path, runs the matching NuRaft prototype `append-apply`, `snapshot-policy-compare`, and `delayed-join` benchmarks, and writes a JSON summary to `~/.cache/typesense/benchmark/raft-recovery-summary.json`. The JSON also includes explicit recovery-path classification (`snapshot-install-only`, `snapshot-install-plus-log-replay`, or `log-replay-only`) plus process-level CPU/RSS samples for the live `braft` leader/follower and the NuRaft benchmark process, with the caveat that this is still prototype-vs-runtime evidence rather than a drop-in server comparison.

## 12) Raft API replay comparison

Use this when you need a focused runtime-vs-runtime comparison on the real API wrapper for the currently implemented NuRaft HTTP surface.

```bash
scripts/benchmark_vs_upstream.sh --build --profile raft-api-replay
```

This mode runs the same `scripts/run_api_tests.sh` replay files against `//:typesense-server` and `//:typesense-server-nuraft-runtime`, currently `tests/collections.test.ts` and `tests/documents.test.ts`, and writes a JSON summary to `~/.cache/typesense/benchmark/raft-api-replay-summary.json`.

## 13) Raft runtime contention comparison

Use this when you need one canonical command for steady-state document read/write pressure on the currently implemented runtime surfaces.

```bash
scripts/benchmark_vs_upstream.sh --build --profile raft-runtime-contention --duration 5s --docs 100 --writer-threads 1 --reader-threads 2 --repeats 3
```

This mode runs the live `//:typesense-server` and `//:typesense-server-nuraft-runtime` binaries directly, preloads a bounded collection, then measures configurable document writers plus configurable document readers against preloaded document ids. It repeats the full comparison `--repeats` times and reports median totals/latencies in `~/.cache/typesense/benchmark/raft-runtime-contention-summary.json`.

Useful isolation variants:

```bash
scripts/benchmark_vs_upstream.sh --build --profile raft-runtime-contention --duration 5s --docs 100 --writer-threads 0 --reader-threads 2 --repeats 3
scripts/benchmark_vs_upstream.sh --build --profile raft-runtime-contention --duration 5s --docs 100 --writer-threads 1 --reader-threads 0 --repeats 3
```

Treat this as bounded runtime contention on currently implemented document CRUD surfaces, not as full product parity. It intentionally does not use the temporary NuRaft search path, because that path is still a simplified compatibility shim rather than decision-grade search behavior.
