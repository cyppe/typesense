#!/usr/bin/env bash

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${TYPESENSE_BAZEL_IMAGE:-typesense/ci-bazel-cuda:local}"
BAZEL_DOCKERFILE="${TYPESENSE_BAZEL_DOCKERFILE:-docker/ci-bazel-cuda.Dockerfile}"
CACHE_DIR="${TYPESENSE_BAZEL_CACHE_DIR:-${HOME}/.cache/typesense/bazel-docker}"
WORKDIR="/work"
VERSION_LABEL="snapshot"
TARGET_ARCH=""
BUILD_BEFORE_ASSEMBLY=0
BUILD_LINUX_PACKAGES=1
EMIT_LG_PAGE16_ALIAS=0
DOCKER_PLATFORM=""

usage() {
	cat <<'EOF'
Usage:
  scripts/release_linux_gpu_deps.sh [options]

Options:
  --build                    Build //:typesense-server with --define=use_cuda=on first
  --skip-packages            Skip DEB/RPM generation
  --emit-lg-page16-alias     Also emit arm64 lg-page16 tarball/package aliases
  --version-label <label>    Version label used in artifact names (default: snapshot)
  --target-arch <arch>       Target arch label (amd64 or arm64; default: host arch)
  --help                     Show this help

Examples:
  scripts/release_linux_gpu_deps.sh --build --version-label 0.0.0-local
  scripts/release_linux_gpu_deps.sh --build --target-arch arm64 --emit-lg-page16-alias --version-label 0.0.0-local

Notes:
  - This is the canonical local Linux replay for the optional GPU deps artifact class.
  - It packages the ONNX Runtime CUDA provider sidecars used by embeddings and personalization.
  - Whisper / voice-query remains CPU-only on this branch.
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

append_optional_repo_env() {
	local -n out_ref=$1
	local env_name="$2"
	local env_value="${!env_name:-}"
	if [[ -n "${env_value}" ]]; then
		out_ref+=("--repo_env=${env_name}=${env_value}")
	fi
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--build)
			BUILD_BEFORE_ASSEMBLY=1
			shift
			;;
		--skip-packages)
			BUILD_LINUX_PACKAGES=0
			shift
			;;
		--emit-lg-page16-alias)
			EMIT_LG_PAGE16_ALIAS=1
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
		:
		;;
	*)
		echo "Unsupported target arch: ${TARGET_ARCH}" >&2
		exit 1
		;;
esac

if ((EMIT_LG_PAGE16_ALIAS)) && [[ "${TARGET_ARCH}" != "arm64" ]]; then
	echo "--emit-lg-page16-alias is only supported with --target-arch arm64" >&2
	exit 1
fi

DOCKER_PLATFORM="linux/${TARGET_ARCH}"

if ((BUILD_BEFORE_ASSEMBLY)); then
	build_args=(build //:typesense-server --define=use_cuda=on)
	append_optional_repo_env build_args "TYPESENSE_ORT_CUDA_ARCHITECTURES"
	append_optional_repo_env build_args "TYPESENSE_ORT_BUILD_JOBS"
	TYPESENSE_BAZEL_IMAGE="${IMAGE}" \
	TYPESENSE_BAZEL_DOCKERFILE="${BAZEL_DOCKERFILE}" \
	TYPESENSE_DOCKER_PLATFORM="${DOCKER_PLATFORM}" \
		"${PROJECT_DIR}/scripts/bazel_in_docker.sh" "${build_args[@]}"
fi

image_needs_build=0
if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
	image_needs_build=1
else
	actual_arch="$(docker image inspect --format '{{.Architecture}}' "${IMAGE}")"
	case "${DOCKER_PLATFORM}" in
		linux/amd64)
			expected_arch="amd64"
			;;
		linux/arm64)
			expected_arch="arm64"
			;;
		*)
			echo "Unsupported Docker platform: ${DOCKER_PLATFORM}" >&2
			exit 1
			;;
	esac
	if [[ "${actual_arch}" != "${expected_arch}" ]]; then
		image_needs_build=1
	fi
fi

if ((image_needs_build)); then
	TYPESENSE_BAZEL_IMAGE="${IMAGE}" \
	TYPESENSE_BAZEL_DOCKERFILE="${BAZEL_DOCKERFILE}" \
	TYPESENSE_DOCKER_PLATFORM="${DOCKER_PLATFORM}" \
		"${PROJECT_DIR}/scripts/bazel_in_docker.sh" --build-image-only
fi

mkdir -p "${CACHE_DIR}"

BUILD_PACKAGES_ENV=0
if ((BUILD_LINUX_PACKAGES)); then
	BUILD_PACKAGES_ENV=1
fi

