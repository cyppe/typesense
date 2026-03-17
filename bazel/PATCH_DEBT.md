# Bazel Patch Debt Inventory

This file tracks custom patches used by Bazel external dependencies.

Last audited: 2026-03-17

## Scope

- Source of truth for active Bazel patches: `MODULE.bazel` `patches = [...]` entries.
- Includes patch files under `bazel/` currently referenced by Bazel module rules.
- Excludes legacy CMake patch flow under `cmake/*.patch`.
- Also tracks fork-backed dependency replacements that are part of item 34's "prefer upstream releases over stale forks" work.

## Fork-backed dependency audit

No active fork-backed Bazel dependencies remain on the supported Docker/Bazel path.

## Removed As Stale

- `@kakasi` switched off the Typesense fork on 2026-03-17.
  - Reason: the real upstream source is `loretoparisi/kakasi`, but the Typesense fork was still `ahead_by=5`, `behind_by=0` and upstream still publishes no GitHub releases/tags. The supported path now pins the upstream commit `390c6d2eeac6c744da634866b68ea09830cac0a7`, carries the exact embedded `japanese_data` payload in-repo, and applies a small repo-owned `bazel/kakasi.patch` for the remaining zero-init / UTF-8 / bad-unicode fixes.
  - Proof: `scripts/bazel_in_docker.sh build //:typesense-server` and `test/scripts/replay_typesense_test.sh CollectionLocaleTest.SearchAgainstJapaneseText`.
- `@clip_tokenizer` switched off the Typesense mirror on 2026-03-17.
  - Reason: repo search identified the real upstream project as `ozanarmagan/clip_tokenizer_cpp`, and the pinned commit `0ca1e2e2e7418108725eaa7fb93e029516ae63fa` is identical in both repos (`git ls-remote` + GitHub compare).
  - Note: neither repo currently publishes tags or GitHub releases, so the supported Bazel path now pins the upstream source commit directly rather than a release artifact.
- `@hnsw` Bazel dependency removed on 2026-03-17.
  - Reason: item 36 finished the USearch production cutover and deleted the remaining in-tree `hnswlib` backend/runtime dependency from the supported Bazel path.
  - Note: schema-level `hnsw_params` remains for API compatibility only; it no longer implies a supported `hnswlib` runtime backend.
  - Note: legacy `cmake/hnsw.cmake` still exists for the old non-canonical CMake flow, but that path is outside this branch's supported Docker/Bazel production contract.
- `bazel/onnx.patch` removed on 2026-03-02.
  - Reason: not referenced by `MODULE.bazel` or Bazel build rules.
  - Note: legacy CMake flow uses `cmake/onnx.patch`, not `bazel/onnx.patch`.
- `bazel/picotls_openssl/0001.patch` removed on 2026-03-02.
  - Reason: `@picotls_openssl//:picotls_openssl` and `//:typesense-server` build cleanly on Bazel 9 with the patch removed.
  - Note: pinned upstream `picotls` commit now already compiles under current toolchain constraints used by this repo.
- `bazel/quicly/0001.patch` removed on 2026-03-04.
  - Reason: fix upstreamed — designated-initializer field order in `loss.h` corrected on upstream `master`.
  - Note: quicly bumped from `46110287` to `c9167711` (fix commit, not master — master has breaking picotls log API changes).

## Active Patch Classification

