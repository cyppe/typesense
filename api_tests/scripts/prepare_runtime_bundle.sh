#!/usr/bin/env bash

set -euo pipefail

# Internal helper for scripts/run_api_tests.sh and release staging.
# Use scripts/run_api_tests.sh for normal API replay instead of calling this directly.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUTPUT_DIR="${1:-${REPO_ROOT}/typesense-runtime-bundle}"
LIB_DIR="${OUTPUT_DIR}/lib"

BAZEL_BIN_PATH="$(readlink -f "${REPO_ROOT}/bazel-bin")"
DEFAULT_SERVER_BINARY="${BAZEL_BIN_PATH}/typesense-server"
SERVER_BINARY_INPUT="${2:-${TYPESENSE_SERVER_BINARY_PATH:-${DEFAULT_SERVER_BINARY}}}"
SERVER_BINARY="$(readlink -f "${SERVER_BINARY_INPUT}")"
SERVER_BINARY_BASENAME="$(basename "${SERVER_BINARY_INPUT}")"

if [[ ! -f "${SERVER_BINARY}" ]]; then
	RUNFILES_BINARY="${SERVER_BINARY}.runfiles/_main/${SERVER_BINARY_BASENAME}"
	if [[ -f "${RUNFILES_BINARY}" ]]; then
		SERVER_BINARY="${RUNFILES_BINARY}"
	fi
fi

if [[ ! -f "${SERVER_BINARY}" ]]; then
	echo "typesense-server binary not found at ${SERVER_BINARY}" >&2
	echo "Build it first with: scripts/bazel_in_docker.sh build //:typesense-server" >&2
	echo "Or pass a custom binary path as arg 2 / TYPESENSE_SERVER_BINARY_PATH." >&2
	exit 1
fi

if [[ -n "${TYPESENSE_BAZEL_CACHE_DIR:-}" ]]; then
	BAZEL_CACHE_ROOT="${TYPESENSE_BAZEL_CACHE_DIR}"
else
	BAZEL_CACHE_ROOT="${HOME}/.cache/typesense/bazel-docker"
fi

mkdir -p "${LIB_DIR}"
chmod u+w "${OUTPUT_DIR}/typesense-server" 2>/dev/null || true
cp "${SERVER_BINARY}" "${OUTPUT_DIR}/typesense-server"

if ldd "${SERVER_BINARY}" 2>/dev/null | grep -q 'libonnxruntime\.so\.1'; then
	ONNX_RUNTIME_LIB="$(find "${BAZEL_CACHE_ROOT}" -type f -path "*/onnxruntime/lib/libonnxruntime.so.1" | sort | tail -n 1)"
	if [[ -z "${ONNX_RUNTIME_LIB}" ]]; then
		echo "Unable to locate libonnxruntime.so.1 under ${BAZEL_CACHE_ROOT}" >&2
		echo "Set TYPESENSE_BAZEL_CACHE_DIR if your Bazel cache lives elsewhere." >&2
		exit 1
	fi

	chmod u+w "${LIB_DIR}/libonnxruntime.so.1" 2>/dev/null || true
	chmod u+w "${LIB_DIR}/libonnxruntime.so" 2>/dev/null || true
	cp "${ONNX_RUNTIME_LIB}" "${LIB_DIR}/libonnxruntime.so.1"
	cp "$(dirname "${ONNX_RUNTIME_LIB}")/libonnxruntime.so" "${LIB_DIR}/libonnxruntime.so"
	RUNTIME_LIB_MSG="Library dir: ${LIB_DIR} (includes libonnxruntime.so.1; optional GPU provider sidecars are not managed by this helper)"
else
	rm -f "${LIB_DIR}/libonnxruntime.so.1" "${LIB_DIR}/libonnxruntime.so"
	RUNTIME_LIB_MSG="Library dir: ${LIB_DIR} (no external libonnxruntime.so.1 needed; optional GPU provider sidecars are packaged separately via typesense-gpu-deps)"
fi

echo "Prepared runtime bundle in ${OUTPUT_DIR}"
echo "Binary: ${OUTPUT_DIR}/typesense-server"
echo "${RUNTIME_LIB_MSG}"
