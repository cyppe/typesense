#!/usr/bin/env bash

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPECTED_BAZEL="$(tr -d '[:space:]' <"${PROJECT_DIR}/.bazelversion")"

status_ok=true

check_cmd() {
	local cmd="$1"
	local hint="$2"
	if ! command -v "${cmd}" >/dev/null 2>&1; then
		echo "[missing] ${cmd} (${hint})"
		status_ok=false
	else
		echo "[ok] ${cmd}: $(command -v "${cmd}")"
	fi
}

echo "Checking local host toolchain for non-Docker Bazel runs..."

check_cmd bazel "install bazelisk or bazel"
check_cmd gcc "install GCC"
check_cmd g++ "install G++"
check_cmd pkg-config "install pkg-config/pkgconf"
check_cmd docker "install Docker for containerized flow"

if command -v bazel >/dev/null 2>&1; then
	actual_bazel="$(bazel --version | awk '{print $2}')"
	echo "[info] bazel version: ${actual_bazel} (repo baseline: ${EXPECTED_BAZEL})"
fi

if command -v gcc >/dev/null 2>&1; then
	echo "[info] gcc version: $(gcc -dumpfullversion -dumpversion)"
fi

if command -v g++ >/dev/null 2>&1; then
	echo "[info] g++ version: $(g++ -dumpfullversion -dumpversion)"
fi

if command -v pkg-config >/dev/null 2>&1; then
	echo "[info] pkg-config version: $(pkg-config --version)"
fi

if [[ "${status_ok}" != true ]]; then
	echo
	echo "One or more required commands are missing."
	echo "Use the Dockerized flow instead: scripts/bazel_in_docker.sh build //:typesense-server"
	exit 1
fi

echo
echo "Local toolchain check passed."
