#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
API_TESTS_DIR="${REPO_ROOT}/api_tests"
RUNTIME_BUNDLE_DIR="${TYPESENSE_RUNTIME_BUNDLE_DIR:-${REPO_ROOT}/typesense-runtime-bundle}"
DATA_DIR="${REPO_ROOT}/tmp/test"
BUN_IMAGE="${TYPESENSE_API_TEST_BUN_IMAGE:-typesense/api-tests-bun:local}"
SERVER_BINARY_PATH="${TYPESENSE_SERVER_BINARY_PATH:-}"
SERVER_BINARY_FLAVOR="${TYPESENSE_SERVER_FLAVOR:-typesense-server}"
USE_HOST_BUN=false
SKIP_INSTALL=false

detect_server_flavor() {
	local binary_path="$1"
	local binary_name
	binary_name="$(basename "${binary_path}")"

	case "${binary_name}" in
	typesense-server-nuraft-runtime)
		echo "nuraft-runtime"
		;;
	*)
		echo "typesense-server"
		;;
	esac
}

usage() {
	cat <<'EOF'
Usage:
  scripts/run_api_tests.sh [--host-bun] [--skip-install] [--runtime-bundle-dir DIR] [--server-binary PATH] [-- <api test args...>]

Defaults:
  - Prepares the runtime bundle from the current Bazel build output
  - Runs the API test CLI in Docker using the repo's Ubuntu-based Bun image
  - Does not require Bun on the host
  - Forces IPv4 API health checks via TYPESENSE_API_HOST=127.0.0.1

Examples:
  scripts/run_api_tests.sh -- --no-secrets --download-migration-binary
  scripts/run_api_tests.sh -- tests/health.test.ts
  scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-nuraft-runtime -- --no-secrets tests/nuraft_runtime_smoke.test.ts
  scripts/run_api_tests.sh --runtime-bundle-dir ./typesense-server-binary -- --no-secrets
  scripts/run_api_tests.sh --server-binary ./bazel-bin/typesense-server-static-one-protobuf-probe -- --no-secrets tests/health.test.ts
  scripts/run_api_tests.sh --host-bun -- --no-secrets tests/health.test.ts

Environment:
  TYPESENSE_API_TEST_BUN_IMAGE  Override Bun image tag (default: typesense/api-tests-bun:local)
  TYPESENSE_RUNTIME_BUNDLE_DIR  Reuse an existing runtime bundle instead of preparing one in typesense-runtime-bundle
  TYPESENSE_SERVER_BINARY_PATH  Use a custom server binary when preparing the runtime bundle
  TYPESENSE_API_HOST            Set internally to 127.0.0.1 for reliable Dockerized health checks
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--host-bun)
		USE_HOST_BUN=true
		shift
		;;
	--skip-install)
		SKIP_INSTALL=true
		shift
		;;
	--runtime-bundle-dir)
		RUNTIME_BUNDLE_DIR="$2"
		shift 2
		;;
	--server-binary)
		SERVER_BINARY_PATH="$2"
		shift 2
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
if [[ -n "${SERVER_BINARY_PATH}" ]]; then
	SERVER_BINARY_FLAVOR="$(detect_server_flavor "${SERVER_BINARY_PATH}")"
	bash "${REPO_ROOT}/api_tests/scripts/prepare_runtime_bundle.sh" "${RUNTIME_BUNDLE_DIR}" "${SERVER_BINARY_PATH}"
elif [[ ! -x "${RUNTIME_BUNDLE_DIR}/typesense-server" ]]; then
	bash "${REPO_ROOT}/api_tests/scripts/prepare_runtime_bundle.sh" "${RUNTIME_BUNDLE_DIR}"
fi

API_TEST_ARGS=("$@")

ENV_VARS=(
	"TYPESENSE_BINARY_PATH=${RUNTIME_BUNDLE_DIR}/typesense-server"
	"LD_LIBRARY_PATH=${RUNTIME_BUNDLE_DIR}/lib"
	"TYPESENSE_DATA_DIR=${DATA_DIR}"
	"TYPESENSE_API_HOST=127.0.0.1"
	"TYPESENSE_SERVER_FLAVOR=${SERVER_BINARY_FLAVOR}"
)

if [[ "${USE_HOST_BUN}" == "true" ]]; then
	if [[ "${SKIP_INSTALL}" != "true" ]]; then
		(cd "${API_TESTS_DIR}" && bun install --frozen-lockfile)
	fi
	(
		cd "${API_TESTS_DIR}"
		env "${ENV_VARS[@]}" bun src/cli.ts "${API_TEST_ARGS[@]}"
	)
	exit 0
fi

docker build -f "${REPO_ROOT}/api_tests/Dockerfile.bun" -t "${BUN_IMAGE}" "${REPO_ROOT}" >/dev/null

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
	-e "TYPESENSE_API_HOST=127.0.0.1" \
	-e "TYPESENSE_SERVER_FLAVOR=${SERVER_BINARY_FLAVOR}" \
	-v "${REPO_ROOT}:${REPO_ROOT}" \
	-w "${API_TESTS_DIR}" \
	"${BUN_IMAGE}" \
	sh -lc "${INSTALL_PREFIX}bun src/cli.ts${QUOTED_ARGS}"
