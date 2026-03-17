def maybe_with_cuda_architectures(entries, cuda_architectures):
    result = dict(entries)
    if cuda_architectures:
        result["CMAKE_CUDA_ARCHITECTURES"] = cuda_architectures
    return result

def cuda_impl(repository_ctx):
    repository_ctx.file(
        "cuda_helpers.bzl",
        """
def maybe_with_cuda_architectures(entries, cuda_architectures):
    result = dict(entries)
    if cuda_architectures:
        result["CMAKE_CUDA_ARCHITECTURES"] = cuda_architectures
    return result
""",
    )
    repository_ctx.file(
        "ort_build_jobs.bzl",
        "ORT_BUILD_JOBS = \"%s\"" % repository_ctx.os.environ.get("TYPESENSE_ORT_BUILD_JOBS", "3"),
    )
    repository_ctx.file("cuda_home.bzl", "CUDA_HOME = \"%s\"" % repository_ctx.os.environ.get("CUDA_HOME", ""))
    repository_ctx.file("cudnn_home.bzl", "CUDNN_HOME = \"%s\"" % repository_ctx.os.environ.get("CUDNN_HOME", ""))
    repository_ctx.file(
        "cuda_architectures.bzl",
        "CUDA_ARCHITECTURES = \"%s\"" % repository_ctx.os.environ.get("TYPESENSE_ORT_CUDA_ARCHITECTURES", ""),
    )
    repository_ctx.file("BUILD", "exports_files([\"cuda_helpers.bzl\", \"ort_build_jobs.bzl\", \"cuda_home.bzl\", \"cudnn_home.bzl\", \"cuda_architectures.bzl\"])")

cuda_home_repository = repository_rule(
    implementation=cuda_impl,
    environ = ["CUDA_HOME", "CUDNN_HOME", "TYPESENSE_ORT_CUDA_ARCHITECTURES", "TYPESENSE_ORT_BUILD_JOBS"],
)
