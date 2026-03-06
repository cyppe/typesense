# Typesense Modernization Plan (Active TODOs Only)

This plan intentionally contains only remaining modernization work.
Completed and historical migration notes are tracked in git history and PRs.

## AI Agent Instructions

**This document is the single source of truth for modernization work.** Every AI agent working on this codebase MUST:

1. **Read this document first** before starting any modernization task.
2. **Update this document** as part of every task:
   - Mark completed items with `[x]` and remove stale detail (keep one-line summary).
   - Add new issues discovered during work to the appropriate section (or to "Known Issues Backlog" if no section fits).
   - Update the "Priority Queue" section to reflect current state.
   - Add important lessons to "Lessons Learned" if they would save a future agent significant time.
3. **Do not duplicate information** — if something is in `bazel/PATCH_DEBT.md`, reference it, don't copy it here.
4. **Commit this file** as part of the same PR/commit series that completes the work. The document must never be out of sync with the code.
5. **Automation default:** continue executing the next open task in the Priority Queue without waiting for a "continue" prompt; only stop when a true blocking dilemma requires user choice.

## Scope

- Keep Typesense modern across build, dependencies, runtime, and test infrastructure.
- Remove flaky behavior and make tests parallel-safe by default.
- Maintain CI parity between local, Docker, and GitHub Actions.

## Current Baseline (Mar 2026)

- Bazel plus Bzlmod baseline is now `9.0.0` (`.bazelversion`, `MODULE.bazel`) with local Dockerized CI-parity validation green.
- `WORKSPACE` is a stub and module-based dependency resolution is active.
- C++ suite `//:typesense-test` is passing in CI-parity Docker after deterministic tie-breaker fixes in grouping tests.
- API no-secrets suite is healthy across all harness phases with migration auto-download; dedicated TEI lane is green when `TYPESENSE_TEST_TEI_URL` is configured.
- Parallel stress validation now shows isolated temp/model paths per process after helper migration; no active shared-path collision failure is known.

## Quick Reference: How To Build And Test

`TESTING_RUNBOOK.md` is the canonical owner for build/test/replay/API command lines.

- Build server: `scripts/bazel_in_docker.sh build //:typesense-server`
- Run API suite: `scripts/run_api_tests.sh -- --no-secrets --download-migration-binary`
- Run benchmarks: `scripts/benchmark_vs_upstream.sh --build --profile standard`

Keep this section short and point to the owning docs instead of duplicating the full command matrix here.

### Environment variables used by `bazel_in_docker.sh`

| Variable | Default | Purpose |
|---|---|---|
| `TYPESENSE_BAZEL_IMAGE` | `typesense/ci-bazel:local` | Docker image tag (CI uses `typesense/ci-bazel:ci`) |
| `TYPESENSE_BAZEL_CACHE_DIR` | `~/.cache/typesense/bazel-docker` | Host-side Bazel cache mount |
| `TYPESENSE_BAZEL_REPO_ENV_CONLYOPTS` | `-std=gnu17` | C-only compiler options passed to Bazel repo env |

### Model artifacts for embedding tests

Some C++ tests require a pre-warmed model cache:
```bash
curl https://dl.typesense.org/ci/tyrec/tyrec-1-models.tar.gz > ./test/resources/models.tar.gz
bash test/scripts/prewarm_e5_small_model.sh "$PWD/tmp/ci-models"
# Then pass: --test_env=TYPESENSE_TEST_MODELS_DIR=/work/tmp/ci-models
```

## CI Workflow Map

All workflows run on `ubuntu-24.04`. Nightly lanes are staggered to avoid resource contention:

| Workflow | File | Trigger | Schedule | Purpose |
|---|---|---|---|---|
| `tests` | `tests.yml` | push, PR | — | Primary gate: build + warning guardrails + C++ tests + API tests + TEI |
| `flake-detection` | `flake-detection.yml` | nightly, PR, manual | `0 2 * * *` | 20x reruns of 6 historically flaky C++ tests + 10x API documents test |
| `sanitizer-testing` | `sanitizer-testing.yml` | nightly, manual | `0 4 * * *` | Full C++ suite under ASAN+UBSAN and TSAN (parallel jobs) |
| `nightly-extended` | `nightly-extended.yml` | nightly, manual | `0 6 * * *` | Full C++ suite 5x stress + full API suite 3x multi-node longevity |
| `benchmark-testing` | `benchmark-testing.yml` | nightly, manual | `0 */12 * * *` | Performance benchmarks |

### Warning guardrails