| Patch | External dep | Class | Why it exists | Owner | Reference | Next action |
|---|---|---|---|---|---|---|
| `bazel/onnxruntime.patch` | `onnx_runtime` | keep (medium-term) | Re-audited on ORT `1.24.3`: still needed to set `onnxruntime_EXTENSIONS_PATH`, patch the fetched extensions zlib 1.3.x guard/static image codec export behavior, rewrite fetched `OrtOpLoader` statics to process lifetime so ASAN no longer trips on ORT Extensions teardown, and inject imported external protobuf targets for the one-protobuf static build. External Abseil version sync is now handled patch-free in `bazel/onnxruntime.BUILD` via `FETCHCONTENT_SOURCE_DIR_ABSEIL_CPP` for that same one-protobuf lane. | Build modernization (P1.7) | [cmake/external@3a728b7](https://github.com/microsoft/onnxruntime/tree/3a728b75062256951b6e19ce718907cf1a1d4cf0/cmake/external) | Re-check on the next ORT + extensions bump; drop only when upstream covers path export, static codec install/export, the custom-op teardown path, and a supported external-protobuf path. Keep the BUILD-level Abseil source override unless upstream exposes a cleaner version-sync hook. |
| `bazel/onnx_ext.patch` | `onnx_runtime_extensions` | keep (short-term) | Trims tokenizer/operator surface and header deps to build selected operators under current toolchain. | Build modernization (P1.7) | [tokenizer headers@1f9d7ee](https://github.com/microsoft/onnxruntime-extensions/tree/1f9d7ee0c80b4f94946ee5650cb242aa01dd6278/operators/tokenizer) | Revisit with newer extensions release; prefer upstream/stable API include layout. |
| `bazel/kakasi.patch` | `kakasi` | keep (short-term) | Carries the remaining Typesense-vs-upstream source delta after switching from the fork to upstream `loretoparisi/kakasi`: zero-initialize `Character` buffers, use `get1byte()` instead of `getchar()` for UTF-8 reads, and skip invalid UTF-8 sequences instead of terminating conversion early. The embedded `japanese_data` payload now lives in-repo rather than in an external fork. | Build modernization (P1.7) | [kakasi@390c6d2](https://github.com/loretoparisi/kakasi/tree/390c6d2eeac6c744da634866b68ea09830cac0a7) | Re-audit on any upstream `kakasi` movement; drop it once those fixes land upstream or a usable release/source includes them. |
| `bazel/whisper.patch` | `whisper.cpp` | keep (minimal) | Non-speech token expansion only (1 hunk, 1 file). Pin `2eeeba56` = v1.8.3. Upgraded from `022756a8` (pre-v1.7.x). All CUDA dlopen/dlsym hunks eliminated by upgrade. | Build modernization (P1.7) | [whisper.cpp@2eeeba5](https://github.com/ggml-org/whisper.cpp/tree/2eeeba56e9edd762b4b38467bab96c2517163158) | Patch is at minimum. Non-speech token list is Typesense-specific (punctuation suppression for voice queries). Monitor upstream for inclusion. |
| `bazel/h2o/h2o_725e54bc932fbe0c6e208db4e71eb1df79ec43ff.patch` | `h2o` | keep (medium-term) | Now reduced to CMakeLists.txt-only delta: removes CONFIGURE_FILE for .pc files and strips INSTALL targets for binaries/pkg-config. The nine conflicting `deps/brotli/**/BUILD` files are now removed via `MODULE.bazel` `patch_cmds` (same pattern as ICU). Reduced from 774 lines to 48 lines. | Build modernization (P1.7b) | [h2o CMakeLists@725e54b](https://github.com/h2o/h2o/blob/725e54bc932fbe0c6e208db4e71eb1df79ec43ff/CMakeLists.txt) | Re-test with newer h2o tag; the remaining CMakeLists.txt delta is the minimum needed for rules_foreign_cc. |
| `bazel/icu/icu.patch` | `icu` | keep (short-term) | Now reduced to the `icudefs.mk.in` AR fix only; the six conflicting upstream `BUILD.bazel` files are removed via `MODULE.bazel` `patch_cmds` before `configure_make` runs. Source: upstream ICU 78.2 release tarball. | Build modernization (P1.7) | [ICU 78.2 release](https://github.com/unicode-org/icu/releases/tag/release-78.2) | Re-check whether foreign_cc can consume the upstream `@AR@` substitution cleanly on a future rules/toolchain bump; until then keep the AR fix and explicit BUILD-file removals. |


## Immediate Follow-ups

1. ~~**whisper.cpp v1.8.x upgrade:**~~ **Done.** Upgraded to v1.8.3. Patch reduced from 7 hunks/4 files to 1 hunk/1 file.
2. `bazel/icu/icu.patch` is at minimum (AR fix only). `bazel/onnxruntime.patch` re-audited on ORT `1.24.3` — still non-droppable, now including the fetched ORT Extensions custom-op teardown fix needed for clean ASAN exits. External Abseil version sync is now handled without patch growth via `bazel/onnxruntime.BUILD`'s `FETCHCONTENT_SOURCE_DIR_ABSEIL_CPP` override on the one-protobuf static lane.
3. `bazel/h2o` patch is at minimum (48 lines, CMakeLists.txt-only). Re-test with newer h2o tag.
4. Re-audit this file after any dependency version bump in `MODULE.bazel`.
5. Re-audit `bazel/kakasi.patch` on any upstream `kakasi` release/source movement; no fork-backed Bazel dependency exceptions remain.
