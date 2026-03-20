# Typesense Test Runbook

This runbook is Docker-first for reproducibility. Local host toolchains are supported, but optional.

Docs ownership:
- This file owns build/test/replay/API command lines.
- `benchmark/README.md` owns benchmark CLI usage.
- `benchmark/BENCHMARK_RESULTS.md` owns benchmark decisions and tuning guidance.

## Entrypoint layout

- Start with the repo-level wrappers under `scripts/` for build, API, benchmark, and release tasks.
- Use `test/scripts/replay_typesense_test.sh` for focused C++ test replay.
- Treat `api_tests/scripts/prepare_runtime_bundle.sh`, `test/scripts/prewarm_public_test_models.sh`, and `debian-pkg/*.sh` as support helpers behind those wrappers unless a workflow/debug note explicitly calls for them.
- `scripts/publish_release.sh` is the publish helper for already-built release artifacts, not a build/replay wrapper.

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
- In shared CI, prefer persisting Bazel `disk-cache`, `repository-cache`, and `bazelisk` only. Do not persist the full output tree when a dependency uses host-native CPU codegen, or hosted runners can reuse binaries that crash on different microarchitectures.

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

## 4) GitHub trigger policy

- `tests.yml` is the only automatic CI gate. It runs on `push` and can also be started manually with `workflow_dispatch`.
- `clang-warning-guard.yml`, `flake-detection.yml`, `sanitizer-testing.yml`, `nightly-extended.yml`, `benchmark-testing.yml`, `ort-bundles.yml`, and `release-binaries.yml` are manual-only workflows.
- Prefer replaying the matching local wrapper command before dispatching a heavy manual workflow. This repo's wrappers are the canonical local equivalents of the GitHub lanes.
- For a short “what is this workflow for, and when should I run it?” reference, see [.github/WORKFLOWS.md](/home/cyppe/Projects/forks/typesense-fork/typesense/.github/WORKFLOWS.md).

## 5) Known gotcha: GCC 15 + rules_foreign_cc pkgconfig

`rules_foreign_cc` 0.15.1 has a known pkgconfig bootstrap failure with GCC 15 C23 defaults (`goption.c ... gboolean bool`).
To keep builds stable while staying modern, the repo pins C mode to gnu17 (`.bazelrc`) for C codepaths.

## 6) Useful debugging commands

```bash
scripts/bazel_in_docker.sh clean --expunge
scripts/bazel_in_docker.sh test --verbose_failures //:typesense-test
```

## 7) Workflow-to-local mapping

Use these before pushing or before manually dispatching the heavier GitHub workflows:

