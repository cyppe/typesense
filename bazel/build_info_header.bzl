def _typesense_build_info_header_impl(ctx):
    output = ctx.actions.declare_file("build_info_generated.h")
    ctx.actions.run(
        executable = ctx.executable._generator,
        inputs = [ctx.info_file],
        outputs = [output],
        arguments = [ctx.info_file.path, output.path],
        mnemonic = "TypesenseBuildInfoHeader",
        progress_message = "Generating %s" % output.short_path,
    )
    return [DefaultInfo(files = depset([output]))]

typesense_build_info_header = rule(
    implementation = _typesense_build_info_header_impl,
    attrs = {
        "_generator": attr.label(
            default = "//bazel:generate_build_info_header.sh",
            executable = True,
            cfg = "exec",
            allow_single_file = True,
        ),
    },
)
