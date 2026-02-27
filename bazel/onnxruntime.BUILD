load("@rules_cc//cc:defs.bzl", "cc_library")

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "hdrs",
    hdrs = glob(["include/onnxruntime/**/*.h"]),
    strip_include_prefix = "include/onnxruntime",
    visibility = ["//visibility:public"],
)

load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

# When built as a shared library, onnxruntime statically links its own
# bundled protobuf v3.21.12 and abseil inside libonnxruntime.so.
# A linker version script hides all internal symbols, only exporting the
# ORT C API.  This prevents ODR violations with the project's protobuf v28.3.
__POSTFIX = """
# cmake install should produce libonnxruntime.so (possibly versioned).
# Ensure the unversioned symlink exists for rules_foreign_cc.
cd $$INSTALLDIR/lib
if [ ! -e libonnxruntime.so ]; then
    for so in libonnxruntime.so.*; do
        if [ -f "$$so" ] && [ ! -L "$$so" ]; then
            ln -sf "$$so" libonnxruntime.so
            break
        fi
    done
fi

# Tests and binaries are linked against SONAME libonnxruntime.so.1.
# Keep an explicit SONAME symlink in the runfiles tree.
if [ ! -e libonnxruntime.so.1 ] && [ -e libonnxruntime.so ]; then
    ln -sf libonnxruntime.so libonnxruntime.so.1
fi
"""

__POSTFIX_WITH_CUDA = __POSTFIX + """
cp $$BUILD_TMPDIR/libonnxruntime_providers_shared.so $$INSTALLDIR/../../../
cp $$BUILD_TMPDIR/libonnxruntime_providers_cuda.so $$INSTALLDIR/../../../
"""


load("@cuda_home_repo//:cuda_home.bzl", "CUDA_HOME")
load("@cuda_home_repo//:cudnn_home.bzl", "CUDNN_HOME")

