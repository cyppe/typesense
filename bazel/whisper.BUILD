load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "whisper_srcs",
    srcs = glob(["**"]),
)

cmake(
    name = "whisper",
    cache_entries = {
        "BUILD_SHARED_LIBS": "OFF",
        "WHISPER_BUILD_EXAMPLES": "OFF",
        "WHISPER_BUILD_TESTS": "OFF",
        "WHISPER_BUILD_SERVER": "OFF",
        "CMAKE_POSITION_INDEPENDENT_CODE": "ON",
        # CPU-only build; CUDA/Metal/Vulkan backends disabled.
        "GGML_CUDA": "OFF",
        "GGML_METAL": "OFF",
        "GGML_VULKAN": "OFF",
        "GGML_BLAS": "OFF",
        # Disable optional deps not needed by Typesense.
        "WHISPER_CURL": "OFF",
        "WHISPER_SDL2": "OFF",
        "WHISPER_COREML": "OFF",
        "WHISPER_OPENVINO": "OFF",
        "WHISPER_FFMPEG": "OFF",
        # Typesense runs whisper single-threaded; skip OpenMP.
        "GGML_OPENMP": "OFF",
        # Let the toolchain choose native optimizations.
        "GGML_NATIVE": "ON",
    },
    build_args = [
        "--", "-j8",
    ],
    lib_source = "//:whisper_srcs",
    out_static_libs = [
        "libwhisper.a",
        "libggml.a",
        "libggml-base.a",
        "libggml-cpu.a",
    ],
    tags = ["requires-network", "no-sandbox"],
)

cc_library(
    name = "whisper_headers",
    hdrs = ["include/whisper.h"] + glob(["ggml/include/*.h"]),
    includes = ["include", "ggml/include"],
    visibility = ["//visibility:public"],
)
