#!/bin/bash

set -euo pipefail

if [[ -z "${TSV:-}" ]]; then
	echo "\$TSV is not provided. Quitting." >&2
	exit 1
fi

if [[ -z "${ARCH:-}" ]]; then
	echo "\$ARCH is not provided. Quitting." >&2
	exit 1
fi

ARTIFACT_SUFFIX="${ARTIFACT_SUFFIX:-}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ARTIFACT_BASENAME="typesense-gpu-deps-${TSV}-linux-${ARCH}${ARTIFACT_SUFFIX}.tar.gz"
PACKAGE_VERSION="${TSV}"
if [[ ! "${PACKAGE_VERSION}" =~ ^[0-9] ]]; then
	PACKAGE_VERSION="0~${PACKAGE_VERSION}"
fi

RELEASE_PACKAGE_DIR="${RELEASE_PACKAGE_DIR:-${SCRIPT_DIR}/../artifacts/packages}"
mkdir -p "${RELEASE_PACKAGE_DIR}"

if [[ -n "${RELEASE_TARBALL_PATH:-}" ]]; then
	RELEASE_TARBALL="${RELEASE_TARBALL_PATH}"
else
	RELEASE_TARBALL=""
	for candidate_dir in "${RELEASE_ARTIFACT_DIR:-${SCRIPT_DIR}/../artifacts}" "${SCRIPT_DIR}/../bazel-bin"; do
		candidate_path="${candidate_dir}/${ARTIFACT_BASENAME}"
		if [[ -f "${candidate_path}" ]]; then
			RELEASE_TARBALL="${candidate_path}"
			break
		fi
	done
fi

if [[ -z "${RELEASE_TARBALL}" ]]; then
	echo "Release tarball not found for ${ARTIFACT_BASENAME}." >&2
	echo "Set RELEASE_TARBALL_PATH or RELEASE_ARTIFACT_DIR to point at the built GPU deps artifact." >&2
	exit 1
fi

RELEASE_SHA256_PATH="${RELEASE_SHA256_PATH:-${RELEASE_TARBALL}.sha256.txt}"
if [[ -f "${RELEASE_SHA256_PATH}" ]]; then
	expected_sha256=$(awk "{print \$1}" "${RELEASE_SHA256_PATH}")
	actual_sha256=$(sha256sum "${RELEASE_TARBALL}" | cut -d" " -f1)
	if [[ "${expected_sha256}" != "${actual_sha256}" ]]; then
		echo "Release tarball checksum mismatch for ${RELEASE_TARBALL}." >&2
		exit 1
	fi
fi

DEB_BUILD_DIR="/tmp/typesense-gpu-deb-build"
EXTRACT_DIR="/tmp/typesense-gpu-deps-${TSV}"
RPM_BUILD_DIR="/tmp/typesense-gpu-rpm-build"
DEB_FILE_BASENAME="typesense-gpu-deps-${TSV}-${ARCH}${ARTIFACT_SUFFIX}.deb"
RPM_RELEASE_SUFFIX="${ARTIFACT_SUFFIX//-/.}"

RPM_ARCH="${ARCH}"
if [[ "${ARCH}" == "amd64" ]]; then
	RPM_ARCH="x86_64"
elif [[ "${ARCH}" == "arm64" ]]; then
	RPM_ARCH="aarch64"
fi
RPM_FILE_BASENAME="typesense-gpu-deps-${TSV}-1${RPM_RELEASE_SUFFIX}.${RPM_ARCH}.rpm"

set -x

rm -rf "${DEB_BUILD_DIR}"
mkdir -p "${DEB_BUILD_DIR}"
cp -r "${SCRIPT_DIR}/typesense-gpu-deps" "${DEB_BUILD_DIR}"

rm -rf "${EXTRACT_DIR}"
mkdir -p "${EXTRACT_DIR}"
tar -xzf "${RELEASE_TARBALL}" -C "${EXTRACT_DIR}"
mkdir -p "${DEB_BUILD_DIR}/typesense-gpu-deps/usr/lib/"

