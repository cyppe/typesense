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
- [x] Dependency set is current, with patch debt minimized and documented exceptions only. *(13 active patches with justifications in `bazel/PATCH_DEBT.md`. Protobuf 34 blocked on brpc upstream — documented exception.)*
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
- [x] `whisper.patch` reduced from 11 hunks to 7 — dropped compiler warning flags, whitespace noise, backtrace removal, and the CMake compile-definition hunk by moving `GGML_USE_CUBLAS` into `bazel/whisper.BUILD`. Non-speech token expansion kept (test-verified: voice query expects punctuation-free output).
- [x] `quicly/0001.patch` dropped — bumped quicly to `c9167711` (fix commit).
- [ ] Replace patch-only forks with released upstream versions where possible (13 active patches documented with next actions in `bazel/PATCH_DEBT.md`).
- [x] Move brpc inline `patch_cmds` to proper patch file `bazel/brpc/0001_dynamic_annotations_guards.patch`.
- [x] Switch ICU from Typesense fork (`github.com/typesense/icu`) to upstream release tarball (ICU 71.1). Fork carried zero source mods. Patch content unchanged.
- [x] **ICU version upgrade (71.1 → 78.2):** Upgraded to ICU 78.2 (Unicode 14→17, CLDR 48). Same 6 upstream BUILD.bazel files still ship, but they are now removed via `MODULE.bazel` `patch_cmds`; `bazel/icu/icu.patch` has been reduced to just the `icudefs.mk.in` AR fix. Zero Typesense code changes required — only stable APIs used.

**Braft patch audit (all 7 active braft patches confirmed non-droppable):**
- `0001` — fixes build against modern protobuf/abseil; upstream dormant (last real release 2021).
- `0002_ipv6_braft`, `0002_ipv6_brpc`, `0002_ipv6_butil` — Typesense IPv6 support; no upstream equivalent.
- `0004_util_namespace` — namespace fix required by modern abseil; upstream has not adopted.
- `0005_bazel9_string_view` — Bazel 9 / C++20 string_view compat; upstream has not adopted.
- `0006_cxx20_shuffle` — replaces removed `std::random_shuffle`; upstream has not adopted a modern-toolchain equivalent.

Separate patch-debt note:
- `quicly/0001.patch` — dropped (quicly bumped to `c9167711`).

**Gotchas for patch work:**
- Patch droppability must be verified with a full `bazel build //:typesense-server` inside Docker — header-only changes can appear to succeed in isolation but fail at link time.
- `bazel/whisper.patch` is the largest remaining patch and highest maintenance risk (CUDA/shared-loading paths). ICU patch debt is lower now: the six conflicting upstream `BUILD.bazel` files are removed via `MODULE.bazel` `patch_cmds`, and `bazel/icu/icu.patch` now only carries the `icudefs.mk.in` AR fix.
- `bazel/onnxruntime.patch` was re-audited after the ORT `1.24.3` bump and is still required for extensions path export, zlib 1.3.x/static image-codec handling, and the one-protobuf imported-target path.
- `bazel/PATCH_DEBT.md` now tracks concrete upstream references for the active brpc/braft upstream-candidate patches (`apache/brpc#2904`, `baidu/braft#237`, `baidu/braft#524`, `baidu/braft#517`); `0006_cxx20_shuffle.patch` still has no exact upstream issue.
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
  - Protobuf `33.5` -> `34.0` remains the largest obvious core bump still open, but is still blocked by `brpc` compatibility.
- [ ] Medium-priority candidates worth evaluating next:
  - ONNX Runtime `1.24.2` -> `1.24.3` is now done; canonical Docker build passed, `ldd bazel-bin/typesense-server` still shows no `libonnxruntime.so.1` dependency, and the Dockerized API `tests/health.test.ts` replay passed against the promoted binary.
  - `libarchive` `3.7.7` -> `3.8.x` for packaging/security posture. *(Now in progress on `3.8.5`.)*
  - `snappy` `1.1.7` -> `1.2.x` for compiler/perf hygiene. *(Now done on `1.2.2`.)*
  - `typesense-js` in `tests/` `2.0.3` -> `3.0.2` is now done to match the benchmark toolchain client line; `pnpm exec tsc --noEmit` passes in `tests/` after the bump.
