load("@rules_cc//cc:defs.bzl", "cc_library")

# lz4hc.c textually includes lz4.c (#include "lz4.c") for common defs.
# Bazel sandboxing requires textual_hdrs for this, but the same file
# can't be in both srcs and textual_hdrs. We copy lz4.c for compilation
# as a separate unit, and keep the original as a textual header.
genrule(
    name = "lz4_impl_src",
    srcs = ["lib/lz4.c"],
    outs = ["lz4_impl.c"],
    cmd = "cp $< $@",
)

cc_library(
    name = "lz4",
    srcs = [
        "lz4_impl.c",
        "lib/lz4hc.c",
        "lib/lz4frame.c",
        "lib/xxhash.c",
    ],
    hdrs = [
        "lib/lz4.h",
        "lib/lz4hc.h",
        "lib/lz4frame.h",
        "lib/lz4frame_static.h",
        "lib/xxhash.h",
    ],
    textual_hdrs = [
        "lib/lz4.c",
    ],
    copts = [
        "-O3",
        "-fPIC",
    ],
    includes = ["lib"],
    visibility = ["//visibility:public"],
)
