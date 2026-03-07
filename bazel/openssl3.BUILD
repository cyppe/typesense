load("@rules_foreign_cc//foreign_cc:defs.bzl", "configure_make")

config_setting(
    name = "darwin_arm64",
    constraint_values = [
        "@platforms//os:macos",
        "@platforms//cpu:arm64",
    ],
)

config_setting(
    name = "darwin_x86_64",
    constraint_values = [
        "@platforms//os:macos",
        "@platforms//cpu:x86_64",
    ],
)

config_setting(
    name = "linux_arm64",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:arm64",
    ],
)

config_setting(
    name = "linux_x86_64",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)

configure_make(
    name = "openssl",
    configure_command = "Configure",
    configure_in_place = True,
    configure_options = select({
        ":darwin_arm64": ["darwin64-arm64-cc"],
        ":darwin_x86_64": ["darwin64-x86_64-cc"],
        ":linux_arm64": ["linux-aarch64"],
        ":linux_x86_64": ["linux-x86_64"],
        "//conditions:default": [],
    }) + [
        "--libdir=lib",
        "no-shared",
        "no-tests",
        "no-apps",
    ],
    env = select({
        "@platforms//os:macos": {
            "AR": "",
            "PERL": "$$EXT_BUILD_ROOT/$(PERL)",
        },
        "//conditions:default": {
            "PERL": "$$EXT_BUILD_ROOT/$(PERL)",
        },
    }),
    lib_name = "openssl",
    lib_source = ":all_srcs",
    out_static_libs = [
        "libssl.a",
        "libcrypto.a",
    ],
    targets = ["install_sw"],
    toolchains = ["@rules_perl//:current_toolchain"],
    visibility = ["//visibility:public"],
)

alias(
    name = "crypto",
    actual = "openssl",
    visibility = ["//visibility:public"],
)

alias(
    name = "ssl",
    actual = "openssl",
    visibility = ["//visibility:public"],
)