CI enforces zero-warning policy for both GCC and Clang via:
- `scripts/check_gcc_warning_guardrail.sh` (env: `TYPESENSE_GCC_WARNING_MAX=0`, `TYPESENSE_GCC_TRACKED_WARNING_MAX=0`)
- `scripts/check_clang_warning_guardrail.sh` (env: `TYPESENSE_CLANG_WARNING_MAX=0`, `TYPESENSE_CLANG_TRACKED_WARNING_MAX=0`, `TYPESENSE_CLANG_SPARSEPP_WARNING_MAX=0`)

If you add new first-party code, fix any warnings before committing. Third-party warnings are excluded by the guardrail scripts automatically.

## Definition Of Done (100% Modernized)

- [x] All test suites pass on a clean checkout in CI and locally with documented commands. *(CI green; local repro via `scripts/bazel_in_docker.sh` documented in Quick Reference above.)*
- [x] No known flaky tests after stress validation (`--runs_per_test=20`) on targeted suites. *(Flake-detection and nightly-extended lanes run on schedule; P0 items 1–5 eliminated all known flake sources.)*
- [x] Tests are parallel-safe (no shared writable paths, no port collisions, no hidden global state). *(P0 items 1–4: temp dirs, dynamic ports, no globals — all done and stress-validated.)*
- [x] Bazel 9 migration is complete and stable. *(P1.6 done; `.bazelversion` = 9.0.0, CI green.)*
- [x] Dependency set is current, with patch debt minimized and documented exceptions only. *(12 active patches with justifications in `bazel/PATCH_DEBT.md`. Protobuf 34 blocked on brpc upstream — documented exception.)*
- [x] Warning policy is enforced (no uncontrolled warning growth). *(GCC + Clang guardrails at zero; CI enforces via guardrail scripts.)*

## Priority 0 - Test Reliability And Parallel Safety (Do First)

### 1) Make filesystem usage test-instance isolated

Done. Per-test-instance isolated directories via `test/temp_dir_utils.h`, model fixtures use `TEST_TMPDIR` with per-process fallback. Validated under parallel stress.

### 2) Eliminate port and process collisions in integration/API tests

Done. Dynamic free-port allocation for single-node and multi-node API runs. Validated under parallel stress with no orphaned processes.

### 3) Fix API test harness reliability regressions

Done. Startup diagnostics, migration-binary auto-download, early-exit detection, and actionable error messages all landed. Full `api_tests --no-secrets` pass verified.

### 4) Remove nondeterministic assertions

Done. Audit complete — stabilized grouping, curation ordering, embedding polling, analytics event matching, and sleep-based waits. No test relies on implicit container or filesystem iteration ordering.

### 5) Add an explicit flake-detection lane

- [x] Add CI jobs for stress reruns of historically flaky targets.
- [x] Publish failing seed and command details in CI artifacts for reproducibility.
- [x] Flakes are detected before merge instead of after release (PR trigger added to `flake-detection.yml`).

## Priority 1 - Build And Dependency Modernization

### 6) Migrate from Bazel 8.6.0 to Bazel 9

Done. Repo default is Bazel 9.0.0. CI and local Docker lanes are green. Platform-based config settings, strict action env, and all downstream deps verified.

### 6b) Protobuf upgrade staging (post-Bazel-9 stabilization)

- [x] Staged path completed: `29.0` -> `29.5` -> `33.4` -> `33.5` with Bazel 9 compatibility.
- [ ] Track and resolve Bazel-rule compatibility in external deps (notably `brpc` patching that currently loads `@com_google_protobuf//bazel:cc_proto_library.bzl`).
- [ ] Re-attempt Protobuf 34 only after downstream deps are updated for removed APIs.

