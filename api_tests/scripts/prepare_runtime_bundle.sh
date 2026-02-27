#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUTPUT_DIR="${1:-${REPO_ROOT}/typesense-runtime-bundle}"
LIB_DIR="${OUTPUT_DIR}/lib"

BAZEL_BIN_PATH="$(readlink -f "${REPO_ROOT}/bazel-bin")"
SERVER_BINARY="${BAZEL_BIN_PATH}/typesense-server"

if [[ ! -f "${SERVER_BINARY}" ]]; then
	echo "typesense-server binary not found at ${SERVER_BINARY}" >&2
	echo "Build it first with: scripts/bazel_in_docker.sh build //:typesense-server" >&2
	exit 1
fi

if [[ -n "${TYPESENSE_BAZEL_CACHE_DIR:-}" ]]; then
	BAZEL_CACHE_ROOT="${TYPESENSE_BAZEL_CACHE_DIR}"
else
	BAZEL_CACHE_ROOT="${HOME}/.cache/typesense/bazel-docker"
fi

mkdir -p "${LIB_DIR}"
cp "${SERVER_BINARY}" "${OUTPUT_DIR}/typesense-server"

ONNX_RUNTIME_LIB="$(find "${BAZEL_CACHE_ROOT}" -type f -path "*/onnxruntime/lib/libonnxruntime.so.1" | sort | tail -n 1)"
if [[ -z "${ONNX_RUNTIME_LIB}" ]]; then
	echo "Unable to locate libonnxruntime.so.1 under ${BAZEL_CACHE_ROOT}" >&2
	echo "Set TYPESENSE_BAZEL_CACHE_DIR if your Bazel cache lives elsewhere." >&2
	exit 1
fi

cp "${ONNX_RUNTIME_LIB}" "${LIB_DIR}/libonnxruntime.so.1"
cp "$(dirname "${ONNX_RUNTIME_LIB}")/libonnxruntime.so" "${LIB_DIR}/libonnxruntime.so"

echo "Prepared runtime bundle in ${OUTPUT_DIR}"
echo "Binary: ${OUTPUT_DIR}/typesense-server"
echo "Library dir: ${LIB_DIR}"