- [ ] Keep treating patch-debt reduction as at least as important as raw version bumps; some deps (for example `whisper.cpp`, `braft`) matter more because of maintenance surface than because they are numerically old.

### 7d) Raft replacement feasibility sprint (NuRaft)

Determine whether replacing `braft`/`brpc` with NuRaft would be a net improvement in correctness, maintainability, and performance, not just a dependency swap.

**Investigation summary (Mar 2026):**

- NuRaft is the strongest in-process replacement candidate found so far; a Rust Raft service remains a possible long-term architecture, but it is a larger boundary change and should not be the first replacement experiment.
- NuRaft appears materially healthier than `braft` on release cadence and active feature work: current upstream docs and examples are maintained, latest release is `v3.0.0` (2025), and the feature set includes pre-vote, leadership expiration, learners, custom quorum control, auto-forwarding, streaming mode, parallel log appending, and scheduled snapshots.
- We explicitly reviewed `docs/how_to_use.md` and the example implementations under `examples/`; they confirm the main migration reality: NuRaft is a library, not a drop-in runtime. Typesense would need its own durable `state_mgr`, `log_store`, and `state_machine` integration instead of relying on `braft` + `brpc` built-ins.
- The examples are useful for API shape and lifecycle, but they are intentionally lightweight (`in_memory_state_mgr`, in-memory log store, CLI-driven add/remove). They do not solve Typesense's persistent log/meta storage, RocksDB snapshot transport, HTTP leader redirect behavior, or node-IP refresh behavior.
- Current Typesense coupling to `braft` is deep in `src/raft_server.cpp` and `src/typesense_server_utils.cpp`: `braft::Node`, `braft::Task`, `braft::Closure`, built-in RPC service wiring, URI-based log/meta/snapshot storage, peer reset flows, and leader/follower status checks are all first-class parts of the server lifecycle.

**Why this sprint exists:**

- `braft`/`brpc` are now one of the main blockers to future cleanup: patch debt, glog retention, and the Protobuf 34 upgrade are all coupled to that stack.
- A replacement is only worth doing if it is actually better than `braft` for Typesense's workload. A library with nicer APIs but worse commit latency, catch-up, or snapshot behavior is not an upgrade.

**Sprint goal:**

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

