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
- Heavy auxiliary workflows are now manual-only by policy; `tests.yml` is the only automatic GitHub Actions gate and the local wrapper commands in `TESTING_RUNBOOK.md` are the preferred pre-push validation path.

## Quick Reference: How To Build And Test

`TESTING_RUNBOOK.md` is the canonical owner for build/test/replay/API command lines.

- Build server: `scripts/bazel_in_docker.sh build //:typesense-server`
- Run API suite: `scripts/run_api_tests.sh -- --no-secrets --download-migration-binary`
- Run benchmarks: `TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile standard --scope core`

Keep this section short and point to the owning docs instead of duplicating the full command matrix here.

**Critical build/test gotchas:**
- **Always use `scripts/bazel_in_docker.sh`** for builds, never raw `docker run ... bazel build`. The script handles image building, caching, user permissions, and bazelisk configuration.
- **Rebuild the Docker image after Dockerfile changes:** `scripts/bazel_in_docker.sh --build-image-only`. Stale images may run an older Bazel version (e.g., 8.6.0 instead of 9.0.0).
- **Clear stale bazel caches** if root-owned files block builds: `sudo rm -rf /tmp/typesense-bazel-cache-fork` or the configured cache dir.
- **Update the runtime bundle binary** before API tests: `run_api_tests.sh` only copies the binary if missing. After a rebuild, `rm typesense-runtime-bundle/typesense-server` before running tests.
- **Stop stale Docker containers** that may hold build locks or ports: check with `docker ps` before starting a new build/test run.
- **ICU dependency:** `nuraft_lib` requires `@icu` in BUILD deps because it transitively includes `string_utils.h` → `unicode/normalizer2.h`.

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

All workflows run on `ubuntu-24.04`. Only `tests.yml` runs automatically; the heavier lanes are manual on purpose so local Dockerized repro can happen before spending GitHub runner time.

| Workflow | File | Trigger | Schedule | Purpose |
|---|---|---|---|---|
| `tests` | `tests.yml` | push, manual | — | Primary gate: build + warning guardrails + C++ tests + API tests + TEI |
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
- [x] Dependency set is current, with patch debt minimized and documented exceptions only. *(5 active patches with justifications in `bazel/PATCH_DEBT.md`. The old `brpc`/`braft` patch stack is gone from this branch.)*
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
- [ ] Replace patch-only forks with released upstream versions where possible (5 active patches documented with next actions in `bazel/PATCH_DEBT.md`).
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
- [ ] Medium-priority candidates worth evaluating next:
  - ONNX Runtime `1.24.2` -> `1.24.3` is now done; canonical Docker build passed, `ldd bazel-bin/typesense-server` still shows no `libonnxruntime.so.1` dependency, and the Dockerized API `tests/health.test.ts` replay passed against the promoted binary.
  - `libarchive` `3.7.7` -> `3.8.5` is now done.
  - `snappy` `1.1.7` -> `1.2.x` for compiler/perf hygiene. *(Now done on `1.2.2`.)*
  - `typesense-js` in `tests/` `2.0.3` -> `3.0.2` is now done to match the benchmark toolchain client line; `bun run check` passes in `tests/` after the bump.
  - JS tooling baseline is now aligned on Bun `1.3.10` plus Node `24.14.0` LTS for optional host-side flows. `tests/` no longer carries pnpm metadata, benchmark wrapper invocations use Bun, and shared package pins were refreshed to the current stable lines (`eslint 10.0.3`, `typescript-eslint 8.57.0`, `vitest 4.1.0`, `openai 6.27.0`, `zod 4.3.6`, `@types/node 25.4.0`). `tests/src/error.ts` needed the expected Zod 4 compatibility fix (`error.errors` -> `error.issues`), and `bun outdated` is now clean in `api_tests/`, `benchmark/`, and `tests/`.
  - `abseil-cpp` `20250814.1` -> `20260107.1` attempted but **blocked by ORT ABI mismatch**: ORT internally builds `re2` against its own fetched abseil (lts_20250814), so linking fails when the external abseil uses a different LTS namespace tag. Fix requires injecting external abseil into ORT's CMake build (same pattern as the protobuf injection in `onnxruntime.patch`). Defer until ORT's abseil injection is implemented.
- [ ] Keep treating patch-debt reduction as at least as important as raw version bumps; some deps (for example `whisper.cpp`, `h2o`) matter more because of maintenance surface than because they are numerically old.
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
- [ ] Once the no-secrets wrapper is green, rerun the env-dependent suites with the correct setup instead of leaving them as “not yet tried”:
  - migration replay with a legacy binary,
  - TEI/embedding suites with the required model/service env,
  - secret-gated conversation flows.
- [ ] **Benchmark validation on NuRaft-only server (planned):** Run the existing k6 benchmark profiles (`benchmark/scenarios/`) against the NuRaft-backed `typesense-server` to establish a baseline. Compare against the historical braft numbers in `benchmark/BENCHMARK_RESULTS.md`. Focus on: write throughput (single-node), search latency under write load (mixed), import throughput, and multi-node write forwarding latency. Document results as the new NuRaft baseline in `BENCHMARK_RESULTS.md`. Keep historical braft data for reference but stop using old side-by-side lanes.
- [x] **Expose NuRaft configuration as CLI args and ENV overrides.** *(Done Mar 2026: all NuRaft `raft_params` are exposed as `--raft-*` CLI args and `TYPESENSE_RAFT_*` ENV vars in `typesense_nuraft_runtime.cpp`. Covered: heartbeat interval, election timeout bounds, reserved log items, client request timeout, auto-forwarding toggle and timeout, snapshot distance, leadership expiry, and Asio thread pool size. Defaults are production-sensible; ENV is applied first with CLI taking priority.)*

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