shared_objects=("${EXTRACT_DIR}"/*.so)
if [[ ! -e "${shared_objects[0]}" ]]; then
	echo "No shared libraries found under ${EXTRACT_DIR}" >&2
	exit 1
fi
cp "${shared_objects[@]}" "${DEB_BUILD_DIR}/typesense-gpu-deps/usr/lib/"

rm -rf "${EXTRACT_DIR}" "${EXTRACT_DIR}.tar.gz"

while IFS= read -r -d "" file; do
	sed -i "s/\$VERSION/${PACKAGE_VERSION}/g" "${file}"
	sed -i "s/\$ARCH/${ARCH}/g" "${file}"
done < <(find "${DEB_BUILD_DIR}/typesense-gpu-deps/DEBIAN" -type f -print0)

dpkg-deb -Zgzip -z6 \
	-b "${DEB_BUILD_DIR}/typesense-gpu-deps" "${DEB_BUILD_DIR}/${DEB_FILE_BASENAME}"

rm -rf "${RPM_BUILD_DIR}"
mkdir -p "${RPM_BUILD_DIR}"
cp "${DEB_BUILD_DIR}/${DEB_FILE_BASENAME}" "${RPM_BUILD_DIR}"
(
	cd "${RPM_BUILD_DIR}"
	alien --scripts -k -r -g -v "${RPM_BUILD_DIR}/${DEB_FILE_BASENAME}"
)

while IFS= read -r -d "" spec_file; do
	sed -i "s#%dir \"/\"##" "${spec_file}"
	sed -i "s#%dir \"/usr/bin/\"##" "${spec_file}"
	sed -i "s/%config/%config(noreplace)/g" "${spec_file}"
	sed -i "s/^Release: 1/Release: 1${RPM_RELEASE_SUFFIX}/" "${spec_file}"
done < <(find "${RPM_BUILD_DIR}" -maxdepth 10 -type f -name "*.spec" -print0)

mapfile -t spec_files < <(find "${RPM_BUILD_DIR}" -maxdepth 10 -type f -name "*.spec")
if [[ "${#spec_files[@]}" -eq 0 ]]; then
	echo "Unable to locate generated RPM spec file under ${RPM_BUILD_DIR}" >&2
	exit 1
fi

SPEC_FILE="${spec_files[0]}"
SPEC_BUILD_DIR="$(dirname "${SPEC_FILE}")"
RPM_BUILDROOT="$(mktemp -d /tmp/typesense-gpu-rpm-build/buildroot.XXXXXX)"
cp -a "${SPEC_BUILD_DIR}/." "${RPM_BUILDROOT}/"
find "${RPM_BUILDROOT}" -maxdepth 1 -type f -name "*.spec" -delete
SPEC_FILE_COPY="${SPEC_FILE%.spec}-copy.spec"

cp "${SPEC_FILE}" "${SPEC_FILE_COPY}"

PRE_LINE=$(grep -n "%pre" "${SPEC_FILE_COPY}" | cut -f1 -d: || true)
if [[ -n "${PRE_LINE}" ]]; then
	START_LINE=$((PRE_LINE - 1))
	head -"${START_LINE}" "${SPEC_FILE_COPY}" >"${SPEC_FILE}"
else
	cp "${SPEC_FILE_COPY}" "${SPEC_FILE}"
fi

{
	echo "%prep"
	printf "%s\n" \
		"cat >/tmp/find_requires.sh <<EOF" \
		"#!/bin/sh" \
		"%{__find_requires} | grep -v GLIBC_PRIVATE" \
		"exit 0" \
		"EOF"
	echo "chmod +x /tmp/find_requires.sh"
	echo "%define _use_internal_dependency_generator 0"
	echo "%define __find_requires /tmp/find_requires.sh"
} >>"${SPEC_FILE}"

if [[ -n "${PRE_LINE}" ]]; then
	tail -n+"${START_LINE}" "${SPEC_FILE_COPY}" >>"${SPEC_FILE}"
fi

rm "${SPEC_FILE_COPY}"

(
	cd "${SPEC_BUILD_DIR}"
	rpmbuild --target="${RPM_ARCH}" --buildroot "${RPM_BUILDROOT}" -bb "${SPEC_FILE}"
)

cp "${DEB_BUILD_DIR}/${DEB_FILE_BASENAME}" "${RELEASE_PACKAGE_DIR}"
GENERATED_RPM="$(find "${RPM_BUILD_DIR}" /root/rpmbuild/RPMS -type f -name "typesense-gpu-deps-*.rpm" 2>/dev/null | head -n 1 || true)"
if [[ -z "${GENERATED_RPM}" ]]; then
	echo "Unable to locate generated GPU RPM artifact." >&2
	exit 1
fi
cp "${GENERATED_RPM}" "${RELEASE_PACKAGE_DIR}/${RPM_FILE_BASENAME}"
