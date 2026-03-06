#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
API_TESTS_DIR="${REPO_ROOT}/api_tests"
RUNTIME_BUNDLE_DIR="${REPO_ROOT}/typesense-runtime-bundle"
DATA_DIR="${REPO_ROOT}/tmp/test"
BUN_IMAGE="${TYPESENSE_API_TEST_BUN_IMAGE:-oven/bun:1.3.10}"
USE_DOCKER_BUN=false
SKIP_INSTALL=false

usage() {
	cat <<'EOF'
Usage:
  scripts/run_api_tests.sh [--docker-bun] [--skip-install] [-- <api test args...>]

Defaults:
  - Prepares the runtime bundle from the current Bazel build output
  - Runs the API test CLI with host Bun for reliable host process/port orchestration
  - Keeps Dockerized Bun available via --docker-bun when you explicitly want it

Examples:
  scripts/run_api_tests.sh -- --no-secrets --download-migration-binary
  scripts/run_api_tests.sh -- tests/health.test.ts
  scripts/run_api_tests.sh --docker-bun -- --no-secrets tests/health.test.ts

Environment:
  TYPESENSE_API_TEST_BUN_IMAGE  Override Bun image tag (default: oven/bun:1.3.10)
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--docker-bun)
		USE_DOCKER_BUN=true
		shift
		;;
	--skip-install)
		SKIP_INSTALL=true
		shift
		;;
	--help | -h)
		usage
		exit 0
		;;
	--)
		shift
		break
		;;
	*)
		break
		;;
	esac
done

mkdir -p "${DATA_DIR}"
bash "${REPO_ROOT}/api_tests/scripts/prepare_runtime_bundle.sh" "${RUNTIME_BUNDLE_DIR}"

API_TEST_ARGS=("$@")
ENV_VARS=(
	"TYPESENSE_BINARY_PATH=${RUNTIME_BUNDLE_DIR}/typesense-server"
	"LD_LIBRARY_PATH=${RUNTIME_BUNDLE_DIR}/lib"
	"TYPESENSE_DATA_DIR=${DATA_DIR}"
)

if [[ "${USE_DOCKER_BUN}" != "true" ]]; then
	if [[ "${SKIP_INSTALL}" != "true" ]]; then
		(cd "${API_TESTS_DIR}" && bun install --frozen-lockfile)
	fi
	(
		cd "${API_TESTS_DIR}"
		env "${ENV_VARS[@]}" bun src/cli.ts "${API_TEST_ARGS[@]}"
	)
	exit 0
fi

if [[ "${SKIP_INSTALL}" == "true" ]]; then
	INSTALL_PREFIX=""
else
	INSTALL_PREFIX="bun install --frozen-lockfile && "
fi

printf -v QUOTED_ARGS ' %q' "${API_TEST_ARGS[@]}"

docker run \
	--rm \
	--init \
	--network host \
	--user "$(id -u):$(id -g)" \
	-e HOME=/tmp \
	-e "TYPESENSE_BINARY_PATH=${RUNTIME_BUNDLE_DIR}/typesense-server" \
	-e "LD_LIBRARY_PATH=${RUNTIME_BUNDLE_DIR}/lib" \
	-e "TYPESENSE_DATA_DIR=${DATA_DIR}" \
	-v "${REPO_ROOT}:${REPO_ROOT}" \
	-w "${API_TESTS_DIR}" \
	"${BUN_IMAGE}" \
	sh -lc "${INSTALL_PREFIX}bun src/cli.ts${QUOTED_ARGS}"