- [x] GCC and Clang warning guardrails at zero (`tracked=0`, `total=0` for both compilers).
- [x] CI enforces guardrails via `scripts/check_clang_warning_guardrail.sh` and `scripts/check_gcc_warning_guardrail.sh` with failure-artifact upload.
- [x] First-party C++ test signedness cleanup committed (`e9bea816`); all `-Wsign-compare` warnings resolved.
- [x] Unused-variable warnings across test files and src cleaned up (`8420f815`).
- [ ] **Tighten third-party warning suppression:** Current build emits warnings from external deps during sanitizer builds (and some in normal builds). Goal: zero warnings visible during any build config. Known sources:
  - `protobuf`: `-Wsign-compare` in repeated field accessors (suppressed by `--per_file_copt` for normal builds; verify sanitizer builds too)
  - `abseil-cpp`: deprecated C++20 implicit lambda capture of `this` in `container_internal` headers
  - `abseil-cpp`: TSAN `atomic_thread_fence` warning in `synchronization/internal/graphcycles.cc`
  - Bazel/JVM startup: deprecated `-Xverify:none` / `-noverify` banner from the Java runtime used under Bazel/Bazelisk
  - Approach: use the narrowest possible suppression or config fix for each source. Prefer per-file or per-external-target compiler flags, or tool-specific startup config, over global `-w` / broad `-Wno-*` flags. Do not hide first-party warnings or future new warnings from third-party code that have not been audited yet.
- Done when:
  - [x] Warning debt trend is downward and enforced by CI.
  - [ ] Zero warnings visible in normal, ASAN, and TSAN builds.

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
  - ~~`actions/download-artifact` `@v5` → `@v4`~~ done (tests.yml normalized to @v4).
  - ~~`actions/setup-node` `@v3` → `@v4`~~ done (benchmark-testing.yml).
  - ~~`dawidd6/action-download-artifact` `@v2` → `@v6`~~ done (benchmark-testing.yml).
  - `oven-sh/setup-bun` `@v2` — already on latest major.
