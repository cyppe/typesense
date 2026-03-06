#!/usr/bin/env bash

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET="${TYPESENSE_TARGET:-}"

if [[ -z "${TARGET}" ]]; then
	echo "Deprecated: ci_build.sh is no longer the supported entrypoint." >&2
	echo "Use one of these instead:" >&2
	echo "  scripts/bazel_in_docker.sh build //:typesense-server" >&2
	echo "  scripts/bazel_in_docker.sh test //:typesense-test" >&2
	exit 1
fi

case "${TARGET}" in
typesense-server | typesense-test)
	if [[ "$*" == *"--build-deploy-image"* ]] || [[ "$*" == *"--package-binary"* ]] || [[ "$*" == *"--with-cuda"* ]] || [[ "$*" == *"--with-jemalloc-lg-page16"* ]]; then
		echo "Deprecated: ci_build.sh packaging/image flags are no longer supported here." >&2
		echo "Use scripts/bazel_in_docker.sh for canonical build/test flows and add a dedicated documented script if packaging must remain supported." >&2
		exit 1
	fi
	exec "${PROJECT_DIR}/scripts/bazel_in_docker.sh" build "//:${TARGET}" "$@"
	;;
*)
	echo "Unsupported TYPESENSE_TARGET: ${TARGET}" >&2
	echo "Supported compatibility targets: typesense-server, typesense-test" >&2
	exit 1
	;;
esac
