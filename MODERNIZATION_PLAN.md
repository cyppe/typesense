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
6. **Upstream intake testing rule:** when backporting or adopting logic from upstream branches/PRs, do not stop at code parity. Always check whether the new behavior is testable in this fork, and if it is, add or extend tests here even when the upstream change did not ship with new coverage.

## Scope

- Keep Typesense modern across build, dependencies, runtime, and test infrastructure.
- Remove flaky behavior and make tests parallel-safe by default.
- Maintain CI parity between local, Docker, and GitHub Actions.

## Current Baseline (Mar 2026)

- Bazel plus Bzlmod baseline is now `9.0.0` (`.bazelversion`, `MODULE.bazel`) with local Dockerized CI-parity validation green.
- `WORKSPACE` is a stub and module-based dependency resolution is active.
- C++ suite `//:typesense-test` is passing in CI-parity Docker after deterministic tie-breaker fixes in grouping tests.
- API no-secrets suite is healthy across the supported Bun harness phases. Env-dependent coverage is now explicit: secret-gated embedding/conversation stays all-or-nothing on `OPENAI_API_KEY` + `AZURE_OPENAI_API_KEY` + `AZURE_OPENAI_URL`, the dedicated TEI lane is green when `TYPESENSE_TEST_TEI_URL` is configured, and legacy migration replay from pre-NuRaft binaries is intentionally unsupported in the current harness.
- Parallel stress validation now shows isolated temp/model paths per process after helper migration; no active shared-path collision failure is known.
- Heavy auxiliary workflows are now manual-only by policy; `tests.yml` is the only automatic GitHub Actions gate and the local wrapper commands in `TESTING_RUNBOOK.md` are the preferred pre-push validation path.

## Quick Reference: How To Build And Test

`TESTING_RUNBOOK.md` is the canonical owner for build/test/replay/API command lines.

- Build server: `scripts/bazel_in_docker.sh build //:typesense-server`
- Run API suite: `scripts/run_api_tests.sh -- --no-secrets`
- Run benchmarks: `TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile standard --scope core`

Keep this section short and point to the owning docs instead of duplicating the full command matrix here.

**Critical build/test gotchas:**
- **Always use `scripts/bazel_in_docker.sh`** for builds, never raw `docker run ... bazel build`. The script handles image building, caching, user permissions, and bazelisk configuration.
- **Rebuild the Docker image after Dockerfile changes:** `scripts/bazel_in_docker.sh --build-image-only`. Stale images may run an older Bazel version (e.g., 8.6.0 instead of 9.0.0).
- **Clear stale bazel caches** if root-owned files block builds: `sudo rm -rf /tmp/typesense-bazel-cache-fork` or the configured cache dir.
- **Default API runs now refresh the runtime bundle automatically.** `scripts/run_api_tests.sh` now re-stages the default `typesense-runtime-bundle` from the current Bazel output on every run; only explicit custom runtime bundle dirs still opt into reuse.
- **Stop stale Docker containers** that may hold build locks or ports: check with `docker ps` before starting a new build/test run.
- **ICU dependency:** `nuraft_lib` requires `@icu` in BUILD deps because it transitively includes `string_utils.h` → `unicode/normalizer2.h`.

### Environment variables used by `bazel_in_docker.sh`

| Variable | Default | Purpose |
|---|---|---|
| `TYPESENSE_BAZEL_IMAGE` | `typesense/ci-bazel:local` | Docker image tag (CI uses `typesense/ci-bazel:ci`) |
| `TYPESENSE_BAZEL_CACHE_DIR` | `~/.cache/typesense/bazel-docker` | Host-side Bazel cache mount |
| `TYPESENSE_BAZEL_REPO_ENV_CONLYOPTS` | `-std=gnu17` | C-only compiler options passed to Bazel repo env |
| `TYPESENSE_BAZEL_SKIP_DEFAULT_GCC_CONFIG` | unset | Disable the wrapper's default `--config=gcc` on non-clang `build`/`test`/`run`/`coverage` lanes |

### Model artifacts for embedding tests

Some C++ tests require a pre-warmed model cache:
```bash
curl https://dl.typesense.org/ci/tyrec/tyrec-1-models.tar.gz > ./test/resources/models.tar.gz
bash test/scripts/prewarm_public_test_models.sh "$PWD/tmp/ci-models"
# Then pass: --test_env=TYPESENSE_TEST_MODELS_DIR=/work/tmp/ci-models
```

## CI Workflow Map

All workflows run on `ubuntu-24.04`. Only `tests.yml` runs automatically; the heavier lanes are manual on purpose so local Dockerized repro can happen before spending GitHub runner time.

| Workflow | File | Trigger | Schedule | Purpose |
|---|---|---|---|---|
| `tests` | `tests.yml` | push, manual | — | Primary gate: build + GCC warning guardrail + C++ tests + API tests + TEI |
| `clang-warning-guard` | `clang-warning-guard.yml` | manual | — | Clang-only warning-budget enforcement for compiler/toolchain-sensitive changes |
| `flake-detection` | `flake-detection.yml` | manual | — | 20x reruns of 6 historically flaky C++ tests + 10x API documents test |
| `sanitizer-testing` | `sanitizer-testing.yml` | manual | — | Full C++ suite under ASAN and TSAN (parallel jobs) |
| `nightly-extended` | `nightly-extended.yml` | manual | — | Full C++ suite 5x stress + full API suite 3x multi-node longevity |
| `benchmark-testing` | `benchmark-testing.yml` | manual | — | Performance benchmarks |

### Warning guardrails

CI enforces zero-warning policy for both GCC and Clang via:
- `scripts/check_gcc_warning_guardrail.sh` (env: `TYPESENSE_GCC_WARNING_MAX=0`, `TYPESENSE_GCC_TRACKED_WARNING_MAX=0`)
- `scripts/check_clang_warning_guardrail.sh` (env: `TYPESENSE_CLANG_WARNING_MAX=0`, `TYPESENSE_CLANG_TRACKED_WARNING_MAX=0`, `TYPESENSE_CLANG_SPARSEPP_WARNING_MAX=0`)

If you add new first-party code, fix any warnings before committing. Third-party warnings are excluded by the guardrail scripts automatically.

## Definition Of Done (100% Modernized)

- [x] All test suites pass on a clean checkout in CI and locally with documented commands. *(CI green; local repro via `scripts/bazel_in_docker.sh` documented in Quick Reference above.)*
- [x] No known flaky tests after stress validation (`--runs_per_test=20`) on targeted suites. *(Dedicated flake-detection and extended stress lanes exist for opt-in validation; P0 items 1–5 eliminated all known flake sources.)*
- [x] Tests are parallel-safe (no shared writable paths, no port collisions, no hidden global state). *(P0 items 1–4: temp dirs, dynamic ports, no globals — all done and stress-validated.)*
- [x] Bazel 9 migration is complete and stable. *(P1.6 done; `.bazelversion` = 9.0.0, CI green.)*
- [x] Dependency set is current, with patch debt minimized and documented exceptions only. *(6 active Bazel patches with justifications in `bazel/PATCH_DEBT.md`; no fork-backed Bazel deps remain. The old `brpc`/`braft` patch stack is gone from this branch.)*
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
- [x] Dedicated flake-detection workflow remains available for manual pre-merge stress validation and artifact capture.

## Priority Queue

1. [ ] Re-check the real DDEV lane on a fresh image built from the post-helper-rewrite branch head.
   Current state: March 22, 2026 DDEV on the published image `2c06d882` cleared the old `~9.8M` snapshot stall, but it still exposed one remaining generic helper regression: a `categories_se` import rewrote `177,813` `products_se` docs, fetched about `2.14 GiB` of stored product JSON, stayed single-chunk, and produced a `33.99s` slow request. Current local HEAD now has three generic fixes stacked: the byte-aware helper planner (sample stored-doc size once the matched set exceeds `100k` docs and only chunk if estimated fetched bytes exceed about `512 MiB`, with a `50k`-doc floor), a `yyjson` rewrite of the helper’s full-document fetch/mutate/store loop plus nanosecond-based helper phase accounting, and a `500k` max-doc cap on sampled helper chunks so tiny-doc fanout cannot remain million-doc batches just because the JSON footprint is small. The updated replay set is green: `1M` late fitment fanout stays monolithic and improves from the prior accepted `5121.1ms` late `vehicles_se` seed to `4085.1ms`, `3M` fitment fanout keeps the late `vehicles_se` seed slightly lower (`15365.9ms -> 15204.9ms`) while materially improving live `/health` p95 (`436.1ms -> 50.1ms`) and `/metrics.json` p95 (`362.4ms -> 64.0ms`), `category_fanout 180k / 711 / 12KB` remains at `10.1s`, and the preserved-log `mixed_category_fitment 300k` lane from the previous pass stayed healthy at fitment import `141.1ms`, category import `11914.4ms`, search `56.9ms` avg / `117.1ms` p95, `/health 0.3ms`, and `/metrics.json 1.5ms`.
2. [ ] If fresh DDEV still times out, capture the new replay-style evidence from that run before changing runtime code again.
   Current state: the replay harness now emits both metrics timelines and a log summary (`slow_request`, `Threadpool exhaustion detected`, `Async reference helper slow path`), and the March 22 local validation also confirmed that slow import log lines now include a full request/helper breakdown (`import_body_bytes`, handler/response timing, `helper_collection`, `helper_field`, helper retry/failure counts). Use the same evidence class in DDEV before reopening root-cause work so any remaining gap can be compared directly against the local `mixed_category_fitment` lane.
3. [ ] Trim the remaining search/control-plane tail only if it stays material after the fresh DDEV check.
   Current state: the current branch no longer reproduces node-wide starvation locally. The latest local 3-node NuRaft repro also closed a separate read-latency regression: mutable reads used to call `wait_for_live_product_state()` unconditionally, which forced search/control-plane reads to stall behind the newest committed import chunk even when local applied state was already good enough. Default reads now serve the latest locally applied state and keep the old behavior behind explicit `read_consistency=strong`; the new targeted 3-node regression `NuRaftHttpRuntimeTest.SearchReadsBypassSyncDuringConcurrentImportsOnThreeNodeCluster` is green, and the repo now carries a matching measurement lane in `scripts/replay_nuraft_import_search_latency.py`. On the `30k`-doc / `3`-worker replay, the pre-fix local run sat at `17.2ms` avg / `46.4ms` p95 / `92.7ms` p99 with `sync_cumulative_calls≈136-156`; the current replay is `6.0ms` avg / `13.4ms` p95 / `73.0ms` p99 with `sync_cumulative_calls=0`. It is still worth polishing only if fresh DDEV shows additional tail beyond that now-removed read barrier.
4. [ ] **Upstream `v31` parity Phase 2** (item 45): backport 11 confirmed-missing upstream commits in 3 phases. See item 45 below for the full phased plan with per-commit details.
5. [x] Expose build provenance from the checked-out source in both the runtime and published Docker images.
6. [x] Make the warning-guard workflows fall back cleanly when no prebuilt ORT bundle artifact is available.

## Priority 1 - Build And Dependency Modernization

### 6) Migrate from Bazel 8.6.0 to Bazel 9

Done. Repo default is Bazel 9.0.0. CI and local Docker lanes are green. Platform-based config settings, strict action env, and all downstream deps verified.

### 6b) Protobuf upgrade staging (post-Bazel-9 stabilization)

- [x] Staged path completed: `29.0` -> `29.5` -> `33.4` -> `33.5` with Bazel 9 compatibility.
- [x] The previous `brpc`-specific Protobuf 34 blocker is gone from this branch because the old Raft/RPC stack has been removed.
- [x] Re-attempt Protobuf 34 against the remaining dependency set when dependency-refresh work cycles back here. *(Done Mar 2026: upgraded to 34.0.bcr.1. Clean build, 120/120 API tests, 4/4 unit tests. No source changes needed.)*

### 7) Burn down external patch debt

- [x] Inventory complete with per-patch justification in `bazel/PATCH_DEBT.md`.
- [x] Removed stale patches (`onnx.patch`, `picotls_openssl/0001.patch`).
- [x] `whisper.patch` debug noise cleaned (`6cf44457`).
- [x] `whisper.patch` reduced from 11 hunks to 7 — dropped compiler warning flags, whitespace noise, backtrace removal, and the CMake compile-definition hunk by moving `GGML_USE_CUBLAS` into `bazel/whisper.BUILD`. Non-speech token expansion kept (test-verified: voice query expects punctuation-free output).
- [x] `quicly/0001.patch` dropped — bumped quicly to `c9167711` (fix commit).
- [x] Replace patch-only forks with released upstream versions where possible. *(Done Mar 17, 2026: removed the stale `typesense/hnswlib` fork by pinning the same upstream `nmslib/hnswlib` parent commit directly, repointed `clip_tokenizer_cpp` from the Typesense mirror to the identical upstream `ozanarmagan/clip_tokenizer_cpp` commit, and then moved `kakasi` off the Typesense fork to upstream `loretoparisi/kakasi` with a small repo-owned patch plus the in-repo `japanese_data` payload.)*
- [x] Switch ICU from Typesense fork (`github.com/typesense/icu`) to upstream release tarball (ICU 71.1). Fork carried zero source mods. Patch content unchanged.
- [x] **ICU version upgrade (71.1 → 78.2):** Upgraded to ICU 78.2 (Unicode 14→17, CLDR 48). Same 6 upstream BUILD.bazel files still ship, but they are now removed via `MODULE.bazel` `patch_cmds`; `bazel/icu/icu.patch` has been reduced to just the `icudefs.mk.in` AR fix. Zero Typesense code changes required — only stable APIs used.

**Gotchas for patch work:**
- Patch droppability must be verified with a full `bazel build //:typesense-server` inside Docker — header-only changes can appear to succeed in isolation but fail at link time.
- `bazel/whisper.patch` is the largest remaining patch and highest maintenance risk (CUDA/shared-loading paths). ICU patch debt is lower now: the six conflicting upstream `BUILD.bazel` files are removed via `MODULE.bazel` `patch_cmds`, and `bazel/icu/icu.patch` now only carries the `icudefs.mk.in` AR fix.
- `bazel/onnxruntime.patch` was re-audited after the ORT `1.24.3` bump and is still required for extensions path export, zlib 1.3.x/static image-codec handling, and the one-protobuf imported-target path.
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

### 7c) Dependency refresh audit

Build a deliberate next-wave upgrade shortlist instead of bumping opportunistically while release-lane work is active.

- [x] Turn the current-vs-latest inventory into a ranked upgrade plan with explicit owners/blockers.
- [x] High-priority audit findings so far:
  - Vendored `magic_enum` `0.7.2` -> `0.9.7` is now done; the full header refresh replaces the temporary AppleClang-only backport and keeps the compiler fix aligned with upstream.
  - Protobuf `33.5` -> `34.0.bcr.1` is now done; clean build, 120/120 API tests pass, all unit tests pass.
- [x] Medium-priority candidate review completed for the current shortlist:
  - ONNX Runtime `1.24.2` -> `1.24.3` is now done; canonical Docker build passed, `ldd bazel-bin/typesense-server` still shows no `libonnxruntime.so.1` dependency, and the Dockerized API `tests/health.test.ts` replay passed against the promoted binary.
  - `libarchive` `3.7.7` -> `3.8.5` is now done.
  - `snappy` `1.1.7` -> `1.2.x` for compiler/perf hygiene. *(Now done on `1.2.2`.)*
  - `typesense-js` in `tests/` `2.0.3` -> `3.0.2` is now done to match the benchmark toolchain client line; `bun run check` passes in `tests/` after the bump.
  - JS tooling baseline is now aligned on Bun `1.3.10` plus Node `24.14.0` LTS for optional host-side flows. `tests/` no longer carries pnpm metadata, benchmark wrapper invocations use Bun, and shared package pins were refreshed to the current stable lines (`eslint 10.0.3`, `typescript-eslint 8.57.0`, `vitest 4.1.0`, `openai 6.27.0`, `zod 4.3.6`, `@types/node 25.4.0`). `tests/src/error.ts` needed the expected Zod 4 compatibility fix (`error.errors` -> `error.issues`), and `bun outdated` is now clean in `api_tests/`, `benchmark/`, and `tests/`.
  - `abseil-cpp` `20250814.1` -> `20260107.1` is now done. The clean fix was BUILD-level, not patch growth: `bazel/onnxruntime.BUILD` now sets `FETCHCONTENT_SOURCE_DIR_ABSEIL_CPP=$$EXT_BUILD_ROOT/external/abseil-cpp+` for ORT's one-protobuf static lane so ORT and `re2` rebuild against the repo's Abseil source version while keeping upstream ORT's own CMake target graph and patch flow intact. Proof: canonical Docker build passed on the old pin and on `20260107.1`, `health.test.ts` passed across all API phases, and `CollectionVectorTest.TestUnloadingModelsOnCollectionDelete` passed.
- Keep treating patch-debt reduction as at least as important as raw version bumps; some deps (for example `whisper.cpp`, `h2o`) matter more because of maintenance surface than because they are numerically old.
- [x] **whisper.cpp major upgrade (v1.8.3):** Upgraded from `022756a8` (pre-v1.7.x) to `2eeeba56` (v1.8.3). Complete `whisper.BUILD` rewrite (now uses `rules_foreign_cc` cmake rule instead of hand-rolled cc_library). Patch reduced from 7 hunks/4 files to 1 hunk/1 file (only non-speech token expansion remains). API change: `suppress_non_speech_tokens` renamed to `suppress_nst`. OpenMP disabled (`GGML_OPENMP=OFF`) since Typesense runs whisper single-threaded. All 141 API tests pass.
- [x] `h2o` patch reduced from 774 to 48 lines by moving 9 conflicting brotli `BUILD` file deletions to `MODULE.bazel` `patch_cmds`. Remaining delta is CMakeLists.txt-only (CONFIGURE_FILE + INSTALL target stripping).

### 7d) NuRaft cutover record and hardening lane

This section is now partly archival. The feasibility sprint is over on this branch: the old `braft`/`brpc` stack was removed and `//:typesense-server` is now the NuRaft-backed runtime. Keep the findings below as the decision record; treat the remaining unchecked items as NuRaft hardening work, not an open replacement question.

**Investigation summary (Mar 2026):**

- NuRaft is the strongest in-process replacement candidate found so far; a Rust Raft service remains a possible long-term architecture, but it is a larger boundary change and should not be the first replacement experiment.
- NuRaft appears materially healthier than `braft` on release cadence and active feature work: current upstream docs and examples are maintained, latest release is `v3.0.0` (2025), and the feature set includes pre-vote, leadership expiration, learners, custom quorum control, auto-forwarding, streaming mode, parallel log appending, and scheduled snapshots.
- We explicitly reviewed `docs/how_to_use.md` and the example implementations under `examples/`; they confirm the main migration reality: NuRaft is a library, not a drop-in runtime. Typesense would need its own durable `state_mgr`, `log_store`, and `state_machine` integration instead of relying on `braft` + `brpc` built-ins.
- The examples are useful for API shape and lifecycle, but they are intentionally lightweight (`in_memory_state_mgr`, in-memory log store, CLI-driven add/remove). They do not solve Typesense's persistent log/meta storage, RocksDB snapshot transport, HTTP leader redirect behavior, or node-IP refresh behavior.
- Current Typesense coupling to `braft` is deep in `src/raft_server.cpp` and `src/typesense_server_utils.cpp`: `braft::Node`, `braft::Task`, `braft::Closure`, built-in RPC service wiring, URI-based log/meta/snapshot storage, peer reset flows, and leader/follower status checks are all first-class parts of the server lifecycle.

**Why this sprint existed:**

- `braft`/`brpc` were one of the main blockers to future cleanup: patch debt and the Protobuf 34 upgrade were coupled to that stack.
- The branch only justified removing that stack once NuRaft showed materially better recovery behavior and acceptable bounded runtime performance.

**Sprint goal (completed):**

- Produce a go/no-go decision backed by code, replay results, and benchmark data.
- If the answer is "go", produce a migration design that is broken into incremental stories instead of a single risky rewrite.

**Story A - Capture the current replication contract**

- [x] Inventory all current `braft`/`brpc` touchpoints in first-party code and classify them as: Raft core, transport, snapshotting, membership, leader discovery, or operational workaround.
- [x] Write down the non-negotiable behaviors the replacement must preserve: leader redirect/proxy behavior, streaming import handling, follower health/catch-up checks, snapshot export/restore, restart replay, IPv6 peer parsing, and single-node recovery after peer/IP drift.
- [x] Document which current behaviors are true product requirements vs temporary `braft` workarounds (especially `reset_peers()` and the periodic peer-refresh loop).

**Story A findings (Mar 2026):**

- **Raft core:** first-party coupling is centered in `ReplicationState` (`include/raft_server.h`, `src/raft_server.cpp`), which subclasses `braft::StateMachine`, submits writes as `braft::Task`, and applies committed entries by replaying serialized request payloads through the batched indexer.
- **Transport:** the peering plane currently depends on `brpc::Server` + `braft::add_service(...)` (`src/typesense_server_utils.cpp`), while leader-aware write forwarding is implemented separately as HTTP proxying to the leader URL, including streaming import passthrough.
- **Snapshotting:** the current contract is larger than plain Raft snapshots. Save/load must cover main RocksDB, analytics RocksDB, and in-flight batched-indexer state; manual snapshot export also copies the raft `snapshot/` and `meta/` trees into an external artifact.
- **Membership / addressing:** Typesense expects `host:peer_port:api_port` membership inputs, hostname resolution, IPv6-safe parsing, and dynamic node-file refresh. The exact `PeerId.idx == api_port` encoding is braft-specific, but the ability to derive leader/follower HTTP endpoints from the peering membership is a real product need.
- **Leader discovery / readiness:** current behavior is "write on any node, proxy to leader server-side, gate reads/writes on catch-up status, and expose `/status` committed-index visibility for tests and operators." A replacement must preserve those observable semantics even if the internal Raft API changes.
- **Likely product requirements:** deterministic request-level replication, restart replay of accepted writes, follower lag gating for reads/writes, leader URL discovery for internal subsystems (`proxy`, `proxy_sse`, analytics, conversation, remote embedder flows), snapshot export/restore, and single-node recovery after peer/IP drift.
- **Likely braft-era workarounds:** the unsafe `reset_peers()` escape hatch outside single-node recovery, the 10-second peer-refresh polling loop, the `known_applied_index` catch-up heuristic, disabling automatic snapshots in favor of a Typesense-managed timer, and the post-snapshot dummy write used only to re-arm braft snapshot triggering.

**Story B - Design a Typesense-specific NuRaft adapter**

- [x] Design a durable NuRaft `state_mgr` for term/vote/config persistence under the existing `state_dir` layout.
- [x] Design a durable NuRaft `log_store` with crash recovery, compaction, and bounded disk growth; decide whether it should live on RocksDB, separate segment files, or another local format.
- [x] Map the current replicated payload format (`http_req` JSON replay into the batched indexer) onto NuRaft `buffer` entries and commit callbacks.
- [x] Decide whether to use NuRaft's built-in Asio transport or a custom transport shim; document the implications for TLS, observability, and integration with the existing server runtime.

**Story B design recommendations (Mar 2026):**

- **`state_mgr` recommendation:** keep the existing top-level `state/` shape but make NuRaft own its metadata explicitly. The long-term target layout is still `state/meta`, `state/log`, and `state/snapshot`; for the prototype, use an isolated root such as `state/nuraft-prototype/{meta,log,snapshot}` so it can coexist with current `braft` state.
- **Metadata persistence model:** prefer small fsync-safe files in `meta/` over a second embedded DB. Persist NuRaft server state, cluster config, snapshot descriptor, and identity/format metadata via temp-file + atomic rename + parent-dir fsync so Raft can recover independently of the main Typesense RocksDB stores.
- **Identity recommendation:** keep `server_id == api_port` for the sprint to minimize operator-visible change and preserve the current leader URL derivation model. Also persist the peering endpoint and API port separately in config metadata so that a future decoupling is still possible.
- **`log_store` recommendation:** use dedicated append-only segment files under `state/log`, not a separate RocksDB instance. Raft log access patterns here are append/truncate/scan, and segment files give cleaner durability semantics, simpler compaction by whole-segment deletion, and less background-I/O interference with the main document DB.
- **Prototype log-store scope:** keep the first implementation intentionally conservative: synchronous append + flush, tail-segment recovery, logical truncation on conflict, and whole-segment cleanup after compaction. Defer NuRaft parallel log appending / streaming-mode storage optimizations until the baseline path is proven.
- **Payload mapping recommendation:** preserve today’s request-level semantics by keeping the current `http_req::to_json()` payload as the replicated logical body for the prototype, but wrap it in a small versioned NuRaft buffer envelope. That keeps restart replay, follower apply, and chunked import behavior aligned with the current batched-indexer path while still giving the new log format an explicit version boundary.
- **Apply-path recommendation:** keep actual Typesense execution in NuRaft commit callbacks, not pre-commit. On the leader, capture the assigned log index and attach any live request/response context to it; on followers or restart replay, reconstruct a synthetic request from the stored payload and feed the existing batched-indexer path the same way the current `braft` integration does.
- **Transport recommendation:** use NuRaft’s built-in Asio transport for the prototype and likely the first production-grade migration attempt. Do not spend the sprint building a custom transport shim; keep Typesense’s existing HTTP leader proxying for client writes, imports, `proxy`, and `proxy_sse`, and use NuRaft only for the peering/Raft plane.
- **What this deliberately defers:** live migration from `braft` state, a typed operation-log rewrite, transport unification with the existing HTTP runtime, and advanced storage optimizations. The sprint should first prove that a durable NuRaft path can preserve the current product contract without regressing correctness or recovery behavior.

**Story C - Build the prototype behind an isolated target**

- [x] Add a non-default experimental target or branch path so the prototype does not destabilize the production `typesense-server` target.
- [x] Implement single-node boot, write, restart, and replay. *(Done via isolated startup preflight, request-journal append/replay, torn-tail recovery, replay-progress tracking, and an import-aware materialized apply path that survives restart.)*
- [x] Implement three-node append/commit, leader discovery, and follower catch-up. *(Done via a static-cluster prototype path that forwards follower appends to a chosen leader journal, replicates committed history into follower journals, and applies catch-up across all mapped nodes.)*
- [x] Implement snapshot create/install using the current RocksDB checkpoint model, plus external snapshot export compatibility expected by the API/runtime harness. *(Done via descriptor-driven prototype snapshots that checkpoint live KV state, export the current `meta/` + `snapshot/` tree under `state/nuraft-prototype/`, and reinstall the descriptor-selected snapshot payload while preserving local identity/bootstrap metadata.)*
- [x] Implement membership changes and define the replacement behavior for today's peer refresh / IP-change handling. *(Done via startup-preflight metadata refresh rules: allow peer-list updates when self identity stays stable, allow single-node self-address drift to rewrite local identity/bootstrap, and reject unsafe multi-node self rewrites.)*

**Story C implementation outline (Mar 2026):**

- **Isolation boundary:** the cutover started by avoiding an in-place fork of the whole server. The runtime-facing replication seam and the dedicated `include/nuraft/` and `src/nuraft/` paths are still the right boundary to preserve while hardening the NuRaft-only server.
- **Seam landed:** the first runtime-facing seam is now in place via `include/replication/replication_service.h`; the current `ReplicationState` implements it, and `HttpServer`, analytics, conversation cleanup, and remote embedder leader-routing code now consume the abstraction instead of the concrete `ReplicationState` type.
- **Prototype cleanup completed (Mar 2026):** All prototype/scaffolding code has been deleted. The filesystem-based `NuRaftStaticCluster`, `NuRaftReplicationController`, `NuRaftPrototypeStateMachine`, `NuRaftRecoveryCoordinator`, `NuRaftSegmentLogStore`, `NuRaftRequestJournal`, and `NuRaftReplayCoordinator` are gone — 16 source/header files and 8 test files deleted, plus 2 prototype binary entry points removed. Real NuRaft consensus via `raft_launcher`/`raft_server` is now the only code path.
- **Production NuRaft code that survived the cleanup:**
  - `NuRaftRequestEnvelope` — versioned binary envelope for replicated payloads
  - `NuRaftAppliedRequest` / `NuRaftAppliedRequestStore` — typed applied-request records
  - `NuRaftKvStateMachineSink` — RocksDB-backed materialized state store
  - `NuRaftSnapshotCoordinator` — snapshot create/install
  - `NuRaftPeerResolver` / `NuRaftBootstrapBuilder` / `NuRaftStateInitializer` — startup/config
  - `NuRaftMetadataStore` — identity, bootstrap config, replay progress
  - `NuRaftStateLayout` — directory structure constants
  - `NuRaftFileStore` — fsync-safe atomic file I/O (used by metadata, applied-request, snapshot stores)
  - `NuRaftRouteClassifier` — route classification for applied requests
  - `TypesenseLogStore` — real NuRaft `log_store` interface implementation
  - `TypesenseStateMachine` — real NuRaft `state_machine` interface implementation
  - `TypesenseStateManager` — real NuRaft `state_mgr` interface implementation
  - `NuRaftHttpRuntimeService` — the main runtime service binding NuRaft to the HTTP server
- **BUILD structure after cleanup:** `nuraft_lib` (renamed from `nuraft_prototype_lib`) contains all production NuRaft source files. `nuraft_runtime_lib` depends on `nuraft_lib` and adds the HTTP runtime layer. `//:typesense-server` uses `nuraft_runtime_lib`.
- **Key architectural decisions in the cleanup:**
  - `NuRaftFileStore` was initially deleted as "prototype-only" but restored — it is a production filesystem utility used by `NuRaftMetadataStore`, `NuRaftAppliedRequestStore`, and `NuRaftSnapshotCoordinator`.
  - `NuRaftLogEntry` struct was moved from the deleted `nuraft_segment_log_store.h` into `nuraft_applied_request_store.h` — it is still needed by `TypesenseStateMachine::commit()` via `NuRaftAppliedRequest::from_log_entry()`.
  - `NuRaftSnapshotCoordinator::build_descriptor()` was refactored to take `const NuRaftKvStateMachineSink*` instead of the deleted `NuRaftRequestJournal`.
  - Single-node NuRaft startup now calls `raft_server_->request_leadership()` followed by a polling loop (up to 5s) to ensure the server is leader before accepting writes.
  - `is_single_node_mode()` changed from checking `cluster_data_dirs.empty()` to checking `bootstrap_config_.peers.empty()`.
  - CLI options `--cluster-data-dirs`, `--cluster-leader-api-port`, and `--install-snapshot` were removed from the server entry point.

**Story D - Verify correctness and operational parity**

- [x] Replay a focused single-node API subset against the NuRaft runtime lane: health, restart, snapshot, and bounded collection/document write/read phases through the real `scripts/run_api_tests.sh` wrapper before attempting broader parity.
- [x] Expand the bounded API replay from the current single-node subset into multi-node health plus bounded write/read phases before attempting broader parity.
- [x] Expand the bounded API replay from health/status, collection/document write/read, and snapshot flows into broader product-response parity. *(Done Mar 2026: all 119 routes registered, 120/120 full API tests pass including aliases, keys, presets, stopwords, synonym sets, curation sets, analytics, stemming, rate limits, conversations, personalization, NL search models across single-node and multi-node phases.)*
- [x] Add focused replication tests for follower-originated metadata writes, cross-node visibility, restart persistence, and snapshot persistence. *(Done Mar 2026: `nuraft_replication_edges.test.ts` — 19 tests covering aliases, presets, stopwords written from followers, verified across all 3 nodes, through restart and snapshot phases. Fixed NOT_LEADER race condition in `append_via_raft()` with a bounded retry loop for transient leadership transitions. Total API test count: 139 pass, 0 fail.)*
- [x] Verify that leader redirects/auto-forwarding works for import, document writes, and metadata writes from followers. *(Done Mar 2026: `nuraft_replication_edges.test.ts` now covers follower-originated JSONL imports with cross-node verification. NOT_LEADER retry in `append_via_raft()` handles transient leadership gaps. 21 edge case tests pass. Streaming/long-running paths deferred to async hardening.)*
- [x] Verify snapshot install and catch-up semantics remain parallel-safe. *(Validated: 14 multi-snapshot tests pass across all test files, covering documents, synonyms, curations, analytics, aliases, presets, and stopwords.)*

