# Bazel Patch Debt Inventory

This file tracks custom patches used by Bazel external dependencies.

Last audited: 2026-03-09

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
| `bazel/whisper.patch` | `whisper.cpp` | keep (medium debt) | CUDA/shared-loading via dlopen + non-speech token suppression required by current integration path. Now 7 hunks across 4 files (down from 11). | Build modernization (P1.7) | [whisper.cpp@022756a](https://github.com/ggerganov/whisper.cpp/tree/022756a87204cd06c5d58f67b3708b550dcc38b0) | Debug noise cleaned (`6cf44457`). Non-essential hunks dropped (compiler warnings, whitespace, backtrace removal), and the `GGML_USE_CUBLAS` compile-definition hunk moved into `bazel/whisper.BUILD`. Non-speech token expansion kept (test-verified requirement). |
| `bazel/brpc/0001_dynamic_annotations_guards.patch` | `com_github_brpc_brpc` | upstream candidate | Adds `#ifndef` guards to `dynamic_annotations.h` macros to avoid redefinition warnings when Abseil's version is also included. Migrates `cc_proto_library` load from `@rules_cc` to `@com_google_protobuf`. | Build modernization (P1.7) | [dynamic_annotations.h@37efd2e](https://github.com/apache/brpc/blob/37efd2e2b26aa6408fd308490acf14d86713c3fc/src/butil/third_party/dynamic_annotations/dynamic_annotations.h) | Drop guards once brpc stops bundling its own `dynamic_annotations.h` or updates it with upstream guards. |
| `bazel/braft/0001.patch` | `com_github_brpc_braft` | upstream candidate | Platform-conditional flags and Bazel portability fixes. | Build modernization (P1.7) | [braft BUILD@ab0017f](https://github.com/baidu/braft/blob/ab0017f0b98d429138d83a04d3ed351197d671a9/BUILD) | Check latest braft/brpc Bazel rules and drop if equivalent support exists upstream. |
| `bazel/braft/0002_ipv6_node_manager.patch` | `com_github_brpc_braft` | keep (product-specific) | IPv6 endpoint handling needed for Typesense node-manager behavior. | Build modernization (P1.7) | [node_manager.cpp@ab0017f](https://github.com/baidu/braft/blob/ab0017f0b98d429138d83a04d3ed351197d671a9/src/braft/node_manager.cpp) | Keep unless upstream accepts feature-equivalent IPv6 logic. |
| `bazel/braft/0002_ipv6_configuration.patch` | `com_github_brpc_braft` | keep (product-specific) | IPv6-aware configuration parsing for peer endpoints. | Build modernization (P1.7) | [configuration.h@ab0017f](https://github.com/baidu/braft/blob/ab0017f0b98d429138d83a04d3ed351197d671a9/src/braft/configuration.h) | Keep unless upstream accepts equivalent parser support. |
| `bazel/braft/0002_ipv6_node.patch` | `com_github_brpc_braft` | keep (product-specific) | IPv6-safe startup validation for node endpoint checks. | Build modernization (P1.7) | [node.cpp@ab0017f](https://github.com/baidu/braft/blob/ab0017f0b98d429138d83a04d3ed351197d671a9/src/braft/node.cpp) | Keep unless upstream adopts equivalent endpoint logic. |
| `bazel/braft/0004_util_namespace.patch` | `com_github_brpc_braft` | upstream candidate | Namespace compatibility fix in util sampling code. | Build modernization (P1.7) | [util.cpp@ab0017f](https://github.com/baidu/braft/blob/ab0017f0b98d429138d83a04d3ed351197d671a9/src/braft/util.cpp) | Still required on current pinned braft (server build fails in `src/braft/util.cpp` without `detail::Sample` namespace fix); re-check on next braft bump. |
| `bazel/braft/0005_bazel9_string_view.patch` | `com_github_brpc_braft` | upstream candidate | Fixes `%s` formatting with string-view style return to satisfy modern toolchain/Bazel 9 builds. | Build modernization (P1.7) | [replicator.cpp@ab0017f](https://github.com/baidu/braft/blob/ab0017f0b98d429138d83a04d3ed351197d671a9/src/braft/replicator.cpp) | Upstream or drop once dependency includes fix. |
| `bazel/braft/0006_cxx20_shuffle.patch` | `com_github_brpc_braft` | upstream candidate | Replaces removed `std::random_shuffle` with `std::shuffle` plus explicit headers so braft still builds under the repo's C++20 toolchain. | Build modernization (P1.7) | [replicator.cpp@ab0017f](https://github.com/baidu/braft/blob/ab0017f0b98d429138d83a04d3ed351197d671a9/src/braft/replicator.cpp) | Upstream or drop once braft adopts a C++17+/C++20-safe shuffle implementation. |
| `bazel/h2o/h2o_725e54bc932fbe0c6e208db4e71eb1df79ec43ff.patch` | `h2o` | keep (medium-term) | Removes CONFIGURE_FILE for .pc files, strips INSTALL targets for binaries/pkg-config, deletes `deps/brotli/**/BUILD` files that conflict with Bazel. | Build modernization (P1.7b) | [h2o CMakeLists@725e54b](https://github.com/h2o/h2o/blob/725e54bc932fbe0c6e208db4e71eb1df79ec43ff/CMakeLists.txt) | Re-test with newer h2o tag and reduce to minimal install-related delta. |
| `bazel/icu/icu.patch` | `icu` | keep (short-term) | Now reduced to the `icudefs.mk.in` AR fix only; the six conflicting upstream `BUILD.bazel` files are removed via `MODULE.bazel` `patch_cmds` before `configure_make` runs. Source: upstream ICU 78.2 release tarball. | Build modernization (P1.7) | [ICU 78.2 release](https://github.com/unicode-org/icu/releases/tag/release-78.2) | Re-check whether foreign_cc can consume the upstream `@AR@` substitution cleanly on a future rules/toolchain bump; until then keep the AR fix and explicit BUILD-file removals. |


## Immediate Follow-ups

1. Prioritize shrinking `bazel/whisper.patch` and the remaining non-product-specific upstream-candidate patches. `bazel/icu/icu.patch` has been reduced to the AR fix only, and `bazel/onnxruntime.patch` was re-audited on ORT `1.24.3` and is still non-droppable for now.
2. For each `upstream candidate`, open/track upstream reference and record link in this file.
3. Re-audit this file after any dependency version bump in `MODULE.bazel`.