- [x] Keep `flake-detection.yml` available as the dedicated manual stress workflow for source/build-system changes.
- [x] Bazel disk cache persistence via `actions/cache@v4` across all CI workflows.
  - Caches `${{ github.workspace }}/.cache/bazel-docker` (disk-cache + repository-cache + bazelisk).
  - Key: `bazel-{os}-{workflow}-{hashFiles('MODULE.bazel','BUILD','.bazelrc','bazel/**')}` with prefix restore-keys.
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
# Prerequisites: Docker, Bun 1.3+, a built typesense-server binary

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
| `docker/ci-bazel.Dockerfile` | CI Docker image definition |
| `test/temp_dir_utils.h` | Per-test-instance temp directory isolation |
| `test/scripts/prewarm_e5_small_model.sh` | Model cache warmup for embedding tests |
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
| 1 | ~~Coordinated h2o/picotls/quicly bump~~ | P1.7b | **done** | All 3 deps bumped, patch regenerated, build verified. |
| 2 | ~~ICU fork→upstream tarball~~ | P1.7 | **done** | Switched from `typesense/icu` fork to official ICU 71.1 release tarball. Patch unchanged. |
| 3 | ~~CI action version bumps~~ | P2.11 | **done** | `actions/checkout` v4→v6, `actions/upload-artifact` v4→v7 across all 5 workflows. |
| 4 | ~~ICU version upgrade (71→78)~~ | P1.7 | **done** | Upgraded to ICU 78.2 (Unicode 14→17, CLDR 48). Patch regenerated. |
| 5 | ~~Patch debt reduction (whisper)~~ | P1.7 | **done** | Reduced from 11 hunks to 8, then to 1 hunk via v1.8.3 upgrade. |
| 6 | ~~CI hygiene backlog (remaining)~~ | P2.11 | **done** | Bazel disk cache via `actions/cache@v4` in all workflows. |
| 7 | ~~Benchmark infrastructure audit~~ | P2.13 | **done** | Stack audited, local execution documented, cross-fork comparison feasible. |
| 8 | ~~Fix pre-existing test warnings~~ | Known Issues | **done** | Narrowing + trigraph warnings fixed. |
| 9 | Protobuf 34 upgrade | P1.6b | **done** | Upgraded from 33.5 to 34.0.bcr.1. Updated MODULE.bazel and onnxruntime.BUILD version strings. Clean build, 120/120 API tests pass, all 4 NuRaft unit tests pass. No code changes required — protobuf is only an indirect dependency via OnnxRuntime and SentencePiece. |
| 10 | ~~Cross-fork benchmark script~~ | P2.13 | **done** | `scripts/benchmark_vs_upstream.sh` for upstream comparison. |
| 11 | ~~Definition of Done audit~~ | DoD | **done** | All 6 checkboxes verified and marked complete. |
| 12 | ~~Fix Bazel 9.0.0 not running in Docker~~ | P1.6 | **done** | Verified with `scripts/bazel_in_docker.sh version` and `TYPESENSE_BAZEL_IMAGE=typesense/ci-bazel:ci scripts/bazel_in_docker.sh version`: Bazelisk `v1.28.1`, Bazel `9.0.0`. CI workflows already run `--build-image-only`; stale local images were the mismatch source. |
| 13 | RocksDB perf tuning Phase 3 (data-driven) | P2.13 | **done** | Runs 9-13 complete: observability, sweeps, read-path optimizations, `max-indexing-concurrency` validation, and import `batch_size` A/B check. Final policy keeps conservative defaults with hardware-based tuning guidance. |
| 14 | ~~Benchmark observability: full metrics collection + Grafana dashboard~~ | P2.13 | **done** | Core observability is in place: benchmark runs collect system/API/RocksDB metrics continuously and dashboard includes concurrent search+import visibility. Tuning-specific counter extraction is tracked under item 13. |
| 15 | ~~JS/Docker workflow consolidation~~ | P2 DX | **done** | Benchmark/API tooling is Bun-first, benchmark CI now uses the shared wrapper, and API tests have a Dockerized wrapper entrypoint. |
| 16 | ~~Static ONNX Runtime linkage probe~~ | Known Issues | **done** | Promoted `typesense-server` to the one-Protobuf static ORT path. `ldd bazel-bin/typesense-server` shows no `libonnxruntime.so.1`, the no-secrets API suite passes (including migration replay), and direct local `ts/e5-small` embedding/vector-search smoke succeeds. |
| 17 | ~~Release packaging / multi-arch workflow hardening~~ | Known Issues | **done** | Full draft workflow validation is now green across `linux-amd64`, `linux-arm64`, `darwin-arm64`, and `darwin-amd64`, including Linux DEB/RPM generation and Darwin tarball validation. The workflow still says `draft`, but the remaining work is promotion/cleanup, not technical break-fixing. |
| 18 | Dependency refresh audit (current vs latest) | P1 Build/Deps | **done** | All actionable deps refreshed: magic_enum 0.9.7, libarchive 3.8.5, snappy 1.2.2, ORT 1.24.3, typesense-js 3.0.2, protobuf 34.0.bcr.1. Core infra deps (curl 8.18.0, openssl 3.6.1, jemalloc 5.3.0, zstd 1.5.7, lz4 1.10.0) all confirmed at latest. Patch debt: 5 active patches at minimum, h2o reduced to 48 lines. Abseil upgrade blocked by ORT ABI. whisper.cpp upgraded to v1.8.3 (patch down to 1 hunk). |
| 19 | NuRaft replacement cutover and hardening | P1.7d | **done** | Real NuRaft consensus is the only path. All 120/120 API tests pass (0 failures). Prototype code deleted, CLI/ENV config exposed, analytics counter bugs fixed, snapshot identity fixed. Remaining follow-ups (async/streaming, route audit, env-dependent suites) tracked as unchecked items in P1.7d. |
| 20 | Sanitizer lane stabilization + ORT extensions boundary audit | Known Issues | **in progress** | Local-first stabilization work is now broadly validated on GitHub: `tests`, `sanitizer-testing`, `nightly-extended`, and `flake-detection` are all green on the current `v32` line, and the narrow `release-binaries` linux-amd64 lane is also green after fixing its stale help-banner smoke assertion. Root cause for the prior suite-only `_rand(seed)` failure was a missing random-sort sentinel in `populate_sort_mapping()`, which left `field_values` undefined and could route scoring into the wrong branch under ASAN. The hosted-only sanitizer reds were latency-budget mismatches, not new sanitizer reports: `CollectionSpecificMoreTest.SearchCutoffTest` needed a slightly larger TSAN-only cutoff to exercise `search_cutoff` instead of returning a hard 408, and `CollectionVectorTest.TestVoiceQuery` needed a larger ASAN search deadline because the test validates voice-query integration, not timeout behavior. The nightly local blocker was a parallel-safe test bug in `ArchiveUtilsTest`, which used a shared `/tmp/archive_utils_test` directory across `runs_per_test` invocations; switching it to the repo temp-dir helper removed the collision. The hosted nightly failure was also workflow shape, not a product regression: GitHub's standard private `ubuntu-24.04` runner only provides 2 vCPUs, and Bazel was launching four `typesense-test` stress reruns concurrently, which exhausted ONNX Runtime thread creation (`pthread_create failed`, `EAGAIN`). The workflow now keeps the same 5x stress signal but caps Bazel to `--local_test_jobs=1` for that hosted lane. The benchmark lane fix also split cleanly into five parts: workflow drift (stale server CLI flags) is fixed; the NuRaft import path now delegates `kDocumentImport` through the real registered async handler instead of the mirror-plus-synthetic-response path, restoring documented `200` plus per-document newline-delimited results locally; hosted benchmarking now uses smaller client-side indexing chunks on GitHub so it stays inside the inherited upstream `60s` H2O request timeout without changing product behavior; the benchmark harness now uses k6's `handleSummary()` hook for the indexing lane to emit a machine-readable `import_duration` summary directly to stderr, which avoids both Influx timing gaps and Docker bind-mount permission issues from file-based summary export; and the indexing benchmark now uses an explicit `per-vu-iterations` scenario with a much larger hosted-CI `maxDuration` so k6 does not ignore the cap or terminate the single import iteration at its default `10m` ceiling before checks and Trend values are finalized. The hosted replays proved that both `20m` and `30m` were still too tight: the `20m` run hit k6's end-of-test summary at `09:06:48Z` after starting at `08:46:10Z`, and the later `30m` run still timed out at `10:05:58Z` after starting the indexing phase at `09:35:20Z`. The local self-compare replay on the latest branch tip completed successfully, emitted the normal ASCII benchmark graphs, and indexed all `1,000,000` documents with zero response-contract warnings, which means the remaining red was not a local benchmark-path bug. The next root cause was workflow semantics: a `workflow_dispatch` benchmark launched in parallel with `git push` can run on the previous branch tip, so the repo now treats benchmark selection as "benchmark this workflow SHA if and only if it already has a successful `tests.yml` artifact; otherwise fail fast." The workflow now prints the checked-out SHA and refuses to benchmark stale artifacts by default, instead selecting the current workflow SHA as the candidate and the most recent earlier successful `tests.yml` run on the same branch as the baseline. The harness also polls collection summary after import instead of assuming one immediate read is authoritative, and opts JavaScript actions into Node 24 explicitly so `oven-sh/setup-bun@v2` is exercised against the post-Node-20 runner path before GitHub flips the default. Local validation of the import-handler delegation fix is good: direct 2-doc and 5,000-doc single-node imports return `200`, emit the expected number of response lines, and land all documents; targeted API suites `nuraft_runtime_documents_crud.test.ts`, `nuraft_replication_edges.test.ts`, and `documents.test.ts` pass against the rebuilt binary with the stronger assertions. The full release matrix also exposed a new macOS-specific packaging issue: upstream `ggml` enables `GGML_BLAS=ON` by default on Apple, which makes `libggml.a` reference `ggml_backend_blas_reg()` while our Bazel staging only ships the core static libs. Remaining work is final hosted confirmation that the benchmark lane stays green on the latest SHA, the macOS release rerun after forcing `GGML_BLAS=OFF` for whisper's CPU-only build, and the longer-term ORT Extensions registration cleanup while keeping the promoted binary self-contained. |
| 21 | NuRaft import/runtime parity + benchmark refactor sprint | P1 Runtime/Perf | **in progress** | Runtime-side parity now uses bounded logical NuRaft import chunks plus one logical import-handler replay, the direct `1M` single-POST gate is green, final local `quick/core` self-compare plus `standard/core` upstream compare are green (`29.383s -> 26.906s` import, heavy search scenarios all faster), and the broad `--no-secrets --download-migration-binary` API gate is green again. Remaining work is pushing this final replay-model fix and confirming the hosted `core` lane on that SHA. |

