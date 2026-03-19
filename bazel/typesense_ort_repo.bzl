_STATIC_LIBS = [
    ("onnxruntime_session", "ort/lib/libonnxruntime_session.a"),
    ("onnxruntime_optimizer", "ort/lib/libonnxruntime_optimizer.a"),
    ("onnxruntime_providers", "ort/lib/libonnxruntime_providers.a"),
    ("onnxruntime_util", "ort/lib/libonnxruntime_util.a"),
    ("onnxruntime_framework", "ort/lib/libonnxruntime_framework.a"),
    ("onnxruntime_graph", "ort/lib/libonnxruntime_graph.a"),
    ("onnxruntime_lora", "ort/lib/libonnxruntime_lora.a"),
    ("onnxruntime_mlas", "ort/lib/libonnxruntime_mlas.a"),
    ("onnxruntime_common", "ort/lib/libonnxruntime_common.a"),
    ("onnxruntime_flatbuffers", "ort/lib/libonnxruntime_flatbuffers.a"),
    ("libjpeg_static", "ort/lib/liblibjpeg_static_c.a"),
    ("libpng_static", "ort/lib/liblibpng_static_c.a"),
    ("ortcustomops", "ort/lib/libortcustomops.a"),
    ("ocos_operators", "ort/lib/libocos_operators.a"),
    ("noexcep_operators", "ort/lib/libnoexcep_operators.a"),
    ("onnx_dep", "ort/lib/_deps/onnx-build/libonnx.a"),
    ("onnx_proto_dep", "ort/lib/_deps/onnx-build/libonnx_proto.a"),
    ("re2_dep", "ort/lib/_deps/re2-build/libre2.a"),
    ("absl_base_dep", "ort/lib/_deps/abseil_cpp-build/absl/base/libabsl_base.a"),
    ("absl_throw_delegate_dep", "ort/lib/_deps/abseil_cpp-build/absl/base/libabsl_throw_delegate.a"),
    ("absl_raw_hash_set_dep", "ort/lib/_deps/abseil_cpp-build/absl/container/libabsl_raw_hash_set.a"),
    ("absl_hash_dep", "ort/lib/_deps/abseil_cpp-build/absl/hash/libabsl_hash.a"),
    ("absl_city_dep", "ort/lib/_deps/abseil_cpp-build/absl/hash/libabsl_city.a"),
    ("absl_hashtablez_sampler_dep", "ort/lib/_deps/abseil_cpp-build/absl/container/libabsl_hashtablez_sampler.a"),
    ("cpuinfo_dep", "ort/lib/_deps/pytorch_cpuinfo-build/libcpuinfo.a"),
]

def _render_alias_build():
    return """package(default_visibility = [\"//visibility:public\"])

alias(
    name = "onnxruntime_static_one_protobuf",
    actual = "@onnx_runtime//:onnxruntime_static_one_protobuf",
)

alias(
    name = "onnxruntime_static_one_protobuf_lib",
    actual = "@onnx_runtime//:onnxruntime_static_one_protobuf_lib",
)

filegroup(
    name = "gpu_provider_sidecars",
    srcs = [],
)
"""

def _render_prebuilt_build():
    lines = [
        "load(\"@rules_cc//cc:defs.bzl\", \"cc_import\", \"cc_library\")",
        "",
        "package(default_visibility = [\"//visibility:public\"])",
        "",
        "cc_library(",
        "    name = \"hdrs\",",
        "    hdrs = glob([\"ort/include/onnxruntime/**/*.h\"]),",
        "    strip_include_prefix = \"ort/include/onnxruntime\",",
        ")",
        "",
    ]

    for name, path in _STATIC_LIBS:
        lines.extend([
            "cc_import(",
            "    name = \"%s\"," % name,
            "    static_library = \"%s\"," % path,
            ")",
            "",
        ])

    dep_lines = ["        \":%s\"," % name for name, _ in _STATIC_LIBS]

    lines.extend([
        "cc_library(",
        "    name = \"onnxruntime_static_one_protobuf\",",
        "    deps = [",
    ] + dep_lines + [
        "    ],",
        ")",
        "",
        "cc_library(",
        "    name = \"onnxruntime_static_one_protobuf_lib\",",
        "    deps = [",
        "        \":onnxruntime_static_one_protobuf\",",
        "        \":hdrs\",",
        "        \"@onnx_runtime_extensions//:operators_headers\",",
        "    ],",
        "    linkopts = select({",
        "        \"@platforms//os:linux\": [\"-static-libstdc++\", \"-static-libgcc\"],",
        "        \"//conditions:default\": [],",
        "    }),",
        ")",
        "",
        "exports_files([",
        "    \"ort/lib/libonnxruntime_providers_shared.so\",",
        "    \"ort/lib/libonnxruntime_providers_cuda.so\",",
        "])",
        "",
        "filegroup(",
        "    name = \"gpu_provider_sidecars\",",
        "    srcs = [",
        "        \"ort/lib/libonnxruntime_providers_shared.so\",",
        "        \"ort/lib/libonnxruntime_providers_cuda.so\",",
        "    ],",
        ")",
        "",
    ])

    return "\n".join(lines)

def _typesense_ort_repository_impl(repository_ctx):
    bundle_dir = repository_ctx.os.environ.get("TYPESENSE_ORT_PREBUILT_BUNDLE_DIR", "").strip()

    repository_ctx.file("REPO.bazel", "")

    if not bundle_dir:
        repository_ctx.file("BUILD.bazel", _render_alias_build())
        return

    bundle_path = repository_ctx.path(bundle_dir)
    if not bundle_path.exists:
        fail("TYPESENSE_ORT_PREBUILT_BUNDLE_DIR does not exist: %s" % bundle_dir)

    repository_ctx.symlink(bundle_path, "ort")
    repository_ctx.file("BUILD.bazel", _render_prebuilt_build())

typesense_ort_repository = repository_rule(
    implementation = _typesense_ort_repository_impl,
    environ = [
        "TYPESENSE_ORT_PREBUILT_BUNDLE_DIR",
    ],
)
