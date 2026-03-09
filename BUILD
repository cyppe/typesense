load("@com_grail_bazel_compdb//:defs.bzl", "compilation_database")
load("@com_grail_bazel_output_base_util//:defs.bzl", "OUTPUT_BASE")
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

# Target to generate a compile_commands.json compilation database file
compilation_database(
    name = "compdb",
    output_base = OUTPUT_BASE,
    targets = [
        "//:typesense-server",
        "//:search",
        "//:benchmark",
    ],
)

filegroup(
    name = "src_files",
    srcs = glob(["src/*.cpp"]),
)

cc_library(
    name = "headers",
    hdrs = glob([
        "include/**/*.h",
        "include/**/*.hpp",
    ]),
    includes = ["include"],
)

config_setting(
    name = "with_cuda",
    define_values = { "use_cuda": "on" }
)

cc_library(
    name = "common_deps",
    defines = [
        "NDEBUG",
    ],
    linkopts = select({
        "@platforms//os:macos": ["-framework Foundation -framework SystemConfiguration"],
        "//conditions:default": [],
    }),
    deps = [
        ":headers",
        "@onnx_runtime//:onnxruntime_static_one_protobuf_lib",
        "@sentencepiece",
        "@sentencepiece//:sentencepiece_headers",
        "@com_github_brpc_braft//:braft",
        "@com_github_brpc_brpc//:brpc",
        "@com_github_google_glog//:glog",  # retained for brpc/braft
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/log:absl_check",
        "@com_google_absl//absl/log:initialize",
        "@com_google_absl//absl/log:globals",
        "@com_google_absl//absl/log:log_sink",
        "@com_google_absl//absl/log:log_sink_registry",
        "@com_google_absl//absl/log:log_entry",
        "@curl",
        "@for",
        "@h2o",
        "@iconv",
        "@icu",
        "@kakasi",
        "@lrucache",
        "@rocksdb",
        "@s2geometry",
        "@hnsw",
        "@clip_tokenizer//:clip",
        "@whisper.cpp//:whisper",
        "@whisper.cpp//:whisper_headers",
        "@snowball",
        "@snowball//:snowball_headers",
        "@archive",
        # "@zip",
    ] + select({
        ":asan_mode": [],
        ":tsan_mode": [],
        "//conditions:default": ["@jemalloc"],
    }))

cc_library(
    name = "linux_deps",
    defines = [
        "NDEBUG",
    ],
    deps = [
        "@elfutils//:libdw",
    ],
)

cc_library(
    name = "common_deps_static_probe",
    defines = [
        "NDEBUG",
    ],
    linkopts = select({
        "@platforms//os:macos": ["-framework Foundation -framework SystemConfiguration"],
        "//conditions:default": [],
    }),
    deps = [
        ":headers",
        "@onnx_runtime//:onnxruntime_static_lib",
        "@sentencepiece",
        "@sentencepiece//:sentencepiece_headers",
        "@com_github_brpc_braft//:braft",
        "@com_github_brpc_brpc//:brpc",
        "@com_github_google_glog//:glog",  # retained for brpc/braft
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/log:absl_check",
        "@com_google_absl//absl/log:initialize",
        "@com_google_absl//absl/log:globals",
        "@com_google_absl//absl/log:log_sink",
        "@com_google_absl//absl/log:log_sink_registry",
        "@com_google_absl//absl/log:log_entry",
        "@curl",
        "@for",
        "@h2o",
        "@iconv",
        "@icu",
        "@kakasi",
        "@lrucache",
        "@rocksdb",
        "@s2geometry",
        "@hnsw",
        "@clip_tokenizer//:clip",
        "@whisper.cpp//:whisper",
        "@whisper.cpp//:whisper_headers",
        "@snowball",
        "@snowball//:snowball_headers",
        "@archive",
    ] + select({
        ":asan_mode": [],
        ":tsan_mode": [],
        "//conditions:default": ["@jemalloc"],
    }),
)

