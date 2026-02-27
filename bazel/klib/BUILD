load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "klib",
    tags = ["manual"],
    hdrs = glob(["*.h"], allow_empty = True),
    includes = ["."],
)
