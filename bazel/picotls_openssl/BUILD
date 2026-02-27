load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "picotls_openssl",
    tags = ["manual"],
    srcs = [
        "lib/openssl.c",
        "lib/pembase64.c",
        "lib/picotls.c",
    ] + glob(["lib/*.h"]),
    hdrs = glob(["include/**/*.h"], allow_empty = True),
    copts = [
        "-std=c99",
    ],
    includes = ["include"],
    deps = [
        "@openssl",
    ],
)
