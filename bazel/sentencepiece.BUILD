load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])


filegroup(
    name = "sentencepiece_src",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "sentencepiece_headers",
    hdrs = glob(["src/**/*.h"]),
    includes = ["src"],
    visibility = ["//visibility:public"],
    strip_include_prefix = "src"
)


cmake(
    name = "sentencepiece",
    lib_source = "//:sentencepiece_src",
    out_static_libs = ["libsentencepiece.a"],
    out_include_dir = "include",
    build_args = [
        "--config Release",
        "--target sentencepiece-static",
    ],
    install = False,
    build_data = ["@com_google_protobuf//:protoc"],
    cache_entries = {
        'SPM_PROTOBUF_PROVIDER': 'package',
        'SPM_USE_BUILTIN_PROTOBUF': 'OFF',
        'Protobuf_LIBRARY': '$$EXT_BUILD_DEPS/lib/libprotobuf.a',
        'Protobuf_LITE_LIBRARY': '$$EXT_BUILD_DEPS/lib/libprotobuf_lite.a',
        'Protobuf_PROTOC_EXECUTABLE': '$(execpath @com_google_protobuf//:protoc)',
        'Protobuf_INCLUDE_DIR': '$$EXT_BUILD_ROOT/external/protobuf+/src',
        'CMAKE_POLICY_DEFAULT_CMP0111': 'OLD',
        'CMAKE_CXX_FLAGS': '-I$$EXT_BUILD_ROOT/external/abseil-cpp+ -I$$EXT_BUILD_ROOT/external/protobuf+/third_party/utf8_range',
    },
    deps = [
        "@com_google_absl//absl/base:core_headers",
        "@com_google_protobuf//:protobuf_lite",
        "@com_google_protobuf//:protobuf",
        "@com_google_protobuf//:protobuf_headers",
    ],
    tags = ["no-sandbox"],
    postfix_script = """
        echo "Installing sentencepiece"
        cp $$BUILD_TMPDIR/src/libsentencepiece.a $$INSTALLDIR/lib/libsentencepiece.a
    """,
)