### Backlog map (active / later / archival)

Use this to decide what to pick next without scanning multiple files.

- **Active now (execution lane):** item **21** (`NuRaft import/runtime parity + benchmark refactor sprint`) — the final replay-model runtime refactor is in, the direct `1M` single-POST gate is green, both local post-fix core reruns are green, and the broad API gate is done. Remaining work is pushing this exact state and confirming the hosted `core` lane with the timeout override path.
- **Recently finished:** item **18** (`Dependency refresh audit`) — all actionable deps at latest, patch debt at minimum. Item **19** (`NuRaft cutover`) — 120/120 API tests. Item **9** (`Protobuf 34`) — 34.0.bcr.1.
- **Recently finished:** whisper.cpp v1.8.3 upgrade — patch reduced from 7 hunks to 1, BUILD rewrite to cmake rule. NuRaft async/streaming parity verified against upstream (both synchronous, full match).
- **Later (planned but not started):** Compile warning cleanup (surgical `per_file_copt` suppression of third-party warnings from protobuf, abseil, libstdc++ regex — zero-noise build output). Env-dependent test suites (blocked on infrastructure). Abseil `20260107.1` upgrade (blocked by ORT ABI mismatch). ONNX Runtime Extensions registration cleanup: keep static/self-contained ORT core packaging, but revisit whether image custom ops can move to a more explicit registration model to reduce sanitizer teardown debt without reintroducing runtime `libonnxruntime.so.1` coupling. Release workflow follow-up: keep local Linux replay as the first validation step, because stale smoke-test assertions like the old `Command line usage:` grep can break packaging even when the binary itself is healthy, and consider extracting Linux tarball/package assembly into a repo-owned wrapper so CI and local replay stop depending on workflow-only shell blocks.
- **Archival/reference (not immediate execution lanes):**
  - `benchmark/BENCHMARK_RESULTS.md` P2/P3 backlog items (experimental/future ideas).
  - `TODO.md` upstream product backlog (not the modernization source of truth; mine opportunistically only when an item aligns with current modernization goals).

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
- The broad canonical API gate is green again: `scripts/run_api_tests.sh -- --no-secrets --download-migration-binary`.
- A benchmark-only hardening fix was needed after those reruns started producing long search-phase stderr bursts: the harness now launches the long-lived Typesense Docker process with execa `buffer: false`, so Bun/get-stream no longer aborts `standard/core` while buffering server logs that are already being consumed incrementally.

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
- [x] Verify follower/leader behavior remains correct and deterministic after the refactor; do not regress the earlier local fix that routed `kDocumentImport` through the real registered handler. *(Targeted API suites `documents`, `nuraft_runtime_documents_crud`, and `nuraft_replication_edges` passed during development, and the broad `--no-secrets --download-migration-binary` API gate is green on the final replay-model build.)*
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
  - `scripts/run_api_tests.sh -- --no-secrets --download-migration-binary`
- [ ] Hosted confirmation after local green:
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
- [ ] The default benchmark lane finishes comfortably inside GitHub's workflow timeout budget on the latest SHA.
- [x] `benchmark/BENCHMARK_RESULTS.md` and this plan both explain the new benchmark/runtime policy in one short section each.

### Upstream TODO candidates worth pulling in (post-Phase-3 queue)

From `TODO.md`, these are the highest-value items that still align with current modernization/perf goals:

- ~~**Replication throughput control:**~~ N/A — `MAX_UPDATES_TO_SEND` was a `braft` concept. NuRaft controls replication internally; its parameters are already exposed as `--raft-*` CLI args.
- **Indexing hot-path efficiency:** reduce avoidable string copies during indexing/import code paths. *(Deferred: needs profiling-driven identification of specific hot paths.)*
- **Search work budget tuning:** make "minimum results" heuristic configurable instead of coupling to `max_results` behavior. *(Deferred: deep search-algorithm change. The existing `exhaustive_search`, `search_cutoff_ms`, and `drop_tokens_threshold` parameters provide some control already.)*
- ~~**Numeric safety hardening:**~~ done — `coerce_int32_t()` and `coerce_int64_t()` in `validator.cpp` now bounds-check float values before casting, preventing UB from out-of-range floats. Returns 400 or drops the field per dirty_values policy.
- ~~**Reliability coverage:**~~ done — `nuraft_replication_edges.test.ts` adds 19 focused multi-node tests covering follower-originated metadata writes, cross-node visibility, restart persistence, and snapshot persistence. Fixed NOT_LEADER race in `append_via_raft()`. Total: 139 pass, 0 fail.

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
  - `scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server -- --no-secrets --download-migration-binary` passing the broader no-secrets API suite, including migration replay from the v29 source binary.
  - a direct probe-binary smoke that prewarms `ts/e5-small`, creates an embedding collection, indexes a document, observes a populated embedding (`384` dims), and returns a vector-search hit.
