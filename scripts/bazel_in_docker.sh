#!/usr/bin/env bash

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${TYPESENSE_BAZEL_IMAGE:-typesense/ci-bazel:local}"
CACHE_DIR="${TYPESENSE_BAZEL_CACHE_DIR:-${HOME}/.cache/typesense/bazel-docker}"
WORKDIR="${TYPESENSE_BAZEL_WORKDIR:-/work}"
OUTPUT_ROOT="${TYPESENSE_BAZEL_OUTPUT_ROOT:-${CACHE_DIR}}"
DISK_CACHE="${TYPESENSE_BAZEL_DISK_CACHE:-${OUTPUT_ROOT}/disk-cache}"
REPOSITORY_CACHE="${TYPESENSE_BAZEL_REPOSITORY_CACHE:-${OUTPUT_ROOT}/repository-cache}"
BAZELISK_HOME_DIR="${TYPESENSE_BAZELISK_HOME:-${OUTPUT_ROOT}/bazelisk}"
DOCKERFILE_PATH="${TYPESENSE_BAZEL_DOCKERFILE:-docker/ci-bazel.Dockerfile}"
DOCKER_CONTEXT="${TYPESENSE_BAZEL_DOCKER_CONTEXT:-docker}"
DOCKER_PLATFORM="${TYPESENSE_DOCKER_PLATFORM:-}"
REPO_ENV_CONLYOPTS="${TYPESENSE_BAZEL_REPO_ENV_CONLYOPTS:--std=gnu17}"
SKIP_DEFAULT_GCC_CONFIG="${TYPESENSE_BAZEL_SKIP_DEFAULT_GCC_CONFIG:-}"

usage() {
	cat <<'EOF'
Usage:
  scripts/bazel_in_docker.sh --build-image-only
  scripts/bazel_in_docker.sh <bazel-subcommand> [args...]

Defaults:
  - Docker image: typesense/ci-bazel:local
  - Host cache dir: ~/.cache/typesense/bazel-docker
  - C repo env opts: -std=gnu17

Examples:
  scripts/bazel_in_docker.sh --build-image-only
  scripts/bazel_in_docker.sh build //:typesense-server
  scripts/bazel_in_docker.sh test --cache_test_results=no //:typesense-test

Environment:
  TYPESENSE_BAZEL_IMAGE               Override Docker image tag
  TYPESENSE_BAZEL_CACHE_DIR           Override host cache/output root
  TYPESENSE_BAZEL_DOCKERFILE          Override Dockerfile path
  TYPESENSE_BAZEL_DOCKER_CONTEXT      Override Docker build context
  TYPESENSE_DOCKER_PLATFORM           Override Docker build/run platform (for example linux/arm64)
  TYPESENSE_BAZEL_REPO_ENV_CONLYOPTS  Override C-only repo env opts
  TYPESENSE_BAZEL_SKIP_DEFAULT_GCC_CONFIG
                                      Disable the wrapper's default --config=gcc
EOF
}

platform_arch() {
	case "$1" in
		linux/amd64)
			echo "amd64"
			;;
		linux/arm64)
			echo "arm64"
			;;
		"")
			return 1
			;;
		*)
			echo "Unsupported TYPESENSE_DOCKER_PLATFORM: $1" >&2
			exit 1
			;;
	esac
}

build_image() {
	local build_args=(
		docker build
		--pull \
		--file "${PROJECT_DIR}/${DOCKERFILE_PATH}" \
		--tag "${IMAGE}" \
	)
	if [[ -n "${DOCKER_PLATFORM}" ]]; then
		build_args+=(--platform "${DOCKER_PLATFORM}")
	fi
	build_args+=("${PROJECT_DIR}/${DOCKER_CONTEXT}")
	"${build_args[@]}"
}

if [[ "${1:-}" == "--help" ]] || [[ "${1:-}" == "-h" ]]; then
	usage
	exit 0
fi

if [[ "${1:-}" == "--build-image-only" ]]; then
	build_image
	exit 0
fi

if [[ $# -eq 0 ]]; then
	usage
	exit 1
fi

mkdir -p "${CACHE_DIR}" "${DISK_CACHE}" "${REPOSITORY_CACHE}" "${BAZELISK_HOME_DIR}"

image_needs_build=0
if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
	image_needs_build=1
elif [[ -n "${DOCKER_PLATFORM}" ]]; then
	requested_arch="$(platform_arch "${DOCKER_PLATFORM}")"
	actual_arch="$(docker image inspect --format '{{.Architecture}}' "${IMAGE}")"
	if [[ "${actual_arch}" != "${requested_arch}" ]]; then
		image_needs_build=1
	fi
fi

if ((image_needs_build)); then
	build_image
fi

default_to_gcc_config=0
if [[ -z "${SKIP_DEFAULT_GCC_CONFIG}" ]] && [[ "${1}" =~ ^(build|test|run|coverage)$ ]]; then
	default_to_gcc_config=1
	for arg in "$@"; do
		case "${arg}" in
			--config=gcc|--repo_env=CC=*clang*|--repo_env=CXX=*clang*|--config=clang)
				default_to_gcc_config=0
				break
				;;
		esac
	done
fi

if ((default_to_gcc_config)); then
	bazel_args=(--batch "${1}" "--config=gcc" "${@:2}")
else
	bazel_args=(--batch "$@")
fi

if [[ "${1}" =~ ^(build|test|run|coverage|fetch|query|aquery|cquery)$ ]]; then
	bazel_args=("${bazel_args[0]}" "${bazel_args[1]}" "--disk_cache=${DISK_CACHE}" "--repository_cache=${REPOSITORY_CACHE}" "${bazel_args[@]:2}")
fi
if [[ -n "${REPO_ENV_CONLYOPTS}" ]] && [[ "${1}" =~ ^(build|test|run|coverage)$ ]]; then
	bazel_args=("${bazel_args[0]}" "${bazel_args[1]}" "--repo_env=BAZEL_CONLYOPTS=${REPO_ENV_CONLYOPTS}" "${bazel_args[@]:2}")
fi

docker_env_args=(
	-e "USER=$(id -u)"
	-e HOME=/tmp
	-e "BAZELISK_HOME=${BAZELISK_HOME_DIR}"
)

if [[ -n "${USE_BAZEL_VERSION:-}" ]]; then
	docker_env_args+=("-e" "USE_BAZEL_VERSION=${USE_BAZEL_VERSION}")
fi

if [[ -n "${BAZELISK_GITHUB_TOKEN:-}" ]]; then
	docker_env_args+=("-e" "BAZELISK_GITHUB_TOKEN=${BAZELISK_GITHUB_TOKEN}")
fi

docker_run_args=(
	docker run
	--rm
	--user "$(id -u):$(id -g)"
	"${docker_env_args[@]}"
	-v "${PROJECT_DIR}:${WORKDIR}"
	-v "${CACHE_DIR}:${OUTPUT_ROOT}"
	-w "${WORKDIR}"
)

if [[ -n "${DOCKER_PLATFORM}" ]]; then
	docker_run_args+=(--platform "${DOCKER_PLATFORM}")
fi

docker_run_args+=(
	"${IMAGE}"
	--output_user_root="${OUTPUT_ROOT}"
	"${bazel_args[@]}"
)

"${docker_run_args[@]}"