- **Isolation boundary:** do not fork the whole server binary in-place and do not mutate `ReplicationState`. Add a small runtime-facing replication interface first, let the current `braft` path implement it unchanged, and keep all NuRaft code under dedicated `include/nuraft/` and `src/nuraft/` paths.
- **Seam landed:** the first runtime-facing seam is now in place via `include/replication/replication_service.h`; the current `ReplicationState` implements it, and `HttpServer`, analytics, conversation cleanup, and remote embedder leader-routing code now consume the abstraction instead of the concrete `ReplicationState` type.
- **Prototype target shell landed:** `//:typesense-server-nuraft-prototype` now builds as an isolated prototype binary with its own `src/main/typesense_nuraft_prototype.cpp` entrypoint and `src/nuraft/nuraft_replication_controller.cpp`, while `//:typesense-server` still builds unchanged.
- **First prototype payload primitive landed:** `NuRaftRequestEnvelope` now provides a versioned binary envelope around the current `http_req::to_json()` payload shape, with focused codec coverage in `//:nuraft-request-envelope-test`. This gives the prototype a stable boundary before any NuRaft buffer/commit plumbing exists.
- **Prototype storage groundwork landed:** `NuRaftStateLayout` and `NuRaftFileStore` now define the isolated `state/nuraft-prototype/{meta,log,snapshot}` layout and provide fsync-safe atomic file writes for future metadata/state persistence, with focused coverage in `//:nuraft-file-store-test`.
- **Prototype metadata groundwork landed:** `NuRaftMetadataStore` now persists and validates the first typed metadata artifact (`identity.json`) on top of the atomic file-store layer, with focused coverage in `//:nuraft-metadata-store-test`.
- **Prototype peer groundwork landed:** `NuRaftPeerResolver` now parses the current `host:peer_port:api_port` node format, preserves bracketed IPv6 hosts, and derives `server_id == api_port` plus leader URLs with focused coverage in `//:nuraft-peer-resolver-test`.
- **Prototype bootstrap metadata groundwork landed:** `NuRaftMetadataStore` now also persists a typed `bootstrap_config.json` artifact that records `default_group`, self, peer set, and API scheme intent with validation for duplicate server ids and missing self membership, covered by `//:nuraft-bootstrap-config-test`.
- **Prototype bootstrap builder landed:** `NuRaftBootstrapBuilder` now turns either the current empty-`--nodes` single-node defaults or an existing comma-separated node list into a validated prototype bootstrap config, selecting `self` by `api_port` and preserving bracketed IPv6 peers, covered by `//:nuraft-bootstrap-builder-test`.
- **Prototype state initialization groundwork landed:** `NuRaftStateInitializer` now creates the isolated prototype layout and persists both `identity.json` and `bootstrap_config.json` from one validated options struct, giving the future controller/state-manager path an idempotent startup preflight with focused coverage in `//:nuraft-state-initializer-test`.
- **Prototype startup preflight landed:** `NuRaftReplicationController` now accepts minimal prototype CLI flags (`--data-dir`, `--node-host`, `--api-port`, `--peering-port`, `--nodes`, `--api-uses-ssl`), persists the isolated bootstrap metadata via `NuRaftStateInitializer`, and reports the derived leader URL/peer count, covered by `//:nuraft-replication-controller-test`.
- **Prototype segment-log groundwork landed:** `NuRaftSegmentLogStore` now appends versioned request envelopes into the isolated log directory, replays them with contiguous index validation on restart, and rejects truncated/corrupt tails for now instead of inventing recovery rules too early, covered by `//:nuraft-segment-log-store-test`.
- **Prototype write/replay scaffolding landed:** `NuRaftRequestJournal` now wraps the segment-log store for request-json append/replay, and `typesense-server-nuraft-prototype` can append one request (`--append-request-json`) and replay persisted entries (`--replay-log`) after startup preflight, covered by `//:nuraft-request-journal-test` and the updated `//:nuraft-replication-controller-test`.
- **Prototype replay-progress groundwork landed:** `NuRaftReplayCoordinator` now persists `replay_progress.json`, filters pending journal entries by `last_applied_index`, and rejects progress moves beyond the persisted log, giving the prototype a bounded restart-replay contract before any real state-machine apply path exists, covered by `//:nuraft-replay-coordinator-test` plus extended metadata-store coverage.
- **Prototype torn-tail recovery primitive landed:** `NuRaftSegmentLogStore` now exposes an explicit `recover_truncated_tail()` path that trims only truncated EOF garbage and preserves the valid prefix/next index, so the future restart path can adopt bounded torn-write recovery without silently masking other log corruption, covered by expanded `//:nuraft-segment-log-store-test` coverage.
- **Prototype apply path landed:** `NuRaftPrototypeStateMachine` now consumes pending replay entries, appends applied request JSON into a durable prototype sink, advances replay progress, and is callable from `typesense-server-nuraft-prototype` via `--apply-pending`, giving Story C its first end-to-end single-node append/restart/apply loop beyond raw log inspection, covered by `//:nuraft-prototype-state-machine-test` and expanded controller coverage.
- **Structured prototype apply contract landed:** `NuRaftAppliedRequestStore` replaces the raw JSONL sink with typed applied-request records derived from the current request payload shape (`route_hash`, params, metadata, body, chunk flags, timing/index metadata), while `NuRaftRequestJournal` now normalizes shorthand CLI request bodies into minimal request payloads so append/replay/apply all speak one structured contract, covered by `//:nuraft-applied-request-store-test` plus updated journal/state-machine/controller coverage.
- **Prototype startup recovery/apply automation landed:** `NuRaftRequestJournal` now exposes explicit truncated-tail recovery, and `typesense-server-nuraft-prototype` can recover torn EOF garbage (`--recover-truncated-tail`) and apply pending entries during startup (`--auto-apply-pending`) without separate manual steps, giving the single-node prototype a safer restart loop while keeping destructive recovery opt-in, covered by expanded journal/controller tests.
- **Prototype route classification landed:** `NuRaftRouteClassifier` now tags applied and replayed prototype requests with coarse Typesense write semantics (`document_import`, `document_write`, `document_delete`, `collection_create`, `collection_drop`, `unknown`) based on the current route-hash contract, making the prototype state-machine output closer to real write/application intent without needing a live `HttpServer`, covered by `//:nuraft-route-classifier-test` and expanded applied-request/controller coverage.
- **Prototype state-machine sink seam landed:** `NuRaftPrototypeStateMachine` now writes through a `NuRaftStateMachineSink` interface with a default file-backed implementation, so the next step can swap in a closer Typesense-side apply handler without rewriting replay/progress orchestration, covered by `//:nuraft-state-machine-sink-test` plus injectable-sink state-machine coverage.
- **Prototype materialized apply sink landed:** the optional `NuRaftKvStateMachineSink` persists applied-request history plus a RocksDB-backed materialized view under `state/nuraft-prototype/materialized_state`, mapping collection create/drop and direct document write/delete routes into deterministic state keys. The prototype CLI accepts `--state-machine-sink=kv` and `--dump-materialized-state` so single-node append/restart/apply can exercise a closer Typesense-style state mutation path without touching `//:typesense-server`, covered by expanded `//:nuraft-state-machine-sink-test` and `//:nuraft-replication-controller-test` coverage.
- **Prototype import replay adapter landed:** the KV sink now reassembles chunked import bodies by request `start_ts`, survives sink restarts, materializes complete import lines into `state/documents/<collection>/<id>`, and persists per-import summary / carryover state under `state/imports/...` and `state/import_buffers/...`. That makes the isolated single-node path materially closer to batched-indexer replay semantics without pulling NuRaft code into `//:typesense-server`.
- **Prototype static cluster adapter landed:** `NuRaftStaticCluster` now lets the isolated prototype model a fixed three-node topology by mapping `server_id -> data_dir`, discovering a chosen leader, forwarding follower-originated appends into the leader journal, replicating contiguous committed history into follower journals, and applying catch-up across all nodes via the existing state-machine sinks. The prototype CLI now exposes `--cluster-data-dirs`, `--cluster-leader-api-port`, `--replicate-cluster`, and `--dump-cluster-status`, covered by `//:nuraft-static-cluster-test` and expanded controller coverage.
- **Prototype snapshot/export parity landed:** `NuRaftSnapshotCoordinator` now captures an installable snapshot of the isolated prototype meta/log/materialized-state tree, exports it in the same high-level `state/nuraft-prototype/{meta,snapshot}` shape the current external snapshot flow uses, and installs the descriptor-selected payload back while preserving the target node's local identity/bootstrap metadata. When the live KV sink is available, materialized-state export uses a RocksDB checkpoint; install also clears stale destination materialized state when the source snapshot does not carry one. The prototype CLI exposes `--create-snapshot`, `--install-snapshot`, and `--dump-snapshot-descriptor`, covered by `//:nuraft-snapshot-coordinator-test` plus expanded controller coverage.
- **Prototype membership/IP-drift policy landed:** `NuRaftStateInitializer` now treats `--nodes` as refreshable bootstrap metadata instead of blindly rewriting persisted identity on every startup. The prototype refreshes peer lists when self identity stays stable, allows single-node self-address drift to rewrite local identity/bootstrap metadata, and rejects multi-node self-address rewrites as unsafe, covered by expanded `//:nuraft-state-initializer-test` and `//:nuraft-replication-controller-test` coverage.
- **Recommended new files/classes:** add a tiny `include/replication/replication_service.h` interface, then implement a prototype-specific `nuraft_replication_controller`, `nuraft_state_manager`, `nuraft_segment_log_store`, `nuraft_state_machine`, `nuraft_snapshot_coordinator`, `nuraft_peer_resolver`, and `nuraft_request_envelope` layer. Reuse `http_req`, `BatchedIndexer`, `Store`, route registration, and the existing HTTP proxy surface instead of rewriting product logic.
- **Recommended target shape:** keep NuRaft prototype code out of `//:typesense-server`. Add separate prototype-only Bazel targets (for example a `replication_interface` library, a `nuraft_prototype_lib`, and a dedicated `typesense-server-nuraft-prototype` binary) so the production binary and test targets do not pick up NuRaft transitively.
- **Recommended on-disk isolation:** for Story C, keep all prototype state under `state/nuraft-prototype/{meta,log,snapshot}`. Do not share `state/meta`, `state/log`, or `state/snapshot` with `braft` during the feasibility sprint.
- **Milestone order:**
  1. compile-only prototype shell and isolated target;
  2. single-node boot/write/restart/replay;
  3. crash-safe single-node recovery and tail-log recovery;
  4. static three-node append/commit + leader proxy parity;
  5. snapshot save/load/install + external snapshot export parity;
  6. only then membership/IP-drift handling and optional NuRaft-specific features.