- After promotion, the same validations also pass on the real `typesense-server` target: `scripts/run_api_tests.sh -- --no-secrets --download-migration-binary`, `ldd bazel-bin/typesense-server`, and a direct local `ts/e5-small` embedding/vector-search smoke.
- `api_tests/scripts/prepare_runtime_bundle.sh` is now binary-shape-aware: it copies `libonnxruntime.so.1` only when `ldd` shows the tested binary actually needs it. `scripts/run_api_tests.sh` also accepts `--server-binary` / `TYPESENSE_SERVER_BINARY_PATH`, so the API harness can validate either shared-ORT or self-contained binaries without manual bundle surgery.
- `.github/workflows/tests.yml` no longer uploads `libonnxruntime.so*` as a required build artifact, because the promoted server bundle may legitimately be just the binary.
- `.github/workflows/tests.yml` and the draft `.github/workflows/release-binaries.yml` now include `ldd` guardrails that fail if `typesense-server` silently regresses back to a `libonnxruntime.so.1` dependency on Linux.
- The draft `.github/workflows/release-binaries.yml` now also writes `typesense-server.md5.txt` into each staged release bundle and uploads a tarball `.sha256.txt` sidecar so downstream packaging helpers have the expected checksum metadata.
- The draft release workflow now verifies the packaged tarball actually contains `typesense-server` plus `typesense-server.md5.txt`, and that the embedded MD5 manifest matches the packaged binary bytes.
- The draft release workflow now smoke-tests the staged `typesense-server` with `--help`, which gives the macOS lanes at least a minimal runtime sanity check before packaging/upload.
- `debian-pkg/generate_deb_rpm.sh` now resolves release tarballs from either the new `artifacts/` output or legacy `bazel-bin/`, and verifies the tarball SHA256 sidecar when present before extracting.
- `publish_release.sh` now uploads the new `artifacts/typesense-server-<version>-<platform>.tar.gz` outputs plus their `.sha256.txt` sidecars, while still falling back to legacy `build-Linux` / `build-Darwin` tarballs if needed.
- End-to-end Linux package validation now succeeds locally against a workflow-style tarball: in an Ubuntu 24.04 container with `alien`, `rpm`, and `dpkg-dev`, `TSV=0.0.0-local ARCH=amd64 RELEASE_ARTIFACT_DIR=./artifacts RELEASE_PACKAGE_DIR=./artifacts/packages bash debian-pkg/generate_deb_rpm.sh` produced both `.deb` and `.rpm` outputs from the staged tarball.
- Based on that validation, the draft `.github/workflows/release-binaries.yml` now includes Linux DEB/RPM generation + upload steps driven from the tarball it already built, instead of leaving package generation entirely manual.
- First real GitHub run of `release-binaries` on `v32` found workflow bugs, not release-shape regressions: Linux built the self-contained binary successfully but the smoke step treated the server's expected `--help`/usage exit (`1`) as failure, and macOS arm64 failed before the build because a step-level `PATH` override hid Homebrew's `bazelisk`.
- The current workflow fix keeps the Linux smoke test but accepts exit `0/1` so long as `Command line usage:` is printed, and the macOS build step now computes Homebrew prefixes at runtime and prepends them to the existing `PATH` instead of replacing it.
- Second GitHub run on commit `030b706f` confirms Linux lanes end-to-end (amd64 + arm64) are green, including tarball checks, DEB/RPM generation, and artifact uploads. Remaining blocker is macOS arm64 fetch-time failure in `@s2geometry` because `MODULE.bazel` used a GNU-style `sed -i -E ...` patch command that is not BSD-sed portable.
- Subsequent Darwin-specific hardening closed the remaining macOS blockers: `apple_support` registration for ObjC toolchain analysis, host/portable patch commands, `h2o` OpenSSL root staging, vendored `magic_enum` AppleClang warning suppression, hermetic curl feature toggles (`CURL_USE_PKGCONFIG=OFF`, `CURL_BROTLI=OFF`, `CURL_ZSTD=OFF`, `USE_NGHTTP2=OFF`), and explicit Apple framework linkopts for ONNX Runtime Extensions image operators.
- Targeted Darwin validation now passes on `darwin-arm64` via run `22824046410`, including build, staging, smoke, tarball, and artifact upload.
- Full draft multi-arch validation is now green via run `22825402217`: `linux-amd64`, `linux-arm64`, `darwin-arm64`, and `darwin-amd64` all succeeded, with Linux package generation/upload and Darwin tarball validation both passing.
- Upstream still has open build-packaging friction for downstream consumers (for example ONNX Runtime issue `microsoft/onnxruntime#7150` about modern CMake/vcpkg/external-project support), so do not assume the remaining productionization work will be patch-free.

### Takeover snapshot for item 17

If a new agent takes over mid-stream, assume the following:

