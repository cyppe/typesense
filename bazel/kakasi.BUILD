load("@rules_foreign_cc//foreign_cc:defs.bzl", "configure_make")

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)

configure_make(
    name = "kakasi",
    configure_in_place = True,
    configure_options = ["--enable-shared=no"],
    env = select({
        # https://github.com/bazelbuild/rules_foreign_cc/issues/947#issuecomment-1208960469
        "@platforms//os:macos": {
            "AR": "",
        },
        # Sanitizer flags break kakasi's configure iconv detection
        # (EUC-JP/UTF-8 test programs fail under instrumentation).
        # Override CFLAGS/CXXFLAGS to strip sanitizer flags for this dep.
        "@@//:asan_mode": {
            "CFLAGS": "-fno-sanitize=address",
            "CXXFLAGS": "-fno-sanitize=address",
            "LDFLAGS": "-fno-sanitize=address",
        },
        "@@//:tsan_mode": {
            "CFLAGS": "-fno-sanitize=thread",
            "CXXFLAGS": "-fno-sanitize=thread",
            "LDFLAGS": "-fno-sanitize=thread",
        },
        "//conditions:default": {},
    }),
    lib_source = "@kakasi//:all_srcs",
    out_static_libs = ["libkakasi.a"],
    visibility = ["//visibility:public"],
    deps = [
        "@iconv",
    ],
)
