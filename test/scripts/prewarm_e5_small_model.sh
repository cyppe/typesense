#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUTPUT_DIR="${1:-${REPO_ROOT}/tmp/ci-models}"
MODEL_DIR="${OUTPUT_DIR}/ts_e5-small"
MODEL_BASE_URL="https://models.typesense.org/public/e5-small"

mkdir -p "${MODEL_DIR}"

download_file() {
	local url="$1"
	local dest="$2"
	local tmp="${dest}.tmp"

	curl \
		--fail \
		--location \
		--retry 8 \
		--retry-all-errors \
		--retry-delay 2 \
		--connect-timeout 10 \
		--max-time 900 \
		"${url}" \
		--output "${tmp}"

	mv "${tmp}" "${dest}"
}

download_file "${MODEL_BASE_URL}/config.json" "${MODEL_DIR}/config.json"
download_file "${MODEL_BASE_URL}/model.onnx" "${MODEL_DIR}/model.onnx"
download_file "${MODEL_BASE_URL}/vocab.txt" "${MODEL_DIR}/vocab.txt"

echo "Prepared model cache at ${OUTPUT_DIR}"
