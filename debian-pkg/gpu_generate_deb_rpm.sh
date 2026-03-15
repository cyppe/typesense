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
BAZEL_BIN_DIR="${SCRIPT_DIR}/../bazel-bin"
DEB_BUILD_DIR="/tmp/typesense-gpu-deb-build"
EXTRACT_DIR="/tmp/typesense-gpu-deps-${TSV}"
RPM_BUILD_DIR="/tmp/typesense-gpu-rpm-build"
DEB_FILE_BASENAME="typesense-gpu-deps-${TSV}-${ARCH}${ARTIFACT_SUFFIX}.deb"
RPM_RELEASE_SUFFIX="${ARTIFACT_SUFFIX//-/.}"
TARBALL_PATH="${BAZEL_BIN_DIR}/typesense-gpu-deps-${TSV}-linux-${ARCH}${ARTIFACT_SUFFIX}.tar.gz"

RPM_ARCH="${ARCH}"
if [[ "${ARCH}" == "amd64" ]]; then
  RPM_ARCH="x86_64"
elif [[ "${ARCH}" == "arm64" ]]; then
  RPM_ARCH="aarch64"
fi

set -x

rm -rf "${DEB_BUILD_DIR}"
mkdir -p "${DEB_BUILD_DIR}"
cp -r "${SCRIPT_DIR}/typesense-gpu-deps" "${DEB_BUILD_DIR}"

rm -rf "${EXTRACT_DIR}"
mkdir -p "${EXTRACT_DIR}"
tar -xzf "${TARBALL_PATH}" -C "${EXTRACT_DIR}"
mkdir -p "${DEB_BUILD_DIR}/typesense-gpu-deps/usr/lib/"

shared_objects=("${EXTRACT_DIR}"/*.so)
if [[ ! -e "${shared_objects[0]}" ]]; then
  echo "No shared libraries found under ${EXTRACT_DIR}" >&2
  exit 1
fi
cp "${shared_objects[@]}" "${DEB_BUILD_DIR}/typesense-gpu-deps/usr/lib/"

rm -rf "${EXTRACT_DIR}" "${EXTRACT_DIR}.tar.gz"

while IFS= read -r -d '' file; do
  sed -i "s/\$VERSION/${TSV}/g" "${file}"
  sed -i "s/\$ARCH/${ARCH}/g" "${file}"
done < <(find "${DEB_BUILD_DIR}" -maxdepth 10 -type f -print0)

dpkg-deb -Zgzip -z6 \
         -b "${DEB_BUILD_DIR}/typesense-gpu-deps" "${DEB_BUILD_DIR}/${DEB_FILE_BASENAME}"

rm -rf "${RPM_BUILD_DIR}"
mkdir -p "${RPM_BUILD_DIR}"
cp "${DEB_BUILD_DIR}/${DEB_FILE_BASENAME}" "${RPM_BUILD_DIR}"
(
  cd "${RPM_BUILD_DIR}"
  alien --scripts -k -r -g -v "${RPM_BUILD_DIR}/${DEB_FILE_BASENAME}"
)

while IFS= read -r -d '' spec_file; do
  sed -i 's#%dir "/"##' "${spec_file}"
  sed -i 's#%dir "/usr/bin/"##' "${spec_file}"
  sed -i 's/%config/%config(noreplace)/g' "${spec_file}"
  sed -i "s/^Release: 1/Release: 1${RPM_RELEASE_SUFFIX}/" "${spec_file}"
done < <(find "${RPM_BUILD_DIR}" -maxdepth 10 -type f -name '*.spec' -print0)

SPEC_BUILD_DIR="${RPM_BUILD_DIR}/typesense-gpu-deps-${TSV}"
SPEC_FILE="${SPEC_BUILD_DIR}/typesense-gpu-deps-${TSV}-1.spec"
(
  cd "${SPEC_BUILD_DIR}"
  rpmbuild --target="${RPM_ARCH}" --buildroot "${SPEC_BUILD_DIR}" -bb "${SPEC_FILE}"
)

cp "${RPM_BUILD_DIR}/${DEB_FILE_BASENAME}" "${BAZEL_BIN_DIR}"
cp "${RPM_BUILD_DIR}/typesense-gpu-deps-${TSV}-1${RPM_RELEASE_SUFFIX}.${RPM_ARCH}.rpm" "${BAZEL_BIN_DIR}"
