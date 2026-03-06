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

## 6) API replay (one-command style)

When API tests fail in CI (especially startup/runtime linker issues), use the API wrapper. It prepares the runtime bundle automatically and runs the Bun harness in Docker by default, so the host does not need Bun installed:

```bash
scripts/bazel_in_docker.sh build //:typesense-server
scripts/run_api_tests.sh -- --no-secrets --download-migration-binary
```

To replay a single lane/file quickly, append the test path:

```bash
scripts/run_api_tests.sh -- --no-secrets tests/health.test.ts
```

If you intentionally want to bypass the Dockerized Bun image and use host Bun:

```bash
scripts/run_api_tests.sh --host-bun -- --no-secrets tests/health.test.ts
```

## 7) C++ integration replay (one-command style)

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