**Blocker (do NOT attempt Protobuf 34 yet):** `protobuf@34.0` stable is on BCR, but `brpc 1.16.0` (latest release, Jan 2025) still calls `FieldDescriptor::is_optional()` which was removed in the Protobuf 34 C++ API. No upstream brpc fix exists. Attempting the bump will produce a compile error in brpc. The only path forward is waiting for a brpc release that drops the removed API, or carrying a brpc patch (high maintenance risk). Monitor [brpc releases](https://github.com/apache/brpc/releases) for a version that is protobuf-34-compatible.

### 7) Burn down external patch debt

- [x] Inventory complete with per-patch justification in `bazel/PATCH_DEBT.md`.
- [x] Removed stale patches (`onnx.patch`, `picotls_openssl/0001.patch`).
- [x] `whisper.patch` debug noise cleaned (`6cf44457`).
- [x] `whisper.patch` reduced from 11 hunks to 8 — dropped compiler warning flags, whitespace noise, backtrace removal. Non-speech token expansion kept (test-verified: voice query expects punctuation-free output).
- [x] `quicly/0001.patch` dropped — bumped quicly to `c9167711` (fix commit).
- [ ] Replace patch-only forks with released upstream versions where possible (12 active patches documented with next actions in `bazel/PATCH_DEBT.md`).
- [x] Move brpc inline `patch_cmds` to proper patch file `bazel/brpc/0001_dynamic_annotations_guards.patch`.
- [x] Switch ICU from Typesense fork (`github.com/typesense/icu`) to upstream release tarball (ICU 71.1). Fork carried zero source mods. Patch content unchanged.
- [x] **ICU version upgrade (71.1 → 78.2):** Upgraded to ICU 78.2 (Unicode 14→17, CLDR 48). Same 6 upstream BUILD.bazel files still ship; patch regenerated from 78.2 tarball. AR fix still needed. Zero Typesense code changes required — only stable APIs used.

**Braft patch audit (all 6 confirmed non-droppable):**
- `0001` — fixes build against modern protobuf/abseil; upstream dormant (last real release 2021).
- `0002_ipv6_braft`, `0002_ipv6_brpc`, `0002_ipv6_butil` — Typesense IPv6 support; no upstream equivalent.
- `0004_util_namespace` — namespace fix required by modern abseil; upstream has not adopted.
- `0005_bazel9_string_view` — Bazel 9 / C++20 string_view compat; upstream has not adopted.
- `quicly/0001.patch` — dropped (quicly bumped to `c9167711`).

**Gotchas for patch work:**
- Patch droppability must be verified with a full `bazel build //:typesense-server` inside Docker — header-only changes can appear to succeed in isolation but fail at link time.
- `bazel/whisper.patch` is the largest remaining patch and highest maintenance risk (CUDA/shared-loading paths). ICU patch (`bazel/icu/icu.patch`) was reduced by switching from the Typesense fork to the official ICU 71.1 release tarball — patch content unchanged (BUILD deletions + AR fix) but fork dependency eliminated.
- See `bazel/PATCH_DEBT.md` for the full classified inventory with upstream references and next actions.

### 7b) Coordinated h2o / picotls / quicly bump

These three h2o-ecosystem deps are tightly coupled and must be bumped together. Our pins are ~3.5 years old.

**Security motivation (primary driver):**
- 4 HIGH severity fixes: HTTP/2 Rapid Reset DDoS, HTTP/2 MadeYouReset DDoS, 2x QUIC remote DoS (CVE-2025-61684)
- 2 MEDIUM: TLS session resumption misdirection, 0-RTT access control bypass
- 3 LOW: QUIC state exhaustion, HTTP/3 cancel crash, headers directive ignored

**Performance / feature gains:**
- QUIC pacing + jumpstart (smoother sending, faster reconnects)
- io_uring async file loading in h2o
- ECN (Explicit Congestion Notification) for QUIC
- Post-quantum key exchange (X25519MLKEM768) via OpenSSL 3.5
- ACME built-in certificate management
- OpenSSL 3.5 compat fixes in picotls

**Known breaking changes to handle:**
- `quicly_error_t` expanded from 16-bit to 62-bit error codes — non-issue for Typesense (quicly only used transitively through h2o)
- h2o patch (`bazel/h2o/h2o_*.patch`) must be re-evaluated against new cmake layout
- `retire_cid.c` removed from quicly; BUILD file needs one line deleted
- h2olog tracing subsystem rewritten (no impact on Typesense integration)
- New quicly stats struct fields, pacing config, path migration callbacks (additive, transparent)

**Approach:** Bump all three to match h2o HEAD's submodule pins (h2o→`725e54bc`, picotls→`4e443c11`, quicly→`fd724e03`). Re-evaluate h2o patch. Fix call-site breakage.

- [x] Bump picotls to `4e443c11` and verify BUILD still works
- [x] Bump quicly to `fd724e03` (master), update BUILD for removed/added source files
- [x] Bump h2o to `725e54bc`, re-evaluate and update h2o patch
- [x] Fix any call-site breakage from quicly error type change (none needed — APIs stable)
- [x] Full build + test verification

**Investigation findings (Mar 2026):**

*picotls (`7970614` → `4e443c11`, 864 commits):*
- Core files unchanged: `lib/openssl.c`, `lib/pembase64.c`, `lib/picotls.c` all still exist.
- New files added: `lib/hpke.c` (HPKE/post-quantum), `lib/mbedtls.c`, `lib/mbedtls_sign.c` (MbedTLS backend). These are optional backends — `bazel/picotls_openssl/BUILD` needs no changes (we only compile the OpenSSL backend).
- Minor changes to `lib/openssl.c` (5 add, 6 del) and `lib/fusion.c` (10 add, 6 del).
- Risk: LOW.

*quicly (`c9167711` → `fd724e03`, 669 commits):*
- `lib/retire_cid.c` confirmed REMOVED. All other `.c` files unchanged in set.
- No new `.c` files added to `lib/`.
- BUILD change: delete `"lib/retire_cid.c"` from source list.
- `quicly_error_t` 16→62 bit expansion is transparent — Typesense never calls quicly directly.
- Risk: LOW.

*h2o (`1491a703` → `725e54bc`):*
- CMakeLists.txt significantly restructured: version.h generation changed, fusion/boringssl cmake modules extracted, new optional features added.
- New cmake options to disable: `WITH_IO_URING=OFF`, `WITH_AEGIS=OFF`, `WITH_ACME=OFF`.
- Patch file `bazel/h2o/h2o_*.patch` must be regenerated against new CMakeLists.txt. Current patch actions (remove install rules, skip brotli subdirs) still needed but context lines will differ.
- Risk: MEDIUM-HIGH (patch rework is the hardest part).

*Call-site audit:*
- Typesense uses h2o exclusively through `src/http_server.cpp` and `include/http_server.h`.
- No direct quicly or picotls API usage found.
- All h2o APIs used (`h2o_config_init`, `h2o_evloop_create`, `h2o_accept`, `h2o_send`, `h2o_timer_*`, etc.) are stable public APIs unchanged between versions.
- Expected Typesense code changes: NONE.

*Concrete commit sequence for the bump:*
1. Bump picotls to `4e443c11` in MODULE.bazel (BUILD unchanged).
2. Bump quicly to `fd724e03` in MODULE.bazel; delete `lib/retire_cid.c` from `bazel/quicly/BUILD`.
3. Bump h2o to `725e54bc` in MODULE.bazel; regenerate patch file; add `-DWITH_IO_URING=OFF -DWITH_AEGIS=OFF -DWITH_ACME=OFF` to `bazel/h2o/BUILD` generate_args.
4. Full Docker build + test verification.

### 8) Modernize logging stack (glog to Abseil Logging)

Done. Backend swapped from glog to Abseil Logging. Key artifacts:

- `include/logger.h` — facade macros (`TS_LOG`, `TS_CHECK`, etc.) now delegate to `ABSL_LOG`/`ABSL_CHECK` (prefixed to avoid collision with glog's `LOG()` leaking through brpc).
- `include/ts_log_sink.h` — custom `absl::LogSink` for file output (replaces glog's `SetLogDestination`).
- `src/typesense_server_utils.cpp` — `init_root_logger()` rewritten: glog silenced but kept for brpc/braft, Abseil is primary backend.
- glog remains in `common_deps` as a transitive dependency for brpc/braft (documented exception, commented in BUILD).
- Console logs moved from stdout to stderr (Abseil default, more standard for daemons).

### 9) Warning policy and compiler hygiene

- [x] GCC and Clang warning guardrails at zero (`tracked=0`, `total=0` for both compilers).
- [x] CI enforces guardrails via `scripts/check_clang_warning_guardrail.sh` and `scripts/check_gcc_warning_guardrail.sh` with failure-artifact upload.
- [x] First-party C++ test signedness cleanup committed (`e9bea816`); all `-Wsign-compare` warnings resolved.
- [x] Unused-variable warnings across test files and src cleaned up (`8420f815`).
- Done when:
  - [x] Warning debt trend is downward and enforced by CI.

## Priority 2 - CI And Developer Experience

### 10) Unify test and build entry points

Done. Dockerized Bazel wrapper (`scripts/bazel_in_docker.sh`), CI uses repo Docker toolchain, legacy scripts removed. Repro steps match CI behavior.

### 11) Add modern verification lanes

- [x] Add sanitizer coverage (ASAN, UBSAN, and TSAN).
  - `.bazelrc` `--config=asan` with ASAN+UBSAN flags, `--config=tsan` with TSAN flags.
  - Jemalloc exclusion via `NO_JEMALLOC` define and `select()` in BUILD for both sanitizer configs.
  - Nightly CI lane in `.github/workflows/sanitizer-testing.yml` with parallel ASAN and TSAN jobs.
- [x] Add optional slower nightly checks (stress, migration, multi-node longevity).
  - Full-suite stress (`--runs_per_test=5`) and multi-node API longevity lanes in `.github/workflows/nightly-extended.yml`.
**CI hygiene backlog:**
- [x] Pin Bazelisk version in `docker/ci-bazel.Dockerfile` (pinned to v1.28.1).
- [x] Pin clang version explicitly in Dockerfile (pinned to `clang-18` with `update-alternatives` symlinks).
- [x] Upgrade GitHub Action versions across workflows (audit as of Mar 2026):
  - `actions/checkout` `@v4` → `@v6` — done (credential persistence default change audited; not affected since we don't use `persist-credentials` or post-checkout token operations).
  - `actions/upload-artifact` `@v4` → `@v7` — done (purely additive).
  - ~~`actions/download-artifact` `@v5` → `@v4`~~ done (tests.yml normalized to @v4).
  - ~~`actions/setup-node` `@v3` → `@v4`~~ done (benchmark-testing.yml).
  - ~~`dawidd6/action-download-artifact` `@v2` → `@v6`~~ done (benchmark-testing.yml).
  - `oven-sh/setup-bun` `@v2` — already on latest major.
- [x] Add `bazel/` and `docker/` to flake-detection PR path trigger.
- [x] Bazel disk cache persistence via `actions/cache@v4` across all CI workflows.
  - Caches `${{ github.workspace }}/.cache/bazel-docker` (disk-cache + repository-cache + bazelisk).
  - Key: `bazel-{os}-{workflow}-{hashFiles('MODULE.bazel','BUILD','.bazelrc','bazel/**')}` with prefix restore-keys.
  - Sanitizer jobs use config-specific keys (`-asan-`, `-tsan-`) since different build configs produce incompatible artifacts.
  - `save-always` gated on `github.event_name != 'pull_request'` for workflows with PR triggers (tests, flake-detection) to prevent cache pollution.
  - Cache size reporting step (`du -sh`) added to every job for monitoring; review after first few runs and split if >5GB.

- Done when:
  - [x] Regressions in memory, UB, and race-prone areas are caught automatically.

### 12) Improve observability of test failures

- [x] Standardize artifact capture (CI uploads diagnostics, guardrail logs, testlogs, and API artifacts on failure).
- [x] Provide one-command replay for failed API and integration scenarios.
- Done when:
  - [x] Most failures can be reproduced from CI artifacts without guesswork.

### 13) Benchmark infrastructure audit and local benchmarking

- [x] Investigate current `benchmark-testing.yml` workflow.
- [x] Enable local benchmark execution (documented below).
- [x] Cross-fork comparison: `scripts/benchmark_vs_upstream.sh` downloads an upstream release binary and runs the full benchmark suite against the fork build. Canonical usage now lives in `benchmark/README.md` and `scripts/benchmark_vs_upstream.sh --help`.
- [x] Evaluate whether CI benchmark should compare against a fixed baseline (upstream release) in addition to previous-run regression detection. Decision: the cross-fork script serves as the baseline comparison mechanism; CI continues with commit-to-commit regression detection.

**Audit findings (Mar 2026):**

**Stack:** TypeScript CLI (`benchmark/`) using [Commander](https://github.com/tj/commander.js) + [neverthrow](https://github.com/supermacro/neverthrow). Repo-managed JS tooling is Bun-first. Load generation via [k6](https://k6.io/) (Grafana). Metrics stored in InfluxDB 1.8. Visualization via Grafana 8.5.21. All three services run as Docker Compose containers (`benchmark/docker-compose.yml`).

**CI workflow** (`benchmark-testing.yml`): Runs every 12 hours. Downloads the two most recent successful `typesense-server` binaries from the `tests` workflow using `dawidd6/action-download-artifact@v6`. Starts Docker Compose services, builds the CLI, runs `./dist/index.js benchmark --binaries <old> <new> -c <old-sha> <new-sha> --duration 1m`. InfluxDB data is persisted across runs via artifact upload/download. Results are compared using configurable p95 regression thresholds per scenario.

**Benchmark scenarios (9 search + 1 indexing):**
- `just_q` — typeahead-style incremental search (3-char permutations)
- `q_star` — wildcard match-all
- `filter_simple` / `filter_complex` — single and multi-field filters
- `sort_simple` / `sort_eval_condition` / `sort_eval_score` — sort with `_eval()` expressions
- `facet` — multi-field faceting
- `group` — group-by query
- Bulk import — 1M record JSONL ingestion

Each search scenario runs at **50 VUs** and **100 VUs** sequentially with 5s gaps. k6 reports `search_processing_time_ms` (p95) to InfluxDB tagged by commit hash, scenario, and VU count. The CLI queries InfluxDB, computes percentage change, and fails if regression exceeds configured thresholds (default 50% or absolute ms ceiling per scenario).

**Local execution:**

```bash
# Prerequisites: Docker, Bun 1.3+, a built typesense-server binary

# The supported default is the root wrapper:
scripts/benchmark_vs_upstream.sh --build --profile standard

# Use the raw benchmark CLI only when developing benchmark tooling itself.
```

**Cross-fork comparison feasibility:** The `--binaries` flag accepts arbitrary binary paths, so comparing fork vs upstream is straightforward: download a release binary from typesense.org, build the fork binary, and pass both. The CLI already generates comparison tables and ASCII plots. No workflow changes needed — just provide two binaries.

## Key Files And Directories

| Path | Purpose |
|---|---|
| `MODULE.bazel` | Bzlmod dependency declarations, pinned versions, patch references |
| `.bazelrc` | Bazel build configs including sanitizer flags (`--config=asan`, `--config=tsan`) |
| `.bazelversion` | Pinned Bazel version (`9.0.0`) |
| `BUILD` | Top-level build targets (`//:typesense-server`, `//:typesense-test`) |
| `bazel/` | External dep BUILD files, patches, and `PATCH_DEBT.md` inventory |
| `scripts/bazel_in_docker.sh` | Dockerized build/test wrapper (single entry point for all builds) |
| `scripts/run_api_tests.sh` | Dockerized API test wrapper (runtime bundle + Bun harness) |
| `scripts/benchmark_vs_upstream.sh` | Benchmark wrapper for upstream-vs-fork or explicit binary comparisons |
| `docker/ci-bazel.Dockerfile` | CI Docker image definition |
| `test/temp_dir_utils.h` | Per-test-instance temp directory isolation |
| `test/scripts/prewarm_e5_small_model.sh` | Model cache warmup for embedding tests |
| `api_tests/` | TypeScript/Bun API test suite |
| `include/logger.h` | Logging facade (Abseil backend, `TS_LOG`/`TS_CHECK` macros) |
| `include/ts_log_sink.h` | Custom `absl::LogSink` for file output |
| `bazel/PATCH_DEBT.md` | Classified inventory of all active external dependency patches |

## Priority Queue (What To Work On Next)

This is the **living priority list**. AI agents should pick the top non-blocked item and execute it. Update this list after completing any task.

| # | Task | Section | Status | Notes |
|---|------|---------|--------|-------|
| 1 | ~~Coordinated h2o/picotls/quicly bump~~ | P1.7b | **done** | All 3 deps bumped, patch regenerated, build verified. |
| 2 | ~~ICU fork→upstream tarball~~ | P1.7 | **done** | Switched from `typesense/icu` fork to official ICU 71.1 release tarball. Patch unchanged. |
| 3 | ~~CI action version bumps~~ | P2.11 | **done** | `actions/checkout` v4→v6, `actions/upload-artifact` v4→v7 across all 5 workflows. |
| 4 | ~~ICU version upgrade (71→78)~~ | P1.7 | **done** | Upgraded to ICU 78.2 (Unicode 14→17, CLDR 48). Patch regenerated. |
| 5 | ~~Patch debt reduction (whisper)~~ | P1.7 | **done** | Reduced from 11 hunks to 8. Dropped 3 non-essential hunks. |
| 6 | ~~CI hygiene backlog (remaining)~~ | P2.11 | **done** | Bazel disk cache via `actions/cache@v4` in all workflows. |
| 7 | ~~Benchmark infrastructure audit~~ | P2.13 | **done** | Stack audited, local execution documented, cross-fork comparison feasible. |
| 8 | ~~Fix pre-existing test warnings~~ | Known Issues | **done** | Narrowing + trigraph warnings fixed. |
| 9 | Protobuf 34 upgrade | P1.6b | **blocked** | Waiting on brpc upstream. Monitor only. |
| 10 | ~~Cross-fork benchmark script~~ | P2.13 | **done** | `scripts/benchmark_vs_upstream.sh` for upstream comparison. |
| 11 | ~~Definition of Done audit~~ | DoD | **done** | All 6 checkboxes verified and marked complete. |
| 12 | ~~Fix Bazel 9.0.0 not running in Docker~~ | P1.6 | **done** | Verified with `scripts/bazel_in_docker.sh version` and `TYPESENSE_BAZEL_IMAGE=typesense/ci-bazel:ci scripts/bazel_in_docker.sh version`: Bazelisk `v1.28.1`, Bazel `9.0.0`. CI workflows already run `--build-image-only`; stale local images were the mismatch source. |
| 13 | RocksDB perf tuning Phase 3 (data-driven) | P2.13 | **done** | Runs 9-13 complete: observability, sweeps, read-path optimizations, `max-indexing-concurrency` validation, and import `batch_size` A/B check. Final policy keeps conservative defaults with hardware-based tuning guidance. |
| 14 | ~~Benchmark observability: full metrics collection + Grafana dashboard~~ | P2.13 | **done** | Core observability is in place: benchmark runs collect system/API/RocksDB metrics continuously and dashboard includes concurrent search+import visibility. Tuning-specific counter extraction is tracked under item 13. |
| 15 | ~~JS/Docker workflow consolidation~~ | P2 DX | **done** | Benchmark/API tooling is Bun-first, benchmark CI now uses the shared wrapper, and API tests have a Dockerized wrapper entrypoint. |

### Backlog map (active / later / archival)

Use this to decide what to pick next without scanning multiple files.

- **Active now (execution lane):** no active tooling-lane work remains; proceed to the next non-blocked modernization item.
- **Later (blocked or dependency-coupled):** item **9** (`Protobuf 34`) and section **6b** (`brpc`/rule compatibility work) plus section **7** patch-debt follow-up (`replace patch-only forks`) when dependency updates are available.
- **Archival/reference (not immediate execution lanes):**
  - `benchmark/BENCHMARK_RESULTS.md` P2/P3 backlog items (experimental/future ideas).
  - `TODO.md` upstream product backlog (not the modernization source of truth; mine opportunistically only when an item aligns with current modernization goals).

### Upstream TODO candidates worth pulling in (post-Phase-3 queue)

From `TODO.md`, these are the highest-value items that still align with current modernization/perf goals:

- **Replication throughput control:** parameterize replica `MAX_UPDATES_TO_SEND` (currently not exposed as a tunable in our config surface).
- **Indexing hot-path efficiency:** reduce avoidable string copies during indexing/import code paths.
- **Search work budget tuning:** make "minimum results" heuristic configurable instead of coupling to `max_results` behavior.
- **Numeric safety hardening:** add validation for float values beyond `INT32_MAX` in int32-related paths.
- **Reliability coverage:** expand replication test coverage for replay/catch-up edge cases.

These were intentionally deferred while item 13 was active and are now strong next candidates post-Phase-3.

### 14) Benchmark observability: full metrics collection + Grafana dashboard improvements

Done. Core observability plumbing landed:

- Metrics collection now runs during benchmark comparisons and captures `/metrics.json`, `/stats.json`, and `/debug?rocksdb_stats=true` snapshots.
- Grafana dashboard includes concurrent search+import panels and expanded RocksDB/system/API visibility.
- Benchmark runs now preserve enough telemetry to explain most import/search regressions without rerunning blindly.

Remaining tuning-specific counter extraction (RocksDB statistics parser, p99-first views, per-scenario split panels) is tracked as part of item 13 Phase 3.

### 15) JS tooling and Docker workflow consolidation

Done.

- Benchmark CLI and workflow now use Bun instead of pnpm.
- `scripts/benchmark_vs_upstream.sh` supports explicit baseline/fork binaries so CI and local runs share the same wrapper.
- `scripts/run_api_tests.sh` provides the single supported API wrapper entrypoint; it now defaults to the repo's Dockerized Bun image and keeps `--host-bun` only as an escape hatch.
- Legacy `ci_build.sh` is now a compatibility shim that redirects users to the canonical Bazel wrapper instead of acting as a parallel build system.

### 13a) RocksDB Phase 3 execution plan (next chunk)

**Goal:** complete one bounded, data-driven tuning iteration with measurable gains and low regression risk.

**Inputs from existing benchmark lessons (Runs 5-8 in `benchmark/BENCHMARK_RESULTS.md`):**

- Keep `db-block-size=16KB` as baseline (4KB only helped `filter_complex`; 16KB won broader sort/facet mix).
- Keep the current write-path baseline (`128MB` write buffers, `4` buffers, `unordered_write=true`, `max_subcompactions=2`) since this produced the strongest import wins.
- Prioritize validating `max-indexing-concurrency=16` early; prior run data showed import improvement and removed the `filter_complex` regression.
- Do not spend cycles re-testing previously invalid aggressive configs unless new counter evidence suggests a different bottleneck.

**Phase A — Baseline and gates (no code changes):**

- [x] Run **standard** baseline twice to reduce noise:
  - `scripts/benchmark_vs_upstream.sh --profile standard --server-args --max-indexing-concurrency=4`
- [x] Run **write-stress** baseline twice:
  - `scripts/benchmark_vs_upstream.sh --profile write-stress --server-args --max-indexing-concurrency=4`
  - Completed: archives `20260305-211730-write-stress---max-indexing-concurrency4` and `20260305-214513-write-stress---max-indexing-concurrency4`
- [x] Archive benchmark tables + `metrics/*.json` snapshots under a dated run folder.
- [x] Use keep/drop gates for every candidate: keep only if import throughput improves by >=5% (or import p95 improves by >=5%) and no search p95/p99 regression exceeds 3%.
  - Applied to Runs 9-13; high-throughput candidates that exceeded concurrent-search gates remain non-default, and `max-indexing-concurrency` policy is now CPU-tier based.

**Phase B — Observability needed for data-driven tuning:**

- [x] Parse `rocksdb_statistics` from `/debug?rocksdb_stats=true` and emit time-series in `benchmark/src/benchmarks/metrics-collector.ts` for:
  - block cache hit/miss counters and hit ratio
  - bloom filter usefulness counters
  - stall time (`STALL_MICROS`)
  - compaction read/write bytes (write amplification proxy)
- [x] Expand `/stats.json` extraction in `benchmark/src/benchmarks/metrics-collector.ts` to include import/search/write p99 metrics.
- [x] Add matching Grafana panels in `benchmark/dashboards/typesense-benchmark.json` (hit ratio, stalls, compaction bytes, p99).

**Phase C — Tuning candidates (low risk first):**

- [x] Evaluate `max-indexing-concurrency=16` as a default candidate in `include/tsconfig.h` and fallback defaults in `src/tsconfig.cpp`, then benchmark.
  - Completed with write-stress validation repeats; final decision is to keep default `4` and document CPU-tier production recommendations (Run 12 + archives `20260305-224512-write-stress`, `20260305-231242-write-stress`).
- [x] Add controlled A/B toggles and sweeps for:
  - [x] `optimize_filters_for_hits` (sweep run complete)
  - [x] explicit `max_background_jobs` (sweep run complete)
  - [x] `db_compression_parallel_threads` (4 vs 8) (sweep run complete)
- [x] Implement read-path optimizations for bulk update/delete flows:
  - [x] add `Store::multi_get(...)` in `include/store.h` + `src/store.cpp`
  - [x] add `fill_cache` control to `get_document_from_store(...)` in `include/collection.h` + `src/collection.cpp`
  - [x] use `MultiGet` + `fill_cache=false` in `Collection::cascade_remove_docs`, `Collection::update_matching_filter`, and `Collection::update_async_references_with_lock`

**Phase D — Validation and closeout:**

- [x] Run targeted tests for update/join/cascade paths plus full `//:typesense-test`.
- [x] Re-run benchmark comparison against upstream and previous fork commit.
  - Upstream-vs-fork reruns completed for write-stress baseline and the `max-indexing-concurrency=16` default candidate.
- [x] Update `benchmark/BENCHMARK_RESULTS.md` with tables and rationale; then update Priority Queue status.
  - Runs 9-13 now documented with gate outcomes and references.

## Known Issues Backlog

Pre-existing issues discovered during modernization work. Fix opportunistically or as part of related tasks.

### Logging behavior change

Console log output moved from stdout to stderr as part of the glog→Abseil swap (P1.8). This is more standard for daemons but may affect users who pipe stdout. Documented in commit `2139421d`.

## Lessons Learned

Important patterns and gotchas that save future AI agents significant time. Keep this section concise — only add entries that would prevent wasted effort.

1. **Patch files must match exact upstream content.** When creating a `.patch` file for an external dep, always clone the exact pinned commit and generate the diff with `git diff`. Do NOT write patches by hand — Bazel's built-in patch applicator is strict and rejects content mismatches silently. Context lines must match byte-for-byte.

2. **glog and Abseil `LOG()` macro collision.** brpc/braft transitively include `<glog/logging.h>` which defines `LOG()`. Abseil also defines `LOG()`. The solution is to use Abseil's **prefixed** macros (`ABSL_LOG`, `ABSL_CHECK`) in the facade to avoid the collision entirely. Never use unprefixed `LOG()` in first-party code.

3. **glog cannot be removed while brpc/braft are dependencies.** glog must remain in `common_deps` with `google::InitGoogleLogging()` called at startup, or brpc crashes. Silence glog output with `FLAGS_stderrthreshold = google::NUM_SEVERITIES` and empty `SetLogDestination` calls.

4. **Patch droppability verification requires full Docker build.** Header-only changes can appear to succeed in isolation but fail at link time. Always verify with `bazel build //:typesense-server` inside the Docker container.

5. **Pre-existing test warnings are excluded from CI guardrails** by the warning scripts. They won't block CI but should still be fixed to maintain code quality. Check for new warnings after any change by reading build output.

6. **braft patches are all non-droppable** as of Mar 2026 — upstream braft is dormant (last real release 2021). Don't waste time trying to drop them; just maintain them.

7. **`max-indexing-concurrency` is hardware-sensitive.** Benchmark wins at 16 do not automatically justify a strict global default of 16. Keep conservative defaults for broad deployability (4), and document CPU-tier tuning guidance for production overrides. A future adaptive startup heuristic (CPU+memory aware) is a better long-term path than a single aggressive default.

8. **Import API `batch_size` must stay wired to actual indexing batches.** The request parameter is now passed through to `Collection::add_many(...)` and controls import-side batch flushing; avoid regressing this by reintroducing a hardcoded internal batch size in the API path.

9. **Import `batch_size` is a secondary throughput knob on this dataset.** A/B checks (`40` vs `1000`) showed only marginal import delta (~0.2% in current runs). Keep default `40` for mixed workloads; use larger values only as deliberate ingest-window overrides.

10. **Dockerized API harness should force IPv4 localhost.** Inside the API Bun container, `localhost` health checks can miss servers that are listening on IPv4 only. Set `TYPESENSE_API_HOST=127.0.0.1` in the wrapper to keep Dockerized API runs reliable.
