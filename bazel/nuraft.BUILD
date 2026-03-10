load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)

CMAKE_CACHE_ENTRIES = {
    "BUILD_SHARED_LIBS": "OFF",
    "CMAKE_BUILD_TYPE": "Release",
    "CMAKE_CXX_STANDARD": "20",
    "DISABLE_SSL": "0",
    "BUILD_TESTING": "OFF",
    "BUILD_EXAMPLES": "OFF",
    "OPENSSL_ROOT_DIR": "$$EXT_BUILD_DEPS/openssl",
    "CMAKE_CXX_FLAGS": "-fPIC -DASIO_STANDALONE -I$$EXT_BUILD_DEPS/asio/asio/include",
}

cmake(
    name = "nuraft",
    build_args = ["-j8"],
    cache_entries = CMAKE_CACHE_ENTRIES,
    lib_source = ":all_srcs",
    out_static_libs = ["libnuraft.a"],
    deps = [
        "@openssl",
        "@asio",
    ],
)
