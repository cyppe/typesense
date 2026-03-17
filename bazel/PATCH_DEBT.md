# Bazel Patch Debt Inventory

This file tracks custom patches used by Bazel external dependencies.

Last audited: 2026-03-17

## Scope

- Source of truth for active Bazel patches: `MODULE.bazel` `patches = [...]` entries.
- Includes patch files under `bazel/` currently referenced by Bazel module rules.
- Excludes legacy CMake patch flow under `cmake/*.patch`.
- Also tracks fork-backed dependency replacements that are part of item 34's "prefer upstream releases over stale forks" work.

## Fork-backed dependency audit

| Dep | Current source | Status | Why it exists / next action |
|---|---|---|---|
| `hnsw` | upstream `nmslib/hnswlib` commit `687d9817` | switched on 2026-03-17 | Replaced the stale `typesense/hnswlib` fork pin with the same upstream parent commit. GitHub compare showed the fork was `ahead_by=0`, `behind_by=54`, so it carried no fork-only commits. Attempting the upstream `v0.8.0` release was a no-go because it lacks the `searchKnnCloserFirst(..., ef, filter)` overload and `repair_zero_indegree()` that `src/index.cpp` currently uses. |
| `kakasi` | `typesense/kakasi` fork commit `77f2d1ce` | keep for now | GitHub compare against parent `loretoparisi/kakasi` shows the fork is `ahead_by=5`, `behind_by=0`, and the parent repo has no GitHub release history recorded here, so this is not a drop-in upstream release swap yet. Audit those five commits before attempting an upstream move. |
| `clip_tokenizer` | `typesense/clip_tokenizer_cpp` | keep for now | The repo is not a GitHub fork and no release history / upstream parent is recorded here yet, so there is no authoritative upstream release path to switch to today. Revisit only after identifying the real upstream project and release contract. |

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
| `bazel/onnxruntime.patch` | `onnx_runtime` | keep (medium-term) | Re-audited on ORT `1.24.3`: still needed to set `onnxruntime_EXTENSIONS_PATH`, patch the fetched extensions zlib 1.3.x guard/static image codec export behavior, rewrite fetched `OrtOpLoader` statics to process lifetime so ASAN no longer trips on ORT Extensions teardown, and inject imported external protobuf targets for the one-protobuf static build. External Abseil version sync is now handled patch-free in `bazel/onnxruntime.BUILD` via `FETCHCONTENT_SOURCE_DIR_ABSEIL_CPP` for that same one-protobuf lane. | Build modernization (P1.7) | [cmake/external@3a728b7](https://github.com/microsoft/onnxruntime/tree/3a728b75062256951b6e19ce718907cf1a1d4cf0/cmake/external) | Re-check on the next ORT + extensions bump; drop only when upstream covers path export, static codec install/export, the custom-op teardown path, and a supported external-protobuf path. Keep the BUILD-level Abseil source override unless upstream exposes a cleaner version-sync hook. |
| `bazel/onnx_ext.patch` | `onnx_runtime_extensions` | keep (short-term) | Trims tokenizer/operator surface and header deps to build selected operators under current toolchain. | Build modernization (P1.7) | [tokenizer headers@1f9d7ee](https://github.com/microsoft/onnxruntime-extensions/tree/1f9d7ee0c80b4f94946ee5650cb242aa01dd6278/operators/tokenizer) | Revisit with newer extensions release; prefer upstream/stable API include layout. |
| `bazel/whisper.patch` | `whisper.cpp` | keep (minimal) | Non-speech token expansion only (1 hunk, 1 file). Pin `2eeeba56` = v1.8.3. Upgraded from `022756a8` (pre-v1.7.x). All CUDA dlopen/dlsym hunks eliminated by upgrade. | Build modernization (P1.7) | [whisper.cpp@2eeeba5](https://github.com/ggml-org/whisper.cpp/tree/2eeeba56e9edd762b4b38467bab96c2517163158) | Patch is at minimum. Non-speech token list is Typesense-specific (punctuation suppression for voice queries). Monitor upstream for inclusion. |
| `bazel/h2o/h2o_725e54bc932fbe0c6e208db4e71eb1df79ec43ff.patch` | `h2o` | keep (medium-term) | Now reduced to CMakeLists.txt-only delta: removes CONFIGURE_FILE for .pc files and strips INSTALL targets for binaries/pkg-config. The nine conflicting `deps/brotli/**/BUILD` files are now removed via `MODULE.bazel` `patch_cmds` (same pattern as ICU). Reduced from 774 lines to 48 lines. | Build modernization (P1.7b) | [h2o CMakeLists@725e54b](https://github.com/h2o/h2o/blob/725e54bc932fbe0c6e208db4e71eb1df79ec43ff/CMakeLists.txt) | Re-test with newer h2o tag; the remaining CMakeLists.txt delta is the minimum needed for rules_foreign_cc. |
| `bazel/icu/icu.patch` | `icu` | keep (short-term) | Now reduced to the `icudefs.mk.in` AR fix only; the six conflicting upstream `BUILD.bazel` files are removed via `MODULE.bazel` `patch_cmds` before `configure_make` runs. Source: upstream ICU 78.2 release tarball. | Build modernization (P1.7) | [ICU 78.2 release](https://github.com/unicode-org/icu/releases/tag/release-78.2) | Re-check whether foreign_cc can consume the upstream `@AR@` substitution cleanly on a future rules/toolchain bump; until then keep the AR fix and explicit BUILD-file removals. |


## Immediate Follow-ups

1. ~~**whisper.cpp v1.8.x upgrade:**~~ **Done.** Upgraded to v1.8.3. Patch reduced from 7 hunks/4 files to 1 hunk/1 file.
2. `bazel/icu/icu.patch` is at minimum (AR fix only). `bazel/onnxruntime.patch` re-audited on ORT `1.24.3` — still non-droppable, now including the fetched ORT Extensions custom-op teardown fix needed for clean ASAN exits. External Abseil version sync is now handled without patch growth via `bazel/onnxruntime.BUILD`'s `FETCHCONTENT_SOURCE_DIR_ABSEIL_CPP` override on the one-protobuf static lane.
3. `bazel/h2o` patch is at minimum (48 lines, CMakeLists.txt-only). Re-test with newer h2o tag.
4. Re-audit this file after any dependency version bump in `MODULE.bazel`.
5. Continue the remaining fork-backed audit after the `hnsw` upstream switch: `kakasi` still carries 5 fork-only commits over its parent, and `clip_tokenizer_cpp` still needs an identified upstream release path before it can leave the Typesense org.
