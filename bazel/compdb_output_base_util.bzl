def _output_base_util_impl(repository_ctx):
    res = repository_ctx.execute(["pwd"])
    if res.return_code != 0:
        fail("getting output base failed (%d): %s" % (res.return_code, res.stderr))

    path_components = res.stdout.rstrip("\n").split("/")[:-2]
    output_base = "/".join(path_components)

    repository_ctx.file("BUILD.bazel", "")
    repository_ctx.file("defs.bzl", "OUTPUT_BASE = '%s'" % output_base)

output_base_util_repository = repository_rule(
    implementation = _output_base_util_impl,
    local = True,
)
