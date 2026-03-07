load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "operators",
    srcs = [
        "operators/tokenizer/bert_tokenizer.cc",
        "operators/tokenizer/basic_tokenizer.cc",
        "base/string_utils.cc",
    ],
    hdrs = [
        "operators/tokenizer/bert_tokenizer.hpp",
        "operators/tokenizer/basic_tokenizer.hpp",
        "base/ustring.h",
        "base/string_utils.h",
        "base/ortx_stubs.h",
    ],
    includes = ["operators", "base"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "operators_headers",
    hdrs = [
        "operators/tokenizer/bert_tokenizer.hpp",
        "operators/tokenizer/basic_tokenizer.hpp",
        "base/ustring.h",
        "base/string_utils.h",
        "base/ortx_stubs.h",
    ],
    includes = ["operators", "base"],
    visibility = ["//visibility:public"],
)