cc_library(
    name = "common_deps_static_one_protobuf_probe",
    defines = [
        "NDEBUG",
    ],
    linkopts = select({
        "@platforms//os:macos": ["-framework Foundation -framework SystemConfiguration"],
        "//conditions:default": [],
    }),
    deps = [
        ":headers",
        "@onnx_runtime//:onnxruntime_static_one_protobuf_lib",
        "@sentencepiece",
        "@sentencepiece//:sentencepiece_headers",
        "@com_github_brpc_braft//:braft",
        "@com_github_brpc_brpc//:brpc",
        "@com_github_google_glog//:glog",  # retained for brpc/braft
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/log:absl_check",
        "@com_google_absl//absl/log:initialize",
        "@com_google_absl//absl/log:globals",
        "@com_google_absl//absl/log:log_sink",
        "@com_google_absl//absl/log:log_sink_registry",
        "@com_google_absl//absl/log:log_entry",
        "@curl",
        "@for",
        "@h2o",
        "@iconv",
        "@icu",
        "@kakasi",
        "@lrucache",
        "@rocksdb",
        "@s2geometry",
        "@hnsw",
        "@clip_tokenizer//:clip",
        "@whisper.cpp//:whisper",
        "@whisper.cpp//:whisper_headers",
        "@snowball",
        "@snowball//:snowball_headers",
        "@archive",
    ] + select({
        ":asan_mode": [],
        ":tsan_mode": [],
        "//conditions:default": ["@jemalloc"],
    }),
)

COPTS = [
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-Werror=return-type",
    "-fsized-deallocation",
    "-O2",
    "-g",
]

cc_binary(
    name = "typesense-server",
    srcs = [
        "src/main/typesense_server.cpp",
        ":src_files",
    ],
    local_defines = [
        "TYPESENSE_VERSION=\\\"$(TYPESENSE_VERSION)\\\""
    ],
    linkopts = select({
        "@platforms//os:linux": ["-static-libstdc++", "-static-libgcc", "-fuse-ld=lld"],
        "@platforms//os:macos": ["-framework Foundation", "-framework Accelerate", "-framework Metal", "-framework MetalKit"],
        "//conditions:default": [],
    }),
    copts = COPTS + select({
        "@platforms//os:linux": ["-DBACKWARD_HAS_DW=1", "-DBACKWARD_HAS_UNWIND=1"],
        "//conditions:default": [],
    }),
    deps = [":common_deps"] +  select({
        "@platforms//os:linux": [":linux_deps"],
        "//conditions:default": [],
    }),
)

cc_binary(
    name = "search",
    srcs = [
        "src/main/main.cpp",
        ":src_files",
    ],
    copts = COPTS,
    deps = [":common_deps"],
)

cc_binary(
    name = "typesense-server-static-probe",
    srcs = [
        "src/main/typesense_server.cpp",
        ":src_files",
    ],
    local_defines = [
        "TYPESENSE_VERSION=\\\"$(TYPESENSE_VERSION)\\\"",
    ],
    linkopts = select({
        "@platforms//os:linux": ["-static-libstdc++", "-static-libgcc", "-fuse-ld=lld"],
        "@platforms//os:macos": ["-framework Foundation", "-framework Accelerate", "-framework Metal", "-framework MetalKit"],
        "//conditions:default": [],
    }),
    copts = COPTS + select({
        "@platforms//os:linux": ["-DBACKWARD_HAS_DW=1", "-DBACKWARD_HAS_UNWIND=1"],
        "//conditions:default": [],
    }),
    deps = [":common_deps_static_probe"] + select({
        "@platforms//os:linux": [":linux_deps"],
        "//conditions:default": [],
    }),
)

cc_binary(
    name = "typesense-server-static-one-protobuf-probe",
    srcs = [
        "src/main/typesense_server.cpp",
        ":src_files",
    ],
    local_defines = [
        "TYPESENSE_VERSION=\\\"$(TYPESENSE_VERSION)\\\"",
    ],
    linkopts = select({
        "@platforms//os:linux": ["-static-libstdc++", "-static-libgcc", "-fuse-ld=lld"],
        "@platforms//os:macos": ["-framework Foundation", "-framework Accelerate", "-framework Metal", "-framework MetalKit"],
        "//conditions:default": [],
    }),
    copts = COPTS + select({
        "@platforms//os:linux": ["-DBACKWARD_HAS_DW=1", "-DBACKWARD_HAS_UNWIND=1"],
        "//conditions:default": [],
    }),
    deps = [":common_deps_static_one_protobuf_probe"] + select({
        "@platforms//os:linux": [":linux_deps"],
        "//conditions:default": [],
    }),
)

cc_binary(
    name = "benchmark",
    srcs = [
        "src/main/benchmark.cpp",
        ":src_files",
    ],
    copts = COPTS,
    deps = [":common_deps"],
)