- Confirmed finished work:
  - Static probe already proved the real blocker is duplicate protobuf runtimes, not export/install plumbing.
  - ORT Extensions static export-set issues, build-tree include metadata, and zlib 1.3 guard were already patched far enough to reach the final link result.
  - Research is complete enough to know ORT can theoretically reuse external protobuf; the gap is Bazel/foreign_cc plumbing, not lack of an upstream ORT hook.
  - The exact-pinned-commit patch now applies cleanly, `//:typesense-server` builds successfully, and `ldd` confirms the promoted binary has no `libonnxruntime.so.1` dependency.
  - The canonical `scripts/bazel_in_docker.sh build //:typesense-server` now uses the one-Protobuf static ORT path and `ldd` confirms it has no `libonnxruntime.so.1` dependency.
  - API runtime smoke now passes against the probe via `scripts/run_api_tests.sh --server-binary ... tests/health.test.ts`.
  - The full no-secrets API suite, including migration replay, now passes against the probe via `scripts/run_api_tests.sh --server-binary ... -- --no-secrets --download-migration-binary`.
  - Direct local embedding smoke now passes against the probe using public model `ts/e5-small` (embedding created and vector search succeeds).
  - The full no-secrets API suite and direct local embedding smoke also pass against the promoted main `typesense-server` target.
- Immediate next coding tasks:
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
  - modified: `publish_release.sh`
  - modified: `api_tests/scripts/prepare_runtime_bundle.sh`
  - modified: `scripts/run_api_tests.sh`
- Important caution: `.github/workflows/release-binaries.yml` is still just a draft. The linkage/runtime model is now settled locally, but the workflow itself still needs cross-platform packaging validation before it should be treated as production-ready.

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

11. **Dockerized API harness should force IPv4 localhost.** Inside the API Bun container, `localhost` health checks can miss servers that are listening on IPv4 only. Set `TYPESENSE_API_HOST=127.0.0.1` in the wrapper to keep Dockerized API runs reliable.

12. **Upstream ships self-contained core CPU artifacts, and this fork now matches that on the promoted local target.** Keep checking with `ldd` after future ORT/build-graph changes so the repo does not silently regress back to a `libonnxruntime.so.1` runtime dependency.

14. **Sanitizer flags leak into `rules_foreign_cc` configure scripts.** Bazel's `--copt -fsanitize=X` applies globally, breaking autoconf detection in deps like kakasi and iconv. Fix by adding `env = select({"@@//:asan_mode": {"CFLAGS": "-fno-sanitize=address", ...}})` to each foreign_cc target. Note: `@@//` (not `@//`) is required in Bazel 9 Bzlmod to reference main-repo config_settings from external BUILD files.

15. **UBSAN breaks abseil with GCC 14.** `-fsanitize=undefined` causes constexpr evaluation failures in `absl/container/internal/hash_policy_traits.h:158`. Keep ASAN and TSAN separate; do not combine UBSAN with either until abseil ships a fix.

13. **Static ONNX Runtime probes hit multiple false-front blockers before the real protobuf conflict.** Modern CMake 3.31 first rejects ONNX Runtime Extensions' export set and build-tree include metadata, then the static vision build trips an upstream zlib-1.3 guard. Patch through those only far enough to reach the final link result; they are not the core reason this repo needs the shared `libonnxruntime.so.1` boundary.

14. **The real static-link blocker is duplicate protobuf runtimes in one binary.** After adding ONNX Runtime's bundled Protobuf 21.12 archives to the static probe, `ld.lld` reports duplicate `google::protobuf` symbols against the repo's Protobuf 33 runtime. That is the concrete evidence that a self-contained upstream-style binary needs a one-Protobuf strategy, not just more archive copying.

15. **ORT already has an external protobuf hook; our Bazel plumbing is what blocks it.** `onnxruntime_external_deps.cmake` already supports `find_package`/pre-existing protobuf targets, but `bazel/onnxruntime.BUILD` forces `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER`. Future work should focus on feeding ORT one protobuf from Bazel/foreign_cc rather than assuming ORT itself must be fundamentally redesigned.

16. **`rules_foreign_cc` exposes raw Bazel protobuf artifacts more naturally than CMake packages.** Inside foreign_cc, this repo already has a working pattern in `bazel/sentencepiece.BUILD`: pass `libprotobuf.a`, `libprotobuf_lite.a`, `protoc`, and include paths directly. There is no ready-made protobuf CMake package in this repo's foreign_cc flow today, so the fastest prototype path is imported targets or a tiny synthetic package, not waiting for a full upstream-style protobuf install tree.

17. **ORT 1.24 static builds now include `libonnxruntime_lora.a`.** If Bazel static targets do not expose that archive, final linking fails with unresolved `onnxruntime::adapters::utils::*` symbols from `lora_adapters.cc`, even after the protobuf collision itself is fixed.

18. **Check the produced binary with `ldd` before claiming packaging parity.** A successful static-probe build is not enough; confirm whether the final executable still depends on `libonnxruntime.so.1` so release-workflow decisions are based on the binary shape, not just the archive list.

