load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

package(default_visibility = ["//visibility:public"])

config_setting(
    name = "darwin_arm64",
    constraint_values = [
        "@platforms//os:macos",
        "@platforms//cpu:arm64",
    ],
)

config_setting(
    name = "darwin_x86_64",
    constraint_values = [
        "@platforms//os:macos",
        "@platforms//cpu:x86_64",
    ],
)

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)

CMAKE_CACHE_ENTRIES = {
    "BUILD_SHARED_LIBS": "OFF",
    "CMAKE_BUILD_TYPE": "Release",
}

COMMON_GENERATE_ARGS = [
    "-DCMAKE_FIND_LIBRARY_SUFFIXES=.a",
    "-DCMAKE_CXX_FLAGS=-DNDEBUG",
    "-DCMAKE_C_FLAGS=-DNDEBUG",
    "-DWITH_MRUBY=OFF",
    "-DWITH_CCACHE=OFF",
    "-DWITH_IO_URING=OFF",
    "-DWITH_AEGIS=OFF",
    "-DWITH_ACME=OFF",
]

MACOS_GENERATE_ARGS = [
    "-DOPENSSL_USE_STATIC_LIBS=TRUE",
]

DARWIN_ARM64_OPENSSL_PREFIX = "bazel-out/darwin_arm64-fastbuild/bin/external/+http_archive+openssl/openssl"

DARWIN_X86_64_OPENSSL_PREFIX = "bazel-out/darwin_x86_64-fastbuild/bin/external/+http_archive+openssl/openssl"

CMAKE_MACOS_CACHE_ENTRIES = dict(CMAKE_CACHE_ENTRIES.items() + {}.items())

CMAKE_LINUX_CACHE_ENTRIES = dict(CMAKE_CACHE_ENTRIES.items() + {
    "CMAKE_C_FLAGS": "-fPIC",
}.items())

cmake(
    name = "h2o",
    tags = ["manual"],
    build_args = ["-j8"],
    cache_entries = select({
        "@platforms//os:linux": CMAKE_LINUX_CACHE_ENTRIES,
        "@platforms//os:macos": CMAKE_MACOS_CACHE_ENTRIES,
        "//conditions:default": CMAKE_CACHE_ENTRIES,
    }),
    generate_args = COMMON_GENERATE_ARGS + select({
        ":darwin_arm64": MACOS_GENERATE_ARGS + [
            "-DOPENSSL_ROOT_DIR=%s" % DARWIN_ARM64_OPENSSL_PREFIX,
            "-DOPENSSL_INCLUDE_DIR=%s/include" % DARWIN_ARM64_OPENSSL_PREFIX,
            "-DOPENSSL_CRYPTO_LIBRARY=%s/lib/libcrypto.a" % DARWIN_ARM64_OPENSSL_PREFIX,
            "-DOPENSSL_SSL_LIBRARY=%s/lib/libssl.a" % DARWIN_ARM64_OPENSSL_PREFIX,
        ],
        ":darwin_x86_64": MACOS_GENERATE_ARGS + [
            "-DOPENSSL_ROOT_DIR=%s" % DARWIN_X86_64_OPENSSL_PREFIX,
            "-DOPENSSL_INCLUDE_DIR=%s/include" % DARWIN_X86_64_OPENSSL_PREFIX,
            "-DOPENSSL_CRYPTO_LIBRARY=%s/lib/libcrypto.a" % DARWIN_X86_64_OPENSSL_PREFIX,
            "-DOPENSSL_SSL_LIBRARY=%s/lib/libssl.a" % DARWIN_X86_64_OPENSSL_PREFIX,
        ],
        "//conditions:default": [],
    }),
    lib_source = ":all_srcs",
    install = False,
    out_static_libs = ["libh2o-evloop.a"],
    targets = ["libh2o-evloop"],
    postfix_script = """
        mkdir -p $$INSTALLDIR/lib $$INSTALLDIR/include
        cp $$BUILD_TMPDIR/libh2o-evloop.a $$INSTALLDIR/lib/libh2o-evloop.a
        src_dir=$$(grep '^CMAKE_HOME_DIRECTORY:INTERNAL=' CMakeCache.txt | cut -d= -f2)
        cp -r "$$src_dir/include/"* $$INSTALLDIR/include/
    """,
    deps = [
        "@picotls_openssl",
        "@quicly",
    ],
)