cc_library(
    name = "nuraft_prototype_lib",
    srcs = [
        "src/nuraft/nuraft_bootstrap_builder.cpp",
        "src/nuraft/nuraft_replication_controller.cpp",
        "src/nuraft/nuraft_file_store.cpp",
        "src/nuraft/nuraft_applied_request_store.cpp",
        "src/nuraft/nuraft_metadata_store.cpp",
        "src/nuraft/nuraft_peer_resolver.cpp",
        "src/nuraft/nuraft_prototype_state_machine.cpp",
        "src/nuraft/nuraft_request_journal.cpp",
        "src/nuraft/nuraft_request_envelope.cpp",
        "src/nuraft/nuraft_replay_coordinator.cpp",
        "src/nuraft/nuraft_route_classifier.cpp",
        "src/nuraft/nuraft_segment_log_store.cpp",
        "src/nuraft/nuraft_state_initializer.cpp",
        "src/nuraft/nuraft_state_layout.cpp",
    ],
    copts = COPTS,
    deps = [":headers"],
)

cc_binary(
    name = "typesense-server-nuraft-prototype",
    srcs = [
        "src/main/typesense_nuraft_prototype.cpp",
    ],
    local_defines = [
        "TYPESENSE_VERSION=\"$(TYPESENSE_VERSION)\"",
    ],
    copts = COPTS,
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
    ],
)

cc_test(
    name = "nuraft-bootstrap-builder-test",
    srcs = [
        "test/nuraft_bootstrap_builder_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nuraft-state-initializer-test",
    srcs = [
        "test/nuraft_state_initializer_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nuraft-replication-controller-test",
    srcs = [
        "test/nuraft_replication_controller_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nuraft-segment-log-store-test",
    srcs = [
        "test/nuraft_segment_log_store_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nuraft-request-journal-test",
    srcs = [
        "test/nuraft_request_journal_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nuraft-replay-coordinator-test",
    srcs = [
        "test/nuraft_replay_coordinator_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nuraft-prototype-state-machine-test",
    srcs = [
        "test/nuraft_prototype_state_machine_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nuraft-applied-request-store-test",
    srcs = [
        "test/nuraft_applied_request_store_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nuraft-route-classifier-test",
    srcs = [
        "test/nuraft_route_classifier_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nuraft-request-envelope-test",
    srcs = [
        "test/nuraft_request_envelope_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nuraft-file-store-test",
    srcs = [
        "test/nuraft_file_store_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    includes = ["test"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nuraft-metadata-store-test",
    srcs = [
        "test/nuraft_metadata_store_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nuraft-peer-resolver-test",
    srcs = [
        "test/nuraft_peer_resolver_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nuraft-bootstrap-config-test",
    srcs = [
        "test/nuraft_bootstrap_config_test.cpp",
    ],
    copts = COPTS + ["-O0", "-DTEST_BUILD"],
    deps = [
        ":headers",
        ":nuraft_prototype_lib",
        "@com_google_googletest//:gtest_main",
    ],
)

filegroup(
    name = "test_src_files",
    srcs = glob(["test/*.cpp"]) + [
        "test/runfiles_utils.h",
        "test/temp_dir_utils.h",
    ],
)

filegroup(
    name = "test_data_files",
    srcs = glob([
        "test/**/*.txt",
        "test/**/*.ini",
        "test/**/*.jsonl",
        "test/**/*.gz",
    ]),
)

TEST_COPTS = [
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-Werror=return-type",
    "-fsized-deallocation",
    "-g",
    "-DTEST_BUILD"
]

ASAN_COPTS = [
    "-fsanitize=address",
    "-fno-omit-frame-pointer",
    "-DASAN_BUILD"
]

config_setting(
    name = "release_mode",
    define_values = { "mode": "release" }
)

config_setting(
    name = "asan_mode",
    define_values = { "mode": "asan" },
)

config_setting(
    name = "tsan_mode",
    define_values = { "mode": "tsan" },
)

cc_test(
    name = "typesense-test",
    size = "large",
    srcs = [
        ":src_files",
        ":test_src_files",
    ],
    copts = TEST_COPTS + select({
        ":release_mode": ["-O2"],
        ":asan_mode": ["-O0"] + ASAN_COPTS,
        "//conditions:default": ["-O0"]
    }),
    data = [
        ":test_data_files",
        "@libart//:data",
        "@token_offsets//file",
    ],
    deps = [
        ":common_deps",
        "@com_google_googletest//:gtest",
    ],
    defines = [
        "ROOT_DIR="
    ],
    linkopts = select({
       ":asan_mode": ["-fsanitize=address", "-fuse-ld=lld"],
       "//conditions:default": []
    }) +  select({
       "@platforms//os:linux": ["-fuse-ld=lld"],
       "//conditions:default": [],
   })
)