__ONNXRUNTIME_WITHOUT_CUDA = {'onnxruntime_RUN_ONNX_TESTS':'OFF',
'onnxruntime_BUILD_UNIT_TESTS':'OFF',
'onnxruntime_GENERATE_TEST_REPORTS':'ON',
'onnxruntime_USE_MIMALLOC':'OFF',
'onnxruntime_ENABLE_PYTHON':'OFF',
'onnxruntime_BUILD_CSHARP':'OFF',
'onnxruntime_BUILD_JAVA':'OFF',
'onnxruntime_BUILD_NODEJS':'OFF',
'onnxruntime_BUILD_OBJC':'OFF',
'onnxruntime_BUILD_SHARED_LIB':'ON',
'onnxruntime_BUILD_APPLE_FRAMEWORK':'OFF',
'onnxruntime_USE_DNNL':'OFF',
'onnxruntime_USE_NNAPI_BUILTIN':'OFF',
'onnxruntime_USE_RKNPU':'OFF',
'onnxruntime_USE_LLVM':'OFF',
'onnxruntime_ENABLE_MICROSOFT_INTERNAL':'OFF',
'onnxruntime_USE_VITISAI':'OFF',
'onnxruntime_USE_TENSORRT':'OFF',
'onnxruntime_SKIP_AND_PERFORM_FILTERED_TENSORRT_TESTS':'ON',
'onnxruntime_USE_TENSORRT_BUILTIN_PARSER':'OFF',
'onnxruntime_TENSORRT_PLACEHOLDER_BUILDER':'OFF', 'onnxruntime_USE_MIGRAPHX':'OFF', 'onnxruntime_CROSS_COMPILING':'OFF',
'onnxruntime_DISABLE_CONTRIB_OPS':'OFF', 'onnxruntime_DISABLE_ML_OPS':'OFF',
'onnxruntime_DISABLE_RTTI':'OFF', 'onnxruntime_DISABLE_EXCEPTIONS':'OFF',
'onnxruntime_MINIMAL_BUILD':'OFF', 'onnxruntime_EXTENDED_MINIMAL_BUILD':'OFF',
'onnxruntime_MINIMAL_BUILD_CUSTOM_OPS':'OFF', 'onnxruntime_REDUCED_OPS_BUILD':'OFF',
'onnxruntime_ENABLE_LANGUAGE_INTEROP_OPS':'OFF', 'onnxruntime_USE_DML':'OFF',
'onnxruntime_USE_WINML':'OFF', 'onnxruntime_BUILD_MS_EXPERIMENTAL_OPS':'OFF',
'onnxruntime_USE_TELEMETRY':'OFF', 'onnxruntime_ENABLE_LTO':'OFF',
'onnxruntime_USE_ACL':'OFF', 'onnxruntime_USE_ACL_1902':'OFF',
'onnxruntime_USE_ACL_1905':'OFF', 'onnxruntime_USE_ACL_1908':'OFF',
'onnxruntime_USE_ACL_2002':'OFF', 'onnxruntime_USE_ARMNN':'OFF',
'onnxruntime_ARMNN_RELU_USE_CPU':'ON', 'onnxruntime_ARMNN_BN_USE_CPU':'ON',
'onnxruntime_ENABLE_NVTX_PROFILE':'OFF', 'onnxruntime_ENABLE_TRAINING':'OFF',
'onnxruntime_ENABLE_TRAINING_OPS':'OFF', 'onnxruntime_ENABLE_TRAINING_APIS':'OFF',
'onnxruntime_ENABLE_CPU_FP16_OPS':'OFF', 'onnxruntime_USE_NCCL':'OFF',
'onnxruntime_BUILD_BENCHMARKS':'OFF', 'onnxruntime_USE_ROCM':'OFF',
'Onnxruntime_GCOV_COVERAGE':'OFF', 'onnxruntime_ENABLE_MEMORY_PROFILE':'OFF',
'onnxruntime_ENABLE_CUDA_LINE_NUMBER_INFO':'OFF',
'onnxruntime_BUILD_WEBASSEMBLY':'OFF', 'onnxruntime_BUILD_WEBASSEMBLY_STATIC_LIB':'OFF',
'onnxruntime_ENABLE_WEBASSEMBLY_EXCEPTION_CATCHING':'ON',
'onnxruntime_ENABLE_WEBASSEMBLY_API_EXCEPTION_CATCHING':'OFF',
'onnxruntime_ENABLE_WEBASSEMBLY_EXCEPTION_THROWING':'ON',
'onnxruntime_ENABLE_WEBASSEMBLY_THREADS':'OFF',
'onnxruntime_ENABLE_WEBASSEMBLY_DEBUG_INFO':'OFF',
'onnxruntime_ENABLE_WEBASSEMBLY_PROFILING':'OFF',
'onnxruntime_ENABLE_LAZY_TENSOR':'OFF',
'onnxruntime_ENABLE_EXTERNAL_CUSTOM_OP_SCHEMAS':'OFF', 'onnxruntime_ENABLE_CUDA_PROFILING':'OFF',
'onnxruntime_ENABLE_ROCM_PROFILING':'OFF', 'onnxruntime_USE_XNNPACK':'OFF',
'onnxruntime_USE_CANN':'OFF', 'CMAKE_TLS_VERIFY':'ON', 'FETCHCONTENT_QUIET':'OFF',
'onnxruntime_PYBIND_EXPORT_OPSCHEMA':'OFF', 'onnxruntime_ENABLE_MEMLEAK_CHECKER':'OFF',
'CMAKE_BUILD_TYPE':'Release',
'onnxruntime_USE_EXTENSIONS': 'ON',
'OCOS_ENABLE_BLINGFIRE': 'OFF',
'OCOS_ENABLE_VISION': 'ON',
'OCOS_ENABLE_AUDIO': 'OFF',
'OCOS_ENABLE_DLIB': 'ON',
'OCOS_ENABLE_MATH': 'OFF',
'OCOS_ENABLE_CV2': 'OFF',
'OCOS_ENABLE_OPENCV_CODECS': 'OFF',
# Force onnxruntime to use its own bundled deps (protobuf v21.12, abseil)
# instead of finding the project's protobuf v28.3 via find_package().
'FETCHCONTENT_TRY_FIND_PACKAGE_MODE': 'NEVER',
}