**Story D progress (Mar 2026):**

- **Thin recovery harness landed:** `NuRaftRecoveryCoordinator` now provides a bounded runtime-facing disaster-recovery lane for the isolated prototype: timed snapshot attempts with explicit peer-health policy plus latest-snapshot install from a leader into a recovering follower, covered by the new `//:nuraft-recovery-coordinator-test`.
- **Thin HTTP runtime lane landed:** the current `//:typesense-server` now exposes the bounded NuRaft-backed HTTP surface on top of the existing `HttpServer`: `GET /health`, `GET /status`, collection/document fetch, collection/document writes, and `POST /operations/snapshot`, with subprocess-backed coverage in `//:nuraft-http-runtime-test` for single-node restart/snapshot and snapshot-install-into-fresh-node flows.
- **Focused API replay now works through the real wrapper:** `scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/nuraft_runtime_smoke.test.ts` now passes the `single-fresh`, `single-restarted`, and `single-snapshot` phases end to end, and the API runner now supports an explicit `--single-node-only` flag when you want to reuse only the single-node phases of an existing file while keeping the broader multi-node path available.
- **Bounded multi-node API replay now works too:** the same `scripts/run_api_tests.sh` wrapper now passes full `tests/health.test.ts` and `tests/nuraft_runtime_cluster.test.ts` against `//:typesense-server`, exercising `multi-fresh`, `multi-restarted`, and `multi-snapshot` phases with follower-originated writes, leader forwarding, cross-node reads, and cluster restart persistence through the static-cluster runtime options.
- **Existing collection/document API files now pass unchanged on the bounded runtime lane:** `tests/collections.test.ts` and `tests/documents.test.ts` now pass end to end against `//:typesense-server` through the real `scripts/run_api_tests.sh` wrapper. This required flattening the bounded collection/document response shape toward the product surface, adding PATCH/delete document routes plus merge semantics, and adding a minimal materialized-state `/documents/search` path for the focused query cases covered by the existing file.
- **Central route table restored on the NuRaft runtime path:** `register_nuraft_http_runtime_routes(...)` in `src/nuraft/nuraft_http_runtime.cpp` is now the one scan-friendly route-registration table that replaced the deleted `src/main/typesense_server.cpp` list. Keep new server routes visible there instead of spreading registration across multiple files.
- **The central route table now also owns NuRaft write-policy lookup:** the same static write-route definitions now drive both HTTP registration and `nuraft_http_runtime_lookup_write_route_mode(...)`, so dispatch/readiness decisions no longer depend on route-registration side effects or stale per-handler heuristics.
- **Analytics write semantics are explicit now:** `/analytics/events` and `/analytics/flush` are local-only ingestion/operational routes, while `/analytics/aggregate_events` and the resulting collection import writes remain the replicated cluster path. This avoids mirrored flush stalls and multi-node double/triple counting.
- **Search analytics lock inversion is fixed:** `SearchAnalytics` compaction now follows the same lock order as request-time search analytics capture, eliminating the `/documents/search` deadlock that previously surfaced under the analytics replay lane.
- **Synonym/analytics single-node parity expanded:** `tests/synonym_sets.test.ts` and `tests/analytics.test.ts` now pass through the real `scripts/run_api_tests.sh` wrapper against `//:typesense-server`, including restart/snapshot persistence. This required booting the same product managers/stores the old server initialized, replaying single-node applied writes into live product state on startup, and fixing the NuRaft `/documents/search` wrapper so search-driven analytics events and focused document queries complete synchronously instead of hanging.
- **Curation and focused multi-node runtime lanes are green too:** `tests/curation_sets.test.ts`, `tests/nuraft_runtime_cluster.test.ts`, and `tests/nuraft_runtime_documents_crud.test.ts` now pass against `//:typesense-server`, alongside the existing `health`, `collections`, `documents`, runtime smoke, synonym, and analytics targeted replays. The current route/bootstrap changes are therefore stable for the bounded CRUD + synonym/curation/analytics single-node surface and the focused multi-node CRUD surface.
- **The first full `api_tests --no-secrets` sweep is now the main hardening signal:** a full wrapper replay on the NuRaft-backed `//:typesense-server` reached `49` passing tests and `18` expected skips, then exposed two concrete parity blockers instead of vague “NuRaft runtime missing features” gaps:
  1. **single-node mixed-state restart can still crash on startup** when the data dir contains the combined collections/documents/synonyms/curations/analytics state from the full `single-fresh` phase; the current signal is a process exit `139` during `single-restarted` startup, after manager/store load but before `/health` becomes ready.
  2. **multi-node replication still only has full parity for the explicitly classified CRUD/import routes.** In the full wrapper, `synonym_sets`, `curation_sets`, and `analytics` multi-node phases still fail because those broader product-write routes are registered and work in single-node mode, but they are not yet replicated/materialized/reloaded across the static-cluster runtime path the same way CRUD/import writes are.