```bash
# Public embedding models used by //:typesense-test
bash test/scripts/prewarm_public_test_models.sh "$PWD/tmp/ci-models"

# tests.yml
scripts/bazel_in_docker.sh build //:typesense-server
scripts/bazel_in_docker.sh test --cache_test_results=no --test_output=all //:typesense-test --test_timeout=1200 --flaky_test_attempts=2 --test_env=TYPESENSE_TEST_MODELS_DIR=/work/tmp/ci-models
scripts/run_api_tests.sh -- --no-secrets

# sanitizer-testing.yml
scripts/bazel_in_docker.sh test --config=asan --cache_test_results=no --test_output=errors --test_summary=detailed --flaky_test_attempts=2 //:typesense-test --test_timeout=1800 --test_env=TYPESENSE_TEST_MODELS_DIR=/work/tmp/ci-models
scripts/bazel_in_docker.sh test --config=tsan --cache_test_results=no --test_output=errors --test_summary=detailed --flaky_test_attempts=2 //:typesense-test --test_timeout=3600 --test_env=TYPESENSE_TEST_MODELS_DIR=/work/tmp/ci-models

# flake-detection.yml
scripts/bazel_in_docker.sh test --cache_test_results=no --runs_per_test=20 --test_output=errors --test_summary=detailed //:typesense-test --test_timeout=900 '--test_arg=--gtest_filter=ArtTest.test_art_insert:ArtTest.test_art_insert_search_uuid:ArtTest.test_art_fuzzy_search:MatchTest.MatchScoreWithOffsetWrapAround:CollectionVectorTest.TestImageEmbedding:CollectionCurationTest.OverridesWithSemanticSearch'
for i in $(seq 1 10); do TYPESENSE_DATA_DIR="$PWD/tmp/test-$i" scripts/run_api_tests.sh -- --no-secrets tests/documents.test.ts; done

# nightly-extended.yml
scripts/bazel_in_docker.sh test --cache_test_results=no --runs_per_test=5 --test_output=errors --test_summary=detailed //:typesense-test --test_timeout=1800 --test_env=TYPESENSE_TEST_MODELS_DIR=/work/tmp/ci-models
for i in $(seq 1 3); do TYPESENSE_DATA_DIR="$PWD/tmp/test-$i" scripts/run_api_tests.sh -- --no-secrets; done

# benchmark-testing.yml
TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile standard --scope core

# local benchmark repro against the same latest local binary
TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --self-compare --profile quick --scope core

# ort-bundles.yml
scripts/release_ort_bundle.sh --build --target-arch amd64
scripts/release_ort_bundle.sh --build --target-arch arm64

# startup/help parity for the shipped NuRaft runtime
scripts/bazel_in_docker.sh run //:typesense-server -- --help
scripts/bazel_in_docker.sh test //:nuraft-runtime-options-test --test_output=errors

# focused USearch vector-backend replay: metric-kernel comparison + mixed update/search workload
scripts/bazel_in_docker.sh run //:usearch-vector-backend-benchmark -- --docs 20000 --dims 384 --cycles 10 --updates-per-cycle 400 --replacements-per-cycle 100 --searches-per-cycle 200 --k 20 --ef 80 --kernel-samples 4096 --kernel-repeats 64 --seed 42

# release-binaries.yml (local release replay; use these two wrappers for the Linux server artifact class and the optional Linux GPU deps artifact class)
scripts/release_linux_artifacts.sh --build --with-cuda --version-label 0.0.0-local
scripts/release_linux_artifacts.sh --build --with-cuda --target-arch arm64 --version-label 0.0.0-local
scripts/release_linux_artifacts.sh --build --with-cuda --target-arch arm64 --jemalloc-lg-page16 --version-label 0.0.0-local

# optional Linux GPU deps replay used by release-binaries.yml's gpu-deps job
scripts/release_linux_gpu_deps.sh --build --version-label 0.0.0-local
scripts/release_linux_gpu_deps.sh --build --target-arch arm64 --emit-lg-page16-alias --version-label 0.0.0-local

# release-binaries.yml with a prebuilt ORT bundle extracted inside the checkout
TYPESENSE_ORT_PREBUILT_BUNDLE_DIR="$PWD/tmp/ort-bundle" \
scripts/release_linux_artifacts.sh --with-cuda --version-label 0.0.0-local --target-arch amd64
TYPESENSE_ORT_PREBUILT_BUNDLE_DIR="$PWD/tmp/ort-bundle" \
scripts/release_linux_gpu_deps.sh --version-label 0.0.0-local --target-arch amd64
```

The workflow YAML also layers GitHub-specific cache and artifact plumbing on top of these commands, but the wrappers above are the primary repro paths.
The benchmark wrapper now builds and runs its Bun CLI in Docker by default, so the host does not need Bun installed unless you intentionally use `--host-bun`.
The Linux release wrapper keeps packaging tools inside containers, so the host does not need `alien`, `rpm`, `dpkg-dev`, `objcopy`, or `strip` installed separately.
Linux release binaries now use `--define=use_cuda=on` on Linux so the published `typesense-server` artifact can load optional GPU provider sidecars from `typesense-gpu-deps`. On this branch the GPU surface is limited to ONNX Runtime-backed embeddings/personalization; Whisper remains CPU-only.
Hosted `release-binaries.yml` GPU-deps lanes reuse the matching Linux server build's prebuilt provider sidecars when server artifacts are enabled, and only fall back to a standalone CUDA rebuild when you explicitly dispatch GPU deps without the core server lane.
`ort-bundles.yml` packages the exact CUDA-enabled one-Protobuf ORT install tree into a reusable artifact keyed from the pinned ORT/toolchain inputs. `release-binaries.yml` now auto-discovers the latest matching successful same-branch `ort-bundles.yml` artifact by default, unless you explicitly set `use_prebuilt_ort_bundle=false`; `ort_bundle_run_id` remains as a deterministic override when you want one specific bundle run. `tests.yml` also auto-discovers the latest matching successful same-branch `ort-bundles.yml` artifact before falling back to a source ORT build. The GCC and Clang warning-guardrail builds reuse that same extracted ORT tree too, so the warning-budget checks do not re-enter the slow source ORT path. Local wrapper replays use the same extracted-tree contract through `TYPESENSE_ORT_PREBUILT_BUNDLE_DIR`, which must point inside the checkout so the Dockerized Bazel/release containers can see it at `/work/...`.
Run `clang-warning-guard.yml` manually when a change can plausibly affect clang-only warnings: compiler flags in `.bazelrc`, `scripts/bazel_in_docker.sh`, Bazel external patches/BUILD files, Docker toolchain image changes, or warning-prone first-party C++ touched near templates/macros that GCC and clang diagnose differently. It is no longer part of the automatic push gate.
When `release-binaries.yml` is dispatched with `push_docker_images=true`, the workflow can also publish Linux runtime images to Docker Hub from the already-built Linux release artifacts. Configure `DOCKERHUB_USERNAME` as a GitHub Actions repository variable and `DOCKERHUB_TOKEN` as a repository secret before enabling that path.
Artifact publishing is a separate post-build step via `scripts/publish_release.sh` against the generated `artifacts/` tree, not part of the local replay wrappers above.
Cross-arch local replays of `linux-arm64` or `linux-arm64-lg-page16` from an x86_64 host require Docker arm64 emulation to be enabled.
Darwin release lanes still require native macOS runners. Their current artifact contract is the unstripped `typesense-server` tarball with embedded DWARF plus `typesense-server.md5.txt`; the repo does not currently produce a `.dSYM` sidecar.

