load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "zstd",
    srcs = glob(
        [
            "lib/common/*.c",
            "lib/common/*.h",
            "lib/compress/*.c",
            "lib/compress/*.h",
            "lib/decompress/*.c",
            "lib/decompress/*.h",
            "lib/decompress/*.S",
            "lib/dictBuilder/*.c",
            "lib/dictBuilder/*.h",
        ],
    ),
    hdrs = [
        "lib/zdict.h",
        "lib/zstd.h",
        "lib/zstd_errors.h",
    ],
    copts = [
        "-O3",
        "-fPIC",
    ],
    includes = ["lib"],
    linkopts = ["-pthread"],
    local_defines = [
        "XXH_NAMESPACE=ZSTD_",
        "ZSTD_MULTITHREAD",
    ],
    visibility = ["//visibility:public"],
)
