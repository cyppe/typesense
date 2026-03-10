load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "asio",
    hdrs = glob(["asio/include/**/*.hpp", "asio/include/**/*.ipp"]),
    includes = ["asio/include"],
    defines = ["ASIO_STANDALONE"],
    linkopts = ["-lpthread"],
)