- **Most dangerous traps to avoid:** do not execute writes before NuRaft commit, do not replace HTTP leader proxying with NuRaft auto-forwarding, do not store the Raft log in a second RocksDB, do not share state dirs with the `braft` path, and do not reintroduce a generalized `reset_peers()` escape hatch before the bounded single-node recovery story is understood.

**Story D - Verify correctness and operational parity**

- [ ] Replay a focused API subset against the prototype: health, restart, snapshot, and multi-node write/read phases before attempting the full suite.
- [ ] Add focused replication tests for leadership changes, lagging followers, snapshot installation, restart replay, and peer reconfiguration edge cases.
- [ ] Verify that leader redirects or auto-forwarding semantics are acceptable for import, streaming, and long-running write paths.
- [ ] Verify that snapshot install and catch-up semantics remain parallel-safe and do not regress the repo's current test isolation guarantees.

**Story E - Prove it is actually better than `braft`**

- [ ] Benchmark the prototype against the current `braft` path for commit latency, write throughput, follower catch-up speed, snapshot creation/install time, and steady-state CPU/memory cost.
- [ ] Measure at least one realistic contention case (concurrent write load plus follower recovery or snapshot activity), not just clean single-thread append throughput.
- [ ] Record whether NuRaft's extra features are materially useful to Typesense (`pre-vote`, leadership expiration, learners, streaming mode, parallel log appending) or just theoretical headroom.

