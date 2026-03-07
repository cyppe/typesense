#!/bin/bash

if [ -z "${TYPESENSE_VERSION:-}" ]; then
	echo "\$TYPESENSE_VERSION is not provided. Quitting."
	exit 1
fi

set -ex
CURR_DIR=$(dirname "$0" | while read -r a; do cd "$a" && pwd && break; done)
RELEASE_ARTIFACT_DIR="${RELEASE_ARTIFACT_DIR:-${CURR_DIR}/artifacts}"
RELEASE_PACKAGE_DIR="${RELEASE_PACKAGE_DIR:-${CURR_DIR}/artifacts/packages}"
AWS_PROFILE_NAME="${AWS_PROFILE_NAME:-typesense}"
S3_RELEASE_PREFIX="${S3_RELEASE_PREFIX:-s3://dl.typesense.org/releases}"

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

if [ "${found_tarballs}" = false ]; then
	for legacy_tarball in \
		"${CURR_DIR}/build-Linux/typesense-server-${TYPESENSE_VERSION}-linux-amd64.tar.gz" \
		"${CURR_DIR}/build-Darwin/typesense-server-${TYPESENSE_VERSION}-darwin-amd64.tar.gz"; do
		upload_if_present "${legacy_tarball}"
		upload_if_present "${legacy_tarball}.sha256.txt"
	done
fi

if [ -d "${RELEASE_PACKAGE_DIR}" ]; then
	while IFS= read -r -d '' package_file; do
		upload_if_present "${package_file}"
	done < <(find "${RELEASE_PACKAGE_DIR}" -maxdepth 1 -type f \( -name "typesense-server-${TYPESENSE_VERSION}-*.deb" -o -name "typesense-server-${TYPESENSE_VERSION}-*.rpm" \) -print0 | sort -z)
fi
