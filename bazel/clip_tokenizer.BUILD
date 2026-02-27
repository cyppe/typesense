load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name="clip",
    srcs=["clip_tokenizer.cpp"],
    hdrs= glob(["**/*.h"]),
    includes=["."],
    deps=["@icu"],
    visibility=["//visibility:public"],
    linkstatic=1
)