19. **Hermetic static macOS builds need explicit dependency opt-outs and Apple framework linkopts.** `rules_foreign_cc` static outputs can silently pick up host features (for example curl auto-enabling Brotli) or drop upstream CMake framework link directives (for example ONNX Runtime Extensions' ImageIO/CoreGraphics/CoreServices linkage). Prefer explicit `CURL_*` feature toggles and mirror upstream Apple framework linkopts in Bazel wrappers instead of relying on host discovery.

20. **Runtime-bundle prep must follow the tested binary's actual dependency shape.** Do not hardcode `libonnxruntime.so.1` into API/release bundle prep for every artifact; detect whether the selected binary needs that shared library, or self-contained probes will fail validation for the wrong reason.

21. **CI artifact upload paths must not require optional runtime libs.** Once `typesense-server` is self-contained, workflows like `.github/workflows/tests.yml` should upload the binary itself and treat `libonnxruntime.so*` as conditional, not mandatory.

22. **Release tarballs need checksum metadata inside and outside the archive.** `debian-pkg/generate_deb_rpm.sh` expects `typesense-server.md5.txt` inside the extracted tarball, and release automation benefits from a tarball-level SHA256 sidecar. Keep both when changing artifact assembly.

23. **Downstream packaging helpers should resolve both draft and legacy artifact locations.** During workflow migration, scripts like `debian-pkg/generate_deb_rpm.sh` and `publish_release.sh` should prefer the new `artifacts/` layout but keep a legacy fallback until the older release path is fully retired.

23. **Alien-derived RPM layouts are not stable enough for hardcoded spec/buildroot paths.** `generate_deb_rpm.sh` should discover the generated `.spec`, use a clean copied buildroot, and avoid assuming the output directory name exactly matches the package version string.

24. **Bazel `patch_cmds` must avoid GNU-only `sed -i` assumptions when macOS lanes are in scope.** Commands like `sed -i -E ...` can parse differently under BSD sed and break repository fetches; prefer portable `perl`/`python` edits or explicit cross-platform flag forms.

25. **Add a workflow input before splitting release workflows.** A single `release-binaries.yml` with a `target_scope` / matrix-filter input was enough to debug one macOS lane quickly without duplicating logic across multiple workflows. Keep one canonical workflow unless maintenance pressure truly forces a reusable split.

26. **`tests/` currently has no Vitest spec files.** For dependency-only maintenance in that package, `bun run check` is the useful verification command; `bun run test` is not the primary signal until real test files exist.

27. **Prefer exact version pins to floating tags in Bun-managed packages.** `bun update --latest` is useful for discovering upgrades, but manifests and lockfiles should land on concrete version numbers rather than `latest` so CI and local replays stay reproducible.

27. **Chunked import replay must key carryover by request identity, not log index.** For the NuRaft prototype's materialized apply path, import fragments only reassemble correctly across restart when the pending tail is stored under a stable request id (`start_ts`, with log-index fallback for synthetic single-chunk cases), mirroring how the batched indexer treats one logical import request across multiple chunks.

28. **Prototype snapshot install must preserve local node identity/bootstrap metadata.** A NuRaft snapshot can legitimately replace replay progress, log history, and materialized state, but follower install should not silently clone another node's `identity.json` or bootstrap self-address. Preserve local identity/bootstrap on install unless you are intentionally building a one-off cold-copy artifact.

29. **Use a RocksDB checkpoint for live prototype KV snapshots when a sink handle is available.** Recursive directory copies are acceptable as a cold fallback, but the closer prototype path is to reuse the open `NuRaftKvStateMachineSink` DB handle and export a checkpointed materialized-state tree, matching the real snapshot direction more closely.
30. **Do not silently rewrite persisted self identity for a multi-node NuRaft prototype.** Refreshing peer lists from `--nodes` is acceptable metadata churn, and single-node self-address drift is a real recovery case, but changing a persisted node's own peer endpoint inside a multi-node bootstrap is the same class of unsafe escape hatch as `reset_peers()` and should fail loudly.
31. **Prototype follower catch-up should reject divergent history, not auto-heal it.** If a follower log no longer matches the leader prefix, the feasibility prototype should fail loudly so the migration decision sees the real repair gap instead of hiding it behind implicit truncation or overwrite behavior.
39. **Keep heavyweight CI lanes manual unless they are the main gate.** GitHub's `workflow_dispatch` and reusable-workflow guidance fit this repo better than always-on cron for sanitizer, flake, stress, and benchmark lanes. Prefer the documented local wrapper commands first, then dispatch the corresponding heavy workflow only when you want GitHub-hosted confirmation.
40. **ONNX Runtime Extensions vision custom ops currently trip ASAN's `new_delete_type_mismatch` at process teardown.** Keep `EnableOrtCustomOps()` off plain text/personalization sessions, and if the image-processor path still needs extensions, document the temporary `ASAN_OPTIONS=new_delete_type_mismatch=0` workaround instead of pretending it is a first-party memory bug.
41. **Destroy image/text embedders as part of normal collection-manager teardown.** If ONNX Runtime image sessions survive until process-exit static destruction, ASAN reports third-party cleanup crashes that do not reflect steady-state runtime behavior. `CollectionManager::dispose()` should release embedder caches before the process starts unwinding globals.
42. **Whisper voice-query inference is not a useful TSAN target.** `CollectionVectorTest.TestVoiceQuery` passes in the normal lane but returns a functional transcription error under TSAN instrumentation without producing a sanitizer report. Keep that test skipped under TSAN so the lane focuses on actionable concurrency signal.
43. **`_rand(seed)` is a product contract, not a best-effort shuffle.** Typesense's public docs and release notes promise that the same seed yields the same ordering across searches. Tests should assert seed stability and seed-to-seed variation, not a hardcoded permutation that can change when insertion order or scoring internals move.
44. **Random sorting needs its own explicit sort sentinel in `Index::populate_sort_mapping()`.** Leaving `_rand` to fall through without initializing `field_values[i]` can misroute `compute_sort_scores()` into another sentinel branch under sanitizer builds. Defensive zero-initialization plus a dedicated `random_order_sentinel_value` fixed the suite-only ASAN crash and restored the documented same-seed behavior.
45. **Sanitizer-instrumented tests need deadlines that match the behavior under test.** ASAN and TSAN routinely add enough overhead that very small `search_cutoff_ms` values turn intent-specific tests into generic 408 failures. For timeout-sensitive tests, raise the deadline only enough to keep exercising the intended branch under instrumentation; for integration tests like voice query, use a larger sanitizer-only budget if the test is validating correctness rather than latency.
46. **Benchmark harnesses should disable execa buffering for long-lived noisy server processes.** If the harness already consumes `stdout`/`stderr` incrementally, leaving execa on its default `buffer: true` only creates a second in-memory copy and can fail long benchmark lanes with Bun/get-stream `maxBuffer` errors when the server emits sustained warnings.
46. **Stress lanes are only useful if tests are parallel-safe across Bazel reruns.** `ArchiveUtilsTest` looked fine in single runs but failed immediately under `--runs_per_test` because it shared a fixed `/tmp/archive_utils_test` path. Use the repo's temp-dir helper for filesystem tests so local stress and hosted nightly lanes measure real flakiness instead of cross-run collisions.
47. **Hosted runner capacity is part of test design, not just build speed.** GitHub's standard private Linux runners expose only 2 vCPUs, so `--runs_per_test=5` on the monolithic `//:typesense-test` target caused four heavy ORT-enabled runs to overlap and fail with `pthread_create(...): EAGAIN`. Preserve the repeat count, but cap Bazel test concurrency in the hosted stress lane instead of weakening product coverage.
48. **Release smoke tests should validate the stable interface, not one historical banner string.** The local Linux `release-binaries` replay caught that the workflow still grepped for `Command line usage:` while the current server prints `usage:`. Accept the current banner shape, or both shapes if the CLI entrypoint is in transition, before relying on hosted packaging runs.
49. **Benchmark comparisons must stay branch-local.** A manual benchmark run on `v32` or any feature branch should compare the latest two successful `tests.yml` runs from that same branch, not the repo's latest successful runs globally, or the benchmark can silently compare unrelated SHAs and produce meaningless results.
50. **Benchmark harnesses must track the currently supported server CLI, not historical flags.** The benchmark workflow was still starting `typesense-server` with `--peering-address` and `--api-address`, which the NuRaft runtime no longer accepts. Keep the benchmark launcher aligned with the real runtime entrypoint (`--node-host`, `--listen-address`, comma-separated `--nodes`) or the lane fails before any benchmark signal is collected.
51. **Bulk-import tests must assert both ingestion and response-shape parity.** The NuRaft single-node bulk-import path currently ingests all documents but returns one generic success envelope instead of the documented newline-delimited per-document results. Existing tests missed that because they only asserted `success:true`. For bulk import, assert final document count and that response line count matches the number of input JSONL records when no per-document errors are expected.
52. **Hosted CI one-shot benchmark imports still need an explicit timeout policy.** The product default remains H2O's inherited `60000ms`, but the benchmark lane now restores the upstream one-large-POST shape and uses `TYPESENSE_REQUEST_TIMEOUT_MS` when slower hosted runners need more headroom. Prefer an explicit timeout override over client-side request chunking so the benchmark keeps measuring the real product import path.
53. **On NuRaft-backed bulk routes, `async_req=true` can accidentally turn H2O body aggregation into Raft log granularity.** The old `/documents/import` runtime registration allowed a slow single POST to reach `append_via_raft(...)` at transport-aggregate boundaries instead of at one logical request boundary. For bulk import parity, buffer one logical request before entering the Raft write path.
54. **When a benchmark lane compares new binaries against older same-branch artifacts, prefer env overrides over new CLI flags.** `benchmark-testing.yml` can compare a just-built runtime against an older artifact that does not know a new server flag yet. Forwarding `TYPESENSE_REQUEST_TIMEOUT_MS` through the Dockerized process launcher kept the raised timeout available without breaking older binaries that simply ignore the env var.
32. **Timed snapshot policy is part of the recovery contract, not just a scheduler detail.** If snapshots are blocked on follower health, a lagging follower can be trapped behind a permanently stale recovery point. Keep a test lane that distinguishes "require healthy peers" from "leader-only snapshot" policy so this deadlock class stays visible.
33. **Do not misuse the existing HTTP benchmark wrapper as NuRaft evidence.** `scripts/benchmark_vs_upstream.sh` measures full server binaries behind the normal API/runtime surface. Until NuRaft has either a thin HTTP-facing adapter or a dedicated prototype benchmark harness, Story E needs its own measurement lane.
34. **Prototype benchmarking should stay explicitly separate from the normal HTTP benchmark wrapper until the runtime surfaces actually match.** A dedicated NuRaft microbenchmark target is useful for Story E, but it is still measuring isolated replication/storage paths, not a drop-in server replacement. Treat it as evidence for feasibility, not as a substitute for a later apples-to-apples runtime comparison.
35. **Once the old stack is removed, retire its benchmark lanes instead of letting them rot.** Keep the historical results, but turn dual-runtime wrapper profiles into explicit archival notes so future work does not accidentally benchmark nonsense.

36. **When deleting prototype files, audit transitive includes before removing.** `NuRaftFileStore` was initially classified as prototype-only but was actually a production filesystem utility used by 3 production stores. Similarly, `NuRaftLogEntry` lived in a deleted header but was needed by the real `TypesenseStateMachine`. Always `grep` for users of a struct/class across the whole codebase before deleting its definition.

37. **Single-node NuRaft requires explicit leadership request after init.** `raft_launcher::init()` starts the Raft server but leader election happens asynchronously (200-400ms timeout). Without calling `raft_server_->request_leadership()` + polling, single-node writes fail with NOT_LEADER errors.

38. **After prototype cleanup, the `nuraft_lib` BUILD target contains only production code.** The library was renamed from `nuraft_prototype_lib` to `nuraft_lib`. All references in test targets and `nuraft_runtime_lib` deps must be updated simultaneously.
