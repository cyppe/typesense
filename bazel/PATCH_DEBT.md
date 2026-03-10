# Bazel Patch Debt Inventory

This file tracks custom patches used by Bazel external dependencies.

Last audited: 2026-03-10

## Scope

- Source of truth for active Bazel patches: `MODULE.bazel` `patches = [...]` entries.
- Includes patch files under `bazel/` currently referenced by Bazel module rules.
- Excludes legacy CMake patch flow under `cmake/*.patch`.

## Removed As Stale

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
| `bazel/onnxruntime.patch` | `onnx_runtime` | keep (medium-term) | Re-audited on ORT `1.24.3`: still needed to set `onnxruntime_EXTENSIONS_PATH`, patch the fetched extensions zlib 1.3.x guard/static image codec export behavior, and inject imported external protobuf targets for the one-protobuf static build. | Build modernization (P1.7) | [cmake/external@3a728b7](https://github.com/microsoft/onnxruntime/tree/3a728b75062256951b6e19ce718907cf1a1d4cf0/cmake/external) | Re-check on the next ORT + extensions bump; drop only when upstream covers path export, static codec install/export, and a supported external-protobuf path. |
| `bazel/onnx_ext.patch` | `onnx_runtime_extensions` | keep (short-term) | Trims tokenizer/operator surface and header deps to build selected operators under current toolchain. | Build modernization (P1.7) | [tokenizer headers@1f9d7ee](https://github.com/microsoft/onnxruntime-extensions/tree/1f9d7ee0c80b4f94946ee5650cb242aa01dd6278/operators/tokenizer) | Revisit with newer extensions release; prefer upstream/stable API include layout. |
| `bazel/whisper.patch` | `whisper.cpp` | keep (medium debt) | CUDA/shared-loading via dlopen + non-speech token suppression required by current integration path. Now 7 hunks across 4 files (down from 11). Pin `022756a8` is pre-v1.7.x (still uses deprecated `GGML_USE_CUBLAS`). | Build modernization (P1.7) | [whisper.cpp@022756a](https://github.com/ggerganov/whisper.cpp/tree/022756a87204cd06c5d58f67b3708b550dcc38b0) | All 7 hunks audited and confirmed necessary on current pin. **Major upgrade to v1.8.x planned (P2 sprint):** ggml now has proper backend plugin loading, which would eliminate all 6 CUDA dlopen/dlsym hunks. Flash attention, VAD, and hallucination fixes also available. Requires `whisper.BUILD` rewrite since project structure changed fundamentally. |
| `bazel/h2o/h2o_725e54bc932fbe0c6e208db4e71eb1df79ec43ff.patch` | `h2o` | keep (medium-term) | Now reduced to CMakeLists.txt-only delta: removes CONFIGURE_FILE for .pc files and strips INSTALL targets for binaries/pkg-config. The nine conflicting `deps/brotli/**/BUILD` files are now removed via `MODULE.bazel` `patch_cmds` (same pattern as ICU). Reduced from 774 lines to 48 lines. | Build modernization (P1.7b) | [h2o CMakeLists@725e54b](https://github.com/h2o/h2o/blob/725e54bc932fbe0c6e208db4e71eb1df79ec43ff/CMakeLists.txt) | Re-test with newer h2o tag; the remaining CMakeLists.txt delta is the minimum needed for rules_foreign_cc. |
| `bazel/icu/icu.patch` | `icu` | keep (short-term) | Now reduced to the `icudefs.mk.in` AR fix only; the six conflicting upstream `BUILD.bazel` files are removed via `MODULE.bazel` `patch_cmds` before `configure_make` runs. Source: upstream ICU 78.2 release tarball. | Build modernization (P1.7) | [ICU 78.2 release](https://github.com/unicode-org/icu/releases/tag/release-78.2) | Re-check whether foreign_cc can consume the upstream `@AR@` substitution cleanly on a future rules/toolchain bump; until then keep the AR fix and explicit BUILD-file removals. |


## Immediate Follow-ups

1. **whisper.cpp v1.8.x upgrade (P2 sprint):** Would eliminate 6/7 patch hunks. Major effort — ggml separated into own subproject, `whisper.BUILD` needs rewrite. Planned but not started.
2. `bazel/icu/icu.patch` is at minimum (AR fix only). `bazel/onnxruntime.patch` re-audited on ORT `1.24.3` — still non-droppable.
3. `bazel/h2o` patch is at minimum (48 lines, CMakeLists.txt-only). Re-test with newer h2o tag.
4. Re-audit this file after any dependency version bump in `MODULE.bazel`.
