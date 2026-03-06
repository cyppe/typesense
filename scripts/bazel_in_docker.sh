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
REPO_ENV_CONLYOPTS="${TYPESENSE_BAZEL_REPO_ENV_CONLYOPTS:--std=gnu17}"

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
  TYPESENSE_BAZEL_REPO_ENV_CONLYOPTS  Override C-only repo env opts
EOF
}

build_image() {
	docker build \
		--pull \
		--file "${PROJECT_DIR}/${DOCKERFILE_PATH}" \
		--tag "${IMAGE}" \
		"${PROJECT_DIR}/${DOCKER_CONTEXT}"
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

if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
	build_image
fi

bazel_args=(--batch "$@")
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

docker run \
	--rm \
	--user "$(id -u):$(id -g)" \
	"${docker_env_args[@]}" \
	-v "${PROJECT_DIR}:${WORKDIR}" \
	-v "${CACHE_DIR}:${OUTPUT_ROOT}" \
	-w "${WORKDIR}" \
	"${IMAGE}" \
	--output_user_root="${OUTPUT_ROOT}" \
	"${bazel_args[@]}"