## 8) Sanitizer lanes

Use these for memory, UB, and race checks when you are touching lower-level C++ code, build plumbing, or concurrency-sensitive paths.

```bash
scripts/bazel_in_docker.sh test --config=asan --cache_test_results=no --test_output=errors //:typesense-test --test_timeout=1800
scripts/bazel_in_docker.sh test --config=tsan --cache_test_results=no --test_output=errors //:typesense-test --test_timeout=3600
```

Notes:
- The ASAN lane currently keeps `ASAN_OPTIONS=new_delete_type_mismatch=0` in `.bazelrc` because ONNX Runtime Extensions' vision custom-op loader trips a third-party teardown mismatch.
- `CollectionVectorTest.TestVoiceQuery` is skipped in TSAN builds in code because Whisper inference fails functionally under TSAN instrumentation and does not surface a useful race report.

Focused sanitizer replays used during local stabilization:

```bash
# _rand(seed) / sort-path validation under ASAN
scripts/bazel_in_docker.sh test --config=asan --cache_test_results=no --test_output=errors //:typesense-test --test_timeout=1800 '--test_arg=--gtest_filter=CollectionSortingTest.TestSortByRandomOrder'

# Hosted-ASAN timeout replay for voice query
scripts/bazel_in_docker.sh test --config=asan --cache_test_results=no --test_output=errors --runs_per_test=3 //:typesense-test --test_timeout=3600 '--test_arg=--gtest_filter=CollectionVectorTest.TestVoiceQuery' --test_env=TYPESENSE_TEST_MODELS_DIR=/work/tmp/ci-models

# Current TSAN confirmation subset for sanitizer lane work
scripts/bazel_in_docker.sh test --config=tsan --cache_test_results=no --test_output=errors //:typesense-test --test_timeout=3600 '--test_arg=--gtest_filter=CollectionSortingTest.TestSortByRandomOrder:CollectionSpecificMoreTest.SearchCutoffTest:CollectionVectorTest.TestVoiceQuery:NuRaftBootstrapConfigTest.*:NuRaftHttpRuntimeTest.*:NuRaftStateInitializerTest.*' --test_env=TYPESENSE_TEST_MODELS_DIR=/work/tmp/ci-models

# Hosted-TSAN timeout replay for search_cutoff behavior
scripts/bazel_in_docker.sh test --config=tsan --cache_test_results=no --test_output=errors --runs_per_test=3 //:typesense-test --test_timeout=3600 '--test_arg=--gtest_filter=CollectionSpecificMoreTest.SearchCutoffTest'
```

## 9) API replay (one-command style)

When API tests fail in CI (especially startup/runtime linker issues), use the API wrapper. It prepares the runtime bundle automatically and runs the Bun harness in Docker by default, so the host does not need Bun installed:

```bash
scripts/bazel_in_docker.sh build //:typesense-server
scripts/run_api_tests.sh -- --no-secrets
```

