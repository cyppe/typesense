#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODELS_DIR_HOST="${TYPESENSE_TEST_MODELS_DIR_HOST:-${ROOT_DIR}/tmp/replay-models}"
MODELS_DIR_CONTAINER="${TYPESENSE_TEST_MODELS_DIR_CONTAINER:-/work/tmp/replay-models}"
MODEL_CACHE_MARKER_RELATIVE_PATH="ts_e5-small/model.onnx"

gtest_filter=""
if [[ $# -gt 0 ]] && [[ "${1}" != --* ]]; then
	gtest_filter="${1}"
	shift
fi

mkdir -p "${MODELS_DIR_HOST}"
if [[ ! -f "${MODELS_DIR_HOST}/${MODEL_CACHE_MARKER_RELATIVE_PATH}" ]]; then
	bash "${ROOT_DIR}/test/scripts/prewarm_e5_small_model.sh" "${MODELS_DIR_HOST}"
fi

bazel_args=(
	test
	--cache_test_results=no
	--test_output=all
	//:typesense-test
	--test_timeout=1200
	--flaky_test_attempts=2
	"--test_env=TYPESENSE_TEST_MODELS_DIR=${MODELS_DIR_CONTAINER}"
)

if [[ -n "${gtest_filter}" ]]; then
	bazel_args+=("--test_filter=${gtest_filter}")
fi

if [[ $# -gt 0 ]]; then
	bazel_args+=("$@")
fi

"${ROOT_DIR}/scripts/bazel_in_docker.sh" "${bazel_args[@]}"