docker_run_args=(
	docker run
	--rm
	--user 0:0
	--entrypoint /bin/bash
	-e "VERSION_LABEL=${VERSION_LABEL}"
	-e "TARGET_ARCH=${TARGET_ARCH}"
	-e "BUILD_LINUX_PACKAGES=${BUILD_PACKAGES_ENV}"
	-e "EMIT_LG_PAGE16_ALIAS=${EMIT_LG_PAGE16_ALIAS}"
	-e "TYPESENSE_BAZEL_CACHE_DIR=${CACHE_DIR}"
	-e "HOST_UID=$(id -u)"
	-e "HOST_GID=$(id -g)"
	-v "${PROJECT_DIR}:${WORKDIR}"
	-v "${CACHE_DIR}:${CACHE_DIR}"
	-w "${WORKDIR}"
	--platform "${DOCKER_PLATFORM}"
	"${IMAGE}"
	-lc
	'
set -euo pipefail

PROVIDER_DIR="${PWD}/bazel-bin/external/+new_git_repository+onnx_runtime/onnxruntime_static_one_protobuf/lib"
RELEASE_DIR="${PWD}/release/gpu-deps-linux-${TARGET_ARCH}"
ARTIFACT_DIR="${PWD}/artifacts"
PACKAGE_DIR="${ARTIFACT_DIR}/packages"
ARTIFACT_BASENAME="typesense-gpu-deps-${VERSION_LABEL}-linux-${TARGET_ARCH}"
TARBALL="${ARTIFACT_DIR}/${ARTIFACT_BASENAME}.tar.gz"
PROVIDER_SHARED="${PROVIDER_DIR}/libonnxruntime_providers_shared.so"
PROVIDER_CUDA="${PROVIDER_DIR}/libonnxruntime_providers_cuda.so"

if [[ ! -f "${PROVIDER_SHARED}" || ! -f "${PROVIDER_CUDA}" ]]; then
	echo "Expected ORT CUDA provider sidecars were not found under ${PROVIDER_DIR}." >&2
	echo "Build //:typesense-server with --define=use_cuda=on first or pass --build." >&2
	exit 1
fi

actual_shared="$(file -b "${PROVIDER_SHARED}")"
actual_cuda="$(file -b "${PROVIDER_CUDA}")"
echo "${actual_shared}"
echo "${actual_cuda}"
case "${TARGET_ARCH}" in
	amd64)
		echo "${actual_shared}" | grep -q "x86-64"
		echo "${actual_cuda}" | grep -q "x86-64"
		;;
	arm64)
		echo "${actual_shared}" | grep -q "ARM aarch64"
		echo "${actual_cuda}" | grep -q "ARM aarch64"
		;;
esac

echo "libonnxruntime_providers_cuda.so runtime dependencies:"
objdump -p "${PROVIDER_CUDA}" | grep NEEDED

if [[ "${BUILD_LINUX_PACKAGES}" == "1" ]]; then
	apt-get update >/tmp/release-linux-gpu-deps.apt-update.log
	apt-get install -y --no-install-recommends alien rpm dpkg-dev >/tmp/release-linux-gpu-deps.apt-install.log
fi

rm -rf "${RELEASE_DIR}"
mkdir -p "${RELEASE_DIR}" "${ARTIFACT_DIR}" "${PACKAGE_DIR}"
rm -f "${TARBALL}" "${TARBALL}.sha256.txt"

cp "${PROVIDER_SHARED}" "${RELEASE_DIR}/"
cp "${PROVIDER_CUDA}" "${RELEASE_DIR}/"

tar -czf "${TARBALL}" -C "${RELEASE_DIR}" .
python3 - <<PY
import hashlib
from pathlib import Path

archive = Path(r"${TARBALL}")
Path(str(archive) + ".sha256.txt").write_text(f"{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n")
PY

if [[ "${BUILD_LINUX_PACKAGES}" == "1" ]]; then
	TSV="${VERSION_LABEL}" ARCH="${TARGET_ARCH}" RELEASE_ARTIFACT_DIR="${ARTIFACT_DIR}" RELEASE_PACKAGE_DIR="${PACKAGE_DIR}" \
		bash debian-pkg/gpu_generate_deb_rpm.sh
fi

if [[ "${EMIT_LG_PAGE16_ALIAS}" == "1" ]]; then
	ALIAS_BASENAME="typesense-gpu-deps-${VERSION_LABEL}-linux-arm64-lg-page16"
	ALIAS_TARBALL="${ARTIFACT_DIR}/${ALIAS_BASENAME}.tar.gz"
	cp "${TARBALL}" "${ALIAS_TARBALL}"
	cp "${TARBALL}.sha256.txt" "${ALIAS_TARBALL}.sha256.txt"
	sed -i "s#  ${ARTIFACT_BASENAME}.tar.gz#  ${ALIAS_BASENAME}.tar.gz#" "${ALIAS_TARBALL}.sha256.txt"

	if [[ "${BUILD_LINUX_PACKAGES}" == "1" ]]; then
		TSV="${VERSION_LABEL}" ARCH="arm64" ARTIFACT_SUFFIX="-lg-page16" RELEASE_ARTIFACT_DIR="${ARTIFACT_DIR}" RELEASE_PACKAGE_DIR="${PACKAGE_DIR}" \
			bash debian-pkg/gpu_generate_deb_rpm.sh
	fi
fi

chown -R "${HOST_UID}:${HOST_GID}" "${ARTIFACT_DIR}" "${PWD}/release"

find "${ARTIFACT_DIR}" -maxdepth 1 -type f \( -name "typesense-gpu-deps-${VERSION_LABEL}-*.tar.gz" -o -name "typesense-gpu-deps-${VERSION_LABEL}-*.tar.gz.sha256.txt" \) -print0 | xargs -0 ls -lh
if [[ "${BUILD_LINUX_PACKAGES}" == "1" ]]; then
	find "${PACKAGE_DIR}" -maxdepth 1 -type f \( -name "typesense-gpu-deps-${VERSION_LABEL}-*.deb" -o -name "typesense-gpu-deps-${VERSION_LABEL}*.rpm" \) -print0 | xargs -0 ls -lh
fi
'
)

"${docker_run_args[@]}"