**Story F - Make the decision**

- [ ] Recommend `go` only if the prototype preserves product-critical behavior, removes enough maintenance debt to justify the migration, and does not materially regress write-path or recovery-path performance.
- [ ] Recommend `no-go` if the storage/transport rewrite cost is too high, the operational semantics diverge too far from current Typesense needs, or the benchmark results do not beat/hold the current `braft` path.
- [ ] If `go`, split the full migration into follow-up stories with explicit cut lines (transport, state/log persistence, snapshotting, membership, test migration, benchmark gates).
- [ ] If `no-go`, record the reasons and shift effort back to minimizing `braft`/`brpc` drag rather than leaving the question open.

**Exit criteria for this sprint:**

- A written go/no-go recommendation exists.
- The recommendation is backed by at least one working prototype target and real benchmark data.
- The outcome explicitly answers whether NuRaft is better than `braft` for Typesense, not just whether NuRaft can be made to compile.

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
| 16 | ~~Static ONNX Runtime linkage probe~~ | Known Issues | **done** | Promoted `typesense-server` to the one-Protobuf static ORT path. `ldd bazel-bin/typesense-server` shows no `libonnxruntime.so.1`, the no-secrets API suite passes (including migration replay), and direct local `ts/e5-small` embedding/vector-search smoke succeeds. |
| 17 | ~~Release packaging / multi-arch workflow hardening~~ | Known Issues | **done** | Full draft workflow validation is now green across `linux-amd64`, `linux-arm64`, `darwin-arm64`, and `darwin-amd64`, including Linux DEB/RPM generation and Darwin tarball validation. The workflow still says `draft`, but the remaining work is promotion/cleanup, not technical break-fixing. |
| 18 | Dependency refresh audit (current vs latest) | P1 Build/Deps | **in progress** | Ranked shortlist exists now. `magic_enum` has already been refreshed to `0.9.7`; `libarchive` is now at `3.8.5`; `snappy` is now at `1.2.2`; ONNX Runtime is now at `1.24.3` with the one-protobuf/self-contained checks still green; `tests/` now uses `typesense-js 3.0.2`; the latest patch-debt audits confirmed `bazel/onnxruntime.patch` is still non-droppable on `1.24.3`, trimmed `bazel/whisper.patch` to 7 hunks, reduced `bazel/icu/icu.patch` to the AR fix only, and added upstream-tracker links for the active brpc/braft upstream-candidate patches, so the next work should bias toward the remaining `whisper`/upstream-candidate patch debt or the next deliberate dep candidate, while Protobuf 34 stays blocked on `brpc`. |
| 19 | NuRaft replacement feasibility sprint | P1.7d | **in progress** | Investigation says NuRaft is the only serious in-process replacement candidate, but it is a real subsystem rewrite, not a dependency bump. Stories A and B are complete, and Story C now has the isolated single-node path, static three-node catch-up path, external snapshot/export parity, and a bounded membership/IP-drift policy without pulling NuRaft into `//:typesense-server`. Next step should shift to Story D verification breadth: focused API-phase replay and replication edge-case coverage before any benchmark/go-no-go work. |

