#!/usr/bin/env bash

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${TYPESENSE_BAZEL_IMAGE:-typesense/ci-bazel-cuda:local}"
BAZEL_DOCKERFILE="${TYPESENSE_BAZEL_DOCKERFILE:-docker/ci-bazel-cuda.Dockerfile}"
VERSION_LABEL=""
TARGET_ARCH=""
BUILD_BEFORE_PACKAGING=0
PRINT_KEY_ONLY=0

usage() {
	cat <<'EOF'
Usage:
  scripts/release_ort_bundle.sh [options]

Options:
  --build                    Build @onnx_runtime//:onnxruntime_static_one_protobuf first
  --print-key                Print the computed bundle key and exit
  --version-label <label>    Version label used in artifact names (default: computed key)
  --target-arch <arch>       Target arch label (amd64 or arm64; default: host arch)
  --help                     Show this help

Examples:
  scripts/release_ort_bundle.sh --print-key --target-arch amd64
  scripts/release_ort_bundle.sh --build --target-arch amd64
  scripts/release_ort_bundle.sh --build --target-arch arm64 --version-label ort-demo

Notes:
  - This packages the repo's CUDA-enabled one-Protobuf ONNX Runtime build into a reusable bundle.
  - The bundle contains the install tree under @onnx_runtime//:onnxruntime_static_one_protobuf.
  - Cross-arch local replay requires Docker arm64 emulation when host and target differ.
EOF
}

detect_target_arch() {
	case "$(uname -m)" in
		x86_64|amd64)
			echo "amd64"
			;;
		aarch64|arm64)
			echo "arm64"
			;;
		*)
			echo "Unsupported host architecture: $(uname -m)" >&2
			exit 1
			;;
	esac
}

sha256_file() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$@"
		return
	fi
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$@"
		return
	fi
	echo "Need sha256sum or shasum to compute ORT bundle hashes." >&2
	exit 1
}

sha256_stream() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum
		return
	fi
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256
		return
	fi
	echo "Need sha256sum or shasum to compute ORT bundle hashes." >&2
	exit 1
}

append_optional_repo_env() {
	local -n out_ref=$1
	local env_name="$2"
	local env_value="${!env_name:-}"
	if [[ -n "${env_value}" ]]; then
		out_ref+=("--repo_env=${env_name}=${env_value}")
	fi
}

resolve_dockerfile_path() {
	if [[ "${BAZEL_DOCKERFILE}" = /* ]]; then
		printf '%s\n' "${BAZEL_DOCKERFILE}"
	else
		printf '%s\n' "${PROJECT_DIR}/${BAZEL_DOCKERFILE}"
	fi
}

compute_bundle_key() {
	local dockerfile_path
	dockerfile_path="$(resolve_dockerfile_path)"

	{
		printf 'target_arch=%s\n' "${TARGET_ARCH}"
		printf 'cuda_architectures=%s\n' "${TYPESENSE_ORT_CUDA_ARCHITECTURES:-default}"
		printf 'dockerfile=%s\n' "${dockerfile_path}"
		sha256_file \
			"${PROJECT_DIR}/MODULE.bazel" \
			"${PROJECT_DIR}/bazel/onnxruntime.BUILD" \
			"${PROJECT_DIR}/bazel/onnxruntime.patch" \
			"${PROJECT_DIR}/bazel/onnx_ext.patch" \
			"${PROJECT_DIR}/bazel/onnxruntime_cuda_defs.bzl" \
			"${dockerfile_path}"
	} | sha256_stream | awk '{print $1}' | cut -c1-16
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--build)
			BUILD_BEFORE_PACKAGING=1
			shift
			;;
		--print-key)
			PRINT_KEY_ONLY=1
			shift
			;;
		--version-label)
			VERSION_LABEL="${2:?missing value for --version-label}"
			shift 2
			;;
		--target-arch)
			TARGET_ARCH="${2:?missing value for --target-arch}"
			shift 2
			;;
		--help|-h)
			usage
			exit 0
			;;
		*)
			echo "Unknown option: $1" >&2
			usage >&2
			exit 1
			;;
	esac
done

if [[ -z "${TARGET_ARCH}" ]]; then
	TARGET_ARCH="$(detect_target_arch)"
fi

case "${TARGET_ARCH}" in
	amd64|arm64)
		;;
	*)
		echo "Unsupported target architecture: ${TARGET_ARCH}" >&2
		exit 1
		;;
esac

BUNDLE_KEY="$(compute_bundle_key)"
if ((PRINT_KEY_ONLY)); then
	printf '%s\n' "${BUNDLE_KEY}"
	exit 0
fi

if [[ -z "${VERSION_LABEL}" ]]; then
	VERSION_LABEL="${BUNDLE_KEY}"
fi

DOCKER_PLATFORM="linux/${TARGET_ARCH}"
ORT_OUTPUT_DIR="${PROJECT_DIR}/bazel-bin/external/+new_git_repository+onnx_runtime/onnxruntime_static_one_protobuf"
ARTIFACT_DIR="${PROJECT_DIR}/artifacts"
ARTIFACT_BASENAME="typesense-ort-bundle-${VERSION_LABEL}-linux-${TARGET_ARCH}"
TARBALL="${ARTIFACT_DIR}/${ARTIFACT_BASENAME}.tar.gz"
MANIFEST="${ARTIFACT_DIR}/${ARTIFACT_BASENAME}.json"
SHA256="${TARBALL}.sha256.txt"

if ((BUILD_BEFORE_PACKAGING)); then
	build_args=(build @onnx_runtime//:onnxruntime_static_one_protobuf --define=use_cuda=on)
	append_optional_repo_env build_args "TYPESENSE_ORT_CUDA_ARCHITECTURES"
	append_optional_repo_env build_args "TYPESENSE_ORT_BUILD_JOBS"
	env \
		-u TYPESENSE_ORT_PREBUILT_BUNDLE_DIR \
		TYPESENSE_BAZEL_IMAGE="${IMAGE}" \
		TYPESENSE_BAZEL_DOCKERFILE="${BAZEL_DOCKERFILE}" \
		TYPESENSE_DOCKER_PLATFORM="${DOCKER_PLATFORM}" \
		"${PROJECT_DIR}/scripts/bazel_in_docker.sh" "${build_args[@]}"
fi

if [[ ! -d "${ORT_OUTPUT_DIR}/lib" ]]; then
	echo "Expected ORT bundle directory was not found at ${ORT_OUTPUT_DIR}." >&2
	echo "Build @onnx_runtime//:onnxruntime_static_one_protobuf first or pass --build." >&2
	exit 1
fi

mkdir -p "${ARTIFACT_DIR}"
rm -f "${TARBALL}" "${SHA256}" "${MANIFEST}"

tar -czf "${TARBALL}" -C "${ORT_OUTPUT_DIR}" .
tar_hash="$(sha256_file "${TARBALL}" | awk '{print $1}')"
printf '%s  %s\n' "${tar_hash}" "$(basename "${TARBALL}")" >"${SHA256}"

cat >"${MANIFEST}" <<EOF
{
  "bundle_key": "${BUNDLE_KEY}",
  "version_label": "${VERSION_LABEL}",
  "target_arch": "${TARGET_ARCH}",
  "docker_platform": "${DOCKER_PLATFORM}",
  "ort_output_dir": "onnxruntime_static_one_protobuf",
  "cuda_architectures": "${TYPESENSE_ORT_CUDA_ARCHITECTURES:-default}"
}
EOF

echo "Created ${TARBALL}"
echo "Created ${SHA256}"
echo "Created ${MANIFEST}"
