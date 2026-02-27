load("@rules_foreign_cc//foreign_cc:defs.bzl", "configure_make")
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])


filegroup(
    name = "snowball_src",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "snowball_headers",
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    strip_include_prefix = "include"
)


# Use configure_make instead of make() because configure_in_place=True copies
# sources (not symlinks). The make() rule symlinks source dirs, which breaks
# GCC relative include resolution for snowball's generated headers in src_c/.
configure_make(
    name = "snowball",
    lib_name = "stemmer",
    lib_source = "//:snowball_src",
    out_static_libs = ["libstemmer.a"],
    out_include_dir = "include",
    configure_command = "configure",
    configure_in_place = True,
    configure_options = [],
    env = select({
        "@platforms//os:macos": {
            "AR": "/usr/bin/ar",
        },
        "//conditions:default": {},
    }),
    args = ["-j12"],
    tags = ["no-sandbox"],
    targets = [""],
    postfix_script = """
        echo "Installing snowball"
        cp $$BUILD_TMPDIR/libstemmer.a $$INSTALLDIR/lib/libstemmer.a
    """,
)