### Backlog map (active / later / archival)

Use this to decide what to pick next without scanning multiple files.

- **Active now (execution lane):** item **18** (`Dependency refresh audit`) remains the primary modernization lane. Item **19** (`NuRaft replacement feasibility sprint`) is also in progress by explicit user request; Stories A/B are done and Story C now has the single-node, static-cluster, snapshot/export, and bounded membership/IP-drift milestones checked off, so the next NuRaft work should stay bounded to Story D verification rather than turning into an unscoped rewrite.
- **Recently finished:** item **17** (`Release packaging / multi-arch workflow hardening`) validated the draft workflow end-to-end across both Linux and macOS architectures.
- **Later (blocked or dependency-coupled):** item **9** (`Protobuf 34`), section **6b** (`brpc`/rule compatibility work), section **7** patch-debt follow-up (`replace patch-only forks`) when dependency updates are available, and item **19** (`NuRaft replacement feasibility sprint`) when/if the repo is ready to spend a full replication-focused spike.
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
- Verification now succeeds with both `scripts/bazel_in_docker.sh build //:typesense-server-static-one-protobuf-probe` and the promoted `scripts/bazel_in_docker.sh build //:typesense-server`.
- `common_deps` in `BUILD` now points at `@onnx_runtime//:onnxruntime_static_one_protobuf_lib`, so the main `typesense-server` target uses the validated self-contained path, not just the probe target.
- `ldd bazel-bin/typesense-server` now shows no `libonnxruntime.so.1` dependency. The main local Linux artifact is now self-contained by default.
- Runtime validation now includes:
  - `scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-static-one-protobuf-probe -- --no-secrets tests/health.test.ts` passing all single-node + multi-node health/restart/snapshot phases.
  - `scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-static-one-protobuf-probe -- --no-secrets --download-migration-binary` passing the broader no-secrets API suite, including migration replay from the v29 source binary.
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
  - The exact-pinned-commit patch now applies cleanly, `//:typesense-server-static-one-protobuf-probe` builds successfully, and `ldd` confirms that probe has no `libonnxruntime.so.1` dependency.
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

2. **glog and Abseil `LOG()` macro collision.** brpc/braft transitively include `<glog/logging.h>` which defines `LOG()`. Abseil also defines `LOG()`. The solution is to use Abseil's **prefixed** macros (`ABSL_LOG`, `ABSL_CHECK`) in the facade to avoid the collision entirely. Never use unprefixed `LOG()` in first-party code.