To replay a single lane/file quickly, append the test path:

```bash
scripts/run_api_tests.sh -- --no-secrets tests/health.test.ts
```

Env-dependent API lanes use explicit inputs:

```bash
# Secret-gated lane: mirror CI's all-or-nothing policy and provide all 3 vars together.
OPEN_AI_API_KEY=... \
AZURE_OPENAI_API_KEY=... \
AZURE_OPENAI_URL=... \
scripts/run_api_tests.sh -- tests/embedding.test.ts

OPEN_AI_API_KEY=... \
AZURE_OPENAI_API_KEY=... \
AZURE_OPENAI_URL=... \
scripts/run_api_tests.sh -- tests/conversation.test.ts

# TEI lane: start a local TEI endpoint, then point the API harness at it.
docker rm -f tei-container || true
docker run -d --name tei-container --runtime=runc -p 8080:80 \
  ghcr.io/huggingface/text-embeddings-inference:cpu-latest \
  --model-id sentence-transformers/all-MiniLM-L6-v2
TYPESENSE_TEST_TEI_URL=http://localhost:8080 \
scripts/run_api_tests.sh -- --no-secrets tests/tei-integration.test.ts
docker rm -f tei-container
```

Legacy migration replay from pre-NuRaft binaries is currently unsupported in this Bun harness. Do not rely on the removed `--download-migration-binary` flow until a dedicated migration lane is reintroduced.

To replay the current NuRaft HTTP runtime through the same wrapper:

```bash
scripts/bazel_in_docker.sh build //:typesense-server
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/nuraft_runtime_smoke.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/health.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/nuraft_runtime_cluster.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/collections.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/documents.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --single-node-only --no-secrets tests/synonym_sets.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --single-node-only --no-secrets tests/analytics.test.ts
```

If you intentionally want to reuse only the single-node phases of a file, the API runner now supports:

```bash
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --single-node-only --no-secrets tests/health.test.ts
```

If you intentionally want to bypass the Dockerized Bun image and use host Bun:

```bash
scripts/run_api_tests.sh --host-bun -- --no-secrets tests/health.test.ts
```

## 10) C++ integration replay (one-command style)

When `//:typesense-test` fails in CI, replay the same lane locally with one command. The helper will prewarm the public embedding test model cache and run the Dockerized Bazel test command with CI-like flags.

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

## 11) Historical NuRaft prototype note

The old `//:nuraft-prototype-benchmark` target was deleted when this branch became NuRaft-only. Do not use it for routine benchmark refreshes.

Current benchmark work stays on `scripts/benchmark_vs_upstream.sh`, and Run 27 in `benchmark/BENCHMARK_RESULTS.md` remains the canonical post-cutover baseline until benchmark-sensitive HTTP import/search/runtime changes justify a fresh replay.

## 12) NuRaft HTTP runtime smoke

Use this when you need the bounded HTTP-facing NuRaft lane rather than the CLI-only prototype controller.

```bash
scripts/bazel_in_docker.sh build //:typesense-server
scripts/bazel_in_docker.sh test //:nuraft-http-runtime-test
```

The current runtime smoke covers a real subprocess-backed HTTP binary with health/status, collection/document writes, restart persistence, snapshot export, and snapshot install into a fresh node. It is intentionally narrower than the full `api_tests` harness and currently keeps the NuRaft write/snapshot response path on a simplified synchronous model until broader async/import/streaming parity is revisited.

The same bounded runtime now also passes the real `api_tests` wrapper for the focused single-node smoke lane:

```bash
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/nuraft_runtime_smoke.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/health.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/nuraft_runtime_cluster.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/collections.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/documents.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --single-node-only --no-secrets tests/synonym_sets.test.ts
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --single-node-only --no-secrets tests/analytics.test.ts
```

When you want the broadest current NuRaft runtime regression signal, use the full wrapper replay directly:

```bash
scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets
```

Treat that full no-secrets replay as the main hardening lane now. The targeted suite commands above are still useful for isolating failures quickly, but they are no longer enough on their own to claim broad runtime parity.

## 13) Historical Raft comparison note

The old `raft-recovery`, `raft-api-replay`, and `raft-runtime-contention` wrapper profiles were retired once this branch removed the in-tree `braft` runtime. Keep `benchmark/BENCHMARK_RESULTS.md` as the archival record of those side-by-side cutover benchmarks.
