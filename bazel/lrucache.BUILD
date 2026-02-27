load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "lrucache",
    hdrs = glob(["include/**/*.hpp"]),
    includes = ["include"],
)