3. **glog cannot be removed while brpc/braft are dependencies.** glog must remain in `common_deps` with `google::InitGoogleLogging()` called at startup, or brpc crashes. Silence glog output with `FLAGS_stderrthreshold = google::NUM_SEVERITIES` and empty `SetLogDestination` calls.

4. **Patch droppability verification requires full Docker build.** Header-only changes can appear to succeed in isolation but fail at link time. Always verify with `bazel build //:typesense-server` inside the Docker container.

5. **Pre-existing test warnings are excluded from CI guardrails** by the warning scripts. They won't block CI but should still be fixed to maintain code quality. Check for new warnings after any change by reading build output.

6. **braft patches are all non-droppable** as of Mar 2026 — upstream braft is dormant (last real release 2021). Don't waste time trying to drop them; just maintain them.

7. **NuRaft is the only serious in-process replacement candidate found so far, but it is not a drop-in swap.** Its docs and examples confirm that a real migration would require Typesense-owned persistent `state_mgr`/`log_store` implementations plus new transport/snapshot integration. Treat it as a subsystem rewrite with benchmark gates, not a quick dependency refresh.

8. **`max-indexing-concurrency` is hardware-sensitive.** Benchmark wins at 16 do not automatically justify a strict global default of 16. Keep conservative defaults for broad deployability (4), and document CPU-tier tuning guidance for production overrides. A future adaptive startup heuristic (CPU+memory aware) is a better long-term path than a single aggressive default.

9. **Import API `batch_size` must stay wired to actual indexing batches.** The request parameter is now passed through to `Collection::add_many(...)` and controls import-side batch flushing; avoid regressing this by reintroducing a hardcoded internal batch size in the API path.

10. **Import `batch_size` is a secondary throughput knob on this dataset.** A/B checks (`40` vs `1000`) showed only marginal import delta (~0.2% in current runs). Keep default `40` for mixed workloads; use larger values only as deliberate ingest-window overrides.

11. **Dockerized API harness should force IPv4 localhost.** Inside the API Bun container, `localhost` health checks can miss servers that are listening on IPv4 only. Set `TYPESENSE_API_HOST=127.0.0.1` in the wrapper to keep Dockerized API runs reliable.

12. **Upstream ships self-contained core CPU artifacts, and this fork now matches that on the promoted local target.** Keep checking with `ldd` after future ORT/build-graph changes so the repo does not silently regress back to a `libonnxruntime.so.1` runtime dependency.

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

26. **`tests/` currently has no Vitest spec files.** For dependency-only maintenance in that package, `pnpm exec tsc --noEmit` is the useful verification command; `pnpm exec vitest run` exits with "No test files found".

27. **Chunked import replay must key carryover by request identity, not log index.** For the NuRaft prototype's materialized apply path, import fragments only reassemble correctly across restart when the pending tail is stored under a stable request id (`start_ts`, with log-index fallback for synthetic single-chunk cases), mirroring how the batched indexer treats one logical import request across multiple chunks.

28. **Prototype snapshot install must preserve local node identity/bootstrap metadata.** A NuRaft snapshot can legitimately replace replay progress, log history, and materialized state, but follower install should not silently clone another node's `identity.json` or bootstrap self-address. Preserve local identity/bootstrap on install unless you are intentionally building a one-off cold-copy artifact.

29. **Use a RocksDB checkpoint for live prototype KV snapshots when a sink handle is available.** Recursive directory copies are acceptable as a cold fallback, but the closer prototype path is to reuse the open `NuRaftKvStateMachineSink` DB handle and export a checkpointed materialized-state tree, matching the real snapshot direction more closely.
30. **Do not silently rewrite persisted self identity for a multi-node NuRaft prototype.** Refreshing peer lists from `--nodes` is acceptable metadata churn, and single-node self-address drift is a real recovery case, but changing a persisted node's own peer endpoint inside a multi-node bootstrap is the same class of unsafe escape hatch as `reset_peers()` and should fail loudly.
