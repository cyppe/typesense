load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "usearch",
    hdrs = glob([
        "include/**/*.h",
        "include/**/*.hpp",
        "fp16/include/**/*.h",
        "simsimd/include/**/*.h",
        "simsimd/include/**/*.hpp",
        "stringzilla/include/**/*.h",
        "stringzilla/include/**/*.hpp",
    ], allow_empty = True),
    includes = [
        "include",
        "fp16/include",
        "simsimd/include",
        "stringzilla/include",
    ],
)