__ONNXRUNTIME_WITH_CUDA = {'onnxruntime_RUN_ONNX_TESTS':'OFF',
'onnxruntime_BUILD_UNIT_TESTS':'OFF',
'onnxruntime_GENERATE_TEST_REPORTS':'ON',
'onnxruntime_USE_MIMALLOC':'OFF',
'onnxruntime_ENABLE_PYTHON':'OFF',
'onnxruntime_BUILD_CSHARP':'OFF',
'onnxruntime_BUILD_JAVA':'OFF',
'onnxruntime_BUILD_NODEJS':'OFF',
'onnxruntime_BUILD_OBJC':'OFF',
'onnxruntime_BUILD_SHARED_LIB':'ON',
'onnxruntime_BUILD_APPLE_FRAMEWORK':'OFF',
'onnxruntime_USE_DNNL':'OFF',
'onnxruntime_USE_NNAPI_BUILTIN':'OFF',
'onnxruntime_USE_RKNPU':'OFF',
'onnxruntime_USE_LLVM':'OFF',
'onnxruntime_ENABLE_MICROSOFT_INTERNAL':'OFF',
'onnxruntime_USE_VITISAI':'OFF',
'onnxruntime_USE_TENSORRT':'OFF',
'onnxruntime_SKIP_AND_PERFORM_FILTERED_TENSORRT_TESTS':'ON',
'onnxruntime_USE_TENSORRT_BUILTIN_PARSER':'OFF',
'onnxruntime_TENSORRT_PLACEHOLDER_BUILDER':'OFF', 'onnxruntime_USE_MIGRAPHX':'OFF', 'onnxruntime_CROSS_COMPILING':'OFF',
'onnxruntime_DISABLE_CONTRIB_OPS':'OFF', 'onnxruntime_DISABLE_ML_OPS':'OFF',
'onnxruntime_DISABLE_RTTI':'OFF', 'onnxruntime_DISABLE_EXCEPTIONS':'OFF',
'onnxruntime_MINIMAL_BUILD':'OFF', 'onnxruntime_EXTENDED_MINIMAL_BUILD':'OFF',
'onnxruntime_MINIMAL_BUILD_CUSTOM_OPS':'OFF', 'onnxruntime_REDUCED_OPS_BUILD':'OFF',
'onnxruntime_ENABLE_LANGUAGE_INTEROP_OPS':'OFF', 'onnxruntime_USE_DML':'OFF',
'onnxruntime_USE_WINML':'OFF', 'onnxruntime_BUILD_MS_EXPERIMENTAL_OPS':'OFF',
'onnxruntime_USE_TELEMETRY':'OFF', 'onnxruntime_ENABLE_LTO':'OFF',
'onnxruntime_USE_ACL':'OFF', 'onnxruntime_USE_ACL_1902':'OFF',
'onnxruntime_USE_ACL_1905':'OFF', 'onnxruntime_USE_ACL_1908':'OFF',
'onnxruntime_USE_ACL_2002':'OFF', 'onnxruntime_USE_ARMNN':'OFF',
'onnxruntime_ARMNN_RELU_USE_CPU':'ON', 'onnxruntime_ARMNN_BN_USE_CPU':'ON',
'onnxruntime_ENABLE_NVTX_PROFILE':'OFF', 'onnxruntime_ENABLE_TRAINING':'OFF',
'onnxruntime_ENABLE_TRAINING_OPS':'OFF', 'onnxruntime_ENABLE_TRAINING_APIS':'OFF',
'onnxruntime_ENABLE_CPU_FP16_OPS':'OFF', 'onnxruntime_USE_NCCL':'OFF',
'onnxruntime_BUILD_BENCHMARKS':'OFF', 'onnxruntime_USE_ROCM':'OFF',
'Onnxruntime_GCOV_COVERAGE':'OFF', 'onnxruntime_ENABLE_MEMORY_PROFILE':'OFF',
'onnxruntime_ENABLE_CUDA_LINE_NUMBER_INFO':'OFF',
'onnxruntime_BUILD_WEBASSEMBLY':'OFF', 'onnxruntime_BUILD_WEBASSEMBLY_STATIC_LIB':'OFF',
'onnxruntime_ENABLE_WEBASSEMBLY_EXCEPTION_CATCHING':'ON',
'onnxruntime_ENABLE_WEBASSEMBLY_API_EXCEPTION_CATCHING':'OFF',
'onnxruntime_ENABLE_WEBASSEMBLY_EXCEPTION_THROWING':'ON',
'onnxruntime_ENABLE_WEBASSEMBLY_THREADS':'OFF',
'onnxruntime_ENABLE_WEBASSEMBLY_DEBUG_INFO':'OFF',
'onnxruntime_ENABLE_WEBASSEMBLY_PROFILING':'OFF',
'onnxruntime_ENABLE_LAZY_TENSOR':'OFF',
'onnxruntime_ENABLE_EXTERNAL_CUSTOM_OP_SCHEMAS':'OFF', 'onnxruntime_ENABLE_CUDA_PROFILING':'OFF',
'onnxruntime_ENABLE_ROCM_PROFILING':'OFF', 'onnxruntime_USE_XNNPACK':'OFF',
'onnxruntime_USE_CANN':'OFF', 'CMAKE_TLS_VERIFY':'ON', 'FETCHCONTENT_QUIET':'OFF',
'onnxruntime_PYBIND_EXPORT_OPSCHEMA':'OFF', 'onnxruntime_ENABLE_MEMLEAK_CHECKER':'OFF',
'CMAKE_BUILD_TYPE':'Release',
'onnxruntime_USE_CUDA':'ON', 'onnxruntime_USE_CUDNN':'ON',
'onnxruntime_USE_EXTENSIONS': 'ON',
'onnxruntime_CUDA_HOME': CUDA_HOME,
'onnxruntime_CUDNN_HOME': CUDNN_HOME,
'CMAKE_CUDA_COMPILER': CUDA_HOME + "/bin/nvcc",
'OCOS_ENABLE_BLINGFIRE': 'OFF',
'OCOS_ENABLE_VISION': 'ON',
'OCOS_ENABLE_AUDIO': 'OFF',
'OCOS_ENABLE_DLIB': 'ON',
'OCOS_ENABLE_MATH': 'OFF',
'OCOS_ENABLE_CV2': 'OFF',
'OCOS_ENABLE_OPENCV_CODECS': 'OFF',
'FETCHCONTENT_TRY_FIND_PACKAGE_MODE': 'NEVER',
}

config_setting(
    name = "with_cuda",
    define_values = { "use_cuda": "on" }
)



cmake(
    name = "onnxruntime",
    lib_source = ":all_srcs",
    cache_entries = select({
        ":with_cuda":   __ONNXRUNTIME_WITH_CUDA,
        "//conditions:default": __ONNXRUNTIME_WITHOUT_CUDA
    }),
    working_directory="cmake",
    generate_args = ["--compile-no-warning-as-error"],
    build_args= [
        "--config Release",
        "-j3"
    ],
    tags=["requires-network","no-sandbox"],
    features=["-default_compile_flags","-fno-canonical-system-headers", "-Wno-builtin-macro-redefined"],
    out_shared_libs = [
        "libonnxruntime.so",
        "libonnxruntime.so.1",
    ],
    postfix_script= select({
        ":with_cuda": __POSTFIX_WITH_CUDA,
        "//conditions:default": __POSTFIX
    }),
)

cc_library(
    name = "onnxruntime_lib",
    deps = ["//:onnxruntime", "//:hdrs", "@onnx_runtime_extensions//:operators"],
    visibility = ["//visibility:public"]
)