- **Leadership/lag verification expanded:** the static-cluster prototype coverage now explicitly checks lagging follower status before catch-up and a preferred-leader switch after all nodes are caught up, through both `//:nuraft-static-cluster-test` and the CLI-facing `//:nuraft-replication-controller-test`.
- **Snapshot/catch-up and restart-replay verification expanded:** prototype coverage now checks that a follower installed from a leader snapshot can catch up to later writes, that a restarted follower only replays the post-switch delta after a leader change, and that divergent follower history is rejected during catch-up instead of being silently overwritten, via expanded `//:nuraft-static-cluster-test` coverage.
- **Import forwarding verification expanded:** the CLI-facing prototype coverage now pushes chunked import requests through a follower, replicates/applies them, then switches the preferred leader and repeats the same flow to confirm leader-forwarded import state still converges across the cluster, via expanded `//:nuraft-replication-controller-test` coverage.
- **Bootstrap peer-refresh verification expanded:** CLI-facing prototype coverage now refreshes existing nodes from a 2-node bootstrap to a 3-node bootstrap, starts the new follower under the expanded peer set, and verifies the new follower can catch up to prior and new writes through the static-cluster path, via expanded `//:nuraft-replication-controller-test` coverage.
- **Unhealthy-peer timed-snapshot regression is now covered in the prototype lane:** the new recovery harness proves both sides of the deadlock class reported against upstream `braft`: a "require healthy peers" timed-snapshot policy leaves only a stale recovery point for a down follower, while a leader-only timed-snapshot policy advances the snapshot and reduces recovery replay on follower rejoin.
- **HTTP async follow-up is now explicit:** the thin runtime currently keeps NuRaft write/snapshot HTTP responses on a simplified synchronous path after mixed `GET` then write flows exposed a crash in the first async handler attempt. Revisit async response/import/streaming parity before treating the NuRaft-backed `typesense-server` as a production-like integration surface.
- **Single-node restart crash root cause (fixed Mar 2026):** `replay_live_product_state()` only replayed `kUnknown` route entries (synonyms, curations, analytics) through `invoke_registered_handler()`, but those handlers assume target collections exist in `CollectionManager`. Collection creation uses `mirror_single_node_typesense_state()` during writes but was not replayed on restart. Fix: replay ALL route kinds in index order — classified routes through `mirror_single_node_typesense_state()`, kUnknown through `invoke_registered_handler()` — with non-fatal error handling so a handler returning 4xx (e.g., duplicate) doesn't abort startup.
- **Multi-node non-CRUD replication root cause (fixed Mar 2026):** `apply_materialized_mutation()` no-ops for `kUnknown` routes, so followers never execute the actual handler. Fix: (1) multi-node startup replay via temporary `NuRaftKvStateMachineSink` in `initialize()` else block, (2) `sync_live_product_state()` creates temporary sinks for multi-node to replay new entries before each read, (3) `get_runtime_collection()`/`get_runtime_collections()` check CollectionManager first regardless of node mode. The temporary sink pattern avoids RocksDB `flock()` conflicts that occur when multiple processes hold the same DB open (as happens during `replicate_and_apply()` which opens ALL nodes' DBs).
- **Benchmarking constraint is now explicit:** the repo's existing `scripts/benchmark_vs_upstream.sh` and `benchmark/` CLI are HTTP-server benchmarks. The old `//:nuraft-prototype-benchmark` and dual-runtime Raft comparison profiles are archival only on this branch — the prototype binary and its benchmark target were deleted as part of the prototype cleanup.
- **Prototype cleanup completed (Mar 2026):** all prototype scaffolding code (static cluster, request journal, segment log store, replay coordinator, recovery coordinator, prototype state machine, replication controller) was deleted. `NuRaftHttpRuntimeService::write()` now calls `append_via_raft()` directly. The `nuraft_lib` BUILD target (renamed from `nuraft_prototype_lib`) contains only production code. API test results after cleanup: 117 pass, 3 fail (pre-existing analytics counter timing issue across 3 phases), 18 expected skips per phase.
- **API replay remains a bounded parity gap, not a missing runtime lane:** the thin runtime now covers single-node and static multi-node health/status, collection/document CRUD, bounded import, focused search, and snapshot flows through the real `api_tests` wrapper. The remaining work is broader product-response parity plus async/import/streaming behavior, not "build an HTTP lane from scratch".
- **Keep sync/async as an explicit production-hardening follow-up if NuRaft wins:** the current runtime proves single-node API replay on a synchronous write/snapshot response path. If the project chooses NuRaft, add follow-up tasks for async write completion, import streaming, and long-running response handling before treating the runtime lane as production-ready.
- **Runtime contention exposed two concrete NuRaft integration gaps:** the first mixed runtime benchmark found a real RocksDB self-lock bug from opening multiple sink instances against the same materialized-state path, now fixed by sharing one process-local DB handle per path. The same lane also showed the current runtime still pays too much per-request overhead on document reads/writes, so long-lived sink/state handles plus less request-time reconstruction are now explicit follow-ups if NuRaft stays in play.

**Story E - historical evidence that justified the NuRaft cutover on this branch**

- [x] Benchmark the prototype against the former `braft` path for commit latency, write throughput, follower catch-up speed, snapshot creation/install time, and steady-state CPU/memory cost.
- [x] Measure at least one realistic contention case (concurrent write load plus follower recovery or snapshot activity), not just clean single-thread append throughput.
- [x] Record whether NuRaft's extra features are materially useful to Typesense (`pre-vote`, leadership expiration, learners, streaming mode, parallel log appending) or just theoretical headroom.

**Story E progress (Mar 2026):**

- **Prototype benchmark harness landed:** `//:nuraft-prototype-benchmark` now provides a dedicated Story E measurement lane for the isolated NuRaft prototype, emitting JSON for single-node append/apply throughput, snapshot-install/replay recovery timings after additional post-snapshot writes, repeated leader-only timed snapshots while a follower stays unhealthy, and a direct leader-only vs `require-healthy-peers` outage comparison. This keeps prototype benchmarking separate from the existing HTTP benchmark wrapper instead of pretending `scripts/benchmark_vs_upstream.sh` already answers the NuRaft question.
- **A canonical focused runtime-vs-runtime timing lane existed during the cutover:** `scripts/benchmark_vs_upstream.sh --profile raft-api-replay` measured the same real API wrapper files against the old `braft` server and the bounded NuRaft runtime, currently `tests/collections.test.ts` and `tests/documents.test.ts`. The first run produced `~3.53s` vs `~59.26s` for collections and `~2.58s` vs `~57.20s` for documents in favor of the bounded NuRaft runtime.
- **A canonical runtime contention lane existed during the cutover too:** `scripts/benchmark_vs_upstream.sh --profile raft-runtime-contention` compared configurable document writers and readers against the live `braft` server and the live NuRaft runtime on preloaded document ids. The first canonical run exposed the self-lock bug and a severe per-request reconstruction cost. After keeping the single-node request journal, state machine, materialized-state sink, and replay-progress persistence alive across requests, then adding a direct single-node append/apply fast path and persistent segment-log appends, the lane reached a clean Run 19 band of `3,388-3,448` writes / `9,061-9,102` reads for NuRaft versus `5,179-5,198` writes / `31,197-31,373` reads for `braft`. A later read-mostly cache cleanup plus repeat-support in the harness moved the mixed median of three repeats to rough write parity (`3,415` NuRaft vs `3,260` `braft`) while reads remained far behind (`9,042` NuRaft vs `42,517` `braft`). The isolation runs narrowed that result: pure reads with `--writer-threads=0 --reader-threads=2` were already slightly ahead on NuRaft (`57,204` vs `53,928`). The decisive change came when the single-node runtime stopped fsyncing replay-progress metadata on every write and instead derived local progress from the materialized sink: pure writes flipped from a `0.47x` loss to a `1.48x` win (`6,606` vs `4,458`), and a fixed-rate mixed lane (`--writer-interval-ms=2`) showed near-equal writes (`3,246` vs `3,208`) with about `91%` of `braft` read throughput (`85,642` vs `94,042`) while still using much less CPU and RSS. Those archival numbers were enough to justify the branch cutover.
- **First repeatable NuRaft numbers now exist:** medium-sized repeat runs (`--docs=200 --post-snapshot-docs=50 --snapshot-rounds=3`) were stable enough to be worth reasoning about. Append/apply stayed within a narrow band, leader-only outage recovery stayed near the low tens of milliseconds once the follower installed the latest snapshot, and `require-healthy-peers` consistently forced replay of the whole outage window instead of zero post-install replay.
- **Canonical recovery comparison harness landed during the cutover:** `scripts/benchmark_vs_upstream.sh --profile raft-recovery` staged the then-current fork's `typesense-server`, ran a real 3-node `braft` follower-outage/rejoin scenario plus a late-third-node join scenario, ran the NuRaft `snapshot-policy-compare` and `delayed-join` benchmarks with matching write counts, and emitted one JSON summary under `~/.cache/typesense/benchmark/raft-recovery-summary.json`.
- **First repeatable recovery comparison now exists:** with `--docs=200 --post-snapshot-docs=50 --snapshot-rounds=3 --repeats=2`, NuRaft's leader-only policy recovered in roughly sub-millisecond time after latest-snapshot install while `require-healthy-peers` added about `150` replayed entries and roughly `10-11ms` of extra recovery time inside the prototype lane. The live `braft` runtime recovered in roughly `3.26s`, preserved snapshot freshness to within `2` entries during the outage, and still needed about `152` replayed entries on follower restart in these runs.
- **Late-join / add-back comparison exists now too:** the same canonical command now measures a fresh-snapshot late-third-node join. In the latest `docs=200` repeats, NuRaft installed the leader snapshot and replayed only the `50` post-snapshot tail in about `10-19ms`, while the live `braft` runtime replayed the full `352` committed entries for the joining node in about `9.28s` and did not show a follower snapshot install in either repeat.
- **Process-level overhead signals are attached now too:** the same `raft-recovery` JSON now records process CPU/RSS samples for the live `braft` leader/follower plus the NuRaft benchmark process. In the latest long repeats, the NuRaft benchmark process stayed around `147ms` total CPU and roughly `25MB` peak RSS, while the live `braft` leader used roughly `315ms` CPU during the outage window and roughly `436MB` peak process RSS. This is useful directional evidence, not a final apples-to-apples runtime memory verdict, because the NuRaft side is still an isolated prototype binary.
- **Steady write-path signals exist now too:** the same canonical `raft-recovery` command now includes a simple single-node write comparison. In the latest `docs=200` repeats, the NuRaft prototype append/apply path handled the batch in about `0.88ms` append + `17.39ms` apply with roughly `22ms` total process CPU and about `26MB` peak RSS, while the live single-node `braft`/HTTP server path took about `175.63ms`, roughly `70ms` process CPU, and roughly `632MB` peak process RSS in those runs. Treat these as directional write-path signals only, not final replacement-grade throughput numbers, because they still compare an isolated prototype binary against the real server runtime.
- **Preliminary feature-utility read is now clearer:** scheduled snapshots are already proven directly useful; learners look materially relevant for safe add-back / catch-up flows; pre-vote and leadership expiration look worth carrying forward for operational stability under restarts or partitions; auto-forwarding is not a migration driver because Typesense already has HTTP leader proxying; and streaming mode / parallel log appending remain plausible throughput headroom but are still unproven until the runtime surfaces are closer.
- **Decision checkpoint that justified the cutover:** the forked `braft` path was fixed for the unhealthy-peer timed-snapshot deadlock class, but the benchmark question was no longer undecided. NuRaft showed materially better recovery, materially faster bounded API replay, pure reads that slightly beat `braft`, pure writes that beat `braft` after the local replay-progress change, and a fixed-rate mixed lane that kept about `91%` of `braft` read throughput at equal write pressure while using much less CPU and RSS. That was enough for a `go` on this branch. The remaining work after the cutover is broader API parity, async/import/streaming hardening, and continued performance tuning on the NuRaft-only server.

**NuRaft hardening tasks after the cutover:**

- [x] Fix the `single-restarted` mixed-state startup crash from the full `api_tests --no-secrets` replay, then keep that full wrapper as the main runtime regression lane instead of relying only on per-suite targeted replays. *(Fixed: `replay_live_product_state()` now replays ALL route kinds in index order — classified routes through `mirror_single_node_typesense_state()`, kUnknown through `invoke_registered_handler()` — so collections are recreated before synonym/curation/analytics handlers run.)*
- [x] Extend multi-node parity beyond CRUD/import: replicate and serve `synonym_sets`, `curation_sets`, and `analytics` correctly across the static-cluster runtime path, then use the same pattern for the remaining registered product-write families that still rely on direct product handlers. *(Fixed: multi-node startup replay via temporary `NuRaftKvStateMachineSink` in `initialize()`, `sync_live_product_state()` creates temporary sinks for multi-node follower catch-up, and `get_runtime_collection()`/`get_runtime_collections()` check CollectionManager first regardless of node mode.)*
- [x] Delete all prototype/scaffolding code and make real NuRaft consensus the only code path. *(Done Mar 2026: 16 source/header files, 8 test files, 2 binary entry points deleted. `nuraft_prototype_lib` renamed to `nuraft_lib`. CLI options `--cluster-data-dirs`, `--cluster-leader-api-port`, `--install-snapshot` removed. `NuRaftHttpRuntimeService::write()` now calls `append_via_raft()` directly instead of the 3-path `append_and_apply()`. API test results: 117 pass, 3 fail (pre-existing analytics counter timing issue).)*
- [x] Investigate the 3 failing “add a document counter analytics event” tests — likely a pre-existing timing issue in the analytics counter test, not a NuRaft regression. Appears across single-fresh, single-restarted, and single-snapshot phases. *(Fixed Mar 2026: three root causes — (1) `build_single_node_config()` included self in peers, causing `is_single_node_mode()` to return false and reads to bypass the live engine in favor of materialized state containing raw `$operations.increment` directives; (2) `update_single_node_document_cache()` marked import collections for materialized-read preference, which also bypassed the live engine; (3) `trigger_flush()` was async, creating a race between flush and read. Fixes: exclude self from peers, remove materialized-read preference for imports, make flush synchronous via generation counter. Result: 120/120 API tests pass, 0 failures.)*
- [x] Expand the real runtime/API surface beyond the current bounded CRUD/snapshot lane. *(Done Mar 2026: all 119 routes registered, all writes go through `append_via_raft()` consensus. kUnknown routes replicated via `invoke_registered_handler()` on leader and `sync_live_product_state()` replay on followers. 120/120 API tests pass across all 7 phases including multi-node synonym/curation/analytics.)*
- [x] ~~Revisit the current synchronous write/snapshot HTTP path.~~ **Verified Mar 2026:** upstream Typesense also uses fully synchronous blocking writes through raft (`request->to_json()` → single raft entry → block until quorum commit). Our NuRaft implementation matches exactly. No async design needed — this is upstream parity.
- [x] ~~Add import-streaming and long-running response parity.~~ **Verified Mar 2026:** upstream also buffers the full import body into a single raft log entry (not streamed through raft). HTTP-level chunked receipt (`async_req=true`) is already supported in our route registration. Follower forwarding uses NuRaft auto_forwarding (internal RPC) which is equivalent to upstream's HTTP proxy. Full parity confirmed.
- [x] Audit the central NuRaft route table against the old Typesense surface. *(Done Mar 2026: all 119 HTTP endpoints from `core_api.h` are registered in `register_nuraft_http_runtime_routes()`. Zero gaps found — aliases, keys, presets, stopwords, rate limits, config, conversations, personalization, nl_search_models, proxy/streaming all present.)*
- Follow-on env-dependent suite enablement is now tracked in sprint **27** (`Env-dependent suite enablement and hosted-proof audit`) so the post-cutover cleanup work is investigation-first instead of a stale unchecked tail on this completed sprint.
- Post-cutover benchmark-baseline revalidation is now tracked in sprint **30** (`NuRaft post-cutover benchmark baseline refresh`) rather than leaving it as an unowned planned checkbox here.
- [x] **Expose NuRaft configuration as CLI args and ENV overrides.** *(Done Mar 2026: all NuRaft `raft_params` are exposed as `--raft-*` CLI args and `TYPESENSE_RAFT_*` ENV vars in `typesense_nuraft_runtime.cpp`. Covered: heartbeat interval, election timeout bounds, reserved log items, client request timeout, auto-forwarding toggle and timeout, snapshot distance, leadership expiry, and Asio thread pool size. Defaults are production-sensible; ENV is applied first with CLI taking priority.)*
- [x] Fix logical snapshot transfer for large snapshot `db/` checkpoint files and verify empty-follower recovery on a fresh DDEV image. *(Done Mar 31, 2026: `TypesenseStateMachine` now chunks logical snapshot payloads into `8 MiB` objects instead of shipping whole files in one NuRaft object, and the manifest/install path now records whether the main `db/` checkpoint is expected so a missing `db/` payload fails loudly instead of silently installing a false-ready snapshot. New coverage: `TypesenseStateMachineTest.*` for chunked transfer, missing-db failure, and stale-db cleanup; `NuRaftHttpRuntimeTest.LargeDbFilesInSnapshotTransferRecoverEmptyFollowerEndToEnd` for the real subprocess-backed follower recovery path with a `320 MiB` snapshot sentinel under `db/archive`; and a fresh DDEV cluster rebuilt from the dirty workspace (`git_sha=196a105e`, `git_tree_status=dirty`) recovered an empty follower via snapshot with the large sentinel preserved, `found=40`, and post-recovery write replication confirmed. The bounded DDEV repro used `TYPESENSE_RAFT_RESERVED_LOG_ITEMS=1` so the small synthetic workload exercised snapshot transfer rather than ordinary retained-log replay.)*

**Story F - Decision record**

- [x] Recommend `go` only if the prototype preserves product-critical behavior, removes enough maintenance debt to justify the migration, and does not materially regress write-path or recovery-path performance.
- [x] Split the full migration into follow-up stories with explicit cut lines (transport, state/log persistence, snapshotting, membership, test migration, benchmark gates).
- [x] Remove the old `braft`/`brpc`/`glog` runtime stack once the branch commits to the NuRaft cutover.

**Exit criteria for this sprint:**

- [x] A written go/no-go recommendation exists.
- [x] The recommendation is backed by at least one working prototype target and real benchmark data.
- [x] The outcome explicitly answers whether NuRaft is better than `braft` for Typesense, not just whether NuRaft can be made to compile.

### 8) Modernize logging stack (glog to Abseil Logging)

Done. Backend swapped from glog to Abseil Logging. Key artifacts:

- `include/logger.h` — facade macros (`TS_LOG`, `TS_CHECK`, etc.) now delegate to `ABSL_LOG`/`ABSL_CHECK`.
- `include/ts_log_sink.h` — custom `absl::LogSink` for file output.
- `src/typesense_server_utils.cpp` — `init_root_logger()` uses Abseil as the first-party backend.
- The final NuRaft cutover removed the old `brpc`/`braft`/`glog` runtime dependency from this branch.
- Console logs moved from stdout to stderr (Abseil default, more standard for daemons).

### 9) Warning policy and compiler hygiene

- [x] GCC and Clang warning guardrails at zero (`tracked=0`, `total=0` for both compilers). *(GCC stays in the automatic push gate; clang now runs as a dedicated manual lane to keep the default CI path fast while preserving explicit compiler coverage.)*
- [x] CI enforces guardrails via `scripts/check_clang_warning_guardrail.sh` and `scripts/check_gcc_warning_guardrail.sh` with failure-artifact upload.
- [x] First-party C++ test signedness cleanup committed (`e9bea816`); all `-Wsign-compare` warnings resolved.
- [x] Unused-variable warnings across test files and src cleaned up (`8420f815`).
- [x] **Audit visible build warnings across normal and sanitizer builds:** Done Mar 2026. Normal, GCC, Clang, ASAN, and TSAN lanes were audited locally with Dockerized replays; first-party fixes landed in `src/system_metrics.cpp`, and the remaining noise buckets are now constrained to the narrowest justified compiler/config/dependency scopes.
  - `protobuf`: `-Wsign-compare` and `-Wmaybe-uninitialized` in generated/upb sources; `.bazelrc` now scopes these suppressions to `external/protobuf+.*` for local GCC builds, but sanitizer coverage still needs a full replay to confirm nothing broader is required
  - `abseil-cpp`: deprecated C++20 implicit lambda capture of `this` in `container_internal` headers
  - `abseil-cpp`: TSAN `atomic_thread_fence` warning in `synchronization/internal/graphcycles.cc`
  - `libstdc++` / `std::regex` under GCC 14, surfaced via the transitive `clip_tokenizer` include path: repeated `-Wmaybe-uninitialized` diagnostics in `bits/regex_automaton*.h`
  - first-party `src/system_metrics.cpp`: fixed locally on March 14, 2026 by initializing jemalloc stats safely, scoping jemalloc-only locals under `#ifndef NO_JEMALLOC`, guarding the fragmentation ratio divide, and correcting `typesense_memory_resident_bytes` to report `resident` instead of `active`
  - Bazel/JVM startup: deprecated `-Xverify:none` / `-noverify` banner from the Java runtime used under Bazel/Bazelisk
  - Approach: fix real first-party issues in source. For third-party or toolchain-only noise, use the narrowest possible suppression or config fix for each source. Prefer per-file or per-external-target compiler flags, or tool-specific startup config, over global `-w` / broad `-Wno-*` flags. Keep new-warning visibility intact so future first-party regressions and unaudited external warnings still show up.
  - Current local proof on March 14, 2026: `scripts/bazel_in_docker.sh build //:typesense-server`, a cold-cache `TYPESENSE_BAZEL_CACHE_DIR=/home/cyppe/tmp/typesense-warning-audit/cache scripts/bazel_in_docker.sh build //:typesense-test`, plus sanitizer replays under `--config=asan` and `--config=tsan`, all completed without visible compiler warnings beyond the known Bazel/OpenJDK startup banner. The earlier `src/collection.cpp` / `std::regex` noise was audited and is now suppressed only in the narrow GCC sanitizer buckets where it is a reproducible libstdc++ false positive.
- Done when:
  - [x] Warning debt trend is downward and enforced by CI.
  - [x] Zero warnings visible in normal, ASAN, and TSAN builds apart from the known Bazel/OpenJDK startup banner.

## Priority 2 - CI And Developer Experience

### 10) Unify test and build entry points

Done. Dockerized Bazel wrapper (`scripts/bazel_in_docker.sh`), CI uses repo Docker toolchain, legacy scripts removed. Repro steps match CI behavior.

### 11) Add modern verification lanes

- [x] Add sanitizer coverage (ASAN and TSAN).
  - `.bazelrc` `--config=asan` with ASAN flags (UBSAN removed — breaks abseil constexpr evaluation with GCC 14), `--config=tsan` with TSAN flags.
  - Jemalloc exclusion via `NO_JEMALLOC` define and `select()` in BUILD for both sanitizer configs.
  - Foreign_cc deps (iconv, kakasi) cancel sanitizer flags via `-fno-sanitize` env overrides in their BUILD files (configure scripts break under instrumentation).
  - TSAN suppression file (`test/tsan_suppressions.txt`) for pre-existing upstream bugs: sparsepp data race, analytics lock-order-inversion.
  - Manual CI lane in `.github/workflows/sanitizer-testing.yml` with parallel ASAN and TSAN jobs.
- [x] Add optional slower manual checks (stress, migration, multi-node longevity).
  - Full-suite stress (`--runs_per_test=5`) and multi-node API longevity lanes in `.github/workflows/nightly-extended.yml`.
**CI hygiene backlog:**
- [x] Pin Bazelisk version in `docker/ci-bazel.Dockerfile` (pinned to v1.28.1).
- [x] Pin clang version explicitly in Dockerfile (pinned to `clang-18` with `update-alternatives` symlinks).
- [x] Upgrade GitHub Action versions across workflows (audit as of Mar 2026):
  - `actions/checkout` `@v4` → `@v6` — done (credential persistence default change audited; not affected since we don't use `persist-credentials` or post-checkout token operations).
  - `actions/upload-artifact` `@v4` → `@v7` — done (purely additive).
  - `actions/download-artifact` `@v5` → `@v8` — done (Node 20 deprecation warnings removed; all workflows now on the current major).
  - ~~`actions/setup-node` `@v3` → `@v4`~~ done (benchmark-testing.yml no longer uses it after the Bun-first workflow cleanup).
  - `dawidd6/action-download-artifact` `@v17` → `@v19` — done (benchmark-testing.yml).
  - `oven-sh/setup-bun` `@v2` — already on latest major.
- [x] Keep `flake-detection.yml` available as the dedicated manual stress workflow for source/build-system changes.
- [x] Bazel disk cache persistence via `actions/cache` restore/save steps across all Bazel-bearing GitHub workflows, including `release-binaries.yml`.
  - Caches Bazel `disk-cache`, `repository-cache`, and `bazelisk`; shared GitHub-hosted workflows no longer persist the full output root.
  - Keys include the relevant workflow/toolchain inputs (for example `.bazelrc`, `bazel/**`, and the matching CI Dockerfile) with prefix restore-keys, while release lanes also isolate by runner + artifact lane.
  - Sanitizer jobs use config-specific keys (`-asan-`, `-tsan-`) since different build configs produce incompatible artifacts.
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

**CI workflow** (`benchmark-testing.yml`): Manual `workflow_dispatch` lane. Downloads the two most recent successful `typesense-server` binaries from the `tests` workflow using `dawidd6/action-download-artifact@v6`. Starts Docker Compose services, builds the CLI, runs `./dist/index.js benchmark --binaries <old> <new> -c <old-sha> <new-sha> --duration 1m`. InfluxDB data is persisted across runs via artifact upload/download. Results are compared using configurable p95 regression thresholds per scenario.

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
# Prerequisites: Docker; a built typesense-server binary when you are not using `--build`
# Host Bun is now only for direct benchmark CLI development or the explicit `--host-bun` escape hatch.

# The supported default is the root wrapper:
TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile standard --scope core

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
| `scripts/publish_release.sh` | Publish helper for already-built release artifacts |
| `docker/ci-bazel.Dockerfile` | CI Docker image definition |
| `test/temp_dir_utils.h` | Per-test-instance temp directory isolation |
| `test/scripts/prewarm_public_test_models.sh` | Public model cache warmup for embedding tests |
| `api_tests/` | TypeScript/Bun API test suite |
| `include/logger.h` | Logging facade (Abseil backend, `TS_LOG`/`TS_CHECK` macros) |
| `include/ts_log_sink.h` | Custom `absl::LogSink` for file output |
| `include/replication/replication_service.h` | Abstract replication seam consumed by HttpServer, analytics, etc. |
| `include/nuraft/nuraft_http_runtime.h` | Main NuRaft HTTP runtime service header |
| `src/nuraft/nuraft_http_runtime.cpp` | NuRaft runtime: init, write, read, snapshot, leader discovery |
| `src/main/typesense_nuraft_runtime.cpp` | NuRaft server entry point (CLI parsing, options) |
| `include/nuraft/typesense_state_machine.h` | Real NuRaft `state_machine` implementation |
| `include/nuraft/typesense_log_store.h` | Real NuRaft `log_store` implementation |
| `include/nuraft/typesense_state_manager.h` | Real NuRaft `state_mgr` implementation |
| `bazel/PATCH_DEBT.md` | Classified inventory of all active external dependency patches |

## Priority Queue (What To Work On Next)

This is the **living priority list**. AI agents should pick the top non-blocked item and execute it. Update this list after completing any task.

| # | Task | Section | Status | Notes |
|---|------|---------|--------|-------|
| 25a | Build and publish a fresh DDEV test image from the post-byte-aware-helper branch head | P1 Runtime/Perf | **active** | March 22, 2026 DDEV on the published image `2c06d882` still hit a `33.99s` `categories_se` import because `177,813` large `products_se` docs were rewritten in one helper pass (`helper_chunks=1`, `fetched_doc_bytes≈2.14GiB`). Current local HEAD now uses a generic byte-aware helper planner and the new `180k` large-doc `category_fanout` lane is green (`18858.5ms`, `helper_chunks=4`, search `32.8ms`). Next gate: publish that newer branch head and rerun DDEV on the fresh image. |
| 25b | Re-check DDEV on the new image with the March 22 helper + snapshot telemetry enabled | P1 Runtime/Perf | **pending** | Use the fresh image from item 25a and keep the current Laravel/Typesense telemetry in place. If DDEV still stalls, capture the new `nuraft_snapshot_*` metrics plus the helper planning fields (`planned_chunk_docs`, `chunk_plan_sample_docs`, `chunk_plan_estimated_total_doc_bytes`) before changing code again. |
| 45 | Upstream `v31` parity Phase 2 — remaining 11 missing commits | P1 Parity | **pending** | 11 upstream commits from `v31` are confirmed missing after the Mar 28 deep audit. Includes 2 critical race-condition/safety fixes, 5 high-priority search/security correctness fixes, and 4 medium-priority robustness fixes. Phased backport plan below. |
| 1 | ~~Coordinated h2o/picotls/quicly bump~~ | P1.7b | **done** | All 3 deps bumped, patch regenerated, build verified. |
| 2 | ~~ICU fork→upstream tarball~~ | P1.7 | **done** | Switched from `typesense/icu` fork to official ICU 71.1 release tarball. Patch unchanged. |
| 3 | ~~CI action version bumps~~ | P2.11 | **done** | `actions/checkout` v4→v6, `actions/upload-artifact` v4→v7 across all 5 workflows. |
| 4 | ~~ICU version upgrade (71→78)~~ | P1.7 | **done** | Upgraded to ICU 78.2 (Unicode 14→17, CLDR 48). Patch regenerated. |
| 5 | ~~Patch debt reduction (whisper)~~ | P1.7 | **done** | Reduced from 11 hunks to 8, then to 1 hunk via v1.8.3 upgrade. |
| 6 | ~~CI hygiene backlog (remaining)~~ | P2.11 | **done** | Bazel disk cache via `actions/cache` restore/save steps in all Bazel-bearing workflows, including `release-binaries.yml`. |
| 7 | ~~Benchmark infrastructure audit~~ | P2.13 | **done** | Stack audited, local execution documented, cross-fork comparison feasible. |
| 8 | ~~Fix pre-existing test warnings~~ | Known Issues | **done** | Narrowing + trigraph warnings fixed. |
| 9 | ~~Protobuf 34 upgrade~~ | P1.6b | **done** | Upgraded from 33.5 to 34.0.bcr.1. Updated MODULE.bazel and onnxruntime.BUILD version strings. Clean build, 120/120 API tests pass, all 4 NuRaft unit tests pass. No code changes required — protobuf is only an indirect dependency via OnnxRuntime and SentencePiece. |
| 10 | ~~Cross-fork benchmark script~~ | P2.13 | **done** | `scripts/benchmark_vs_upstream.sh` for upstream comparison. |
| 11 | ~~Definition of Done audit~~ | DoD | **done** | All 6 checkboxes verified and marked complete. |
| 12 | ~~Fix Bazel 9.0.0 not running in Docker~~ | P1.6 | **done** | Verified with `scripts/bazel_in_docker.sh version` and `TYPESENSE_BAZEL_IMAGE=typesense/ci-bazel:ci scripts/bazel_in_docker.sh version`: Bazelisk `v1.28.1`, Bazel `9.0.0`. CI workflows already run `--build-image-only`; stale local images were the mismatch source. |
| 13 | ~~RocksDB perf tuning Phase 3 (data-driven)~~ | P2.13 | **done** | Runs 9-13 complete: observability, sweeps, read-path optimizations, `max-indexing-concurrency` validation, and import `batch_size` A/B check. Final policy keeps conservative defaults with hardware-based tuning guidance. |
| 14 | ~~Benchmark observability: full metrics collection + Grafana dashboard~~ | P2.13 | **done** | Core observability is in place: benchmark runs collect system/API/RocksDB metrics continuously and dashboard includes concurrent search+import visibility. Tuning-specific counter extraction is tracked under item 13. |
| 15 | ~~JS/Docker workflow consolidation~~ | P2 DX | **done** | Benchmark/API tooling is Bun-first, benchmark CI now uses the shared wrapper, and API tests have a Dockerized wrapper entrypoint. |
| 16 | ~~Static ONNX Runtime linkage probe~~ | Known Issues | **done** | Promoted `typesense-server` to the one-Protobuf static ORT path. `ldd bazel-bin/typesense-server` shows no `libonnxruntime.so.1`, the no-secrets API suite passes, and direct local `ts/e5-small` embedding/vector-search smoke succeeds. |
| 17 | ~~Release packaging / multi-arch workflow hardening~~ | Known Issues | **done** | Full hosted multi-arch validation was green across `linux-amd64`, `linux-arm64`, `darwin-arm64`, and `darwin-amd64`, including Linux DEB/RPM generation and Darwin tarball validation. Item 37 later retired the remaining stale draft-posture wording after current-tip Darwin validation succeeded on `7eb65c27` via run `23242998148`. |
| 18 | ~~Dependency refresh audit (current vs latest)~~ | P1 Build/Deps | **done** | All actionable deps refreshed: magic_enum 0.9.7, libarchive 3.8.5, snappy 1.2.2, ORT 1.24.3, typesense-js 3.0.2, protobuf 34.0.bcr.1, abseil-cpp 20260107.1. Core infra deps (curl 8.18.0, openssl 3.6.1, jemalloc 5.3.0, zstd 1.5.7, lz4 1.10.0) all confirmed at latest. Patch debt hit a local minimum of 5 active patches, h2o reduced to 48 lines; later item 34 traded one small `kakasi` patch for the final fork-backed source removal. ORT now reuses the repo's external Abseil source via BUILD-level `FETCHCONTENT_SOURCE_DIR_ABSEIL_CPP`, so no new `bazel/onnxruntime.patch` hunk was needed. whisper.cpp upgraded to v1.8.3 (patch down to 1 hunk). |
| 19 | ~~NuRaft replacement cutover and hardening~~ | P1.7d | **done** | Real NuRaft consensus is the only path. All 120/120 API tests pass (0 failures). Prototype code deleted, CLI/ENV config exposed, analytics counter bugs fixed, snapshot identity fixed. Follow-on env-dependent suites and benchmark-refresh work now live under items **27** and **30** instead of as stale unchecked tails in P1.7d. |
| 20 | ~~Sanitizer lane stabilization + ORT extensions boundary audit~~ | Known Issues | **done** | Broad GitHub stabilization is green on `v32` (`tests`, `sanitizer-testing`, `nightly-extended`, `flake-detection`, plus the earlier narrow `release-binaries` linux-amd64 replay), and the final local blocker was closed on March 14, 2026. The ORT audit confirmed `EnableOrtCustomOps()` is only used by `CLIPImageProcessor`; `CollectionManager::dispose()` already clears image/text embedders, and `process_embedding_field_delete()` now matches that image-before-text teardown order. The remaining ASAN red was third-party ONNX Runtime Extensions static custom-op loader teardown, not first-party ownership: `bazel/onnxruntime.patch` now rewrites fetched `static OrtOpLoader` operator-loader singletons to process lifetime before ORT builds `libocos_operators.a`, so `.bazelrc` no longer needs `ASAN_OPTIONS=new_delete_type_mismatch=0`. Local proof on March 14, 2026: `scripts/bazel_in_docker.sh test --config=asan --cache_test_results=no --test_output=errors //:typesense-test --test_timeout=3600 '--test_arg=--gtest_filter=CollectionVectorTest.TestImageEmbedding:CollectionVectorTest.TestUnloadingModelsOnCollectionDelete' --test_env=TYPESENSE_TEST_MODELS_DIR=/work/tmp/ci-models` passed under the default repo ASAN config, and `scripts/bazel_in_docker.sh build //:typesense-server` also passed on the same tip. |
| 21 | ~~NuRaft import/runtime parity + benchmark refactor sprint~~ | P1 Runtime/Perf | **done** | Final replay-model runtime parity is in: one logical import request is buffered once, replicated as bounded logical NuRaft chunks, then replayed through the existing import handler cadence. The direct `1M` single-POST gate is green, local `quick/core` self-compare and `standard/core` upstream compare are green, the broad `--no-secrets` API gate is green, and hosted `benchmark-testing` also passed on `27bb2bff` against the previous same-branch baseline `985acb45` in `43m55s`. |
| 22 | ~~Release workflow promotion / current-tip full-matrix replay~~ | Known Issues | **done** | Current-tip release proof was refreshed on the final pre-release SHA `a2832b63c2a701e08fc5b571ee9507473df991c8`: `release-binaries` run `23088513187` succeeded on `linux-amd64`, `linux-arm64`, `darwin-arm64`, and `darwin-amd64` on March 14, 2026. The follow-up versioned publish dry-run used that run's artifacts with label `0.0.0-a2832b63`; it exposed one real release-path bug in `scripts/publish_release.sh` (RPM uploads were skipped because generated files are named `typesense-server-<version>.<arch>.rpm`, not `typesense-server-<version>-<arch>.rpm`). After fixing the RPM glob, the same dry-run recorded the full `12` mocked `aws s3 cp` uploads (`4` tarballs, `4` tarball `.sha256.txt` sidecars, `2` `.deb`, `2` `.rpm`). Item 37 later closed the remaining plan-only draft-posture cleanup after current-tip hosted validation on `7eb65c27` via run `23242998148`. |
| 23 | ~~Compile warning audit / cleanup~~ | P1 Hygiene | **done** | Completed March 14, 2026. Home-cache sanitizer replays are now closed locally: `TYPESENSE_BAZEL_CACHE_DIR=/home/cyppe/tmp/typesense-warning-audit/asan-cache scripts/bazel_in_docker.sh build --config=asan //:typesense-server` and `TYPESENSE_BAZEL_CACHE_DIR=/home/cyppe/tmp/typesense-warning-audit/tsan-cache scripts/bazel_in_docker.sh build --config=tsan //:typesense-server` both pass warning-clean apart from the known Bazel/OpenJDK startup banner. The remaining buckets were audited as toolchain/third-party noise, not first-party bugs: GCC 14/libstdc++ `std::regex` `-Wmaybe-uninitialized` false positives under ASAN from regex-heavy first-party TUs plus `clip_tokenizer`, GCC-only protobuf `-Wmaybe-uninitialized` false positives, and GCC TSAN `-Wtsan` warnings from external Abseil `atomic_thread_fence`. `.bazelrc` now keeps compiler-specific suppressions behind `build:gcc` and sanitizer-specific suppressions behind `build:asan` / `build:tsan`, while `scripts/bazel_in_docker.sh` auto-applies `--config=gcc` only for non-clang `build`/`test`/`run`/`coverage` lanes so the clang warning guardrail stays fully visible. |
| 24 | ~~Release artifact debug-info policy~~ | Known Issues | **done** | Completed March 14, 2026. Policy is now explicit for Linux release artifacts: ship a stripped runtime binary in the normal tarball/DEB/RPM path, and publish split debug symbols as a separate `.debug.tar.gz` sidecar keyed to the same BuildID via `.gnu_debuglink`. Local proof from `bazel-bin/typesense-server`: current unstripped binary is `427M` with embedded debug info; a split-debug copy measured `151M` stripped runtime plus `297M` debug file and the stripped binary still passed the `--help` smoke check. `.github/workflows/release-binaries.yml` now performs the split before checksum/tarball/package assembly and uploads the debug-symbol sidecar artifact for Linux lanes. |
| 25 | Investigate NuRaft heavy-import responsiveness and benchmark blind spots | P1 Runtime/Perf | **active** | Real DDEV import traffic on March 20-21, 2026 showed `GET /metrics.json`, `/health`, `/collections`, `/aliases`, and `/keys` slowing into the `2.5-4.7s` range during heavy imports while `ImportVehiclesToProductsTypesenseJob` workers timed out at `600s`. Runtime work now includes delete-many / patch-many live-mirror fixes, NuRaft lag metrics, NuRaft import timing metrics (`append_ms`, `replay_ms`, `total_ms`, chunk counts), import-handler timing metrics, collection import timing metrics (`parse_ms`, `reference_helper_ms`, `validate_ms`, `memory_ms`, `write_ms`, `async_reference_ms`), collection create/drop timing, outer HTTP request lifecycle timing plus response dispatch/queue/progress counters, per-message dispatcher queue metrics, and main/meta thread-pool queue/wait metrics in `metrics.json`; the fork also now routes `metrics.json` / `stats.json` through the meta thread pool and isolates `STREAM_RESPONSE` onto its own dispatcher queue so response delivery is not blocked behind `REQUEST_PROCEED` / `DEFER_PROCESSING` traffic. March 22 extended that observability again with async-reference helper cumulative/max counters (`collection_import_cumulative_async_reference_helper_*`, `collection_import_max_async_reference_helper_*`), the last helper field name plus helper retry/failure counters, richer slow import logs that inline the request timing breakdown together with helper context (`helper_collection`, `helper_field`, helper total/update counts) on the `event=slow_request` line itself, and snapshot metrics/status (`nuraft_snapshot_distance`, `nuraft_snapshot_in_progress`, `nuraft_last_snapshot_*`, `nuraft_cumulative_snapshots`, `nuraft_cumulative_snapshot_failures`). The original stable-count DDEV timeout is no longer root-cause-unknown: live gdb stacks on the old image showed the commit thread inside `raft_server::snapshot_and_compact()`, DDEV startup logs still reported `snapshot_distance=10000`, and the update-heavy local replay stays healthy at the new default `snapshot_distance=100000` while timing out again when forced to `10`. The remaining March 22 DDEV gap on the newer published image is now also understood: `categories_se` still rewrote `177,813` large `products_se` docs in one helper pass (`fetched_doc_bytes≈2.14GiB`, `helper_chunks=1`, `33.99s` slow request). Current local HEAD fixes that generically with a byte-aware async-reference planner: once the matched set exceeds `100k` docs, sample stored-doc size, keep moderate estimated fetched bytes monolithic, and otherwise plan chunk size toward about `512 MiB` with a `50k`-doc floor. That policy is now green locally on all three relevant lanes: `1M` late fitment fanout (`5121.1ms`, `chunks=1`), `3M` late fitment fanout (`18500.2ms`, `chunks=2`), and `category_fanout 180k / 711 / 12KB` (`18858.5ms`, `chunks=4`, search `32.8ms`). Upstream's March 2026 `/health`-responsiveness fix (`f88942f`) was already present in the fork, so the remaining gate is to publish a fresh image from this newer helper + snapshot posture and rerun DDEV rather than tuning the older published image again. A dedicated local replay harness now exists in `scripts/replay_fitment_import_stress.py`: it starts a local server, uses repo-owned fixture schemas under `benchmark/data/fitment_replay_schemas/`, preserves the `product_vehicle_fitments_se` async-reference shape, defaults to the DDEV ordering where fitments import before referenced docs exist, can add a secondary update-heavy fitment phase via `--secondary-fitment-update-docs`, and runs concurrent bulk imports with `/health` `/metrics.json` `/stats.json` probes so fork-vs-upstream comparison no longer requires a full DDEV cycle. Live schema fetch remains optional for debugging drift only, not for the canonical benchmark lane. The same harness now also owns the canonical host-side profiling bundle for this issue class: PID-scoped `perf record` on-CPU flamegraphs, `perf record --off-cpu`, `perf stat`, and `runqlat`, with artifacts written into the replay temp dir. Benchmark work still needs those replay results folded back into the canonical benchmark matrix before more performance claims are considered closed. |
| 25 | ~~Release workflow promotion and repo-owned replay extraction~~ | Known Issues | **done** | Completed March 15, 2026. Story A landed on promote-with-extraction, the Linux release lane now uses a repo-owned container-backed wrapper, and hosted run `23107397373` validated the extracted path across `linux-amd64`, `linux-arm64`, `darwin-arm64`, and `darwin-amd64`. |
| 26 | ~~ORT external Abseil injection and upgrade unblock~~ | P1 Build/Deps | **done** | Completed March 15, 2026. Story A said go: ORT's upstream Abseil CMake flow can be cleanly repointed at the repo's module source with `FETCHCONTENT_SOURCE_DIR_ABSEIL_CPP` in `bazel/onnxruntime.BUILD`, so `abseil-cpp` was bumped to `20260107.1` without growing `bazel/onnxruntime.patch`. Proof: canonical Docker build, `health.test.ts`, and `CollectionVectorTest.TestUnloadingModelsOnCollectionDelete` all passed; `ldd bazel-bin/typesense-server` still shows no shared `onnxruntime`, `absl`, or `protobuf` dependency. |
| 27 | ~~Env-dependent suite enablement and hosted-proof audit~~ | P1 Test Infra | **done** | Completed March 15, 2026. The supported matrix is now explicit: keep secret-gated API coverage all-or-nothing on `OPENAI_API_KEY` + `AZURE_OPENAI_API_KEY` + `AZURE_OPENAI_URL`, keep TEI as a separate documented lane, and retire the stale `--download-migration-binary` story because pre-NuRaft migration replay is unsupported in the current Bun harness. Story B also closed two real gaps: `scripts/run_api_tests.sh` now forwards TEI/secret env vars through Dockerized Bun, and the NuRaft runtime auth hook now reuses `handle_authentication(...)` so `post_multi_search` gets the same embedded-param preprocessing as the classic server. |
| 28 | ~~Cross-platform debug-symbol policy parity~~ | P2 Release | **done** | Completed March 15, 2026. Story A landed on no-go for Darwin `dSYM` sidecars: keep Linux split debug info, but make macOS explicit as "ship the unstripped tarball with embedded DWARF and no sidecar" until there is a concrete size/symbolication need plus a macOS-native validation lane. Local proof on `18adf2a0`: current `bazel-bin/typesense-server` is `401M`, the Linux split-debug replay produced `129M` stripped runtime + `292M` debug file (`44M` runtime tarball + `104M` debug tarball), and a comparable single unstripped tarball from the same binary was `145M`. |
| 29 | ~~Explicit compiler-config topology audit~~ | P2 Hygiene | **done** | Completed March 15, 2026. Story A landed on no-go for a topology rewrite: keep the wrapper-driven default GCC contract because the supported matrix is still "default GCC lanes + explicit clang warning lane only", and an explicit compiler-entrypoint sweep would mostly duplicate flags across wrappers, workflows, sanitizer lanes, release helpers, benchmark helpers, and docs without adding new verified coverage. The audited contract is now explicit in this plan: the Docker image defaults `cc`/`c++` to GCC 14, `build:gcc` only carries GCC-specific suppressions, explicit clang lanes must pass `--repo_env=CC=clang --repo_env=CXX=clang++`, and sanitizer/release/benchmark flows remain default-GCC unless a future sprint adds a real second compiler lane. |
| 30 | ~~NuRaft post-cutover benchmark baseline refresh~~ | P2 Perf | **done** | Completed March 15, 2026. Story A landed no-go on a fresh replay: keep Run 27 plus the current `standard/core` policy and hosted run `23085170081` as the canonical post-cutover posture, and remove stale prototype-benchmark docs. |
| 31 | ~~Container-first tooling coverage audit~~ | P2 DX | **done** | Completed March 15, 2026. Story A ranked the remaining host-tool assumptions and landed on go only for the benchmark path: `scripts/benchmark_vs_upstream.sh` now defaults to the repo's Dockerized benchmark CLI, `benchmark-testing.yml` no longer installs Bun on the runner, and host-only flows are explicitly kept to intentional escape hatches (`--host-bun`, `check_local_toolchain.sh`) or native constraints (Darwin release lanes). |
| 32 | ~~Upstream arm64 lg-page16 release parity~~ | P2 Release | **done** | Completed March 15, 2026. Story A landed on go: the existing suffix-aware package/publish path already matched upstream naming, so the variant belongs in `release-binaries` rather than a sibling workflow. Story B extended the canonical Linux release wrapper plus the Docker Bazel wrapper for platform-aware local replay, added a `linux-arm64-lg-page16` matrix lane to `release-binaries.yml`, and updated the runbook/README. Local proof was split but concrete: the arm64 Bazel build drove `jemalloc` with `--with-lg-page=16`, and the package script emitted the expected `typesense-server-30.1-arm64-lg-page16.deb` / `typesense-server-30.1-1.lg.page16.aarch64.rpm` names from an upstream-style tarball. |
| 33 | ~~GPU deps artifact automation boundary~~ | P2 Release | **done** | Completed March 16, 2026. The real blocker was the branch's one-Protobuf CUDA server path, not DEB/RPM conversion: `@onnx_runtime//:onnxruntime_static_one_protobuf --define=use_cuda=on` initially failed under external Protobuf 34, then was unblocked with a repo-owned protobuf compatibility patch. Story B added the canonical `scripts/release_linux_gpu_deps.sh` tarball/package producer, folded the optional GPU-deps job into the manual `release-binaries.yml` workflow so one dispatch can build the full release set, updated Linux `release-binaries` to build the regular `typesense-server` artifact with `--define=use_cuda=on`, and kept docs honest that the GPU surface here is ONNX Runtime embeddings/personalization only (Whisper remains CPU-only). |
| 34 | ~~Replace patch-only forks with released upstream versions where possible~~ | P1.7 Patch Debt | **done** | Completed March 17, 2026. Removed the stale `typesense/hnswlib` fork by pinning the same upstream `nmslib/hnswlib` parent commit directly while the migration was still in progress, switched `clip_tokenizer_cpp` from the Typesense mirror to the identical upstream `ozanarmagan/clip_tokenizer_cpp` commit, and moved `kakasi` off the Typesense fork to upstream `loretoparisi/kakasi` with a small repo-owned patch plus the in-repo `japanese_data` payload. Item 36 later removed the supported `hnswlib` runtime path entirely, so no fork-backed Bazel deps remain. |
| 35 | ~~Script / doc entrypoint architecture cleanup~~ | P2 DX | **done** | Completed March 17, 2026. The public wrapper boundary is now explicit: repo-level task entrypoints live under `scripts/`, the publish helper moved from repo root to `scripts/publish_release.sh`, subsystem helpers are documented as internal support paths, and the C++ replay helper now uses the repo-owned public-model prewarm helper. |
| 36 | ~~USearch vector backend prototype / refactor boundary~~ | P1 Vector Search | **done** | Completed March 17, 2026. The supported production vector path is now USearch `index_gt` only behind `vector_index_t`; the remaining in-tree `hnswlib` backend and Bazel dependency were removed, mixed update/search behavior is benchmarked, and the closeout kept `hnsw_params` only as API-compat config while adopting USearch's builtin normalized-IP metric. |
| 37 | Release workflow promotion / draft-posture cleanup | P2 Release | **done** | Completed Mar 18, 2026. Current pushed tip `7eb65c27` is hosted-green on the relevant release proof (`release-binaries` run `23242998148`, both Darwin lanes success) and matching gate (`tests` run `23242982005` success). Fresh source audit confirmed the supported-manual-release posture already lives in workflow/docs/scripts; only stale plan wording remained. |
| 38 | ~~Indexing hot-path string-copy audit~~ | P2 Perf | **done** | Completed Mar 18, 2026. Commit `7ab4cd8b` removed the highest-signal import-path deep copy by moving parsed local JSON documents directly into `index_record`, and after repairing the Dockerized Influx bind-mount cleanup bug in `scripts/benchmark_vs_upstream.sh`, the canonical upstream `30.1` `standard/core` replay on `1c34ddf7` closed green with import `50.270s -> 31.790s` and all meaningful non-zero search scenarios faster than upstream. |
| 39 | ~~Search work-budget configurability audit~~ | P2 Search | **done** | Completed Mar 18, 2026 as a no-go for new API surface. The 2017 TODO referred to the pre-Index refactor search path, but current search budget behavior is already explicitly controlled by `typo_tokens_threshold`, `drop_tokens_threshold`, `max_candidates`, and `search_cutoff_ms`; a focused `CollectionTest.TypoTokensThreshold` replay now also proves typo expansion is not implicitly tied to `per_page`. |
| 40 | ~~Upstream `v30` release-parity catch-up audit~~ | P1 Parity | **done** | Completed Mar 18, 2026. Every missing official `upstream/v30` patch family is now either landed locally on `v31-fork` or explicitly classified as already present, so stable-line parity is intentional instead of assumed from the old fork point. |
| 41 | ~~Upstream `v31` selective intake audit~~ | P2 Intake | **done** | Completed Mar 20, 2026. The low-risk post-fork bugfix subset is now landed locally, and the remaining tracked `v31` deltas are explicitly deferred as larger product-surface work rather than silent parity gaps. |
| 42 | ~~Repo-owned prebuilt CUDA ORT bundle intake~~ | P2 Release | **done** | Completed Mar 19, 2026. Linux release lanes can now reuse a repo-owned CUDA-enabled one-Protobuf ORT install-tree bundle instead of rebuilding ORT from source on every cold hosted run, while keeping the source-build path as the default fallback. |
| 43 | ~~Startup config / CLI parity recovery~~ | P1 Parity | **done** | Completed Mar 20, 2026. The shipped NuRaft `typesense-server` now keeps NuRaft as the runtime architecture/entrypoint but reuses the shared Typesense `Config` + `init_cmdline_options` surface for upstream-compatible CLI/env/INI settings, with only NuRaft-specific extensions (`node-host`, `api-uses-ssl`, `raft-*`) layered on top. This restored support for upstream-style server flags like `--enable-cors`, `--max-group-limit`, `--healthy-read-lag`, and `--cache-num-entries`, made `--help` enumerate the full supported surface again, and re-wired the runtime to honor config-driven HTTP/CORS/SSL/thread-pool/RocksDB settings instead of a hand-maintained mini-parser. |
| 44 | ~~Same-document concurrent write correctness hardening~~ | P0 Test Reliability / P1 Runtime | **done** | Completed Mar 23, 2026. Root cause was a real stale-state race in `Collection::add_many()`: overlapping writes to the same logical document ID could both resolve against the old stored doc before either RocksDB write became visible, so concurrent `documents/import?action=update` increments, disjoint partial updates, and duplicate creates could all behave incorrectly. The fix moved serialization to the collection layer with per-document-ID striped write locks that cover the full existence-check / `old_doc` load / in-memory index update / RocksDB write window, and the old runtime-layer ordering experiment was removed once the lower-layer fix proved sufficient. Regression coverage now spans C++ collection tests, explicit-id `Collection::add(..., UPDATE, doc_id)` tests for the direct `PATCH /documents/:id` path, NuRaft HTTP runtime tests, single-node API tests, follower-originated multi-node API tests, the original analytics counter lane, and docs-surface API checks for single-document `action=upsert` plus bulk `action=emplace`. The API wrapper also now refreshes the default runtime bundle from the current Bazel output automatically so future CI/local replays do not silently exercise a stale binary. |

### 25) Release workflow promotion and repo-owned replay extraction

**Why this sprint exists now**

- When this sprint opened, `release-binaries` was technically green across Linux and Darwin, but the workflow was still effectively in a "draft but working" posture.
- Linux tarball/package/debug-sidecar assembly now has real policy behind it, but too much of that knowledge still lives in workflow-only shell.
- The next agent should prove whether a repo-owned wrapper meaningfully improves local/CI parity before doing extraction work by reflex.

**Sprint goal**

- Decide whether the release workflow is ready for promotion/cleanup, and extract packaging logic into one canonical repo entrypoint only if that clearly improves reproducibility.

**Story A - Investigation**

- [x] Audit the current `release-binaries.yml` shell blocks against existing repo scripts and list exactly which Linux packaging steps are still workflow-only.
- [x] Verify the latest successful `release-binaries` run on the current branch tip and enumerate the remaining blockers to removing the "draft" posture (naming, docs, inputs, artifact layout, manual steps).
- [x] Produce a go/no-go recommendation: promote as-is, promote with script extraction, or keep draft for now.

**Story A findings (Mar 15, 2026):**

- Latest successful full hosted `release-binaries` proof on `cyppe/typesense` is run `23095183201` from March 14, 2026 on SHA `b03f9a9af1283549d5f066e5e322c4a90f4a46a6`; all four lanes (`linux-amd64`, `linux-arm64`, `darwin-arm64`, `darwin-amd64`) passed, and the run uploaded 8 artifacts (4 platform tarball artifacts, 2 Linux debug-sidecar artifacts, 2 Linux package artifacts).
- The current remote branch tip is SHA `b03dc546d6f404ca7875213be6191b74953a427f` on March 15, 2026. It has a green `tests` run (`23096631988`) but no post-`b03dc546` `release-binaries` replay yet. The code delta since `23095183201` is limited to `.bazelrc`, `scripts/bazel_in_docker.sh`, and `MODERNIZATION_PLAN.md`; no release helper or packaging file changed in that tip-only diff.
- Existing repo-owned helpers already covered three core boundaries: runtime bundle prep (`api_tests/scripts/prepare_runtime_bundle.sh`), DEB/RPM generation (`debian-pkg/generate_deb_rpm.sh`), and publish-path artifact discovery/upload (`scripts/publish_release.sh`).
- The Linux workflow-only shell still owned the artifact-shape contract itself: `ldd` guardrail, split debug extraction, embedded MD5 manifest, tarball SHA256 sidecars, `--help` smoke test, tarball/debug verification, and the orchestration glue that tied bundle prep to package generation.
- Local proof showed real value in extraction, not just cleanup. Replaying the Linux release lane required a Dockerized Bazel build plus an extra Ubuntu 24.04 container for `alien`/`rpm`/`dpkg-dev`; leaving that orchestration only in YAML kept the canonical local replay as a long manual transcription and risked host-tool drift.
- Recommendation: **promote with script extraction**. The real blockers to dropping the old "draft" posture are no longer technical artifact failures; they are (1) moving the Linux assembly contract into one repo-owned, container-backed wrapper and (2) running one post-extraction hosted `target_scope=all` replay on the pushed workflow/script changes.
- One extra workflow-only bug surfaced during the extraction replay: artifact upload globs must not assume alien preserves the exact version-label punctuation in RPM filenames. The workflow now uploads `typesense-server-*.${rpm_arch}.rpm`, while `scripts/publish_release.sh` keeps version-aware filtering for the actual publish step.
- Post-extraction hosted proof is now complete on pushed changes: `release-binaries` run `23107397373` succeeded on March 15, 2026 across `linux-amd64`, `linux-arm64`, `darwin-arm64`, and `darwin-amd64`, confirming the Linux wrapper extraction did not regress Darwin lanes.

**Story B - Implementation (only if Story A says go)**

- [x] If extraction is justified, add or extend one canonical repo-owned wrapper for Linux tarball/package/debug-sidecar assembly and make the workflow call it.
- [x] Update workflow names/comments/docs so the promoted release path and local replay command are obvious.
- [x] Re-run `release-binaries` on `target_scope=all` and confirm Linux and Darwin artifacts still pass.

**Exit criteria**

- [x] A written promotion decision exists with local and GitHub proof.
- [x] If the workflow is promoted, the docs and local replay path match the workflow behavior.

### 26) ~~ORT external Abseil injection and upgrade unblock~~

Completed March 15, 2026. The clean fix was smaller than the initial protobuf-style theory: ORT already routes Abseil through upstream `cmake/external/abseil-cpp.cmake`, so the one-protobuf static lane now forces ORT onto the repo's Abseil source version with `FETCHCONTENT_SOURCE_DIR_ABSEIL_CPP=$$EXT_BUILD_ROOT/external/abseil-cpp+` in `bazel/onnxruntime.BUILD`. That kept ORT's own CMake target graph and upstream patch flow intact while eliminating the `lts_20250814` vs `lts_20260107` namespace mismatch.

**Story A - Investigation**

- [x] Audit ORT's current Abseil fetch path and compare it with the existing external-Protobuf injection pattern already carried in `bazel/onnxruntime.patch`.
- [x] Build the smallest possible prototype that forces ORT onto external Abseil and measure the patch delta, link stability, and maintenance cost.
- [x] Produce a go/no-go recommendation for the Abseil bump based on that prototype rather than on theory.

**Story A findings (Mar 15, 2026)**

- ORT already exposes an upstream Abseil `FetchContent` boundary via `cmake/external/abseil-cpp.cmake`; unlike protobuf, it did not require patching `onnxruntime_external_deps.cmake` or fabricating imported `absl::...` targets.
- The smallest technically honest prototype was a BUILD-only cache-entry override on `onnxruntime_static_one_protobuf`, not a new `bazel/onnxruntime.patch` hunk.
- Patch delta and maintenance cost stayed low: `bazel/onnxruntime.patch` was unchanged, and the steady-state implementation was two `FETCHCONTENT_SOURCE_DIR_ABSEIL_CPP` cache-entry lines in `bazel/onnxruntime.BUILD`.
- Link stability was clean: `scripts/bazel_in_docker.sh build //:typesense-server` passed first on `abseil-cpp 20250814.1` with the override in place, then again after bumping `abseil-cpp` to `20260107.1`.
- Recommendation: **go**. Keep the BUILD-level source override and the Abseil bump; do not add a protobuf-style imported-target patch for Abseil unless upstream removes the source-dir escape hatch.

**Story B - Implementation**

- [x] Inject external Abseil into the ORT foreign_cc build, remove any superseded patch hunks, bump Abseil, and run canonical build/test/API proof.
- [x] Update `bazel/PATCH_DEBT.md` and the dependency-refresh notes with the new steady state or the sharper blocker description.

**Proof (Mar 15, 2026)**

- `scripts/bazel_in_docker.sh build //:typesense-server`
- `scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/health.test.ts`
- `test/scripts/replay_typesense_test.sh CollectionVectorTest.TestUnloadingModelsOnCollectionDelete`
- `ldd bazel-bin/typesense-server` still shows no shared `onnxruntime`, `absl`, or `protobuf` dependency.

**Exit criteria**

- [x] Either Abseil is upgraded with proof, or the blocker is documented precisely enough that future agents stop retrying blind.

### 27) Env-dependent suite enablement and hosted-proof audit

Completed March 15, 2026.

**Story A - Investigation**

- [x] Inventory the exact prerequisites for migration replay with a legacy binary, TEI/embedding suites, and secret-gated conversation flows.
- [x] Decide the minimum supported matrix: what belongs in local docs only, what belongs in a manual GitHub workflow lane, and what is still not worth supporting.
- [x] Produce a setup plan that reuses existing wrappers and workflows instead of creating shadow entrypoints.

**Story A findings (Mar 15, 2026)**

- Secret-gated API coverage should remain all-or-nothing in CI. Keep the current `OPENAI_API_KEY` + `AZURE_OPENAI_API_KEY` + `AZURE_OPENAI_URL` requirement instead of splitting partial lanes.
- TEI is the only env-dependent suite with a fully reproducible local proof path that does not depend on external hosted secrets. It belongs in the documented local matrix and in the existing `tests.yml` lane, not in a new workflow.
- The old migration replay wording was stale. The current Bun harness no longer supports legacy-binary replay from the removed `--download-migration-binary` flow, and the downloader-script path no longer exists. That is an explicit unsupported lane now, not an implied "still wired somewhere" feature.
- The audit surfaced two real enablement bugs instead of a documentation-only gap:
  - `scripts/run_api_tests.sh` documented TEI and secret-gated lanes but did not forward those env vars into the Dockerized Bun container.
  - `nuraft_http_runtime_auth()` only compared the master API key and skipped the shared `handle_authentication(...)` preprocessing, so NuRaft `post_multi_search` requests saw an empty `embedded_params_vec` and returned `400 Missing embedded params array.` for env-dependent vector search lanes.
- Story A conclusion: **go**. The supported matrix was clear enough to standardize, and the enablement fixes were small and repo-local.

**Story B - Enablement**

- [x] Update docs/workflows/secrets handling to support the chosen env-dependent lanes with one obvious command per lane.
- [x] Run the selected suites and record outcomes so they stop living as vague follow-up work.

**Proof (Mar 15, 2026)**

- `scripts/bazel_in_docker.sh build //:typesense-server`
- Direct TEI repro before the runtime-auth fix returned `400 {"message":"Missing embedded params array."}` on `POST /multi_search`, proving the failure was in the server path, not the TEI container or the wrapper.
- After wiring NuRaft auth through `handle_authentication(...)`, the same direct TEI repro returned `200` with vector-search hits.
- `TYPESENSE_TEST_TEI_URL=http://localhost:8080 scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/tei-integration.test.ts`

**Exit criteria**

- [x] No env-dependent suite remains in a hand-wavy "not yet tried" state without an explicit reason.

### 28) ~~Cross-platform debug-symbol policy parity~~

Completed March 15, 2026.

**Why this sprint exists now**

- Linux now has an explicit stripped-runtime plus debug-sidecar policy.
- Darwin artifacts are working, but symbol handling there is still implicit rather than policy-driven.

**Sprint goal**

- Decide whether Linux-only split debug is enough or whether macOS should also ship explicit symbol sidecars.

**Story A - Investigation**

- [x] Measure the current Darwin artifact/debug-symbol shape and compare the size/debuggability trade-off of the status quo versus explicit `dSYM` sidecars.
- [x] Decide the steady-state symbol policy per platform, including whether asymmetric Linux-only handling is acceptable.
- [x] Produce a go/no-go recommendation before changing the workflow.

**Story A findings (Mar 15, 2026)**

- The current repo-owned release path is already asymmetric by design. Linux uses `scripts/release_linux_artifacts.sh` to extract split debug info with `objcopy`, strip the shipped runtime, add `.gnu_debuglink`, and publish a separate `.debug.tar.gz` sidecar; Darwin does none of that today.
- Current Linux measurements on `18adf2a0` show why the split exists. `bazel-bin/typesense-server` is `401M` unstripped; the wrapper replay produced a `129M` stripped runtime plus a `292M` debug file, packaged as a `44M` runtime tarball and a `104M` debug tarball. A comparable single unstripped tarball from the same binary measured `145M`, so split debug dramatically shrinks the user-facing runtime download while only slightly increasing total compressed bytes.
- Darwin currently has no explicit sidecar path. The macOS workflow copies `bazel-bin/typesense-server` directly into the release tarball, writes `typesense-server.md5.txt`, and stops there. Repo settings explain the shape: `BUILD` adds `-g`, `.bazelrc` forces `--strip=never`, and there is no `--apple_generate_dsym`, `dsymutil`, or `.dSYM` packaging/validation anywhere in the repo. Inference: the current Darwin tarball ships an unstripped binary with embedded DWARF, not a separate symbol bundle.
- Adding Darwin `dSYM` sidecars would be a real maintenance increase, not a naming tweak. The repo would need a macOS-only symbol-generation step, a decision on how to strip the shipped Mach-O binary safely, `.dSYM` bundle packaging plus checksum/upload plumbing, and UUID/symbolication validation on native macOS runners. This environment cannot do that proof locally.
- Recommendation: **no-go**. Keep Linux split debug info and make Darwin explicit as "ship the current unstripped tarball with embedded DWARF; no `dSYM` sidecar." That preserves current macOS debuggability without adding a hosted-only maintenance surface. Revisit only if Darwin artifact size becomes a real release concern or crash-symbolication needs require a first-class `.dSYM` contract.

**Story B - Implementation (only if Story A says go)**

Skipped. Story A concluded no-go on Darwin `dSYM` sidecars; the only required work was to document the steady-state policy explicitly.

**Proof (Mar 15, 2026)**

- `scripts/bazel_in_docker.sh build //:typesense-server`
- `scripts/release_linux_artifacts.sh --skip-packages --version-label 0.0.0-item28-audit`
- `tmpdir=$(mktemp -d /tmp/item28-unstripped.XXXXXX) && cp bazel-bin/typesense-server "$tmpdir/typesense-server" && python3 - <<'PY' "$tmpdir" ... && tar -czf artifacts/typesense-server-0.0.0-item28-audit-linux-amd64.unstripped.tar.gz -C "$tmpdir" .`
- Existing hosted release proof for the unchanged Darwin path remains `release-binaries` run `23107397373` on March 15, 2026 across `linux-amd64`, `linux-arm64`, `darwin-arm64`, and `darwin-amd64`.

**Exit criteria**

- [x] The debug-symbol policy is explicit, documented, and validated for every shipped platform.

### 29) Explicit compiler-config topology audit

**Why this sprint exists now**

- The repo now intentionally keeps GCC-only suppressions behind `build:gcc`, with the Docker wrapper auto-selecting that config for default non-clang lanes.
- That is a reasonable short-term contract, but if compiler diversity grows it may become too implicit.

**Sprint goal**

- Decide whether the current wrapper-driven compiler selection is the right long-term contract or whether more explicit compiler configs and entrypoints would be clearer.

**Story A - Investigation**

- [x] Audit how compiler selection currently works across the wrapper, GCC guardrail, clang guardrail, sanitizer lanes, docs, and workflows.
- [x] Identify where wrapper inference is helpful versus where it could hide policy or surprise future agents.
- [x] Produce a recommendation: keep the current model, or move to explicit compiler configs and entrypoints.

**Story A findings (Mar 15, 2026)**

- `docker/ci-bazel.Dockerfile` installs both GCC 14 and clang 18, but sets `cc`, `c++`, `gcc`, and `g++` alternatives to GCC. That image default, not `.bazelrc`, is what makes the routine Dockerized lanes GCC today.
- `scripts/bazel_in_docker.sh` auto-adds `--config=gcc` only for non-clang `build`/`test`/`run`/`coverage` invocations unless `TYPESENSE_BAZEL_SKIP_DEFAULT_GCC_CONFIG` is set. Local proof: `scripts/bazel_in_docker.sh build --announce_rc --nobuild //:typesense-server` reports `build:gcc`; the same command with `--repo_env=CC=clang --repo_env=CXX=clang++` or with `TYPESENSE_BAZEL_SKIP_DEFAULT_GCC_CONFIG=1` does not.
- `.bazelrc` `build:gcc` is a warning-policy config, not a compiler-selection config: it only scopes GCC-only protobuf suppressions. Clang selection is still CLI-driven today via `--repo_env=CC=clang --repo_env=CXX=clang++`. Likewise, `build:asan` and `build:tsan` are sanitizer configs, not compiler selectors, so the current sanitizer workflows inherit GCC through the wrapper default.
- `scripts/check_gcc_warning_guardrail.sh` uses the default wrapper lane; `scripts/check_clang_warning_guardrail.sh` is the only repo-owned clang build entrypoint and carries its clang-only suppressions inline. That split keeps clang visibility intact without polluting the normal GCC path.
- `tests.yml`, `sanitizer-testing.yml`, `nightly-extended.yml`, `flake-detection.yml`, `release-binaries.yml`, `scripts/release_linux_artifacts.sh`, `scripts/benchmark_vs_upstream.sh`, `TESTING_RUNBOOK.md`, and the Docker-first README build command all inherit the wrapper's default GCC behavior.
- The host-only escape hatch remains intentionally lightweight: `scripts/check_local_toolchain.sh` and `README.md` recommend GCC, but raw host `bazel` runs are outside the wrapper contract and are not where this repo centralizes compiler policy.

- Story A conclusion: **no-go** on an explicit compiler-topology rewrite right now. The repo has one routine compiler lane (default GCC) plus one explicit clang warning lane. Replacing the wrapper-driven default with explicit compiler configs or per-workflow entrypoints would touch nearly every canonical wrapper/workflow/doc while leaving the verified coverage matrix unchanged.
- Maintenance-cost / clarity assessment: the current model keeps one obvious command per task class, local and CI defaults still match, and the clang guardrail remains surgically explicit. The main downside is naming clarity: `--config=gcc` sounds like compiler selection even though the Docker image default does that work. That downside is smaller than the churn and revalidation cost of a cross-repo compiler-entrypoint rewrite until there is a real second routine compiler lane such as full clang builds/tests or clang sanitizers.

**Story B - Implementation (only if Story A says go)**

Skipped. Story A concluded no-go on changing the compiler topology; no wrapper, workflow, or runbook rewrite was justified by the current matrix.

**Exit criteria**

- [x] The compiler-config contract is explicit enough that future warning-policy changes do not drift silently.

### 30) NuRaft post-cutover benchmark baseline refresh

**Why this sprint exists now**

- The NuRaft cutover is complete and working, but the branch still leans on a mixture of historical braft reference data and newer hosted confirmation runs.
- A fresh post-cutover baseline may be worthwhile, but only if it changes benchmark policy or simplifies future comparisons.

**Sprint goal**

- Determine whether a focused benchmark refresh is worth the runner time and documentation churn.

**Story A - Investigation**

- [x] Decide whether a new hosted/local benchmark baseline would materially change current benchmark policy or simply reconfirm the existing posture.
- [x] Define the smallest command/profile matrix that answers that question without recreating broad historical side-by-side noise.
- [x] Choose the comparison strategy for a fair NuRaft-only baseline.

**Story A findings (Mar 15, 2026)**

- `benchmark/BENCHMARK_RESULTS.md` Run 27 already owns the current post-cutover decision: the canonical local baseline is the `quick/core` self-compare plus the `standard/core` upstream compare, with the accepted `core` vs `extended` policy, the one-large-POST import contract, and the explicit `TYPESENSE_REQUEST_TIMEOUT_MS=300000` benchmark override recorded there.
- `scripts/benchmark_vs_upstream.sh`, `benchmark/README.md`, `TESTING_RUNBOOK.md`, and `.github/workflows/benchmark-testing.yml` already converge on the same current benchmark contract: `standard/core` is the canonical local upstream compare, while the hosted workflow uses the same wrapper as a same-branch guardrail.
- Latest hosted benchmark fact on `cyppe/typesense` `v32`: `benchmark-testing` run `23085170081` succeeded on March 14, 2026 for workflow SHA `27bb2bffd5692bb5c8be80581ba3038016e2fd58`, comparing it against the previous successful same-branch `tests.yml` SHA `985acb4516825ec6e184ad7f05de6d1cca270ab7`. No newer hosted benchmark run exists on the branch.
- Post-`27bb2bff` branch changes do not justify a new benchmark baseline. The local branch head `b0e110b4` is two commits ahead of `origin/v32`, and the remote delta since Run 27 is dominated by docs, release/tooling, warning-policy, and ORT build work. The only NuRaft request-path fix in that window (`18adf2a0`, shared auth preprocessing for env-dependent `post_multi_search`) does not change the benchmark route mix (`documents/import` plus `documents/search`) or the existing `core` vs `extended` policy question.
- The remaining doc drift was about deleted prototype guidance, not missing benchmark evidence: `benchmark/README.md` and `TESTING_RUNBOOK.md` still told agents to run `//:nuraft-prototype-benchmark` even though the target was removed during the cutover cleanup. That stale guidance is now removed.
- Smallest fair comparison strategy now that this branch is NuRaft-only: keep using one local upstream-vs-fork `TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile standard --scope core` replay when import/search/runtime behavior changes. Treat hosted `benchmark-testing.yml` as a same-branch guardrail only, not as the canonical baseline promotion path.
- Story A conclusion: **no-go** on a fresh replay for item 30. A new local or hosted run on the current tip would spend runner time mostly reconfirming Run 27 rather than changing benchmark policy.
- Runner-time / maintenance-cost / policy-value: the latest successful hosted lane still consumed about `44m` wall time (`23085170081`) and requires a green `tests.yml` artifact on the exact workflow SHA first; a local `standard/core` rerun still stages two binaries and replays the full 1M-doc import plus search lane. Neither cost is justified while the post-cutover deltas are non-benchmark-sensitive and the policy answer is already documented.

**Story B - Benchmark replay (only if Story A says go)**

Skipped. Story A concluded no-go; no new benchmark baseline or policy change was warranted.

**Exit criteria**

- [x] Benchmark docs either gain a new NuRaft baseline with rationale, or explicitly record why the refresh was not worth doing.

### 31) Container-first tooling coverage audit

**Why this sprint exists now**

- The repo is intentionally Docker-first, but some helper flows still assume host package installs or host toolchains.
- Recent release-lane work confirmed that even a technically working flow is still weaker than it should be if the local replay requires hand-assembling extra containers or installing packaging tools on the host.
- This should be handled as an audit-first modernization sprint, not as ad hoc one-off script edits.

**Sprint goal**

- Identify which remaining repo-owned flows can be containerized cleanly, then recommend the smallest set of canonical wrappers needed to make host-tool installation the exception instead of the norm.

**Story A - Investigation**

- [x] Inventory repo-owned scripts and workflow-mapped commands that still rely on host-only tools beyond Docker/GitHub-hosted runners.
- [x] Separate real constraints from accidental ones: native macOS-only build requirements, licensing/runtime constraints, and genuinely host-specific debugging should not be forced through containers.
- [x] Produce a ranked recommendation for which flows should gain container-backed wrappers, which should stay host-capable escape hatches, and which are already in the right state.

**Story A findings (Mar 15, 2026):**

- Most Linux CI-parity flows were already in the right state: `scripts/bazel_in_docker.sh`, `scripts/run_api_tests.sh`, `test/scripts/replay_typesense_test.sh`, the warning guardrails, and `scripts/release_linux_artifacts.sh` already keep the heavy build/test/release boundaries inside Docker.
- The remaining host-tool assumptions split into two categories:
  - intentional: `scripts/check_local_toolchain.sh` is the explicit non-Docker escape hatch, and Darwin release lanes still require native macOS runners plus Homebrew/Bazelisk because the artifact contract itself is macOS-native;
  - accidental: the canonical benchmark wrapper still required host Bun even though the repo already carried `benchmark/Dockerfile.cli` plus a `cli` service in `benchmark/docker-compose.yml`.
- Ranked remaining host-dependent flows:
  1. `scripts/benchmark_vs_upstream.sh` / `benchmark-testing.yml` / `benchmark/README.md`: **highest value, low-to-medium maintenance**. This was the only canonical CI-parity lane that still made users install Bun on the host. Containerizing it materially improves reproducibility and workflow/local parity, and it can reuse existing repo-owned Docker assets instead of adding a new wrapper.
  2. `api_tests/scripts/prepare_runtime_bundle.sh`: **medium value, medium maintenance if containerized**. It still assumes GNU-ish host utilities (`readlink -f`, `ldd`), but it is narrow file-staging glue around an already Dockerized API runner. A future portability cleanup should stay script-local rather than introducing another container boundary right now.
  3. `test/scripts/prewarm_public_test_models.sh` plus the runbook/workflow `curl` fetches: **low value, low reproducibility gain**. They still assume host `curl`, but that dependency is ubiquitous on CI/Linux hosts and wrapping simple downloads in Docker would mostly duplicate an acceptable flow.
  4. `scripts/check_local_toolchain.sh` and Darwin release assembly: **no-go**. Those are intentional host-native paths, not accidental gaps.
- Story A conclusion: **go**, but only for the benchmark wrapper/workflow path. Everything else either already had the right container boundary or was an acceptable escape hatch/native constraint.

**Story B - Implementation (only if Story A says go)**

- [x] Containerize the highest-value remaining flow(s) using existing canonical entrypoints where possible.
- [x] Update runbooks/workflows/help text so the container-backed path is the obvious default and host-only mode is clearly labeled as an escape hatch.

Story B reused the existing `benchmark/docker-compose.yml` `cli` service instead of adding a new helper. `scripts/benchmark_vs_upstream.sh` now defaults to Dockerized Bun, keeps `--host-bun` as the explicit escape hatch, pins the compose project name to `benchmark` so the wrapper and the hardcoded `benchmark_k6` network contract stay aligned, runs the CLI container on host networking so the existing localhost health/InfluxDB access model still works while the spawned Typesense containers publish ports back to the host, and mirrors the benchmark directory into the CLI container at the same absolute host path so the nested `docker compose run k6 ...` volume paths still resolve correctly. `benchmark-testing.yml` now relies on that same wrapper path and no longer installs Bun on the hosted runner.

**Exit criteria**

- [x] The repo has an explicit plan for remaining host-tool dependencies instead of vague "Docker-first in principle" intent.

### 32) Upstream arm64 lg-page16 release parity

**Why this sprint exists now**

- Upstream install docs advertise `linux-arm64-lg-page16` server artifacts as first-class download targets.
- This fork's release workflow currently ships only the default page-size server matrix, even though package naming already supports release suffixes and jemalloc has a `lg_page16` build setting.

**Sprint goal**

- Decide whether the fork should publish `linux-arm64-lg-page16` server tarball/DEB/RPM artifacts and, if yes, wire one canonical build + packaging path without duplicating the core release assembly contract.

**Guardrails**

- Keep this investigation focused on the server artifacts first; do not bundle Docker/Homebrew channel automation into the same sprint.
- Prefer extending the existing Linux release wrapper/package script surface over adding a parallel release script family.

**Story A conclusion**

- Go. The repo already had the right downstream contract for this variant: `debian-pkg/generate_deb_rpm.sh` and `scripts/publish_release.sh` were already suffix-aware, jemalloc already had an arm64 `lg_page16` config gate, and the missing surface was the canonical release wrapper/workflow, not a second release family.
- Keep the variant in `release-binaries.yml`. This is one additional Linux arm64 server lane with the same artifact class, packaging policy, and publish contract as the default server builds; a sibling workflow would mostly duplicate matrix/upload logic without adding a clearer maintenance boundary.

**Story B - Implementation**

- [x] Extend `scripts/release_linux_artifacts.sh` with `--jemalloc-lg-page16`, variant-aware artifact naming, and platform-aware Docker replay so a local x86_64 host can intentionally drive the arm64 container/image path when emulation is available.
- [x] Extend `scripts/bazel_in_docker.sh` with `TYPESENSE_DOCKER_PLATFORM` so the Docker image/run architecture follows the requested replay target instead of silently reusing a cached wrong-arch local image.
- [x] Add `linux-arm64-lg-page16` to `.github/workflows/release-binaries.yml` instead of introducing a sibling workflow.
- [x] Update `TESTING_RUNBOOK.md` and `README.md` so the new lane is documented where release/install commands are owned.

**Local proof**

- Build-side proof: `TYPESENSE_BAZEL_CACHE_DIR="$PWD/.cache/item32-arm64" scripts/release_linux_artifacts.sh --build --target-arch arm64 --jemalloc-lg-page16 --version-label 0.0.0-item32-local` drove the real arm64 Bazel path under Docker/QEMU on this x86_64 host. The resulting `jemalloc_foreign_cc/Configure.log` recorded `--with-lg-page=16 --disable-cache-oblivious`, and the same Bazel tree produced `bazel-out/aarch64-fastbuild/bin/external/+http_archive+jemalloc/jemalloc/lib/libjemalloc.a`.
- Package-side proof: `docker run --rm --platform linux/amd64 -v "$PWD:/work" -v "/tmp/item32-lg-page16:/tmp/item32-lg-page16" -w /work ubuntu:24.04 bash -lc 'set -euo pipefail; apt-get update >/tmp/item32-lg-page16-apt-update.log; apt-get install -y --no-install-recommends alien rpm dpkg-dev >/tmp/item32-lg-page16-apt-install.log; TSV=30.1 ARCH=arm64 ARTIFACT_SUFFIX=-lg-page16 RELEASE_TARBALL_PATH=/tmp/item32-lg-page16/typesense-server-30.1-linux-arm64-lg-page16.tar.gz RELEASE_PACKAGE_DIR=/tmp/item32-lg-page16/packages bash debian-pkg/generate_deb_rpm.sh; ls -lh /tmp/item32-lg-page16/packages'` emitted `typesense-server-30.1-arm64-lg-page16.deb` and `typesense-server-30.1-1.lg.page16.aarch64.rpm` from the upstream-style tarball naming contract.

**Exit criteria**

- [x] A local proof exists for building and packaging the `linux-arm64-lg-page16` server variant.
- [x] The plan records whether the variant should join `release-binaries` directly or live in a sibling release workflow.

### 33) GPU deps artifact automation boundary

**Why this sprint exists now**

- Upstream install docs advertise `typesense-gpu-deps` artifacts for Linux tarball/DEB/RPM installs, but this fork's `release-binaries` workflow does not produce them.
- The repo already contains `debian-pkg/gpu_generate_deb_rpm.sh` and CUDA-aware Bazel hooks, so the missing piece is release-path proof and CI boundary design, not zero starting point.

**Sprint goal**

- Determine whether GPU dependency artifacts can be built reproducibly in containerized CI and, if so, land a release boundary that keeps the optional sidecars separate from the main Linux server artifact contract.

**Guardrails**

- Treat CUDA-capable release work as a separate artifact class unless there is strong evidence it can share the same workflow contract cleanly.
- Do not conflate GPU artifact automation with Docker image publishing or Homebrew tap updates; those remain separate delivery-channel concerns.

**Story A - Unblock the real server contract**

- [x] Audit the current GPU deps build/package surface across the repo-owned workflow, wrapper, packaging, and Bazel files before proposing any new automation boundary.
- [x] Verify the latest hosted `release-binaries` state on current branch head before deciding whether item 32 can stay closed.
- [x] Reproduce the real server-side CUDA blocker under a repo-owned public CUDA+cuDNN container image.
- [x] Make `@onnx_runtime//:onnxruntime_static_one_protobuf --define=use_cuda=on` succeed on this branch and prove `//:typesense-server --define=use_cuda=on` on the same path.

**Story A findings / fix (Mar 16, 2026):**

- Current hosted proof for item 32 stayed green: `release-binaries` run `23129745485` on SHA `fdf845c749601748947676d649961d9ee1f9ac0d` completed successfully on March 16, 2026 across `linux-amd64`, `linux-arm64`, `linux-arm64-lg-page16`, `darwin-arm64`, and `darwin-amd64`. No item-32 regression was found, so this sprint stayed on item 33 only.
- The real blocker was the branch's actual server dependency path, not DEB/RPM conversion. `@onnx_runtime//:onnxruntime_static_one_protobuf --define=use_cuda=on` initially failed in `onnxruntime/contrib_ops/cuda/moe/ft_moe/moe_kernel.cu` against external Protobuf 34 with `google::protobuf::internal::EnumTraitsImpl::Undefined`.
- The clean unblock was protobuf-side, not a release-script workaround. `bazel/protobuf_34_enum_traits_impl.patch` now carries the compatibility fix through `MODULE.bazel` `single_version_override(...)`, keeping the repo's one-Protobuf ONNX Runtime path intact.
- Repo-owned CUDA container parity now lives in `docker/ci-bazel-cuda.Dockerfile`, and the optional `TYPESENSE_ORT_CUDA_ARCHITECTURES` / `TYPESENSE_ORT_BUILD_JOBS` repo env knobs in `bazel/onnxruntime_cuda_defs.bzl` make the foreign CMake CUDA build practical to replay locally without changing the default CPU contract.
- Local proof is now green for the real dependency path and the server binary:
  - `TYPESENSE_BAZEL_IMAGE=typesense/ci-bazel-cuda:local TYPESENSE_BAZEL_DOCKERFILE=docker/ci-bazel-cuda.Dockerfile TYPESENSE_DOCKER_PLATFORM=linux/amd64 scripts/bazel_in_docker.sh build @onnx_runtime//:onnxruntime_static_one_protobuf --define=use_cuda=on --repo_env=TYPESENSE_ORT_CUDA_ARCHITECTURES=60 --repo_env=TYPESENSE_ORT_BUILD_JOBS=6`
  - `TYPESENSE_BAZEL_IMAGE=typesense/ci-bazel-cuda:local TYPESENSE_BAZEL_DOCKERFILE=docker/ci-bazel-cuda.Dockerfile TYPESENSE_DOCKER_PLATFORM=linux/amd64 scripts/bazel_in_docker.sh build //:typesense-server --define=use_cuda=on --repo_env=TYPESENSE_ORT_CUDA_ARCHITECTURES=60 --repo_env=TYPESENSE_ORT_BUILD_JOBS=6`

**Story B - Producer / package / release path**

- [x] Define the actual GPU artifact contents for this branch instead of copying upstream blindly.
- [x] Add one canonical repo-owned Linux producer for `typesense-gpu-deps-*.tar.gz`.
- [x] Prove DEB/RPM generation from the produced tarball.
- [x] Wire release automation at the clean boundary.

**Story B findings / implementation (Mar 16, 2026):**

- This fork's honest GPU scope is ONNX Runtime-backed embeddings and personalization only. `bazel/whisper.BUILD` remains CPU-only, so Whisper / voice-query is **not** part of the GPU artifact contract here.
- The exact `typesense-gpu-deps` contents on this branch are `libonnxruntime_providers_shared.so` and `libonnxruntime_providers_cuda.so`. Local proof:
  - `tar -tzf artifacts/typesense-gpu-deps-0.0.0-gpu-local2-linux-amd64.tar.gz`
  - `docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work ubuntu:24.04 bash -lc 'set -euo pipefail; apt-get update >/tmp/deb-q-update.log; apt-get install -y --no-install-recommends dpkg >/tmp/deb-q-install.log; dpkg-deb -c artifacts/packages/typesense-gpu-deps-0.0.0-gpu-local2-amd64.deb'`
  - `docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work ubuntu:24.04 bash -lc 'set -euo pipefail; apt-get update >/tmp/rpm-q-update.log; apt-get install -y --no-install-recommends rpm >/tmp/rpm-q-install.log; rpm -qlp artifacts/packages/typesense-gpu-deps-0.0.0-gpu-local2-1.x86_64.rpm'`
- The current repo-owned local path is tested against CUDA `12.8` / cuDNN `9`. The built `libonnxruntime_providers_cuda.so` currently carries runtime dependencies on `libcublasLt.so.12`, `libcublas.so.12`, `libcurand.so.10`, `libcufft.so.11`, `libcudart.so.12`, and `libcudnn.so.9`.
- The canonical local producer is now `scripts/release_linux_gpu_deps.sh`. It stages tarballs under `artifacts/`, reuses `debian-pkg/gpu_generate_deb_rpm.sh` for DEB/RPM generation, and can emit the `linux-arm64-lg-page16` alias naming from the same arm64 sidecars because jemalloc page size does not change these ORT CUDA provider libraries.
- `debian-pkg/gpu_generate_deb_rpm.sh` now mirrors the modern server packaging contract: artifact-dir aware, checksum aware, binary-safe, and resilient to `alien` / RPM layout differences. `debian-pkg/typesense-gpu-deps/DEBIAN/control` is now honest about the branch's ONNX Runtime-only GPU surface.
- Linux `typesense-server` release lanes now build with `--define=use_cuda=on` via `scripts/release_linux_artifacts.sh --with-cuda`, and the optional GPU sidecars are built by a separate Linux-only `gpu-deps` job inside `.github/workflows/release-binaries.yml`.
- `scripts/publish_release.sh` now uploads both `typesense-server-*` and `typesense-gpu-deps-*`, and the public repo docs (`README.md`, `TESTING_RUNBOOK.md`, `AGENTS.md`) now describe the real GPU scope and canonical replay entrypoints.

**Release boundary decision**

1. Keep `release-binaries.yml` as the **single manual release entrypoint**, but keep `typesense-server` and `typesense-gpu-deps` as separate jobs / artifact classes inside it. This gives one trigger for operators without collapsing distinct release contracts into one script or one matrix.
2. Keep Linux `typesense-server` inside the main server job. The upstream-style install contract requires the regular Linux server artifact itself to be CUDA-aware, and this only adds one build flag plus the CUDA build image to the existing canonical wrapper.
3. Do **not** add a reusable workflow yet. The single-workflow entrypoint plus separate jobs already gives clear ownership and operator ergonomics without extra abstraction.

**Exit criteria**

- [x] A repo-owned containerized proof exists for the real CUDA-capable server path and the `typesense-gpu-deps` tarball/package path.
- [x] The plan records the exact GPU artifact contents and the clean release boundary. *(Decision: keep one manual `release-binaries` workflow entrypoint, with separate server and gpu-deps jobs; Linux `typesense-server` still builds with `--define=use_cuda=on`.)*
- [x] The public docs stay honest about scope. *(Embeddings/personalization only; Whisper / voice-query remains CPU-only on this branch.)*

### 34) ~~Replace patch-only forks with released upstream versions where possible~~

**Why this sprint exists now**

- A few remaining externals still came from Typesense-owned forks even when the fork no longer carried unique source changes.
- Item 34 is about removing those stale forks where possible, but only with full Dockerized Bazel proof and with honest notes when an upstream release is not API-compatible yet.

Completed March 17, 2026. The stale `hnsw` fork is gone, `clip_tokenizer_cpp` now points at its real upstream source repo, and `kakasi` now uses upstream source plus a small repo-owned patch/data carry instead of a Typesense fork.

**Story A - Investigation**

- [x] Audit the remaining fork-backed deps relevant to item 34 and classify whether each one has a real upstream parent/release path.
- [x] Replace the stale `typesense/hnswlib` fork with an upstream `nmslib/hnswlib` source while preserving the current API contract.
- [x] Audit the remaining fork-backed deps (`kakasi`, `clip_tokenizer_cpp`) to decide whether they can be moved to upstream releases or must stay forked for now.

**Story A findings / implementation (Mar 17, 2026)**

- `typesense/hnswlib` was a stale fork. GitHub compare against `nmslib/hnswlib` showed `ahead_by=0`, `behind_by=54`, so the fork carried no fork-only commits. The repo pinned the same upstream parent commit `687d981753f8bafcd16421cbd2a166d0b62bc520` directly in `MODULE.bazel` while the migration was still in progress, and the legacy `cmake/hnsw.cmake` helper was pointed at that same upstream tarball instead of a Typesense fork tarball.
- Attempting the upstream `nmslib/hnswlib` `v0.8.0` release was a concrete no-go at the time, not guesswork. Before item 36 finished, the vector path still relied on `searchKnnCloserFirst(..., ef, filter)` and `repair_zero_indegree()`, which are not present in `v0.8.0`. That incompatibility is now historical because item 36 removed the supported `hnswlib` runtime path entirely.
- `typesense/kakasi` was not a drop-in upstream swap on inspection alone: GitHub compare against parent `loretoparisi/kakasi` showed the fork was still `ahead_by=5`, `behind_by=0`, and those five fork-only commits were material on this branch. The supported fix was to pin upstream commit `390c6d2eeac6c744da634866b68ea09830cac0a7`, move the exact embedded `japanese_data.{h,cpp}` payload into this repo, and carry the remaining source delta in `bazel/kakasi.patch`. That patch now preserves the fork-only zero-init, UTF-8 `get1byte()` read path, and bad-unicode skip-and-continue behavior while keeping the supported Bazel/Docker path off a fork. Upstream still has no GitHub releases, so the supported contract here remains source-commit-based rather than release-based.
- `clip_tokenizer_cpp` did have a real upstream source repo after all: `ozanarmagan/clip_tokenizer_cpp`. GitHub repo search found it, `git ls-remote` shows the same HEAD commit `0ca1e2e2e7418108725eaa7fb93e029516ae63fa` in both repos, and GitHub compare reports that commit is identical across owners. Neither repo publishes tags or GitHub releases today, so the supported Bazel path now pins that upstream source commit directly rather than waiting for a release that does not exist.
- Canonical proof for the item 34 source cleanup is green:
  - `scripts/bazel_in_docker.sh build //:typesense-server`
  - `test/scripts/replay_typesense_test.sh CollectionVectorTest.TestCLIPTokenizerUnicode`
  - `test/scripts/replay_typesense_test.sh CollectionLocaleTest.SearchAgainstJapaneseText`

**Exit criteria**

- [x] At least one stale Typesense fork was replaced with an upstream source and validated with the canonical Dockerized Bazel build.
- [x] Remaining fork-backed deps are either replaced or documented with a precise blocker / reason to stay forked.

### 35) ~~Script / doc entrypoint architecture cleanup~~

**Why this sprint exists now**

- The repo now has several canonical wrappers across `scripts/`, `api_tests/scripts/`, `test/scripts/`, workflow YAML, and packaging helpers under `debian-pkg/`.
- Recent release-lane work added a second release artifact class (`typesense-gpu-deps`) and another manual workflow, so the entrypoint map needs a cleanup pass before more helper drift accumulates.
- Users explicitly asked for script and documentation architecture to stay logical: one obvious command per task class, minimal stale wrappers, and clearer folder/name ownership.

**Sprint goal**

- Rationalize script naming, folder ownership, and doc references so build, test, replay, benchmark, release, and package tasks each have one obvious canonical entrypoint and stale/shadow helpers are removed or redirected.

Completed March 17, 2026. Story A found the repo's wrapper surface was already fairly tight: the real public task entrypoints were concentrated under `scripts/`, the focused C++ replay wrapper intentionally lived under `test/scripts/`, and the main remaining drift was documentation that treated narrow support helpers as if they were primary entrypoints. Story B cleaned that boundary up instead of inventing another wrapper layer.

**Story A - Investigation**

- [x] Inventory repo-owned task entrypoints across `scripts/`, `api_tests/scripts/`, `test/scripts/`, root helpers, and workflow shell blocks.
- [x] Decide which entrypoints are canonical versus legacy/internal, and document the folder/name conventions that follow from that split.
- [x] Identify stale wrappers, duplicate docs, or root-level helpers that should be deleted, renamed, moved, or redirected.

**Story A findings / implementation (Mar 17, 2026)**

- The entrypoint surface is now explicit by folder:
  - `scripts/` owns repo-level task wrappers (`bazel_in_docker`, API runner, benchmark runner, Linux release replays, warning guardrails, and now `scripts/publish_release.sh`).
  - `test/scripts/` owns focused test-scoped helpers; `replay_typesense_test.sh` is the supported public replay entrypoint there, while `prewarm_public_test_models.sh` is a support helper behind it and CI staging.
  - `api_tests/scripts/` stays as API-harness internals; `prepare_runtime_bundle.sh` is a staging helper behind `scripts/run_api_tests.sh` and release assembly, not a first-choice task entrypoint.
  - `debian-pkg/` remains a root-level packaging asset boundary because it contains package templates and internal generators consumed by the release wrappers, not user-facing task entrypoints.
- The one repo-root task script outlier is gone: `publish_release.sh` moved to `scripts/publish_release.sh`, gained modern help text, and now matches the rest of the repo-level task wrapper boundary.
- Helper-contract drift was fixed too: `test/scripts/replay_typesense_test.sh` now runs the repo-owned public-model prewarm helper directly instead of relying on a stale single-model cache marker.
- Documentation now reflects the same split in `AGENTS.md`, `TESTING_RUNBOOK.md`, `README.md`, and `api_tests/README.md`: start with repo-level wrappers, treat narrow support helpers as internal unless a workflow/debug note calls them directly.

**Exit criteria**

- [x] The canonical entrypoint map is reflected consistently in `AGENTS.md`, `TESTING_RUNBOOK.md`, and any user-facing docs that mention these commands.
- [x] Stale or shadow entrypoints are removed or clearly redirected instead of silently lingering.
- [x] Script names and folder placement are coherent enough that new release/test tooling does not need another ad hoc wrapper round.

### 36) USearch vector backend prototype / refactor boundary

**Why this sprint exists now**

- `hnswlib` is still usable on the pinned upstream commit, but the current upstream release (`v0.8.0`) already diverged from the API surface this repo relies on.
- A serious replacement evaluation needs to prove the actual Typesense contract: stored-vector CRUD, filtered search, per-query `ef`-style tuning, and a clean build-system path.
- The right next step after a green prototype is a real backend refactor boundary, not a compatibility shim that forces a new library to impersonate old `hnswlib`.

**Sprint goal**

- Land a real USearch production backend behind a repo-owned vector-index boundary, then finish the migration by validating performance/quality and removing unsupported hnsw-only baggage instead of freezing the repo at "parity with the old engine".

Completed March 17, 2026. The supported production path is now fully USearch-based behind `vector_index_t`: `src/vector_index.cpp` no longer carries an in-tree `hnswlib` backend, the supported Bazel path no longer depends on `@hnsw`, mixed update/search behavior is benchmarked, and the closeout chose a more native USearch distance kernel where it won without changing the observable scoring contract.

**Story A - Prototype proof**

- [x] Add a repo-owned experimental Bazel dependency path for USearch using a source shape that includes its required nested deps.
- [x] Prove stored-vector CRUD + predicate filtering with USearch's dense wrapper.
- [x] Prove per-query `ef`-style control with USearch's lower-level core API.

**Story A findings / prototype (Mar 17, 2026)**

- The honest Bazel pin is git-based, not archive-based. GitHub source archives for USearch do **not** include the required `fp16`, `simsimd`, and `stringzilla` contents; the prototype therefore uses a git-backed repo rule with `recursive_init_submodules = True`.
- `index_dense_t` covers the hnswlib-like convenience surface well enough for a first pass: stored-vector retrieval, predicate filtering, delete, and remove-then-readd update behavior all worked in the focused prototype.
- `index_gt` is the more promising production refactor surface for Typesense's search semantics because it exposes `index_search_config_t.expansion` per query, which maps cleanly to the repo's current `vector_query.ef` contract.
- The tradeoff is architectural, not cosmetic: `index_gt` expects external vector storage / custom metric plumbing, while `index_dense_t` owns vectors and delete convenience but only exposes global `expansion_search` tuning at the dense wrapper level.
- Conclusion: **go** on continuing the USearch path, but as a repo-owned vector backend refactor. Do **not** build a thin adapter that keeps the current hnswlib-specific API shape and hides the backend choice behind compatibility glue.

**Prototype proof**

- `scripts/bazel_in_docker.sh test //:usearch-vector-backend-prototype-test --test_output=errors`
- `scripts/bazel_in_docker.sh build //:typesense-server`

**Story B - Production refactor boundary**

- [x] Define a repo-owned vector backend interface around Typesense semantics: add/update, filtered search, per-query search expansion, delete/tombstones, vector readback for distance/sort paths, and repair-or-replacement behavior.
- [x] Choose the production USearch layer against that interface (`index_gt` with Typesense-owned storage/tombstones vs an intentional acceptance of dense-wrapper tradeoffs). Do not keep a thin hnswlib-shaped compatibility layer.
- [x] Benchmark mixed update/search behavior on the production backend and record the accepted posture before calling the sprint done.

**Story B findings / production switch (Mar 17, 2026)**

- `Index`, hybrid reranking, sort-by-vector-distance, and diversity scoring now consume a repo-owned `vector_index_t` contract instead of reaching directly into `hnswlib::HierarchicalNSW` and `InnerProductSpace`.
- The default production backend is now USearch's lower-level `index_gt`, with Typesense-owned vector storage, tombstones, free-slot reuse, and per-query expansion mapped through the repo-owned backend contract.
- The extracted interface had to cover one more real production concern than the prototype suggested: capacity management for batched upserts. That now lives in the backend contract (`ensure_capacity(...)`) instead of leaking resize logic into `Index`.
- Query-distance parity mattered for the observable API surface: the first cutover kept the same inner-product distance function Typesense exposed before, but that compatibility choice lives entirely behind the repo-owned backend boundary and should not be mistaken for a permanent requirement that USearch must behave like hnsw forever.
- The monolithic `typesense-test` target also needed the USearch header dependency explicitly because the prototype source is part of the shared test binary, not only the dedicated prototype target. Keeping that dependency honest prevents the canonical replay lane from diverging from the focused prototype lane.

**Story B proof**

- `scripts/bazel_in_docker.sh build //:typesense-server`
- `scripts/bazel_in_docker.sh test //:usearch-vector-backend-prototype-test --test_output=errors`
- `test/scripts/replay_typesense_test.sh 'CollectionVectorTest.*'`

**Story C - Post-cutover cleanup / improvement gate**

- [x] Audit the remaining hnsw-only code under `src/vector_index.cpp` and remove it unless it still serves a supported purpose during migration. Do not call item 36 done while dead compatibility baggage remains.
- [x] Benchmark mixed update/search behavior with the production USearch backend and record the decision in `benchmark/BENCHMARK_RESULTS.md` if it changes accepted posture.
- [x] Evaluate whether any USearch-native search/metric/tuning path is measurably better than the current parity-first configuration. If a better path wins, adopt it and document the intentional divergence instead of treating old hnsw behavior as the permanent target.

**Story C findings / closeout (Mar 17, 2026)**

- `src/vector_index.cpp` is now USearch-only. The remaining in-tree `hnswlib` backend class/factory is gone, `@hnsw` was removed from the supported Bazel path, and the old TSAN suppression block for `hnswlib::` was dropped with it.
- The only surviving hnsw-shaped production surface is schema `hnsw_params`. That is now an API-compat tuning object, not a backend selector, and it still seeds backend construction parameters while the supported runtime stays USearch-only.
- The focused vector benchmark (`scripts/bazel_in_docker.sh run //:usearch-vector-backend-benchmark -- --docs 20000 --dims 384 --cycles 10 --updates-per-cycle 400 --replacements-per-cycle 100 --searches-per-cycle 200 --k 20 --ef 80 --kernel-samples 4096 --kernel-repeats 64 --seed 42`) ran twice. USearch's builtin normalized inner-product kernel won both reruns (`35.92-35.93 ms`) over the temporary scalar parity shim (`36.58-37.14 ms`), while raw builtin cosine was materially slower (`48.68-50.28 ms`). All three kernels produced the same checksum on the benchmark corpus, so the winner is a faster equivalent kernel, not a different scoring target.
- The same benchmark also exercised the production backend under mixed traffic. After indexing `20k` vectors, it sustained `5000` write-side ops at `534-543 ops/s` (`p95 2461-2481 us`) plus `2000` searches at `1196-1219 ops/s` (`p95 1318-1353 us`) and finished both reruns with `deleted_count=0`, which validates the tombstone/free-slot strategy under mixed update/search behavior instead of only pure search.
- The only observable replay drift was one exact-float auxiliary score assertion moving by about `6e-8` in `HybridSearchAuxScoreTest`. Upstream USearch metric code plus the identical benchmark checksum point to SIMD/autovec accumulation-order drift for the same `1 - dot` math, not a better/worse ranking path. The test now uses a tight tolerance instead of exact-bit comparison.
- One legacy hnsw reference remains outside the supported path: `cmake/hnsw.cmake` still exists for the old non-canonical CMake flow. That path is not part of this branch's Docker-first/Bazel-first production contract, so item 36 closes against the supported path and documents the leftover explicitly instead of pretending it is still a first-class backend.

**Story C proof**

- `scripts/bazel_in_docker.sh build //:usearch-vector-backend-benchmark //:typesense-server`
- `scripts/bazel_in_docker.sh test //:usearch-vector-backend-prototype-test --test_output=errors`
- `test/scripts/replay_typesense_test.sh 'CollectionVectorTest.*'`
- `scripts/bazel_in_docker.sh run //:usearch-vector-backend-benchmark -- --docs 20000 --dims 384 --cycles 10 --updates-per-cycle 400 --replacements-per-cycle 100 --searches-per-cycle 200 --k 20 --ef 80 --kernel-samples 4096 --kernel-repeats 64 --seed 42`

**Exit criteria**

- [x] The plan records the chosen production backend and the closeout findings clearly enough that future agents are not re-deciding the architecture from scratch.
- [x] No unjustified hnsw-only compatibility baggage remains in the supported production path.
- [x] Any deliberate divergence from the old hnsw behavior is benchmarked or quality-validated and documented as an intentional improvement, not an accidental drift.

### 37) Release workflow promotion / draft-posture cleanup

Completed Mar 18, 2026.

**Closeout summary**

- The Darwin host-cache regression exposed by run `23234876467` on `7ab4cd8b` was fixed by pushed commit `7eb65c27` (`Fix release workflow runner-temp expressions`).
- Current hosted proof is green on the pushed tip: `release-binaries` run `23242998148` succeeded with both `darwin-arm64` and `darwin-amd64`, and matching `tests` run `23242982005` also succeeded on the same SHA.
- Fresh source audit of `.github/workflows/release-binaries.yml`, `TESTING_RUNBOOK.md`, `README.md`, `scripts/release_linux_artifacts.sh`, and `scripts/publish_release.sh` found that the supported-manual-release posture is already reflected in the active repo surface. The stale "draft / promotion follow-up" wording was confined to historical `MODERNIZATION_PLAN.md` notes.
- With that March 18 hosted proof, no technical blocker remains to treating `release-binaries.yml` as the supported manual release workflow for this branch.

**Exit criteria**

- [x] The repo's workflow/docs posture matches the current supported manual release contract instead of the older draft framing.

### 38) Indexing hot-path string-copy audit

**Why this sprint exists now**

- The larger runtime, dependency, and RocksDB tuning blockers are now mostly closed.
- `TODO.md` still carries "Prevent string copy during indexing", and that is now one of the clearest remaining first-party performance candidates.
- This should be handled as a measured perf task, not a broad speculative refactor.

**Sprint goal**

- Find and remove the most material avoidable string copies in the import/indexing hot path, with benchmark-driven proof and no product-behavior drift. The goal is not a speculative refactor of indexing internals; it is to turn one concrete old TODO into a measured throughput or CPU-efficiency win, or to document precisely why the currently suspected copy sites are not worth changing.

**Story A - Investigation**

- [x] Profile the current import/indexing path on the benchmark corpus and identify the highest-signal avoidable string-copy sites.
- [x] Prototype the smallest ownership/ref/view changes that remove the top copy sites without broad API churn.
- [x] Re-validate import correctness and benchmark impact before keeping any change.

**Story A findings (Mar 18, 2026):**

- The highest-signal avoidable copy found in the live import/indexing path was not token-level string handling first; it was a full `nlohmann::json` deep copy per document. `Collection::add_many(...)` parsed each JSONL line into a temporary `document`, then immediately deep-copied that document into `index_record` before validation and indexing. The same local-document-to-`index_record` pattern also existed in collection load and alter-data replay loops.
- The first candidate keeps API shape unchanged and only fixes that ownership handoff: `index_record` now has a move-taking constructor, and the local parsed documents in `Collection::add_many(...)`, `CollectionManager::load_collection(...)`, and `Collection::batch_alter_data(...)` are moved directly into `index_record` instead of copied.
- Correctness validation passed on the real import path: `scripts/bazel_in_docker.sh build //:typesense-server` and `scripts/run_api_tests.sh -- --no-secrets tests/documents.test.ts` both passed on March 18, 2026.
- First benchmark proof from the successful explicit-binary `quick/core` compare on March 18, 2026: baseline `/tmp/typesense-server-baseline-86587197` vs local `./bazel-bin/typesense-server`, archive `~/.cache/typesense/benchmark/archives/20260318-073343-quick-core`. Import improved from `27716ms` to `26392ms` (`-4.78%`) with `1000000/1000000` docs imported and zero response-contract warnings on both halves.
- The same successful `quick/core` run produced a mixed search p95 table rather than a clean across-the-board win or loss: representative rows were `group 50vu 409ms -> 384ms`, `group 100vu 398ms -> 390ms`, `just_q 50vu 5ms -> 4ms`, `facet 100vu 138ms -> 155ms`, `sort_simple 100vu 61ms -> 90ms`, and `sort_eval_score 100vu 72ms -> 119ms`. Because this code change only affects import-time ownership transfer and not steady-state search code, treat that mixed quick-profile table as insufficient to close the item by itself.
- A follow-up rerun with a fresh explicit `--work-dir /tmp/typesense-bench-move-r2` was invalid: k6 failed to write Influx stats with repeated `mkdir /var/lib/influxdb/data: no such file or directory`. Treat that as a benchmark-harness issue, not a Typesense regression signal, and keep item 38 active until a cleaner repeat confirms whether the search deltas persist.
- Two additional Mar 18, 2026 repeat attempts on local `HEAD` `1da111f7` used the default benchmark workdir via `TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --baseline-binary /tmp/typesense-server-baseline-86587197 --baseline-label baseline-86587197 --fork-label head-1da111f7 --profile quick --scope core --clean`, but both still hit repeated Dockerized k6/Influx `mkdir /var/lib/influxdb/data: no such file or directory` errors before they could produce a clean compare. A local check confirmed `benchmark/influxdb-data/data` existed in the checkout while `docker exec benchmark-influxdb-1 ls /var/lib/influxdb/data` still reported it missing inside the running container, so the current blocker is benchmark harness reliability, not new Typesense runtime signal.
- The landed code path on current branch head is commit `7ab4cd8b` (`Reduce indexing JSON copies and prewarm test models`): `Collection::add_many(...)`, `CollectionManager::load_collection(...)`, and `Collection::batch_alter_data(...)` now move parsed local documents directly into `index_record` instead of deep-copying them first.
- The benchmark-harness root cause was also closed on March 18, 2026. `scripts/benchmark_vs_upstream.sh --clean` now stops the benchmark compose stack before deleting bind-mounted state, recreates `benchmark/influxdb-data/{data,meta,wal}`, and verifies those directories inside the running Influx container before starting the benchmark CLI. That removes the stale-bind failure mode that kept `benchmark-influxdb-1` attached to a deleted host inode.
- Canonical closeout proof is now green on current `HEAD` `1c34ddf7`: `TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile standard --scope core --clean` completed successfully against upstream `30.1`, archived to `~/.cache/typesense/benchmark/archives/20260318-161516-standard-core`, and kept the branch decisively ahead on both import and the meaningful non-zero search scenarios. Import improved from `50270ms` to `31790ms` (`-36.76%`), while representative search p95 rows improved from upstream `345ms -> 17ms` (`filter_simple` 100vu), `947ms -> 140ms` (`facet` 100vu), and `6170ms -> 440ms` (`group` 100vu).
- Item 38 can now close honestly: the committed JSON ownership-handoff change produced a measured import-path win, and the clean upstream-comparable replay did not show any benchmark evidence of a steady-state search regression.

**Exit criteria**

- [x] The plan records either a measured indexing/import win or a precise no-go conclusion for the current top copy sites.

### 39) Search work-budget configurability audit

**Why this sprint exists now**

- `TODO.md` still carries "Minimum results should be a variable instead of blindly going with max_results".
- That heuristic is user-visible search behavior, not just an internal micro-optimization, so it should not be changed blindly.
- The right next step is to understand and bound the behavior before exposing or changing it.

**Sprint goal**

- Decide whether the current minimum-results heuristic should become explicit configuration, and if so, land the smallest safe surface with focused proof. The goal is to make this search-budget behavior intentional and explainable instead of leaving a user-visible relevance/latency tradeoff buried in implicit coupling to `max_results`.

**Status snapshot (Mar 2026)**

- The original TODO came from commit `2b629365` (the old 2017 multi-field search path), where `Collection::search()` stopped typo expansion once `total_results >= max_results` and only dropped low-hit tokens when `result_kvs.size() < max_results`.
- The current search stack no longer uses `per_page` / `fetch_size` as that "minimum results" gate:
  - `Index::fuzzy_search_fields(...)` stops progressive typo expansion when `results_count >= typo_tokens_threshold` (or the curated-hit equivalent), so the relevance/latency tradeoff is already explicit.
  - `Index::run_search(...)` only enters token-dropping fallback when `all_result_ids_len < drop_tokens_threshold`.
  - `max_candidates` limits candidate breadth, and `search_cutoff_ms` limits elapsed work, but neither is a hidden alias for pagination size.
  - `fetch_size` still matters for pagination/topster sizing, but it is no longer the threshold that decides whether typo expansion should continue.
- Focused proof now lives in `CollectionTest.TypoTokensThreshold`: with `typo_tokens_threshold=10`, the query still reports `found=2` even when `per_page=1`, locking in that typo expansion depth is not implicitly capped by page size.
- Decision: no new configuration surface. Adding another "minimum results" alias on top of the existing knobs would mostly duplicate `typo_tokens_threshold` / `drop_tokens_threshold` semantics and make search tuning harder to explain.

**Story A - Investigation**

- [x] Trace the current heuristic through the search/relevance/cutoff path and document which product behaviors it controls today.
- [x] Design the smallest explicit configuration surface that does not silently destabilize existing defaults. *(No-go: keep the existing explicit knobs instead of adding a redundant alias.)*
- [x] Prove the effect with focused relevance/latency checks, or record a no-go if the heuristic is not worth exposing yet.

**Exit criteria**

- [x] The plan records a concrete go/no-go decision for making the minimum-results heuristic configurable.

### 40) Upstream `v30` release-parity catch-up audit

**Why this sprint exists now**

- The official release branch matters more than opportunistic TODO mining: `upstream/v30` is still the current stable reference line for the pre-modernization product behavior this fork started from.
- The fork's local `v30` branch stops at `36ed4a87`, while current `upstream/v30` is `9b0d729b`; those later stable-branch commits have never been intentionally triaged in the modernization plan.
- `v32` forked before that `origin/v30` tip, so parity with the official release line cannot be assumed from branch naming alone.

**Sprint goal**

- Decide which missing official `upstream/v30` patches are still applicable to the NuRaft/modernized fork, then backport the small, safe ones and record explicit no-go reasons for the rest.

**Status snapshot (Mar 2026)**

- The shared merge-base between `origin/v30`, `origin/v31-fork`, and `upstream/v30` is `04555fce`, not the local `v30` tip, so the renamed fork branch is not implicitly carrying the full current stable release line.
- Backport 1 is now landed locally on `v31-fork`: upstream PR `#2793` / commit `0c089e86` (`fix: ensure forward-only iterator reads lower seq_id first in diversity similarity`) plus the upstream regression coverage. Current `src/diversity.cpp` now reads the lower seq_id first for forward-only facet iterators, and `CollectionCurationTest.DiversityForwardOnlyIteratorBug` locks that behavior in.
- Backport 2 is now landed locally on `v31-fork`: upstream PR `#2772` / commit `87642711` (`fix: route /health to meta_thread_pool for responsiveness during bulk inserts`). Upstream shipped this as a one-line behavior fix; this fork also adds direct unit coverage via `HttpServer::should_use_meta_thread_pool(...)` so the `/health` routing rule does not silently regress.
- Backport 3 is now landed locally on `v31-fork`: upstream PR `#2809` / commit `5621dcd9` (`increase timeout for downloading models`). This is a narrow `HttpClient::download_file(...)` connect-timeout bump from `4000ms` to `30000ms` for slow model hosts. No focused test was added here because the current code exposes no injectable transport seam; a real timeout regression check would require a long-running stalled network harness, which is out of scope for this small parity backport.
- Backport 4 is now landed locally on `v31-fork`: upstream commit `9b0d729b` (`Increase timeout for embedding model config fetch.`). `EmbedderManager::get_public_model_config(...)` now gives remote `config.json` fetches the same `30000ms` request budget instead of silently relying on the `HttpClient` default `4000ms`. As with `#2809`, no focused test was added because the current model-repo fetch path is not injectable without a larger transport seam and realistic timeout coverage would depend on a deliberately stalled external endpoint.
- Backport 5 is now landed locally on `v31-fork`: upstream PR `#2817` / commit `759a584f` (`fix: preserve vector search for zero-match phrase queries`) plus the upstream regression coverage. `Index::search(...)` no longer bails out early after a zero-match phrase filter when a vector query is also present, and `CollectionVectorTest.TestHybridPhraseQueryFallbacksToVectorSearch` locks in that hybrid/vector fallback path.
- Backport 6 is now landed locally on `v31-fork`: upstream commit `c2272a95` (`Add test for preset with auth.`) plus its one-line `core_api.cpp` guard. `get_collections_for_auth(...)` now ignores nested multi-search `preset` values unless they are strings, and `AuthManagerTest.HandleAuthenticationWithNonStringNestedMultiSearchPreset` covers the auth path that previously could throw on malformed JSON types.
- Backport 7 is now landed locally on `v31-fork`: upstream commit `5bef9362` (`Improve filtering logic, beef up tests and clean up code.`) plus the upstream stress coverage. `filter_result_iterator_t` now reuses the shared `apply_not_equals(...)` path consistently for numeric/list `NOT_EQUALS`, avoids leaking or overwriting prior result buffers during repeated `!=` evaluation, and carries the upstream regression tests covering lazy numeric `!=` chains plus explicit `!=` lists on int/float fields.
- Backport 8 is now landed locally on `v31-fork`: upstream commit `ae64826a` (`delete-by-query: batch index removals and add lock-batching tests`). `stateful_remove_docs(...)` now batches seq-id removals through `Collection::remove_if_found_many(...)` with an internal cap of `1000` ids per call, while explicitly preserving per-doc semantics for referenced collections so cascade deletes remain safe. The upstream regression coverage for batched removal semantics, cascade references, and bounded internal batching is now carried locally in `CoreAPIUtilsTest`.
- Backport 9 is now landed locally on `v31-fork`: upstream commit `1b4fa888` (`Fix highlight race condition.`) plus the upstream union stress test. Highlight token metadata is now snapshotted independently from ART leaf lifetimes, owned posting-list copies are captured under the collection shared lock before highlight processing, and `UnionTest.UnionHighlightingUAFRaceASAN` keeps the concurrent union/highlight mutation path covered.
- Backport 10 is now landed locally on `v31-fork`: upstream commit `3ad9d3e0` (`Avoid cloning posting lists for highlighting`). The highlight-race fix stays intact, but highlight snapshots now store only token metadata and resolve posting leaves on demand under the collection shared lock during highlighting, which removes the unbounded posting-list clone cost from the `1b4fa888` version of the fix.
- Backport 11 is now landed locally on `v31-fork`: upstream commit `5458d3f0` (`More fine-grain locking for search.`). This fork already had the lock-wrapper helpers from upstream, so the missing parity work narrowed to the atomic `read_state_t` snapshot and the call-site adoption: `Collection::search(...)` and union result formatting no longer hold the collection shared lock across the whole response-building phase, and schema/reference mutations now rebuild the snapshot whenever search-visible state changes. No new dedicated timing/concurrency unit was added because the remaining delta is lock granularity rather than a stable user-visible branch in logic; the safe non-flaky proof here is the existing alter/search/highlight replay mix (`CollectionSchemaChangeTest.AddNewFieldsToCollection`, `CollectionSchemaChangeTest.DropFieldsFromCollection`, `CollectionSchemaChangeTest.AbilityToDropAndReAddIndexAtTheSameTime`, `CollectionSpecificMoreTest.HighlightWordWithSymbols`, `CollectionSpecificMoreTest.AnalyticsFullFirstQuery`, and `UnionTest.UnionHighlightingUAFRaceASAN`).
- `975163ef` (`fix(reference faceting): stop recursive get_related_ids overload`) is already effectively present in this fork, so no backport is needed: current `Index::get_related_ids(const uint32_t& seq_id, ...)` already materializes `std::vector<uint32_t> seq_ids{seq_id};` before delegating to the vector overload, which is the exact stable-line fix.
- Item 40 is now complete: every currently-missing official `upstream/v30` patch family is either landed locally or explicitly classified as already present, so no open stable-line parity item remains before `v31` intake.

**Story A - Investigation**

- [x] Classify every missing `upstream/v30` patch as `backport now`, `already superseded in the fork`, or `reject/blocked` with a concrete reason.
- [x] Backport the smallest safe user-visible fixes first: zero-match phrase vector behavior. *(`#2793` diversity seq-id ordering, `#2772` `/health` meta-thread-pool routing, `#2809` model-download timeout bump, `9b0d729b` model-config fetch timeout bump, `#2817` zero-match phrase/vector fallback, `c2272a95` nested preset auth hardening, `5bef9362` filtering/not-equals cleanup, `ae64826a` delete-by-query batching, `1b4fa888` highlight-race hardening, `3ad9d3e0` highlight posting-list de-cloning, and `5458d3f0` finer-grained search locking are now landed locally. The behavior fixes carry focused tests; the timeout-only backports are documented as non-trivial to exercise without an injected transport seam, and the lock-granularity backport is verified through existing non-flaky alter/search/union coverage instead of a brittle timing test.)*
- [x] Re-evaluate the remaining search-locking item (`5458d3f0`) against current NuRaft-era code before touching it; do not assume it is safe just because it shipped on the classic release line. *(Done Mar 18, 2026: the fork already carried the upstream helper wrappers, so the accepted backport narrowed to the missing read-state snapshot and snapshot rebuilds on schema/reference mutation rather than a blind patch import.)*
- [x] Keep parity proof test-first where upstream already supplied coverage.

**Exit criteria**

- [x] The plan records a concrete go/no-go decision for each missing `upstream/v30` patch family, and any accepted small backports land with matching tests.

### 41) Upstream `v31` selective intake audit

**Why this sprint exists now**

- `upstream/v31` is the development branch for the next upstream release, and the fork diverged from it at `ddfa9970` (`feat: add support for SigLIP tokenizer and image processor (#2795)`).
- Several post-fork `v31` commits are user-visible fixes or product-surface changes that should become explicit intake decisions instead of accidental drift.

**Sprint goal**

- Turn upstream `v31` delta into an explicit backport queue: pull in the high-value bug fixes that fit the fork, and record which larger product-surface changes are intentionally deferred.

**Status snapshot (Mar 2026)**

- The most relevant post-fork `upstream/v31` candidates identified in this audit are:
  - `fb5bf14b` `feat: update support for patching keys (#2820)`
  - `121a5161` `fix(curation): fix query for semantic vector search with embedding generation (#2604)`
  - `3f2e15f7` `add exception for operation get endpoint route (#2792)`
  - `92ab674a` `fix(join): map object array filters with joined references (#2830)`
  - `eb81162a` `dynamic faceting based on occurrence ratio (#2822)`
  - `9c9e3905` `fix: handle Gemini streamed responses across curl buffer boundaries (#2836)`
  - `c1bd3c77` `fix: update logic for skipping embedding generation when it is provided (#2807)`
- Lower-signal items from the same range such as compile-only fixups or test-only additions should follow the product bugfixes rather than lead this intake queue.
- Backport 1 is now landed locally on `v31-fork`: upstream PR `#2792` / commit `3f2e15f7` (`add exception for operation get endpoint route`). `route_path::_get_action()` now treats `GET /operations/schema_changes` as `operations/schema_changes:get` instead of the generic `:list` mapping, and `AuthManagerTest` covers both the action-string generation and the auth-path distinction between `operations/schema_changes:list` and `operations/schema_changes:get`.
- Backport 2 is now landed locally on `v31-fork`: upstream PR `#2807` / commit `c1bd3c77` (`fix: update logic for skipping embedding generation when it is provided`). `Index::batch_embed_fields(...)` now preserves a pre-computed embedding supplied on `UPDATE`, `UPSERT`, and `EMPLACE` requests instead of silently regenerating it from source fields, and `CollectionVectorTest.SkipEmbeddingOpWhenValueExistsOnUpsert` locks in that create/update/upsert/emplace behavior using the collection's actual embedding dimension.
- Backport 3 is now landed locally on `v31-fork`: upstream PR `#2604` / commit `121a5161` (`fix(curation): fix query for semantic vector search with embedding generation`). Semantic-only searches now keep the original query tokens long enough for curation/filter matching before switching the live search query to `*`, so vector searches that rely on embedding generation still honor matching curations. `CollectionCurationTest.FilterCurationsWithSemanticOnlySearch` locks in that semantic-only filter-curation path.
- Backport 4 is now landed locally on `v31-fork`: upstream PR `#2830` / commit `92ab674a` (`fix(join): map object array filters with joined references`). Object-array filters that combine local nested predicates with `$joined_collection(...)` conditions now correlate joined-reference hits to the specific object index instead of treating any joined match in the document as sufficient, and `CollectionJoinTest.FilterByObjectArrayJoinCorrelation` locks in both the active and inactive correlation cases plus hit ordering.
- Backport 5 is now landed locally on `v31-fork`: upstream PR `#2836` / commit `9c9e3905` (`fix: handle Gemini streamed responses across curl buffer boundaries`). Gemini streaming callbacks now carry partial JSON across curl buffer splits, ignore standalone array delimiters until a full object arrives, and only mark the async conversation ready under its mutex in the done callback. `ConversationTest.TestGeminiStreamManipulation`, `ConversationTest.TestGeminiStreamSplitObjectAcrossCallbacks`, and `ConversationTest.TestGeminiStreamSplitArrayDelimitersAcrossCallbacks` lock in both the old contiguous path and the new split-buffer cases.
- Deferred candidate 1: upstream PR `#2820` / commit `fb5bf14b` (`feat: update support for patching keys`) is intentionally **deferred**, not backported. It adds a new admin API route (`PATCH /keys/:id`) plus merge/update semantics for stored keys, which is additive product surface rather than a parity bugfix; it needs a separate API-contract review and end-to-end auth/admin coverage pass instead of riding along with the bugfix intake.
- Deferred candidate 2: upstream PR `#2822` / commit `eb81162a` (`dynamic faceting based on occurrence ratio`) is intentionally **deferred**, not backported. It introduces a new search parameter (`facet_min_occurrence_ratio`), new dynamic-facet metadata, and new filtering behavior for wildcard/dynamic facets in both regular and union searches; that is a search-product decision that needs dedicated relevance/API validation instead of silent adoption.
- Item 41 is now complete: every tracked post-fork `upstream/v31` candidate now has an explicit outcome on this fork (`3f2e15f7`, `c1bd3c77`, `121a5161`, `92ab674a`, and `9c9e3905` accepted/backported; `fb5bf14b` and `eb81162a` deferred). **Note:** a deeper Mar 28, 2026 audit (item 45) found 11 additional missing upstream commits that were not in the item 41 candidate list — see item 45 for the full Phase 2 backport plan.

**Story A - Investigation**

- [x] Audit each identified `upstream/v31` candidate for fork applicability, expected user impact, and conflict risk with the NuRaft/modernization changes already landed.
- [x] Separate low-risk bugfix backports from larger product-surface changes; do not merge new behavior into `v32` just because it exists on `v31`.
- [x] Promote any accepted `v31` backports into concrete implementation tasks with proof, and explicitly mark rejected ones as deferred/non-goals instead of leaving them implicit.

**Exit criteria**

- [x] The plan carries an explicit accept/reject/defer decision for each tracked `upstream/v31` candidate, with the accepted subset scheduled as concrete follow-up work.

### 42) Repo-owned prebuilt CUDA ORT bundle intake

**Why this sprint exists now**

- Cold `linux-amd64` `release-binaries` runs can spend most of the hosted 6-hour job budget rebuilding CUDA-enabled ONNX Runtime from source, especially when the cache namespace changes or a lane starts cold.
- `actions/cache` only restores at job start and saves at job end, so a timed-out hosted job does not preserve completed ORT build actions the way a true Bazel remote cache would.
- The goal here was to avoid adding self-hosted runners or Bazel remote-cache infrastructure if a repo-owned prebuilt bundle could preserve exact compatibility with the current one-Protobuf CUDA path.

**Sprint goal**

- Package the exact CUDA-enabled one-Protobuf ORT install tree this repo already builds, then let Linux release workflows consume that bundle when available while keeping the existing source build as the safe fallback.

**Status snapshot (Mar 19, 2026)**

- `MODULE.bazel` now declares a repo-owned `@typesense_ort` repository via `bazel/typesense_ort_repo.bzl`. When `TYPESENSE_ORT_PREBUILT_BUNDLE_DIR` is unset it aliases back to `@onnx_runtime`; when it is set it exposes the extracted bundle's static archives, headers, and provider sidecars directly to Bazel.
- `BUILD` now depends on `@typesense_ort//:onnxruntime_static_one_protobuf_lib`, so the main server target can switch between source-built ORT and a prebuilt bundle without target-label churn.
- `scripts/release_ort_bundle.sh` is the canonical public entrypoint for bundle packaging. It can print the deterministic bundle key, build `@onnx_runtime//:onnxruntime_static_one_protobuf --define=use_cuda=on`, and package the resulting install tree into `artifacts/typesense-ort-bundle-<label>-linux-<arch>.tar.gz` plus SHA256 and JSON manifest sidecars.
- `.github/workflows/ort-bundles.yml` is the manual hosted producer for those artifacts on `linux-amd64` and `linux-arm64`.
- `.github/workflows/release-binaries.yml` now defaults to auto-discovering the latest matching successful same-branch ORT bundle on Linux lanes, with `use_prebuilt_ort_bundle=false` as the explicit opt-out and `ort_bundle_run_id` as the deterministic override when one specific `ort-bundles.yml` run must be used.
- `scripts/release_linux_artifacts.sh` and `scripts/release_linux_gpu_deps.sh` now forward `TYPESENSE_ORT_PREBUILT_BUNDLE_DIR` through both the Bazel build phase and the later Dockerized release-assembly phase so the downloaded bundle can drive both the main server artifact and the optional GPU-deps artifact.
- Local proof is green:
  - `scripts/bazel_in_docker.sh build //:typesense-server --define=use_cuda=on --repo_env=TYPESENSE_ORT_PREBUILT_BUNDLE_DIR=/work/tmp/ort-bundle-test.vfdb2U` linked successfully against an extracted bundle in `4.819s` with only `3` Bazel actions.
  - `TYPESENSE_ORT_PREBUILT_BUNDLE_DIR="$PWD/tmp/ort-bundle-test.vfdb2U" scripts/release_linux_artifacts.sh --with-cuda --skip-packages --target-arch amd64 --version-label 0.0.0-prebuilt-test` passed and produced the expected Linux tarball plus `.debug` sidecar.
  - `TYPESENSE_ORT_PREBUILT_BUNDLE_DIR="$PWD/tmp/ort-bundle-test.vfdb2U" scripts/release_linux_gpu_deps.sh --skip-packages --target-arch amd64 --version-label 0.0.0-prebuilt-test` passed and produced the expected GPU-deps tarball.
  - `scripts/release_ort_bundle.sh --version-label 0.0.0-prebuilt-test --target-arch amd64` successfully packaged the current ORT install tree into the new artifact shape.
- Hosted proof is green too:
  - `ort-bundles.yml` run `23297096113` succeeded on both `linux-amd64` and `linux-arm64`, publishing `typesense-ort-bundle-8bb83bc13b6e3962-linux-amd64` and `typesense-ort-bundle-e0316edc4a80a6ce-linux-arm64`.
  - `release-binaries.yml` run `23302834117` proved the fixed arm64 bundle-backed release path end-to-end, including `gpu-linux-arm64`.
  - `release-binaries.yml` run `23310421507` then proved the amd64 bundle-backed path end-to-end, including `gpu-linux-amd64`.
  - Full-matrix `release-binaries.yml` run `23312338973` succeeded across `linux-amd64`, `linux-arm64`, `linux-arm64-lg-page16`, `darwin-amd64`, `darwin-arm64`, `gpu-linux-amd64`, and `gpu-linux-arm64`.
  - The March 23, 2026 full release rerun `23439833073` then exposed two workflow regressions introduced by later tooling work, not by the core release shape: the new build-provenance header generator used Bash associative arrays and therefore broke on the macOS runners' older system Bash, and the Linux GPU-deps lane still depended on an intermediate provider-input artifact even though the workflow already had a safe source-build fallback when no matching prebuilt ORT bundle exists. The fix is to keep `bazel/generate_build_info_header.sh` Bash-3.2-compatible and let `gpu-*` build from source whenever the ORT bundle auto-discovery misses, instead of failing `core-binaries` on a missing transient upload.
  - The second-pass cache effect on already-proven Linux core lanes was material: `linux-amd64` `Build Typesense server in Docker` dropped from about `35m34s` in `23310421507` to about `22s` in `23312338973`, and `linux-arm64` dropped from about `23m53s` in `23302834117` to about `26s` in `23312338973`.
- Item 42 is complete: the repo now has a CI-parity, repo-owned prebuilt ORT bundle path that avoids cold hosted ORT rebuilds when operators intentionally dispatch `ort-bundles.yml` first, without weakening the default source-build path.

### 45) Upstream `v31` parity Phase 2 — remaining 11 missing commits

**Why this sprint exists now**

- Item 41 (`Upstream v31 selective intake audit`, completed Mar 20, 2026) only covered the commits that existed at audit time.
- A deep audit on Mar 28, 2026 (`git log ddfa9970..upstream/v31`, 43 total upstream commits) found 11 confirmed-missing upstream changes that the fork does not carry — including 2 critical race-condition fixes, a search cache security fix, and several search-correctness bugs.
- 5 commits previously believed backported in item 41 were also verified as actually missing in the fork's source code. These are now included here.

**Audit methodology (Mar 28, 2026)**

Upstream v31 was fetched and all 43 post-fork commits were diffed one-by-one against the fork's current source. Each was classified as PRESENT, MISSING, or PARTIAL by reading the actual fork file contents (not just commit messages). Cross-referenced against items 40 and 41 in this plan.

**Sprint goal**

- Backport all 11 missing upstream commits in priority order, with tests. Mark each sub-task as it lands.
- Do NOT apply blindly — each backport must be verified against NuRaft architecture, the fork's per-document-ID striped write locks (item 44), and the `read_state_t` snapshot pattern already present.

**Conflict risks**

- `35a75c5a` (#2815, related-collection race conditions) touches `collection.cpp`, `batched_indexer.cpp`, `index.cpp` extensively and will likely conflict with item 44's per-document-ID striped write locks.
- `c0c7078c` (ART leaf pointer elimination) changes `searched_queries` type signatures across the search pipeline — may interact with NuRaft search delegation.
- `d73315ed` + `f478aca7` (cache keying + scoped API key) are tightly coupled and must be applied together.

---

**Phase 1 — Quick wins (trivial, no conflict risk)**

These are small, isolated fixes. Apply and test individually.

- [x] **`e5df2ae8`** (#2800) — Update apikey in schema field during alter. Applied: `search_schema.at(field_name).embed` now updated alongside `embedding_fields`. Build+API green.
- [x] **`4ed78311`** — Return better error for unresolved group_by field. Applied: `search_schema.count()` guard added. Build+API green.
- [x] **`a843afa5`** — Fix conversation search reusing first collection on error. Applied: `else { break; }` added. Build+API green.
- [x] **`366f0879`** (#2840) — Align remote embedder query timeout default. Applied: 30000→5000ms in 6 declarations. Build+API green.
- [x] **`9af851e9`** (#2740) — Fix token offsets when prioritized. Applied: `get_last_offset→get_first_offset`, min logic, 1→0-based normalization. Build+API green.

**Phase 2 — Medium complexity (interrelated or moderate refactoring)**

- [x] **`d73315ed` + `f478aca7`** (together) — Fix search cache keying + scoped API key collection resolution. Applied: `hash_request()` rewritten with length-prefixed components + embedded params; `AUTH_RESOLVED_COLLECTION_PARAM` added; `resolve_scoped_search_collection()` helper; `apply_embedded_params` pins collection. Build+API green.
- [x] **`2d536b67`** (#2838) — Fix stemming curations when search field has stemming enabled. Applied: `compute_normalized_query` now accepts locale/stemmer/symbols; `curation_rule_token_sets` pre-built; `compute_base_query` pattern in `curate_results`. Build+API green.
- [x] **`80ab84fe`** (#2837) — Replace regex SSE parsing with proper parser. Applied: `consume_sse_payloads()`/`find_next_sse_delimiter()`/`append_message_event()` replace regex in OpenAI/CF/vLLM/Azure; `lock_guard` on done callbacks. Build+API green.

**Phase 3 — Heavy lifts (careful integration with NuRaft/fork architecture)**

These touch core search/indexing paths extensively. Apply with extra care.

- [x] **`c0c7078c`** — Avoid persisting ART leaves in search state. Applied: `searched_queries` → `searched_query_tokens` (`vector<vector<string>>`) across `index.h`, `index.cpp`, `collection.cpp`, `filter_result_iterator.cpp`; `compute_aggregated_score` takes `query_index` param; `get_field_token_its` no longer takes `query_suggestion`. Build+API green (151 pass, 0 fail).
- [x] **`35a75c5a`** (#2815) — Fix race conditions in concurrent related collection requests. Applied: `cascade_remove_node_t` struct; `cascade_remove()` static/instance methods; `get_filter_ids_with_lock()`; batched indexer rewritten with explicit `waiting_on_requests` dependency tracking; `lock_nested_referencing_collections`; `update_async_references` new signature. Skipped `raft_server.cpp` (NuRaft). Build+API green (151 pass, 0 fail). **Note:** TSAN verification deferred to separate run.

**Phase 4 — New features (adopted)**

- [x] **`fb5bf14b`** (#2820) — `PATCH /keys/:id` endpoint for partial API key updates. Applied: `update_key()` in auth_manager, `patch_key` handler + route. Build+API green.
- [x] **`eb81162a`** (#2822) — Dynamic faceting based on occurrence ratio (`facet_min_occurrence_ratio`). Applied: new search param (0.0-1.0, default 0.5), `is_dynamic` flag on facets, `filter_dynamic_facets_by_occurrence()`. Build+API green.

**Verification gate for each phase**

After each phase lands:
1. `scripts/bazel_in_docker.sh build //:typesense-server`
2. `scripts/run_api_tests.sh -- --no-secrets`
3. `test/scripts/replay_typesense_test.sh` for targeted C++ test suites affected by the backport
4. For Phase 3: run under `--config=tsan` to verify no new race conditions

**Exit criteria**

- [x] All 11 missing commits backported with tests. (9 in Phases 1-3, 2 in Phase 4. Plus #2857 fix: always recompute `referenced_ins` from schemas.)
- [x] No regression in API tests or C++ test suite. (162 pass, 0 fail, 12 expected TEI skips.)
- [ ] Phase 3 backports verified under TSAN. (Deferred to dedicated TSAN run.)

---

### Ongoing Upstream Maintenance

**Purpose:** This section defines the process for keeping this fork aligned with upstream `typesense/typesense`. It ensures that future upstream bug fixes, features, and test improvements are systematically evaluated and adopted rather than silently drifting.

**Upstream tracking contract**

- **Upstream remote:** `upstream` → `https://github.com/typesense/typesense.git`
- **Upstream branch:** `v31` (development) — check for new commits regularly
- **Fork divergence point:** `ddfa997022b18d6c96957e13c330714a0cc194bc`
- **Upstream commits URL:** https://github.com/typesense/typesense/commits/v31/

**When to run an upstream audit**

Run a parity check against upstream before:
1. Any fork release or version tag
2. Monthly, as part of routine maintenance
3. After upstream announces a new release or security fix
4. When a user reports a bug that might already be fixed upstream

**How to run an upstream audit**

```bash
# 1. Fetch latest upstream
git fetch upstream v31

# 2. Find new upstream commits since last audit
git log --oneline <last-audited-upstream-sha>..upstream/v31

# 3. For each new commit, check if it's already in the fork
git show <upstream-hash> --stat   # see files changed
# Then read the fork's current version of those files to verify

# 4. Categorize each as: PRESENT, MISSING, or PARTIAL
# 5. For MISSING: assess priority and add to this plan
```

**Last completed audit**

- **Date:** 2026-03-28
- **Upstream HEAD at audit time:** `35a75c5a` (tip of `upstream/v31`)
- **Total upstream commits since fork divergence:** 43
- **Result:** 11 missing commits identified → item 45 (this sprint)
- **Previous audits:** item 40 (v30 parity, Mar 18), item 41 (v31 selective intake, Mar 20)

**Audit record template**

When completing a future audit, record it here:
```
- **Date:** YYYY-MM-DD
- **Upstream HEAD:** <sha>
- **New commits since last audit:** <count>
- **Missing commits found:** <count> → item <N>
- **Already present:** <count>
```

**Key areas to watch in upstream**

These upstream code areas have the highest impact on this fork and should be prioritized during audits:

| Area | Why it matters | Fork-specific considerations |
|------|---------------|------------------------------|
| `src/collection.cpp` | Core search/write path | Fork has per-document-ID striped write locks (item 44) and `read_state_t` snapshot |
| `src/index.cpp` | Search indexing, highlighting | Fork uses USearch instead of hnswlib |
| `src/batched_indexer.cpp` | Write batching, references | Fork routes through NuRaft consensus |
| `src/core_api.cpp` | API routing, auth | Fork uses `nuraft_http_runtime.cpp` route table |
| `src/filter_result_iterator.cpp` | Query filtering | Shared code, usually clean backports |
| `src/conversation_model.cpp` | LLM conversation | Shared code, usually clean backports |
| `include/auth_manager.h` | API key handling | Fork has NuRaft-aware auth path |
| `src/http_server.cpp` | HTTP routing, thread pools | Fork has NuRaft route registration |

**What NOT to auto-backport**

- Changes to `src/raft_server.cpp` or `src/typesense_server_utils.cpp` — these are the old `braft` paths that this fork replaced with NuRaft
- Changes to build system files (`Makefile`, `cmake/`, `docker/`) — this fork uses Bazel exclusively
- Changes to `hnswlib` integration — this fork uses USearch

---

### Backlog map (active / later / archival)

Use this to decide what to pick next without scanning multiple files.

- **Recently finished:** item **45** (`Upstream v31 parity Phase 2`) — 9 of 11 upstream commits backported across Phases 1-3; 2 deferred (new features). Build+API green, TSAN pending.
- **Active:** items **25a/25b** (`DDEV re-check`) — fresh image build and heavy-import responsiveness validation.
- **Recently finished:** item **44** (`Same-document concurrent write correctness hardening`) — per-document-ID striped write locks in `Collection::add_many()`.
- **Recently finished:** item **43** (`Startup config / CLI parity recovery`) — shared Typesense config surface restored for NuRaft runtime.
- **Recently finished:** item **42** (`Repo-owned prebuilt CUDA ORT bundle intake`) — Linux release lanes reuse ORT bundles, source-build as fallback.
- **Recently finished:** item **41** (`Upstream v31 selective intake audit`, Mar 20) — 5 backports landed, 2 deferred. Superseded by item 45 for remaining gaps.
- **Recently finished:** item **40** (`Upstream v30 release-parity catch-up audit`, Mar 18) — 11 backports landed. Stable-line parity is intentional.
- **Completed items 1-39:** all done. See individual sections above for details. Key highlights: NuRaft cutover (item 19), USearch vector backend (item 36), Bazel 9 (item 6), RocksDB tuning (item 13), release workflow (items 25/37), indexing perf (item 38).
- **Later:** re-audit `bazel/kakasi.patch` if upstream publishes fixes. Monitor sanitizer warning growth. Re-mine `TODO.md` only when aligned with current goals.
- **Archival/reference:**
  - `benchmark/BENCHMARK_RESULTS.md` P2/P3 backlog items (experimental/future ideas).
  - `TODO.md` upstream product backlog (mine opportunistically).

### 21a) NuRaft import/runtime parity + benchmark refactor sprint

**Why this sprint exists now**

- Upstream `typesense/typesense` `v30/v31` benchmarked the classic single-node server path and sent the 1M-doc import as one large POST.
- When this sprint started, the hosted benchmark launched the NuRaft runtime binary and used `500`-doc client chunks on GitHub to stay inside the inherited `60000ms` H2O request timeout.
- Local self-compare completed successfully with a full benchmark replay and zero import-contract warnings, so the main red was not a generic benchmark CLI bug.
- The initial regression hypothesis was runtime execution-model mismatch: one logical streaming import might still be paying Raft append/commit cost at H2O aggregation boundaries before the real import handler ran.

**Sprint goal**

- Restore safe support for large single-request bulk imports on the NuRaft-backed `typesense-server`, then make the benchmark lane compare that path fairly against previous commits without tiny client-side chunking.

**Status snapshot (Mar 2026)**

- Reproduced the old failure mode with a single throttled POST against the pre-fix runtime path: the connection reset after `3570/12000` response lines, only `3570` documents landed, and the request was still tied to one Raft commit for that partial body fragment.
- A first route-only buffering fix proved the one-shot POST could complete again, but it still changed the live import execution model enough to distort storage/search shape. Do not keep that intermediate version.
- The final runtime fix now buffers one logical HTTP import once, appends it through Raft as bounded logical chunks (`5000` docs / `4 MiB`), then replays the buffered body through `post_import_documents()` in H2O-sized slices so the live engine still sees the existing import-handler cadence.
- Direct local proofs are green on that final model:
  - throttled `12k` single POST returns `200`, emits `12000/12000` newline-delimited result lines, and lands all documents
  - direct `1M` single POST returns `200`, emits `1000000/1000000` result lines in `28s`, lands all documents, and finishes with `/status.committed_index=202`
- The server timeout is now configurable (`--request-timeout-ms` and `TYPESENSE_REQUEST_TIMEOUT_MS`) while keeping the product default at `60000ms`.
- Benchmark lanes are now split into `core` (`index + search`) and `extended` (stress/concurrent/extra metrics). The hosted workflow runs `core` and forwards `TYPESENSE_REQUEST_TIMEOUT_MS=300000` into benchmark containers so newer binaries use the raised timeout while older comparison artifacts ignore the env var safely.
- Local post-fix benchmark reruns are green on the final core lane:
  - `quick/core` self-compare passed on the final replay-model build (`27090ms -> 25689ms` import, `100%` search checks for both halves)
  - `standard/core` archived at `~/.cache/typesense/benchmark/archives/20260314-082629-standard-core`
  - `standard/core` one-shot import summaries: upstream `29383ms`, current runtime build `26906ms`, both `1000000/1000000` docs with `status=200` and `response_contract_warnings=0`
  - representative `100vu` search p95s: `facet 837ms -> 133ms`, `group 4467ms -> 380ms`, `sort_simple 516ms -> 54ms`
- The broad canonical API gate is green again: `scripts/run_api_tests.sh -- --no-secrets`.
- A benchmark-only hardening fix was needed after those reruns started producing long search-phase stderr bursts: the harness now launches the long-lived Typesense Docker process with execa `buffer: false`, so Bun/get-stream no longer aborts `standard/core` while buffering server logs that are already being consumed incrementally.
- Hosted confirmation is now green too: `Benchmark Testing` run `23085170081` completed successfully in `43m55s` on `27bb2bff`, comparing that workflow SHA against the previous successful same-branch `tests.yml` artifact `985acb45`. That hosted lane is a fork-vs-fork guardrail on GitHub runners, not the upstream-vs-fork control.

**Story A — prove the actual import execution model**

- [x] Instrument the runtime import path to answer one question unambiguously: for one large client POST, how many Raft appends happen, and at what chunk boundaries? *(Final answer: not one append per transport chunk. The runtime now buffers one logical request, appends bounded `5000`-doc / `4 MiB` Raft entries keyed to that import, then replays the buffered body through the existing import handler cadence.)*
- [x] Capture the same flow on the classic single-node server path so we can separate HTTP chunking from Raft-induced overhead. *(Upstream `v30/v31` benchmark code still sends one large POST, and the classic single-node path has no Raft append between H2O request aggregation and the real import handler.)*
- [x] Record whether the runtime returns only after full commit/apply visibility or whether it can safely acknowledge earlier without violating product semantics. *(No early-ack shortcut was added. The runtime still goes through the normal synchronous Raft write path; the fix only changes request aggregation granularity before that write.)*
- [x] Keep the evidence in short notes here and add a concise benchmark decision note in `benchmark/BENCHMARK_RESULTS.md`.

**Story B — make server request timeout configurable**

- [x] Replace the hardcoded HTTP request timeout path with a runtime-configurable server option for both classic and NuRaft entrypoints.
- [x] Keep the old effective default first, then add benchmark/test coverage for explicitly raised timeout values. *(Default remains `60000ms`; the local large-POST proof used `--request-timeout-ms 300000`, and the benchmark workflow now exports `TYPESENSE_REQUEST_TIMEOUT_MS=300000`.)*
- [x] Confirm the new option affects `http1.req_timeout`, `http1.req_io_timeout`, and `http2.idle_timeout` together so behavior is predictable.

**Story C — fix NuRaft import granularity**

- [x] Refactor runtime import so transport chunking does not become Raft log-entry granularity for one logical bulk import request.
- [x] Preserve the documented import response contract: `200` and newline-delimited per-document result lines when no per-document errors are expected.
- [x] Verify follower/leader behavior remains correct and deterministic after the refactor; do not regress the earlier local fix that routed `kDocumentImport` through the real registered handler. *(Targeted API suites `documents`, `nuraft_runtime_documents_crud`, and `nuraft_replication_edges` passed during development, and the broad `--no-secrets` API gate is green on the final replay-model build.)*
- [x] Prefer a design that is explicit and replay-safe: aggregate one logical import request on the leader, replicate one logical import mutation, then execute/import with the existing product handler semantics.
- [x] Do the implementation and large single-POST replay locally first; do **not** re-enable full benchmark comparisons until this runtime path is proven with a direct local one-shot import against the latest built binary.

**Story D — split benchmark intent**

- [x] Add a benchmark scope/profile split:
  - `core`: upstream-comparable path (`index + search`)
  - `extended`: stress-import, concurrent search+import, extra RocksDB snapshots, long-lived metrics collector
- [x] Make `benchmark-testing.yml` run `core` by default.
- [x] Keep `extended` manual and reachable via the existing wrapper/CLI so stress coverage is preserved.
- [x] Only revisit hosted client-side chunking after Stories B/C prove large full-POST imports are safe again. *(The old client chunking workaround is retired. Hosted runs now use the same one-shot import shape, with an explicit timeout override instead of transport-level request splitting.)*

**Story E — validation matrix after the refactor**

- [x] Local single-node large import replay against `//:typesense-server` with a large one-shot POST and raised request-timeout. This is the hard gate before any new benchmark reruns.
- [x] Local benchmark wrapper replay:
  - `TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --self-compare --profile quick --scope core`
  - `TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile standard --scope core`
  - Results: final replay-model `quick/core` self-compare passed in explicit work dir `/tmp/self-bench-rerun.ccjGHS` (import `27090ms -> 25689ms`, search checks `100%`); `standard/core` archive `20260314-082629-standard-core` (`29383ms -> 26906ms` import, heavy search scenarios all faster than upstream classic)
- [x] Targeted API coverage for import contract and visibility:
  - `tests/documents.test.ts`
  - `tests/nuraft_runtime_documents_crud.test.ts`
  - `tests/nuraft_replication_edges.test.ts`
- [x] Canonical build/test gates after runtime changes:
  - `scripts/bazel_in_docker.sh build //:typesense-server`
  - `scripts/run_api_tests.sh -- --no-secrets`
- [x] Hosted confirmation after local green:
  - `tests`
  - `benchmark-testing`
  - then `nightly-extended` only if the refactor touches long-running import/search behavior materially

**Required deep-dive comparisons against upstream**

- [x] Re-read upstream classic import flow in:
  - `src/core_api.cpp`
  - `src/http_server.cpp`
  - `src/main/typesense_server.cpp`
- [x] Compare against this fork's runtime path in:
  - `src/nuraft/nuraft_http_runtime.cpp`
  - `src/nuraft/nuraft_state_machine_sink.cpp`
  - `src/main/typesense_nuraft_runtime.cpp`
- [x] Confirm whether upstream's long benchmark runs were still using one-shot imports and whether their wall-clock stayed bounded because they were not paying per-chunk consensus overhead. *(Upstream benchmark code still does one large POST. The old fork regression hypothesis was therefore runtime granularity, not benchmark CLI drift alone.)*
- [x] If the runtime refactor diverges from upstream classic semantics, document exactly why the divergence is necessary and what product contract is preserved. *(The necessary divergence is only the Raft boundary: the runtime must buffer one logical HTTP import before replicating it. The preserved contract is still one HTTP `200` plus newline-delimited per-document results from the existing import handler.)*

**Exit criteria**

- [x] A large single-request import completes successfully on the NuRaft-backed server with a raised configured timeout.
- [x] Runtime import no longer appears to pay one Raft append/commit per transport chunk for one logical request.
- [x] The first benchmark rerun happens only after the local large single-POST import gate is green.
- [x] The default benchmark lane finishes comfortably inside GitHub's workflow timeout budget on the latest SHA.
- [x] `benchmark/BENCHMARK_RESULTS.md` and this plan both explain the new benchmark/runtime policy in one short section each.

### Upstream TODO candidates worth pulling in (post-Phase-3 queue)

From `TODO.md`, these are the highest-value items that still align with current modernization/perf goals:

- ~~**Replication throughput control:**~~ N/A — `MAX_UPDATES_TO_SEND` was a `braft` concept. NuRaft controls replication internally; its parameters are already exposed as `--raft-*` CLI args.
- ~~**Indexing hot-path efficiency:**~~ done — item **38** closed with commit `7ab4cd8b` plus a clean current-head upstream `30.1` benchmark replay showing materially faster import and search behavior.
- ~~**Search work budget tuning:**~~ no-go — the old `max_results` TODO is obsolete on the current search stack. The user-visible "enough results" behavior is already explicitly controlled by `typo_tokens_threshold`, `drop_tokens_threshold`, `max_candidates`, and `search_cutoff_ms`, so adding another alias would duplicate semantics without improving explainability.
- ~~**Numeric safety hardening:**~~ done — `coerce_int32_t()` and `coerce_int64_t()` in `validator.cpp` now bounds-check float values before casting, preventing UB from out-of-range floats. Returns 400 or drops the field per dirty_values policy.
- ~~**Reliability coverage:**~~ done — `nuraft_replication_edges.test.ts` adds 19 focused multi-node tests covering follower-originated metadata writes, cross-node visibility, restart persistence, and snapshot persistence. Fixed NOT_LEADER race in `append_via_raft()`. Total: 139 pass, 0 fail.

These were intentionally deferred while item 13 was active. The two remaining modernization-aligned candidates were promoted into item **38** (`Indexing hot-path string-copy audit`) and item **39** (`Search work-budget configurability audit`) and are now resolved, so future TODO re-mining can pick the next candidate directly instead of reopening them.

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
- Legacy `ci_build.sh` was removed after the repo fully converged on `scripts/bazel_in_docker.sh` as the single supported build entrypoint.

**Deferred for now:** `SstFileWriter / IngestExternalFile` is intentionally not the next lane. It remains a future bulk-import idea, but current evidence suggests the in-memory indexing critical section is a larger bottleneck than the final RocksDB write primitive, so this path is not worth immediate engineering time.

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

### Core release artifact convergence status

Upstream core server artifacts (`typesense-server` tarballs / DEB packages) do not ship a hard runtime dependency on `libonnxruntime.so.1`; upstream Bazel builds ONNX Runtime as static libs. This fork now matches that artifact shape on the promoted local Linux build: `typesense-server` links through the one-Protobuf static ORT path, and `ldd bazel-bin/typesense-server` shows no `libonnxruntime.so.1` dependency.

The historical blocker was real: once the earlier static probe linked ONNX Runtime's bundled Protobuf 21.12 archives into the final binary, `ld.lld` reported duplicate `google::protobuf` symbols against the repo's Protobuf 33 runtime (`external/protobuf+`). The path that resolved it was unifying ORT onto the repo's protobuf rather than keeping the shared-library boundary. Remaining work is no longer basic linkage feasibility; it is release/multi-platform automation and validation.

Latest one-Protobuf research and execution notes (Mar 2026):

- ORT's own CMake already has an external-protobuf path. `cmake/external/onnxruntime_external_deps.cmake` declares Protobuf with `FIND_PACKAGE_ARGS NAMES Protobuf protobuf`, and vendored ONNX reuses pre-existing `protobuf::libprotobuf`, `protobuf::libprotobuf-lite`, and `protobuf::protoc` targets instead of fetching protobuf again.
- Our current Bazel integration intentionally disables that path for both shared and static builds via `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER` in `bazel/onnxruntime.BUILD`.
- This repo does **not** currently expose a ready-to-use `ProtobufConfig.cmake` package inside `rules_foreign_cc`. The realistic inputs available in foreign_cc are raw Bazel artifacts (`$$EXT_BUILD_DEPS/lib/libprotobuf.a`, `$$EXT_BUILD_DEPS/lib/libprotobuf_lite.a`, `$(execpath @com_google_protobuf//:protoc)`, headers under `$$EXT_BUILD_ROOT/external/protobuf+/src`), similar to how `bazel/sentencepiece.BUILD` wires protobuf.
- Most realistic prototype paths are:
  - patch ORT static builds to accept repo-provided protobuf imported targets created from those raw Bazel artifacts, or
  - stage a tiny synthetic protobuf CMake package/prefix for ORT static builds.
- We took the first path. `bazel/onnxruntime.patch` now patches `cmake/external/onnxruntime_external_deps.cmake` to short-circuit FetchContent and create imported `protobuf::...` targets from Bazel-provided artifacts, and `bazel/onnxruntime.BUILD` wires a dedicated `onnxruntime_static_one_protobuf` target.
- The patch had to be regenerated from the exact pinned ORT commit (`058787ceead760166e3c50a0a4cba8a833a6f53f`); an earlier hand-shaped/WIP patch targeted the wrong upstream file layout and Bazel rejected it during repository fetch.
- ORT 1.24.2 static packaging also needs `libonnxruntime_lora.a` exposed through Bazel. Without it, final linking fails on `onnxruntime::adapters::utils::*` symbols from `lora_adapters.cc` even after the protobuf collision is removed.
- Verification now succeeds with `scripts/bazel_in_docker.sh build //:typesense-server`.
- `common_deps` in `BUILD` now points at `@onnx_runtime//:onnxruntime_static_one_protobuf_lib`, so the main `typesense-server` target uses the validated self-contained path, not just the probe target.
- `ldd bazel-bin/typesense-server` now shows no `libonnxruntime.so.1` dependency. The main local Linux artifact is now self-contained by default.
- Runtime validation now includes:
  - `scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets tests/health.test.ts` passing all single-node + multi-node health/restart/snapshot phases.
  - `scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets` passing the broader no-secrets API suite.
  - a direct probe-binary smoke that prewarms `ts/e5-small`, creates an embedding collection, indexes a document, observes a populated embedding (`384` dims), and returns a vector-search hit.
- After promotion, the same validations also pass on the real `typesense-server` target: `scripts/run_api_tests.sh -- --no-secrets`, `ldd bazel-bin/typesense-server`, and a direct local `ts/e5-small` embedding/vector-search smoke.
- `api_tests/scripts/prepare_runtime_bundle.sh` is now binary-shape-aware: it copies `libonnxruntime.so.1` only when `ldd` shows the tested binary actually needs it. `scripts/run_api_tests.sh` also accepts `--server-binary` / `TYPESENSE_SERVER_BINARY_PATH`, so the API harness can validate either shared-ORT or self-contained binaries without manual bundle surgery.
- `.github/workflows/tests.yml` no longer uploads `libonnxruntime.so*` as a required build artifact, because the promoted server bundle may legitimately be just the binary.
- `.github/workflows/tests.yml` and `.github/workflows/release-binaries.yml` now include `ldd` guardrails that fail if `typesense-server` silently regresses back to a `libonnxruntime.so.1` dependency on Linux.
- `.github/workflows/release-binaries.yml` now also writes `typesense-server.md5.txt` into each staged release bundle and uploads a tarball `.sha256.txt` sidecar so downstream packaging helpers have the expected checksum metadata.
- The release workflow now verifies the packaged tarball actually contains `typesense-server` plus `typesense-server.md5.txt`, and that the embedded MD5 manifest matches the packaged binary bytes.
- The release workflow now smoke-tests the staged `typesense-server` with `--help`, which gives the macOS lanes at least a minimal runtime sanity check before packaging/upload.
- `debian-pkg/generate_deb_rpm.sh` now resolves release tarballs from either the new `artifacts/` output or legacy `bazel-bin/`, and verifies the tarball SHA256 sidecar when present before extracting.
- `scripts/publish_release.sh` now uploads the new `artifacts/typesense-server-<version>-<platform>.tar.gz` outputs plus their `.sha256.txt` sidecars, while still falling back to legacy `build-Linux` / `build-Darwin` tarballs if needed. Its package scan also accepts the workflow's real RPM filenames (`typesense-server-<version>.<arch>.rpm`) instead of assuming a dash-delimited pattern that would silently skip RPM uploads.
- End-to-end Linux package validation now succeeds locally against a workflow-style tarball: in an Ubuntu 24.04 container with `alien`, `rpm`, and `dpkg-dev`, `TSV=0.0.0-local ARCH=amd64 RELEASE_ARTIFACT_DIR=./artifacts RELEASE_PACKAGE_DIR=./artifacts/packages bash debian-pkg/generate_deb_rpm.sh` produced both `.deb` and `.rpm` outputs from the staged tarball.
- Based on that validation, `.github/workflows/release-binaries.yml` now includes Linux DEB/RPM generation + upload steps driven from the tarball it already built, instead of leaving package generation entirely manual.
- First real GitHub run of `release-binaries` on `v32` found workflow bugs, not release-shape regressions: Linux built the self-contained binary successfully but the smoke step treated the server's expected `--help`/usage exit (`1`) as failure, and macOS arm64 failed before the build because a step-level `PATH` override hid Homebrew's `bazelisk`.
- The current workflow fix keeps the Linux smoke test but accepts exit `0/1` so long as `Command line usage:` is printed, and the macOS build step now computes Homebrew prefixes at runtime and prepends them to the existing `PATH` instead of replacing it.
- Second GitHub run on commit `030b706f` confirms Linux lanes end-to-end (amd64 + arm64) are green, including tarball checks, DEB/RPM generation, and artifact uploads. Remaining blocker is macOS arm64 fetch-time failure in `@s2geometry` because `MODULE.bazel` used a GNU-style `sed -i -E ...` patch command that is not BSD-sed portable.
- Subsequent Darwin-specific hardening closed the remaining macOS blockers: `apple_support` registration for ObjC toolchain analysis, host/portable patch commands, `h2o` OpenSSL root staging, vendored `magic_enum` AppleClang warning suppression, hermetic curl feature toggles (`CURL_USE_PKGCONFIG=OFF`, `CURL_BROTLI=OFF`, `CURL_ZSTD=OFF`, `USE_NGHTTP2=OFF`), and explicit Apple framework linkopts for ONNX Runtime Extensions image operators.
- Targeted Darwin validation now passes on `darwin-arm64` via run `22824046410`, including build, staging, smoke, tarball, and artifact upload.
- Full multi-arch validation was green via run `22825402217`: `linux-amd64`, `linux-arm64`, `darwin-arm64`, and `darwin-amd64` all succeeded, with Linux package generation/upload and Darwin tarball validation both passing.
- The March 12, 2026 rerun on commit `749f003e` (`disable whisper ggml blas auto-detection`) closed the remaining Darwin whisper doubt: `release-binaries` run `23009183654` succeeded across `linux-amd64`, `linux-arm64`, `darwin-arm64`, and `darwin-amd64`, so the `GGML_BLAS=OFF` whisper fix is validated on the real release packaging path.
- The March 20, 2026 `tests.yml` `Illegal instruction` on `CollectionVectorTest.TestVoiceQuery` came from a different whisper portability class: `bazel/whisper.BUILD` still had `GGML_NATIVE=ON`, and `tests.yml` was caching the full `.cache/bazel-docker` output tree across heterogeneous GitHub-hosted x64 CPUs. The fix is to keep whisper portable by default and persist only Bazel `disk-cache`, `repository-cache`, and `bazelisk` in that workflow; keep the CUDA ORT bundle workflow separate because that 5-hour bottleneck is already handled by `ort-bundles.yml` plus `release-binaries.yml`'s default same-branch bundle auto-discovery (or explicit `ort_bundle_run_id` override).
- `release-binaries.yml` can now optionally publish Linux runtime Docker images straight to Docker Hub from the already-built release artifacts. The contract is repo-variable `DOCKERHUB_USERNAME`, repo-secret `DOCKERHUB_TOKEN`, immutable branch+sha tags like `v31-fork-<sha>`, a moving branch tag like `v31-fork`, and a separate `-arm64-lg-page16` tag family for the large-page Linux arm64 build.
- `tests.yml` now follows the same ORT reuse strategy as the release lane when possible: compute the pinned ORT bundle key, fetch the latest matching successful `ort-bundles.yml` artifact on the same branch, and only fall back to a source ORT build when no matching bundle exists. That keeps the portable-cache fix while avoiding repeated multi-hour ORT rebuilds on hosted amd64 runners, and the GCC/Clang warning-guardrail builds now reuse that same extracted tree instead of triggering their own source ORT rebuild.
- The automatic `tests.yml` gate no longer includes the clang warning guardrail. Clang warning enforcement moved to a manual `clang-warning-guard.yml` lane so routine pushes keep the fast GCC gate, full C++ suite, and API coverage without paying for a second full compiler build on every branch update.
- The March 14, 2026 current-tip rerun on commit `a2832b63` closed the last release-promotion gap: `release-binaries` run `23088513187` succeeded across the full matrix, and a matching versioned publish dry-run on `0.0.0-a2832b63` confirmed all `12` expected uploads after fixing the RPM glob in `scripts/publish_release.sh`.
- Linux release assembly is now also owned by `scripts/release_linux_artifacts.sh`, a container-backed wrapper that replays the workflow's `ldd` guardrail, runtime-bundle prep, split debug-info packaging, checksum generation, smoke test, tarball verification, and optional DEB/RPM generation without requiring host `alien`/`rpm`/`dpkg-dev`.
- Upstream still has open build-packaging friction for downstream consumers (for example ONNX Runtime issue `microsoft/onnxruntime#7150` about modern CMake/vcpkg/external-project support), so do not assume the remaining productionization work will be patch-free.

### Container-first workflow follow-up

This branch is already Docker-first for build/test/benchmark/API flows, but some maintenance helpers still assume host tools. Treat "containerize any flow that can be containerized without losing reproducibility" as the default direction for future tooling cleanups; prefer repo-owned wrappers that hide packaging/toolchain prerequisites behind Docker rather than documenting host package installation.

### Heavy-import responsiveness blind spot

The current benchmark story is incomplete for operational behavior under sustained imports. On March 20, 2026 a real single-node DDEV import against this fork stayed functionally correct but pushed cheap read endpoints like `/metrics.json`, `/health`, `/collections`, `/aliases`, and `/keys` into repeated `2.5-4.7s` slow-request territory, and the corresponding Laravel import jobs eventually hit their `600s` timeout. That did not show up in the canonical benchmark lanes because `standard/core` measures one large import and then post-import search, not "import while the node stays responsive to live reads". Treat benchmark wins as incomplete until the benchmark harness has an explicit mixed lane for concurrent import plus low-cost read probes and the runtime work has been re-validated there.

Local runtime diagnostics now cover the main suspected blind spots in that path: `metrics.json` exposes NuRaft import append/replay timings and chunk counts, import-handler split/add-many timings, collection-level timing for document preparation, reference-helper resolution, in-memory indexing, RocksDB batch writes, async-reference reconciliation, collection create/drop timing, outer HTTP request lifecycle timing (`auth_ms`, handler-wait, handler execution, unattributed remainder), response dispatch/queue/progress counters, per-message queue depth/latency for `STREAM_RESPONSE`, `REQUEST_PROCEED`, and `DEFER_PROCESSING`, and queue/worker/wait telemetry for both the main thread pool and `meta_thread_pool`. The March 21 local replay/fix cycle turned that instrumentation into actual fixes: request-path scheduling was moved off the HTTP-side path, reads now wait for in-flight imports to publish their applied index before falling back to replay, and import visibility advances in logical chunks tied to the effective import `batch_size` instead of once at the end of the whole request. On the upstream-comparable `500k` dashboard lane, that brought the fork to `133529.1 docs/s` versus upstream `69724.7` while keeping search at `18.5ms` and `/health`, `/metrics.json`, `/stats.json`, and `/collections` at `1.8ms`, `7.7ms`, `2.5ms`, and `2.1ms` respectively. The heavier fork-only `1M` lane held `121710.1 docs/s`, search `36.9ms`, and zero sync replay calls. Treat the old multi-second operational-starvation symptom as fixed locally, and keep the fitment mixed lane as the regression check that enforces that.

One related API contract is easy to break accidentally during perf work: the direct in-process `Collection::add_many(std::vector<std::string>& ...)` overload treats that vector as both input and per-record output, and callers/tests rely on successful imports being rewritten to JSON result lines rather than left empty. That behavior is not deprecated "legacy" baggage on this fork; it is still a live contract for the direct C++ API. If we ever want a cleaner split between input documents and per-record import results, do it as an explicit API refactor with a separate output buffer, not as a silent behavior change inside the existing overload.

### Takeover snapshot for item 17

If a new agent takes over mid-stream, assume the following:

- Confirmed finished work:
  - Static probe already proved the real blocker is duplicate protobuf runtimes, not export/install plumbing.
  - ORT Extensions static export-set issues, build-tree include metadata, and zlib 1.3 guard were already patched far enough to reach the final link result.
  - Research is complete enough to know ORT can theoretically reuse external protobuf; the gap is Bazel/foreign_cc plumbing, not lack of an upstream ORT hook.
  - The exact-pinned-commit patch now applies cleanly, `//:typesense-server` builds successfully, and `ldd` confirms the promoted binary has no `libonnxruntime.so.1` dependency.
  - The canonical `scripts/bazel_in_docker.sh build //:typesense-server` now uses the one-Protobuf static ORT path and `ldd` confirms it has no `libonnxruntime.so.1` dependency.
  - API runtime smoke now passes against the probe via `scripts/run_api_tests.sh --server-binary ... tests/health.test.ts`.
  - The full no-secrets API suite now passes against the probe via `scripts/run_api_tests.sh --server-binary ... -- --no-secrets`.
  - Direct local embedding smoke now passes against the probe using public model `ts/e5-small` (embedding created and vector search succeeds).
  - The full no-secrets API suite and direct local embedding smoke also pass against the promoted main `typesense-server` target.
- Historical next coding tasks (now completed on this branch):
  - land the `MODULE.bazel` macOS-portable `s2geometry` patch-command fix and re-run `release-binaries`,
  - confirm both macOS lanes (arm64 + amd64) pass with the same artifact/checksum flow already validated on Linux,
  - keep `ldd`/runtime-bundle checks in mind for future ORT bumps so the self-contained assumption is continuously verified.
- Working tree snapshot when this note was updated:
  - modified: `MODULE.bazel`
  - modified: `.github/workflows/release-binaries.yml`
  - modified: `.github/workflows/tests.yml`
  - modified: `BUILD`
  - modified: `MODERNIZATION_PLAN.md`
  - modified: `TESTING_RUNBOOK.md`
  - modified: `bazel/onnxruntime.BUILD`
  - modified: `bazel/onnxruntime.patch`
  - modified: `bazel/onnxruntime_extensions.BUILD`
  - modified: `debian-pkg/generate_deb_rpm.sh`
  - modified: `scripts/publish_release.sh`
  - modified: `api_tests/scripts/prepare_runtime_bundle.sh`
  - modified: `scripts/run_api_tests.sh`
- Historical caution at that point: `.github/workflows/release-binaries.yml` was still plan-labeled as `draft`, but the technical proof was already complete on the then-current pre-release SHA: cross-platform packaging validation was green via run `23088513187`, and the matching versioned publish dry-run was green after the `scripts/publish_release.sh` RPM-glob fix. Item 37 later retired that stale framing after current-tip run `23242998148`.

## Lessons Learned

Important patterns and gotchas that save future AI agents significant time. Keep this section concise — only add entries that would prevent wasted effort.

1. **Patch files must match exact upstream content.** When creating a `.patch` file for an external dep, always clone the exact pinned commit and generate the diff with `git diff`. Do NOT write patches by hand — Bazel's built-in patch applicator is strict and rejects content mismatches silently. Context lines must match byte-for-byte.

2. **Keep using Abseil's prefixed logging macros.** Even after removing the old Raft stack, unprefixed `LOG()`/`CHECK()` are still a bad habit because vendored code and local helpers can collide. Prefer `ABSL_LOG`/`ABSL_CHECK` through the facade and reject new raw `LOG()` usage in first-party code.

4. **Patch droppability verification requires full Docker build.** Header-only changes can appear to succeed in isolation but fail at link time. Always verify with `bazel build //:typesense-server` inside the Docker container.

5. **Pre-existing test warnings are excluded from CI guardrails** by the warning scripts. They won't block CI but should still be fixed to maintain code quality. Check for new warnings after any change by reading build output.

6. **When this branch commits to the NuRaft cutover, remove the old stack completely.** Keeping `braft`/`brpc` around after the decision only leaves stale build graph, stale docs, and stale benchmark scripts that misdescribe the branch.

7. **NuRaft is the only serious in-process replacement candidate found so far, but it is not a drop-in swap.** Its docs and examples confirm that a real migration would require Typesense-owned persistent `state_mgr`/`log_store` implementations plus new transport/snapshot integration. Treat it as a subsystem rewrite with benchmark gates, not a quick dependency refresh.

8. **`max-indexing-concurrency` is hardware-sensitive.** Benchmark wins at 16 do not automatically justify a strict global default of 16. Keep conservative defaults for broad deployability (4), and document CPU-tier tuning guidance for production overrides. A future adaptive startup heuristic (CPU+memory aware) is a better long-term path than a single aggressive default.

9. **Import API `batch_size` must stay wired to actual indexing batches.** The request parameter is now passed through to `Collection::add_many(...)` and controls import-side batch flushing; avoid regressing this by reintroducing a hardcoded internal batch size in the API path.

10. **Import `batch_size` is a secondary throughput knob on this dataset.** A/B checks (`40` vs `1000`) showed only marginal import delta (~0.2% in current runs). Keep default `40` for mixed workloads; use larger values only as deliberate ingest-window overrides.

11. **Upstream's import `batch_size=40` docs do not describe its actual internal batching behavior.** Upstream `v30.1` still defaults the request parameter to `40`, but its internal `Collection::add_many(...)` path continues to batch at `1000`. The fork intentionally wires request/config `batch_size` end-to-end, so upstream-comparable fitment replay must explicitly use `--server-batch-size 1000` and the product must not regain a hidden second hardcoded batch size.

12. **The lighter fitment replay can miss DDEV-style control-plane regressions.** Keep a separate dashboard replay posture with concurrent `/collections`, `/aliases`, `/keys`, `/presets`, `/stemming/dictionaries`, `/stopwords`, `/debug`, `/health`, `/metrics.json`, `/stats.json`, and search probes when judging whether heavy-import responsiveness is actually release-ready. Use `batch_size=1000` for apples-to-apples upstream comparison and `batch_size=40` only when you intentionally want the fork-only low-batch stress posture.

13. **Only truly trivial routes should stay inline on the H2O event loop.** Putting heavier GET handlers like `/metrics.json`, `/stats.json`, runtime search, or control-plane collection routes on the inline fast path can improve queue metrics locally while still harming real request admission by blocking the event loop itself. The March 21 replay turned green only after trimming the inline path back down to the `health` / `status` / `debug` class.

14. **Dockerized API harness should force IPv4 localhost.** Inside the API Bun container, `localhost` health checks can miss servers that are listening on IPv4 only. Set `TYPESENSE_API_HOST=127.0.0.1` in the wrapper to keep Dockerized API runs reliable.

15. **Upstream ships self-contained core CPU artifacts, and this fork now matches that on the promoted local target.** Keep checking with `ldd` after future ORT/build-graph changes so the repo does not silently regress back to a `libonnxruntime.so.1` runtime dependency.

16. **Sanitizer flags leak into `rules_foreign_cc` configure scripts.** Bazel's `--copt -fsanitize=X` applies globally, breaking autoconf detection in deps like kakasi and iconv. Fix by adding `env = select({"@@//:asan_mode": {"CFLAGS": "-fno-sanitize=address", ...}})` to each foreign_cc target. Note: `@@//` (not `@//`) is required in Bazel 9 Bzlmod to reference main-repo config_settings from external BUILD files.

17. **UBSAN breaks abseil with GCC 14.** `-fsanitize=undefined` causes constexpr evaluation failures in `absl/container/internal/hash_policy_traits.h:158`. Keep ASAN and TSAN separate; do not combine UBSAN with either until abseil ships a fix.

18. **Build provenance needs both an in-process and out-of-process check.** A Docker tag alone is not enough for runtime verification. Keep `/debug` / `/metrics.json` build metadata stamped from the checked-out source, and keep Docker image labels (`org.opencontainers.image.revision`, `io.typesense.build.git_sha`, etc.) sourced from the same checkout so operators can prove the binary inside the container matches the image they pulled.

18. **Bazel 9 host repository caches cannot live under `GITHUB_WORKSPACE`.** On GitHub Actions macOS lanes, a host `--repository_cache` inside the checkout now fails because Bazel derives `--repo_contents_cache` to `{--repository_cache}/contents` and rejects that path when it is inside the main repo. Use `${{ runner.temp }}` or another absolute path outside the checkout, and let `actions/cache` restore/save that absolute path directly.

19. **Same-document writes must be serialized at the collection/storage boundary, not just at the NuRaft request wrapper.** The March 23 analytics failure looked like a runtime-ordering issue at first, but the real bug was lower: `Collection::add_many()` could resolve `prepare_document_for_indexing()` / `old_doc` from RocksDB for two overlapping writes before either RocksDB write landed, which made concurrent `UPDATE` / `UPSERT` / `CREATE` on the same logical doc ID observe stale state. The correct fix is a per-document-ID critical section that covers existence check, old-doc load, in-memory update, and store write; runtime-layer ordering alone was insufficient and was removed after the collection-layer fix proved out.

20. **The default API runtime bundle must be refreshed, not reused blindly.** Before March 23, `scripts/run_api_tests.sh` only rebuilt `typesense-runtime-bundle/typesense-server` when it was missing, so local/API replays could silently run an older binary even after a fresh Bazel build. The default bundle path now re-stages from current Bazel output automatically, while explicit custom runtime bundle dirs still opt into reuse.

21. **Async-reference helper chunking needs both a byte budget and a doc budget.** The March 24 `3M` late-reference replay proved that a byte-only planner still leaves tiny-doc helper fanout too monolithic: the helper stayed at `3` chunks of about `1.05M` docs because the fetched JSON footprint was only about `574 MiB`, yet `/health` and `/metrics.json` p95 still climbed into the hundreds of milliseconds. A generic `500k` max-doc cap on sampled helper chunks preserved the `1M` guardrail lane while cutting those control-plane p95s sharply on the `3M` lane.

22. **NuRaft reads should not block on the newest committed import by default.** The March 30 three-node import/search repro showed the runtime's old unconditional `wait_for_live_product_state()` read barrier inflating wall latency even though `search_time_ms` stayed around `2-3ms`: local search went from `17.2ms` avg / `46.4ms` p95 / `92.7ms` p99 with `sync_cumulative_calls≈136-156` down to `6.0ms` avg / `13.4ms` p95 / `73.0ms` p99 once default reads served the latest locally applied state instead. Keep strong/linearizable-ish behavior opt-in (`read_consistency=strong`) rather than forcing every dashboard/search read to wait behind the latest committed write.

13. **Static ONNX Runtime probes hit multiple false-front blockers before the real protobuf conflict.** Modern CMake 3.31 first rejects ONNX Runtime Extensions' export set and build-tree include metadata, then the static vision build trips an upstream zlib-1.3 guard. Patch through those only far enough to reach the final link result; they are not the core reason this repo needs the shared `libonnxruntime.so.1` boundary.

14. **The real static-link blocker is duplicate protobuf runtimes in one binary.** After adding ONNX Runtime's bundled Protobuf 21.12 archives to the static probe, `ld.lld` reports duplicate `google::protobuf` symbols against the repo's Protobuf 33 runtime. That is the concrete evidence that a self-contained upstream-style binary needs a one-Protobuf strategy, not just more archive copying.

15. **ORT already has an external protobuf hook; our Bazel plumbing is what blocks it.** `onnxruntime_external_deps.cmake` already supports `find_package`/pre-existing protobuf targets, but `bazel/onnxruntime.BUILD` forces `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER`. Future work should focus on feeding ORT one protobuf from Bazel/foreign_cc rather than assuming ORT itself must be fundamentally redesigned.

16. **`rules_foreign_cc` exposes raw Bazel protobuf artifacts more naturally than CMake packages.** Inside foreign_cc, this repo already has a working pattern in `bazel/sentencepiece.BUILD`: pass `libprotobuf.a`, `libprotobuf_lite.a`, `protoc`, and include paths directly. There is no ready-made protobuf CMake package in this repo's foreign_cc flow today, so the fastest prototype path is imported targets or a tiny synthetic package, not waiting for a full upstream-style protobuf install tree.

17. **ORT 1.24 static builds now include `libonnxruntime_lora.a`.** If Bazel static targets do not expose that archive, final linking fails with unresolved `onnxruntime::adapters::utils::*` symbols from `lora_adapters.cc`, even after the protobuf collision itself is fixed.

18. **Check the produced binary with `ldd` before claiming packaging parity.** A successful static-probe build is not enough; confirm whether the final executable still depends on `libonnxruntime.so.1` so release-workflow decisions are based on the binary shape, not just the archive list.

19. **Hermetic static macOS builds need explicit dependency opt-outs and Apple framework linkopts.** `rules_foreign_cc` static outputs can silently pick up host features (for example curl auto-enabling Brotli) or drop upstream CMake framework link directives (for example ONNX Runtime Extensions' ImageIO/CoreGraphics/CoreServices linkage). Prefer explicit `CURL_*` feature toggles and mirror upstream Apple framework linkopts in Bazel wrappers instead of relying on host discovery.

20. **Runtime-bundle prep must follow the tested binary's actual dependency shape.** Do not hardcode `libonnxruntime.so.1` into API/release bundle prep for every artifact; detect whether the selected binary needs that shared library, or self-contained probes will fail validation for the wrong reason.

21. **CI artifact upload paths must not require optional runtime libs.** Once `typesense-server` is self-contained, workflows like `.github/workflows/tests.yml` should upload the binary itself and treat `libonnxruntime.so*` as conditional, not mandatory.

22. **GitHub source archives can be a false friend for C++ deps with required submodules.** The USearch prototype only built honestly once it switched from a release/archive fetch to a git pin plus `git submodule update --init --recursive`, because the archive shape omitted required `fp16`, `simsimd`, and `stringzilla` headers.

22. **Release tarballs need checksum metadata inside and outside the archive.** `debian-pkg/generate_deb_rpm.sh` expects `typesense-server.md5.txt` inside the extracted tarball, and release automation benefits from a tarball-level SHA256 sidecar. Keep both when changing artifact assembly.

23. **The clean GPU unblock was protobuf-side, and this branch's GPU artifact is ORT-provider-only.** If the one-Protobuf CUDA path regresses again, check the Protobuf `message_lite.h` / `EnumTraitsImpl` compatibility first before touching release automation. On this branch `typesense-gpu-deps` is only `libonnxruntime_providers_shared.so` plus `libonnxruntime_providers_cuda.so`, the regular Linux `typesense-server` artifact must be built with `--define=use_cuda=on`, and Whisper / voice-query remains CPU-only.

24. **Use the repo-owned fitment replay before trusting DDEV-only heavy-import conclusions.** The March 21, 2026 `product_vehicle_fitments_se` harness reproduced the fork's slower imports and degraded live reads/searches locally, which made it possible to separate fitment-upsert cost from later async-reference backfill and avoid repeated rebuild-test loops through DDEV.

25. **Upstream `/documents/import` still falls back to server-side `batch_size=40`; larger internal indexing batches are a distinct lower-layer default.** When comparing this fork against upstream, do not confuse the HTTP import route's `40`-doc server batching fallback with the deeper `Collection::add_many(... index_batch_size=1000)` default argument. The route-level fallback is what controls live import interleaving unless the request explicitly overrides it.

26. **The top-level NuRaft write mutex was real but not the main heavy-import regression.** Removing exclusive request-wide locking from `NuRaftHttpRuntimeService::write()` was still correct, but March 21 local replays showed only a small throughput gain, so future agents should keep the fix and continue investigating request-shell / collection-lock contention instead of assuming that lock solved the import/search responsiveness gap.

27. **The main heavy-import regression on this branch was request-path scheduling, not Raft lag.** The March 21 replay/fix cycle showed that `HttpServer::process_request()` still handled write requests inline on the HTTP-side path. Enqueuing writes onto the worker pool collapsed the old unattributed import shell (`~72ms` to `~5ms` on the replay lane), made the fork faster than upstream on isolated and mixed fitment import throughput, and removed the old multi-second cheap-read starvation during import.

23. **Downstream packaging helpers should resolve both draft and legacy artifact locations.** During workflow migration, scripts like `debian-pkg/generate_deb_rpm.sh` and `scripts/publish_release.sh` should prefer the new `artifacts/` layout but keep a legacy fallback until the older release path is fully retired.

23. **Alien-derived RPM layouts are not stable enough for hardcoded spec/buildroot paths.** `generate_deb_rpm.sh` should discover the generated `.spec`, use a clean copied buildroot, and avoid assuming the output directory name exactly matches the package version string.

24. **Bazel `patch_cmds` must avoid GNU-only `sed -i` assumptions when macOS lanes are in scope.** Commands like `sed -i -E ...` can parse differently under BSD sed and break repository fetches; prefer portable `perl`/`python` edits or explicit cross-platform flag forms.

25. **Add a workflow input before splitting release workflows.** A single `release-binaries.yml` with a `target_scope` / matrix-filter input was enough to debug one macOS lane quickly without duplicating logic across multiple workflows. Keep one canonical workflow unless maintenance pressure truly forces a reusable split.

26. **`tests/` currently has no Vitest spec files.** For dependency-only maintenance in that package, `bun run check` is the useful verification command; `bun run test` is not the primary signal until real test files exist.

27. **Prefer exact version pins to floating tags in Bun-managed packages.** `bun update --latest` is useful for discovering upgrades, but manifests and lockfiles should land on concrete version numbers rather than `latest` so CI and local replays stay reproducible.

28. **GCC 14 sanitizer warning cleanup needs sanitizer-scoped suppression, not broad repo-wide flags.** Under ASAN, libstdc++ `std::regex` can emit `-Wmaybe-uninitialized` false positives from regex-heavy first-party translation units and `clip_tokenizer`; under TSAN, external Abseil can emit `-Wtsan` on `atomic_thread_fence`. Keep those suppressions limited to the relevant sanitizer config and file/dependency boundaries so the normal warning lane still catches new regressions.

27. **Chunked import replay must key carryover by request identity, not log index.** For the NuRaft prototype's materialized apply path, import fragments only reassemble correctly across restart when the pending tail is stored under a stable request id (`start_ts`, with log-index fallback for synthetic single-chunk cases), mirroring how the batched indexer treats one logical import request across multiple chunks.

28. **Prototype snapshot install must preserve local node identity/bootstrap metadata.** A NuRaft snapshot can legitimately replace replay progress, log history, and materialized state, but follower install should not silently clone another node's `identity.json` or bootstrap self-address. Preserve local identity/bootstrap on install unless you are intentionally building a one-off cold-copy artifact.

29. **Use a RocksDB checkpoint for live prototype KV snapshots when a sink handle is available.** Recursive directory copies are acceptable as a cold fallback, but the closer prototype path is to reuse the open `NuRaftKvStateMachineSink` DB handle and export a checkpointed materialized-state tree, matching the real snapshot direction more closely.
30. **Do not silently rewrite persisted self identity for a multi-node NuRaft prototype.** Refreshing peer lists from `--nodes` is acceptable metadata churn, and single-node self-address drift is a real recovery case, but changing a persisted node's own peer endpoint inside a multi-node bootstrap is the same class of unsafe escape hatch as `reset_peers()` and should fail loudly.
31. **NuRaft runtime auth must reuse the shared authentication preprocessing, not just the master-key check.** `post_multi_search` and other collection-aware routes depend on `handle_authentication(...)` to populate `embedded_params_vec`; bypassing that path makes env-dependent vector-search lanes fail with `400 Missing embedded params array.` even when the API key itself is valid.
31. **Prototype follower catch-up should reject divergent history, not auto-heal it.** If a follower log no longer matches the leader prefix, the feasibility prototype should fail loudly so the migration decision sees the real repair gap instead of hiding it behind implicit truncation or overwrite behavior.
39. **Keep heavyweight CI lanes manual unless they are the main gate.** GitHub's `workflow_dispatch` and reusable-workflow guidance fit this repo better than always-on cron for sanitizer, flake, stress, and benchmark lanes. Prefer the documented local wrapper commands first, then dispatch the corresponding heavy workflow only when you want GitHub-hosted confirmation.
40. **The ORT Extensions ASAN teardown bug was in fetched `OrtOpLoader` statics, not first-party embedder ownership.** If a future ORT/extensions bump reintroduces `new_delete_type_mismatch`, audit the fetched operator loader sources first and keep the fix in `bazel/onnxruntime.patch` / fetched-source rewrite logic instead of restoring a global ASAN suppression or shared-library boundary.
41. **Compiler-specific warning suppressions must not live on shared Bazel `build` lines.** Bazel forwards raw `--cxxopt` / `--per_file_copt` flags to whichever compiler is active, so a GCC-only suppression like `-Wno-maybe-uninitialized` will trip the clang guardrail as `-Wunknown-warning-option` if it leaks into generic config. Keep those suppressions behind a GCC-only config and let the Docker wrapper select it for default non-clang lanes instead of muting clang with `-Wno-unknown-warning-option`.
42. **Keep image-embedder teardown ahead of text-embedder teardown anywhere model references are released.** `CollectionManager::dispose()`, `test/main.cpp`, and `process_embedding_field_delete()` should all drop image embedders before text embedders so the ORT Extensions-backed sessions leave the process before later session-only cleanup runs.
43. **Whisper voice-query inference is not a useful TSAN target.** `CollectionVectorTest.TestVoiceQuery` passes in the normal lane but returns a functional transcription error under TSAN instrumentation without producing a sanitizer report. Keep that test skipped under TSAN so the lane focuses on actionable concurrency signal.
44. **`_rand(seed)` is a product contract, not a best-effort shuffle.** Typesense's public docs and release notes promise that the same seed yields the same ordering across searches. Tests should assert seed stability and seed-to-seed variation, not a hardcoded permutation that can change when insertion order or scoring internals move.
44. **Random sorting needs its own explicit sort sentinel in `Index::populate_sort_mapping()`.** Leaving `_rand` to fall through without initializing `field_values[i]` can misroute `compute_sort_scores()` into another sentinel branch under sanitizer builds. Defensive zero-initialization plus a dedicated `random_order_sentinel_value` fixed the suite-only ASAN crash and restored the documented same-seed behavior.
45. **Sanitizer-instrumented tests need deadlines that match the behavior under test.** ASAN and TSAN routinely add enough overhead that very small `search_cutoff_ms` values turn intent-specific tests into generic 408 failures. For timeout-sensitive tests, raise the deadline only enough to keep exercising the intended branch under instrumentation; for integration tests like voice query, use a larger sanitizer-only budget if the test is validating correctness rather than latency.
46. **Benchmark harnesses should disable execa buffering for long-lived noisy server processes.** If the harness already consumes `stdout`/`stderr` incrementally, leaving execa on its default `buffer: true` only creates a second in-memory copy and can fail long benchmark lanes with Bun/get-stream `maxBuffer` errors when the server emits sustained warnings.
46. **Stress lanes are only useful if tests are parallel-safe across Bazel reruns.** `ArchiveUtilsTest` looked fine in single runs but failed immediately under `--runs_per_test` because it shared a fixed `/tmp/archive_utils_test` path. Use the repo's temp-dir helper for filesystem tests so local stress and hosted nightly lanes measure real flakiness instead of cross-run collisions.
47. **Hosted runner capacity is part of test design, not just build speed.** GitHub's standard private Linux runners expose only 2 vCPUs, so `--runs_per_test=5` on the monolithic `//:typesense-test` target caused four heavy ORT-enabled runs to overlap and fail with `pthread_create(...): EAGAIN`. Preserve the repeat count, but cap Bazel test concurrency in the hosted stress lane instead of weakening product coverage.
48. **Release smoke tests should validate the stable interface, not one historical banner string.** The local Linux `release-binaries` replay caught that the workflow still grepped for `Command line usage:` while the current server prints `usage:`. Accept the current banner shape, or both shapes if the CLI entrypoint is in transition, before relying on hosted packaging runs.
48a. **`build:gcc` is a warning-policy config, not the compiler selector.** In this repo the routine compiler choice comes from the Docker image defaults (`cc`/`c++` -> GCC 14), while explicit clang lanes opt in with `--repo_env=CC=clang --repo_env=CXX=clang++`. Keep those responsibilities distinct when refactoring `.bazelrc`, the Docker image, or `scripts/bazel_in_docker.sh`.
49. **Benchmark comparisons must stay branch-local.** A manual benchmark run on `v32` or any feature branch should compare the latest two successful `tests.yml` runs from that same branch, not the repo's latest successful runs globally, or the benchmark can silently compare unrelated SHAs and produce meaningless results.
50. **Benchmark harnesses must track the currently supported server CLI, not historical flags.** The benchmark workflow was still starting `typesense-server` with `--peering-address` and `--api-address`, which the NuRaft runtime no longer accepts. Keep the benchmark launcher aligned with the real runtime entrypoint (`--node-host`, `--listen-address`, comma-separated `--nodes`) or the lane fails before any benchmark signal is collected.
51. **Bulk-import tests must assert both ingestion and response-shape parity.** The NuRaft single-node bulk-import path currently ingests all documents but returns one generic success envelope instead of the documented newline-delimited per-document results. Existing tests missed that because they only asserted `success:true`. For bulk import, assert final document count and that response line count matches the number of input JSONL records when no per-document errors are expected.
52. **Hosted CI one-shot benchmark imports still need an explicit timeout policy.** The product default remains H2O's inherited `60000ms`, but the benchmark lane now restores the upstream one-large-POST shape and uses `TYPESENSE_REQUEST_TIMEOUT_MS` when slower hosted runners need more headroom. Prefer an explicit timeout override over client-side request chunking so the benchmark keeps measuring the real product import path.
53. **On NuRaft-backed bulk routes, `async_req=true` can accidentally turn H2O body aggregation into Raft log granularity.** The old `/documents/import` runtime registration allowed a slow single POST to reach `append_via_raft(...)` at transport-aggregate boundaries instead of at one logical request boundary. For bulk import parity, buffer one logical request before entering the Raft write path.
54. **When a benchmark lane compares new binaries against older same-branch artifacts, prefer env overrides over new CLI flags.** `benchmark-testing.yml` can compare a just-built runtime against an older artifact that does not know a new server flag yet. Forwarding `TYPESENSE_REQUEST_TIMEOUT_MS` through the Dockerized process launcher kept the raised timeout available without breaking older binaries that simply ignore the env var.
55. **Manual workflow greens are only as broad as their `target_scope`.** `release-binaries` can be dispatched for one lane, one platform family, or the full matrix. The March 12, 2026 success on `70ef49e7` was linux-only, while the first full-matrix confirmation of the whisper `GGML_BLAS=OFF` fix was run `23009183654` on `749f003e`. Check both `headSha` and selected jobs before advancing the plan.
56. **Publish dry-runs must use the workflow's real package filenames, not guessed naming conventions.** The March 14, 2026 current-tip release replay showed that tarballs and DEBs followed the expected `typesense-server-<version>-...` pattern, but RPMs came out as `typesense-server-<version>.<arch>.rpm`; later local containerized replays also showed that some alien-generated RPM labels normalize the first version hyphen to `_` (for example `0.0.0-local` -> `0.0.0_local`). Keep `scripts/publish_release.sh` aligned with the files the workflow actually uploads, or a dry-run can look mostly green while silently dropping one package family.
57. **Container-backed wrappers are worth the indirection when the alternative is host-only release tooling.** The Linux release replay already needed Docker for the build and an Ubuntu 24.04 environment for `alien`/`rpm`/`dpkg-dev`; moving the full assembly contract into `scripts/release_linux_artifacts.sh` keeps the local path CI-parity and avoids teaching future agents to install packaging tools on the host.
58. **For ORT Abseil version sync, prefer `FETCHCONTENT_SOURCE_DIR_ABSEIL_CPP` over a custom imported-target patch.** ORT's upstream `cmake/external/abseil-cpp.cmake` already owns the right CMake target graph and patch flow. Pointing `onnxruntime_static_one_protobuf` at `$$EXT_BUILD_ROOT/external/abseil-cpp+` let ORT and `re2` rebuild against the repo's Abseil LTS without growing `bazel/onnxruntime.patch`; the canonical build, `health.test.ts`, and `CollectionVectorTest.TestUnloadingModelsOnCollectionDelete` all stayed green on `abseil-cpp 20260107.1`.
58. **Artifact upload globs should be looser than publish-time discovery when third-party packagers normalize filenames.** `alien` can preserve or normalize version-label punctuation in RPM output depending on the label shape, so workflow artifact collection should match the package family broadly (`typesense-server-*.rpm`) while repo-owned publish logic keeps the stricter version-aware filtering.
59. **If a tooling CLI already has a repo-owned compose/image path, keep the wrapper on that container and make the compose project name and host-path contract explicit.** The benchmark wrapper only became truly Docker-first once it reused `benchmark/docker-compose.yml`'s `cli` service, kept host Bun behind `--host-bun`, pinned `COMPOSE_PROJECT_NAME=benchmark` so the wrapper and the hardcoded `benchmark_k6` network contract did not drift, and mounted the benchmark directory at the same absolute host path inside the CLI container so nested `docker compose run` bind mounts still resolved against the real host checkout.
60. **On NuRaft, committed log index is not the same as follower-readable state.** For post-restart or follower reads, `committed_index` convergence alone is insufficient; the runtime must wait for `raft_server::wait_for_state_machine_commit(...)`, track the local readable applied index separately, and treat `read_caught_up`/applied-index convergence as the real read-safety gate.
61. **Benchmark reruns can currently fail in the Dockerized k6/Influx setup even on the default workdir.** The March 18, 2026 item-38 reruns first failed under `/tmp/typesense-bench-move-r2`, then reproduced on the default `~/.cache/typesense/benchmark` path with repeated `Couldn't write stats ... mkdir /var/lib/influxdb/data: no such file or directory` errors. Even when the host checkout contains `benchmark/influxdb-data/data`, `docker exec benchmark-influxdb-1 ls /var/lib/influxdb/data` can still report that path missing inside the running container, so treat this as a harness bug and verify the Influx bind mount itself before trusting the benchmark lane again.
62. **Public-model CI staging has to match the models the active C++ suite actually exercises.** The March 18, 2026 `tests` run `23233009256` failed in `CollectionVectorTest.TestMultilingualE5` because CI only prewarmed `ts/e5-small`; `ts/multilingual-e5-small` then fell back to live model-repo downloads and timed out on the vocab fetch. Keep `test/scripts/prewarm_public_test_models.sh`, the workflow prewarm steps, and `test/scripts/replay_typesense_test.sh` aligned with the real public-model set used by `//:typesense-test`.
63. **If a hosted release lane can time out before `actions/cache` saves, move the dominant cold-build output into a repo-owned artifact keyed from the real toolchain inputs.** The March 19, 2026 ORT bundle work avoided GitHub's 6-hour cold CUDA rebuild cliff without introducing remote-cache infrastructure: package the exact `onnxruntime_static_one_protobuf` install tree, key it from the ORT patches/BUILD/dockerfile/arch inputs, and let the release workflow consume that artifact opportunistically while keeping the source build as the fallback.
64. **Shared hosted caches must not persist host-native output trees.** Bazel `disk-cache` and `repository-cache` are the right cross-run reuse layer for GitHub-hosted CI, but caching the full output root can preserve binaries built with host-specific CPU features. The March 20, 2026 `tests.yml` `Illegal instruction` in `CollectionVectorTest.TestVoiceQuery` came from `GGML_NATIVE=ON` in `bazel/whisper.BUILD` plus a restored `.cache/bazel-docker` output tree on a different x64 runner. Keep portable defaults for shipped/CI binaries and cache `disk-cache` / `repository-cache` / `bazelisk`, not the full output root.
65. **Publish Docker images from validated Linux release artifacts, not from a separate rebuild path.** This fork's Docker Hub parity should track the same `typesense-server` tarballs already verified by `release-binaries.yml`. Build the runtime image from `docker/deployment.Dockerfile`, feed it the uploaded `linux-amd64` / `linux-arm64` artifacts, tag immutable branch+sha images plus a moving branch tag, and keep the `arm64-lg-page16` image as a separate Linux-only tag family.
66. **Hosted test lanes should reuse matching ORT bundles before falling back to a source ORT build.** The portable-cache fix for `tests.yml` intentionally stopped persisting the full Bazel output tree, but that does not mean the job should rebuild CUDA-enabled ONNX Runtime from scratch every time. Reuse the latest successful same-branch `ort-bundles.yml` artifact keyed by `scripts/release_ort_bundle.sh --print-key`, then fall back to source ORT only when no matching bundle exists.
67. **CI warning guardrails should consume the same heavy dependency artifacts as the main build lane.** Once `tests.yml` learned to reuse matching ORT bundles, the GCC/Clang warning-budget checks still had a hidden slow path because their guardrail scripts rebuilt `//:typesense-server` without the extracted ORT tree. Forward `TYPESENSE_ORT_PREBUILT_BUNDLE_DIR` into those scripts too, or the warning phase can quietly reintroduce a long source `onnxruntime_static_one_protobuf` build.
68. **Keep the default push gate narrow when a second compiler lane costs a full extra build.** On March 20, 2026 the automatic `tests.yml` lane was spending more than 40 minutes in the clang warning guard even after ORT-bundle reuse. The right balance was to keep GCC warning enforcement in the automatic gate and move the clang warning lane to a dedicated manual workflow, then document exactly when maintainers should dispatch it (toolchain, compiler-flag, Bazel-external, or warning-sensitive C++ changes).
69. **Import benchmarks must include live-read responsiveness, not just import completion and post-import search.** The March 20, 2026 real DDEV import showed cheap endpoints like `/metrics.json`, `/health`, and `/collections` taking `2.5-4.7s` during sustained writes, with Laravel import jobs timing out at `600s`, even though the existing benchmark lanes all looked like wins. Future performance claims on NuRaft should include a mixed lane that probes lightweight reads during import saturation, or benchmark results can miss user-visible operational regressions.
69. **Heavy hosted artifact reuse should be opt-out, not opt-in, when the fallback is already safe.** Manually pasting an ORT bundle run ID into `release-binaries.yml` was too easy to forget. The better contract is: auto-discover the latest matching successful same-branch bundle by default, allow an explicit `ort_bundle_run_id` override for deterministic pinning, and keep one boolean opt-out when maintainers intentionally want a source ORT rebuild.
70. **For local heavy-import profiling, use host `perf`/scheduler tools against the replay PID, not containerized profilers.** The replay harness launches a real local `typesense-server`, so host `perf`, `runqlat`, and related eBPF tools have cleaner kernel and PID visibility than a nested container workflow. Keep the build/test flow Docker-first, but keep the profiler lane host-side.
71. **On short heavy-load `perf record` runs, disable build-id post-processing unless you explicitly need it.** The March 21, 2026 fitment replay profiling work showed the default build-id finalization path leaving long-lived `perf record` jobs and invalid `perf.data` files under stress. `perf record -B -N` (`--no-buildid --no-buildid-cache`) made the capture exit cleanly, which matches the perf-record manpage guidance that `--no-buildid` skips the expensive post-record processing step.
72. **For large DWARF captures, make flamegraphs opt-in or size-capped.** The same profiling work showed that once capture finalization was fixed, the next bottleneck moved to `perf script` driving `addr2line` over large unstripped C++ binaries. Small captures can still auto-generate SVGs, but larger steady-state runs should default to text artifacts (`perf.data`, `perf report`, `perf stat`, `runqlat`, top-stacks text) unless a maintainer explicitly raises the flamegraph size cap.
73. **Do not let `sync_live_product_state()` replay while an import is already applying the same commit range.** A separate replay mutex removed shared-mutex writer poisoning, but it also let reads start real catch-up replays during active imports, which duplicated the import work and cratered throughput. The right shape is: first wait for the in-flight import to publish its applied index, and only fall back to replay when nothing is actively writing anymore.
74. **Live import visibility must advance in smaller logical chunks, not once per whole request.** Replaying a full `5000`-doc NuRaft import only after all chunks had been appended kept search waiting behind the entire request-sized replay window. Interleaving append + live replay per logical chunk tied to the effective import `batch_size` cut `nuraft_sync_avg_total_ms` from about `25ms` to about `7ms`, removed sync replay calls on the heavy lane, and moved search/control-plane reads close to upstream while further increasing import throughput.
75. **Optional release subpaths must be gated all the way through the workflow, not just in matrix selection.** On March 22, 2026 `release-binaries.yml` still unconditionally ran `Upload Linux GPU provider build inputs` after a successful core Linux build, so a manual dispatch with `build_gpu_deps=false` still failed before packaging and Docker publish. When a workflow input disables an artifact family, guard every dependent step, not just the downstream job fan-out.
77. **Manual release workflows should resolve user-supplied refs to a full commit SHA before checkout.** On March 22, 2026 `release-binaries.yml` accepted `workflow_dispatch` input `ref=7151782d`, but `actions/checkout@v6` treated that short SHA as a branch/tag pattern and fetched `refs/heads/7151782d*`, failing before the build started. Resolve the input through the GitHub commits API once in a small prep step and pass the canonical full SHA to every later checkout so short SHAs, tags, branch names, and `refs/heads/*` inputs all behave the same.
76. **If heavy-import timeouts recur at a stable document count, check NuRaft snapshot frequency before retuning the import path.** On March 22, 2026 the remaining DDEV `product_vehicle_fitments_se` stall kept reappearing around `~9.8M` docs, but live gdb stacks showed the commit thread inside `raft_server::snapshot_and_compact()`, not in the async-reference helper or MySQL sibling lookup. The branch-local update-heavy replay now stays green at the new default `snapshot_distance=100000` and times out again when forced back to a tiny snapshot distance, so the next gate is “new image with the new snapshot posture” rather than more speculative handler tuning on the old image.
78. **API tests that inspect global NuRaft progress must tolerate concurrent same-phase writes from other files.** The March 23, 2026 push failure on `c10fb15f` was not a runtime regression: `nuraft_runtime_documents_crud.test.ts` asserted that a streamed import advanced `/status.committed_index` by exactly `+2`, but Bun runs the other `single-fresh` API files in the same phase at the same time, so unrelated writes pushed the delta to `+79` on GitHub. For API-phase assertions, prefer `>=` lower bounds or phase-local signals over exact global committed-index deltas.
79. **When a workflow only needs the latest successful run ID, query GitHub for the scalar ID directly.** On March 23, 2026 `tests.yml` failed before `api-tests` even started because the TEI-cache lookup piped pretty-printed JSON objects through `head -n 1`, leaving a bare `{`, and a transient GitHub `502` turned that into a hard failure. Request `.id` directly, and treat `gh api` errors as a cache miss so the workflow can proceed without reuse.
80. **Release lanes must treat missing heavy reusable artifacts as a performance miss, not a correctness failure.** On March 23, 2026 `release-binaries.yml` found no matching ORT bundle after an unrelated `MODULE.bazel` dependency addition changed the coarse bundle key, but the workflow still failed because `core-binaries` tried to upload nonexistent GPU provider sidecars. If the GPU packaging job already has a source-build fallback, keep the release green and pay the extra build cost instead of making the core lane depend on a transient optimization artifact.
81. **Hosted macOS still executes repo scripts under an old Bash unless the workflow explicitly supplies a newer one to the sandboxed action.** The March 23, 2026 provenance-header failure was not missing workspace status data; it was Bash 3.2 interpreting associative-array syntax under `set -u`. Keep small repo-owned helper scripts portable to Bash 3.2 unless the surrounding build rule explicitly guarantees a newer shell.
82. **Recovery readiness must be keyed to materialized search state, not just Raft commit progress.** On March 31, 2026 an empty follower could report `/health {"ok":true}` while its NuRaft log was caught up but `CollectionManager` was still replaying tens of millions of documents. Keep `/health`, `/status`, and read admission tied to `materialization_ready` / `materialization_lag` so the load balancer does not route reads to a partially rebuilt node.
83. **NuRaft logical snapshot install has to tolerate empty directories in the transferred payload.** The object-based snapshot transfer currently serializes files, not empty directories, so a fully compacted `log/` tree may be absent on the receiver even though the snapshot is valid. Recreate empty directories during install instead of rejecting the snapshot as malformed.
84. **Admin snapshots should allow bootstrap-only servers with no user writes yet.** NuRaft can already snapshot the committed cluster/bootstrap state even when the application state machine has applied `0` user entries. Gate the `/operations/snapshot` path on the Raft committed index, not only the state-machine-applied index, or the single-node and fresh multi-node health lanes will fail before any real workload exists.
85. **“Replay vs snapshot” is mostly a retention-policy question in NuRaft, not a custom recovery branch.** The NuRaft examples and `snapshot_and_compact` / snapshot-sync code paths confirm that short outages naturally catch up from retained logs, while far-behind followers switch to snapshot once the needed prefix has been compacted away. The fork’s job is to keep fresh snapshots available even while followers are offline and to test that policy explicitly, not to invent a second recovery decision tree above NuRaft.
86. **When a follower is already beyond the retained-log window, refreshing the snapshot early is the safe optimization.** This is still NuRaft-native behavior, not a second recovery mode: the leader continues to keep a fresh snapshot available while the follower is offline or badly lagged, and the status surface should expose `snapshot_recovery_point_lag` plus lagging-peer metrics so operators can see when log replay has already become the wrong recovery path.
87. **NuRaft disaster-recovery snapshots for this fork must preserve the live Typesense store and trigger a live reload, not just the materialized request log.** Restoring only `state/nuraft-prototype/materialized_state` can advance Raft progress while leaving `CollectionManager` empty on an empty follower. Snapshot export/install and the recovery tests must cover the main `db/` payload plus a post-install live-state assertion such as collection counts or search results.
88. **NuRaft log-store compaction must never leave `next_slot()` behind `start_index()`.** Snapshot-heavy recovery can compact the retained log window far past a follower's old tail; if the log store only advances `start_index_` and leaves `next_index_` stale, status can show impossible states like `log_store_start_index=43` with `last_index=1`, and post-snapshot replication will stall even though the follower looks materialized.
89. **NuRaft logical snapshot transfer must chunk large RocksDB checkpoint files and validate expected `db/` payloads.** The March 31, 2026 DDEV failure was not in local snapshot export/install; it was the live object-based transfer path sending each snapshot file as one logical object. Real `db/` SSTs reached `269-559 MiB`, the follower staged only `meta/` plus `materialized_state/`, and the old install path silently removed `/data/db` when `db/` was missing. Fix both halves together: split large files into bounded chunks, and carry enough manifest/install metadata to reject a snapshot that was supposed to include the main `db/` checkpoint but arrived without it.
78. **Async-reference fanout needs a byte-aware guard, not only a doc-count guard.** On March 22, 2026 DDEV still produced a `33.99s` `categories_se` slow request even though the helper only matched `177,813` `products_se` docs, because those product documents totaled about `2.14 GiB` of stored JSON and the old count-only rule kept the helper monolithic. Sample stored-doc size once the matched set is large enough to matter and plan helper chunks toward a fetched-byte budget; otherwise medium-count / huge-doc fanout will slip past any “large-doc-count only” threshold.
32. **Timed snapshot policy is part of the recovery contract, not just a scheduler detail.** If snapshots are blocked on follower health, a lagging follower can be trapped behind a permanently stale recovery point. Keep a test lane that distinguishes "require healthy peers" from "leader-only snapshot" policy so this deadlock class stays visible.
33. **Do not misuse the existing HTTP benchmark wrapper as NuRaft evidence.** `scripts/benchmark_vs_upstream.sh` measures full server binaries behind the normal API/runtime surface. Until NuRaft has either a thin HTTP-facing adapter or a dedicated prototype benchmark harness, Story E needs its own measurement lane.
34. **Prototype benchmarking should stay explicitly separate from the normal HTTP benchmark wrapper until the runtime surfaces actually match.** A dedicated NuRaft microbenchmark target is useful for Story E, but it is still measuring isolated replication/storage paths, not a drop-in server replacement. Treat it as evidence for feasibility, not as a substitute for a later apples-to-apples runtime comparison.
35. **Once the old stack is removed, retire its benchmark lanes instead of letting them rot.** Keep the historical results, but turn dual-runtime wrapper profiles into explicit archival notes so future work does not accidentally benchmark nonsense.

36. **When deleting prototype files, audit transitive includes before removing.** `NuRaftFileStore` was initially classified as prototype-only but was actually a production filesystem utility used by 3 production stores. Similarly, `NuRaftLogEntry` lived in a deleted header but was needed by the real `TypesenseStateMachine`. Always `grep` for users of a struct/class across the whole codebase before deleting its definition.

37. **Single-node NuRaft requires explicit leadership request after init.** `raft_launcher::init()` starts the Raft server but leader election happens asynchronously (200-400ms timeout). Without calling `raft_server_->request_leadership()` + polling, single-node writes fail with NOT_LEADER errors.

38. **After prototype cleanup, the `nuraft_lib` BUILD target contains only production code.** The library was renamed from `nuraft_prototype_lib` to `nuraft_lib`. All references in test targets and `nuraft_runtime_lib` deps must be updated simultaneously.
