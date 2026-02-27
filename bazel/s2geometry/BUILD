load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

package(default_visibility = ["//visibility:public"])

CMAKE_CACHE_ENTRIES = {
    "BUILD_SHARED_LIBS": "OFF",
    "BUILD_TESTS": "OFF",
    "CMAKE_BUILD_TYPE": "Release",
    "CMAKE_USE_OPENSSL": "ON",
    "OPENSSL_ROOT_DIR": "$$EXT_BUILD_DEPS/openssl",
    # Custom variable consumed by absl_stubs.cmake (created via patch_cmds).
    # rules_foreign_cc strips CMAKE_CXX_FLAGS from cache_entries into the
    # toolchain file, so we use a custom variable instead.
    "ABSL_INCLUDE_DIR": "$$EXT_BUILD_ROOT/external/abseil-cpp+",
}

CMAKE_MACOS_CACHE_ENTRIES = CMAKE_CACHE_ENTRIES

CMAKE_LINUX_CACHE_ENTRIES = dict(CMAKE_CACHE_ENTRIES.items() + {
    "CMAKE_C_FLAGS": "-fPIC",
}.items())

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)

cmake(
    name = "s2geometry",
    tags = ["manual"],
    build_args = ["-j8"],
    cache_entries = select({
        "@platforms//os:linux": CMAKE_LINUX_CACHE_ENTRIES,
        "@platforms//os:macos": CMAKE_MACOS_CACHE_ENTRIES,
        "//conditions:default": CMAKE_CACHE_ENTRIES,
    }),
    generate_args = [
        "-DCMAKE_FIND_LIBRARY_SUFFIXES=.a",
        "-DBUILD_SHARED_LIBS=OFF",
    ],
    lib_source = ":all_srcs",
    out_static_libs = ["libs2.a"],
    targets = ["s2"],
    deps = [
        "@openssl",
        # Abseil headers needed by s2geometry v0.12.0. These cc_library deps
        # make the abseil source tree available in the rules_foreign_cc sandbox.
        # The actual include path is provided via ABSL_INCLUDE_DIR + absl_stubs.cmake.
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:config",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/base:dynamic_annotations",
        "@com_google_absl//absl/container:btree",
        "@com_google_absl//absl/container:fixed_array",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/container:inlined_vector",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:reflection",
        "@com_google_absl//absl/hash",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:absl_check",
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/base:log_severity",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/numeric:int128",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/random:bit_gen_ref",
        "@com_google_absl//absl/random:distributions",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/types:span",
        "@com_google_absl//absl/utility",
    ],
)
