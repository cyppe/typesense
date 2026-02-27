#!/usr/bin/env bash
# TYPESENSE_VERSION=nightly TYPESENSE_TARGET=typesense-server|typesense-test bash ci_build.sh

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="bazel-bin"
TYPESENSE_VERSION="${TYPESENSE_VERSION:-nightly}"

if [[ -z "${TYPESENSE_TARGET:-}" ]]; then
	echo "TYPESENSE_TARGET is required (typesense-server or typesense-test)."
	exit 1
fi

ARCH_NAME="amd64"
if [[ "$*" == *"--graviton2"* ]] || [[ "$*" == *"--arm"* ]]; then
	ARCH_NAME="arm64"
fi

if [[ "${TYPESENSE_USE_DOCKER:-1}" == "1" ]]; then
	BAZEL_RUNNER=("${PROJECT_DIR}/scripts/bazel_in_docker.sh")
else
	BAZEL_RUNNER=("bazel")
fi

JOBS=6
if [[ "$*" =~ --jobs=([0-9]+) ]]; then
	JOBS="${BASH_REMATCH[1]}"
fi

CUDA_FLAGS=()
if [[ "$*" == *"--with-cuda"* ]]; then
	CUDA_FLAGS=(--define use_cuda=on --action_env=CUDA_HOME=/usr/local/cuda --action_env=CUDNN_HOME=/usr/local/cuda)
fi

JEMALLOC_FLAGS=()
USE_JEMALLOC_LG_PAGE16=false
if [[ "$*" == *"--with-jemalloc-lg-page16"* ]]; then
	JEMALLOC_FLAGS=(--define enable_jemalloc_lg_page16=1)
	USE_JEMALLOC_LG_PAGE16=true
fi

"${BAZEL_RUNNER[@]}" build --jobs="${JOBS}" @com_google_protobuf//:protobuf_headers
"${BAZEL_RUNNER[@]}" build --jobs="${JOBS}" @com_google_protobuf//:protobuf_lite
"${BAZEL_RUNNER[@]}" build --jobs="${JOBS}" @com_google_protobuf//:protobuf
"${BAZEL_RUNNER[@]}" build --jobs="${JOBS}" @com_google_protobuf//:protoc

if [[ "$*" == *"--with-cuda"* ]]; then
	"${BAZEL_RUNNER[@]}" build --jobs="${JOBS}" @whisper.cpp//:whisper_cuda_shared "${CUDA_FLAGS[@]}" --experimental_cc_shared_library
	cp -f "${PROJECT_DIR}/${BUILD_DIR}/external/whisper.cpp/libwhisper_cuda_shared.so" "${PROJECT_DIR}/${BUILD_DIR}/"
fi

"${BAZEL_RUNNER[@]}" build \
	--verbose_failures \
	--jobs="${JOBS}" \
	"${CUDA_FLAGS[@]}" \
	"${JEMALLOC_FLAGS[@]}" \
	--define=TYPESENSE_VERSION=\"${TYPESENSE_VERSION}\" \
	"//:${TYPESENSE_TARGET}"

mkdir -p "${PROJECT_DIR}/dist"
cp -f "${PROJECT_DIR}/${BUILD_DIR}/${TYPESENSE_TARGET}" "${PROJECT_DIR}/dist/"

if [[ "$*" == *"--build-deploy-image"* ]]; then
	echo "Creating deployment image for Typesense ${TYPESENSE_VERSION} server ..."
	docker build \
		--platform "linux/${ARCH_NAME}" \
		--file "${PROJECT_DIR}/docker/deployment.Dockerfile" \
		--tag "typesense/typesense:${TYPESENSE_VERSION}" \
		"${PROJECT_DIR}/${BUILD_DIR}"
fi

if [[ "$*" == *"--package-binary"* ]]; then
	RELEASE_NAME="typesense-server-${TYPESENSE_VERSION}-linux-${ARCH_NAME}"
	if [[ "${USE_JEMALLOC_LG_PAGE16}" == true ]]; then
		RELEASE_NAME="${RELEASE_NAME}-lg-page16"
	fi
	printf "%s" "$(md5sum "${PROJECT_DIR}/${BUILD_DIR}/typesense-server" | cut -b-32)" >"${PROJECT_DIR}/${BUILD_DIR}/typesense-server.md5.txt"
	tar -cvzf "${PROJECT_DIR}/${BUILD_DIR}/${RELEASE_NAME}.tar.gz" -C "${PROJECT_DIR}/${BUILD_DIR}" typesense-server typesense-server.md5.txt
	echo "Built binary successfully: ${PROJECT_DIR}/${BUILD_DIR}/${RELEASE_NAME}.tar.gz"

	if [[ "$*" == *"--with-cuda"* ]]; then
		GPU_DEPS_NAME="typesense-gpu-deps-${TYPESENSE_VERSION}-linux-${ARCH_NAME}"
		if [[ "${USE_JEMALLOC_LG_PAGE16}" == true ]]; then
			GPU_DEPS_NAME="${GPU_DEPS_NAME}-lg-page16"
		fi
		tar -cvzf "${PROJECT_DIR}/${BUILD_DIR}/${GPU_DEPS_NAME}.tar.gz" -C "${PROJECT_DIR}/${BUILD_DIR}" libonnxruntime_providers_cuda.so libonnxruntime_providers_shared.so libwhisper_cuda_shared.so
		echo "Built GPU dependencies successfully: ${PROJECT_DIR}/${BUILD_DIR}/${GPU_DEPS_NAME}.tar.gz"
	fi
fi
