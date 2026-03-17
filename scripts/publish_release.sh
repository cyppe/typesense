#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
RELEASE_ARTIFACT_DIR="${RELEASE_ARTIFACT_DIR:-${PROJECT_DIR}/artifacts}"
RELEASE_PACKAGE_DIR="${RELEASE_PACKAGE_DIR:-${PROJECT_DIR}/artifacts/packages}"
AWS_PROFILE_NAME="${AWS_PROFILE_NAME:-typesense}"
S3_RELEASE_PREFIX="${S3_RELEASE_PREFIX:-s3://dl.typesense.org/releases}"

usage() {
	cat <<'EOF'
Usage:
  TYPESENSE_VERSION=<version> scripts/publish_release.sh

Environment:
  TYPESENSE_VERSION       Version label to publish (required)
  RELEASE_ARTIFACT_DIR    Tarball/checksum directory (default: ./artifacts)
  RELEASE_PACKAGE_DIR     Package directory (default: ./artifacts/packages)
  AWS_PROFILE_NAME        AWS CLI profile (default: typesense)
  S3_RELEASE_PREFIX       Destination prefix (default: s3://dl.typesense.org/releases)

Notes:
  - This helper only uploads already-built artifacts. It does not build release outputs.
  - Prefer `release-binaries.yml` or the repo-owned local release wrappers to create artifacts first.
  - Legacy `build-Linux` / `build-Darwin` tarball fallbacks remain until the older layout is fully retired.
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
	usage
	exit 0
fi

: "${TYPESENSE_VERSION:?TYPESENSE_VERSION is required. Use --help for details.}"

upload_if_present() {
	local path="$1"
	if [ ! -f "${path}" ]; then
		return
	fi

	local name
	name=$(basename "${path}")
	aws s3 cp "${path}" "${S3_RELEASE_PREFIX}/${name}" --profile "${AWS_PROFILE_NAME}"
}

found_tarballs=false
if [ -d "${RELEASE_ARTIFACT_DIR}" ]; then
	while IFS= read -r -d '' tarball; do
		found_tarballs=true
		upload_if_present "${tarball}"
		upload_if_present "${tarball}.sha256.txt"
	done < <(find "${RELEASE_ARTIFACT_DIR}" -maxdepth 1 -type f -name "typesense-server-${TYPESENSE_VERSION}-*.tar.gz" -print0 | sort -z)
fi

if [ -d "${RELEASE_ARTIFACT_DIR}" ]; then
	while IFS= read -r -d '' tarball; do
		upload_if_present "${tarball}"
		upload_if_present "${tarball}.sha256.txt"
	done < <(find "${RELEASE_ARTIFACT_DIR}" -maxdepth 1 -type f -name "typesense-gpu-deps-${TYPESENSE_VERSION}-*.tar.gz" -print0 | sort -z)
fi

if [ "${found_tarballs}" = false ]; then
	for legacy_tarball in \
		"${PROJECT_DIR}/build-Linux/typesense-server-${TYPESENSE_VERSION}-linux-amd64.tar.gz" \
		"${PROJECT_DIR}/build-Darwin/typesense-server-${TYPESENSE_VERSION}-darwin-amd64.tar.gz"; do
		upload_if_present "${legacy_tarball}"
		upload_if_present "${legacy_tarball}.sha256.txt"
	done
fi

if [ -d "${RELEASE_PACKAGE_DIR}" ]; then
	while IFS= read -r -d '' package_file; do
		upload_if_present "${package_file}"
	done < <(find "${RELEASE_PACKAGE_DIR}" -maxdepth 1 -type f \( \
		-name "typesense-server-${TYPESENSE_VERSION}-*.deb" -o \
		-name "typesense-server-${TYPESENSE_VERSION}*.rpm" -o \
		-name "typesense-server-${TYPESENSE_VERSION/-/_}*.rpm" \
	\) -print0 | sort -z)
fi

if [ -d "${RELEASE_PACKAGE_DIR}" ]; then
	while IFS= read -r -d '' package_file; do
		upload_if_present "${package_file}"
	done < <(find "${RELEASE_PACKAGE_DIR}" -maxdepth 1 -type f \( \
		-name "typesense-gpu-deps-${TYPESENSE_VERSION}-*.deb" -o \
		-name "typesense-gpu-deps-${TYPESENSE_VERSION}*.rpm" -o \
		-name "typesense-gpu-deps-${TYPESENSE_VERSION/-/_}*.rpm" \
	\) -print0 | sort -z)
fi
