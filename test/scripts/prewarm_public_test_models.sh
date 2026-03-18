#!/usr/bin/env bash

set -euo pipefail

# Internal helper for CI and test/scripts/replay_typesense_test.sh.
# The on-disk cache paths mirror EmbedderManager's public-model layout (`ts_<model-name>`).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUTPUT_DIR="${1:-${REPO_ROOT}/tmp/ci-models}"
MODELS_REPO_URL="https://models.typesense.org/public"

MODEL_NAMES=(
	"e5-small"
	"multilingual-e5-small"
	"e5-small-v2"
	"clip-vit-b-p32"
	"all-MiniLM-L12-v2"
)

mkdir -p "${OUTPUT_DIR}"

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

download_if_missing() {
	local url="$1"
	local dest="$2"

	if [[ -s "${dest}" ]]; then
		return
	fi

	download_file "${url}" "${dest}"
}

json_field() {
	local file="$1"
	local key="$2"

	if command -v jq >/dev/null 2>&1; then
		jq -r --arg key "${key}" '.[$key] // empty' "${file}"
		return
	fi

	if command -v python3 >/dev/null 2>&1; then
		python3 - "${file}" "${key}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)

value = data.get(sys.argv[2], "")
if value is None:
    value = ""

print(value)
PY
		return
	fi

	echo "prewarm_public_test_models.sh requires jq or python3 to parse config.json" >&2
	exit 1
}

prepare_model() {
	local model_name="$1"
	local model_dir="${OUTPUT_DIR}/ts_${model_name}"
	local base_url="${MODELS_REPO_URL}/${model_name}"
	local config_path="${model_dir}/config.json"

	mkdir -p "${model_dir}"
	download_if_missing "${base_url}/config.json" "${config_path}"
	download_if_missing "${base_url}/model.onnx" "${model_dir}/model.onnx"

	local vocab_file_name
	vocab_file_name="$(json_field "${config_path}" "vocab_file_name")"
	if [[ -n "${vocab_file_name}" ]]; then
		download_if_missing "${base_url}/${vocab_file_name}" "${model_dir}/${vocab_file_name}"
	fi

	local tokenizer_file_name
	tokenizer_file_name="$(json_field "${config_path}" "tokenizer_file_name")"
	if [[ -n "${tokenizer_file_name}" ]]; then
		download_if_missing "${base_url}/${tokenizer_file_name}" "${model_dir}/${tokenizer_file_name}"
	fi

	local image_processor_file_name
	image_processor_file_name="$(json_field "${config_path}" "image_processor_file_name")"
	if [[ -n "${image_processor_file_name}" ]]; then
		download_if_missing "${base_url}/${image_processor_file_name}" "${model_dir}/${image_processor_file_name}"
	fi

	local data_md5
	data_md5="$(json_field "${config_path}" "data_md5")"
	if [[ -n "${data_md5}" ]]; then
		download_if_missing "${base_url}/model.onnx_data" "${model_dir}/model.onnx_data"
	fi
}

for model_name in "${MODEL_NAMES[@]}"; do
	prepare_model "${model_name}"
done

echo "Prepared public embedding test model cache at ${OUTPUT_DIR}"
