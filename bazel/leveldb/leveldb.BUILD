# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to You under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License.  You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load("@rules_cc//cc:defs.bzl", "cc_library")

genrule(
    name = "port_config_h",
    srcs = ["@//bazel/leveldb:port_config.h"],
    outs = ["port/port_config.h"],
    cmd = "cp $(location @//bazel/leveldb:port_config.h) $@",
    cmd_bat = "copy /Y $(location @//bazel/leveldb:port_config.h) $@",
)

genrule(
    name = "port_h",
    srcs = ["@//bazel/leveldb:port.h"],
    outs = ["port/port.h"],
    cmd = "cp $(location @//bazel/leveldb:port.h) $@",
    cmd_bat = "copy /Y $(location @//bazel/leveldb:port.h) $@",
)

cc_library(
    name = "leveldb",
    srcs = glob(
        [
            "db/**/*.cc",
            "db/**/*.h",
            "helpers/**/*.cc",
            "helpers/**/*.h",
            "port/**/*.cc",
            "port/**/*.h",
            "table/**/*.cc",
            "table/**/*.h",
            "util/**/*.cc",
            "util/**/*.h",
        ],
        exclude = [
            "**/*_test.cc",
            "**/testutil.*",
            "**/*_bench.cc",
            "**/*_windows*",
            "db/leveldbutil.cc",
        ],
        allow_empty = True,
    ),
    hdrs = glob(
        ["include/**/*.h"],
        exclude = ["doc/**"],
    ) + [
        ":port_h",
        ":port_config_h",
    ],
    includes = [
        ".",
        "include",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@com_github_google_crc32c//:crc32c",
        "@com_github_google_snappy//:snappy",
    ],
)
