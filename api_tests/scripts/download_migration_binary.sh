#!/usr/bin/env bash

set -euo pipefail

VERSION="${1:-29.0}"
TARGET="${2:-linux-amd64}"
OUTPUT_DIR="${3:-./artifacts/v${VERSION%%.*}}"

ARCHIVE_NAME="typesense-server-${VERSION}-${TARGET}.tar.gz"
DOWNLOAD_URL="https://dl.typesense.org/releases/${VERSION}/${ARCHIVE_NAME}"
BINARY_PATH="${OUTPUT_DIR}/typesense-server"

mkdir -p "${OUTPUT_DIR}"

if [[ "${TYPESENSE_FORCE_DOWNLOAD_MIGRATION_BINARY:-0}" != "1" ]] && [[ -x "${BINARY_PATH}" ]]; then
	echo "Using existing migration source binary at ${BINARY_PATH}"
	exit 0
fi

curl -fL "${DOWNLOAD_URL}" -o "${OUTPUT_DIR}/${ARCHIVE_NAME}"
tar -xzf "${OUTPUT_DIR}/${ARCHIVE_NAME}" -C "${OUTPUT_DIR}"

echo "Downloaded migration source binary to ${BINARY_PATH}"
