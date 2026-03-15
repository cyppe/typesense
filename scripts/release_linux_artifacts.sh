#!/usr/bin/env bash

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${TYPESENSE_BAZEL_IMAGE:-typesense/ci-bazel:local}"
CACHE_DIR="${TYPESENSE_BAZEL_CACHE_DIR:-${HOME}/.cache/typesense/bazel-docker}"
WORKDIR="/work"
VERSION_LABEL="snapshot"
TARGET_ARCH=""
BUILD_BEFORE_ASSEMBLY=0
BUILD_LINUX_PACKAGES=1

usage() {
	cat <<'EOF'
Usage:
  scripts/release_linux_artifacts.sh [options]

Options:
  --build                    Build //:typesense-server first via scripts/bazel_in_docker.sh
  --skip-packages            Skip DEB/RPM generation
  --version-label <label>    Version label used in artifact names (default: snapshot)
  --target-arch <arch>       Target arch label (amd64 or arm64; default: host arch)
  --help                     Show this help

Examples:
  scripts/release_linux_artifacts.sh --build --version-label 0.0.0-local
  scripts/release_linux_artifacts.sh --version-label 0.0.0-b03f9a9a --target-arch amd64

Notes:
  - This is the canonical local Linux replay for release-binaries.yml.
  - It keeps release assembly container-backed so the host only needs Docker.
  - Output paths match the workflow: release/linux-<arch> and artifacts/.
EOF
}

detect_target_arch() {
	case "$(uname -m)" in
		x86_64|amd64)
			echo "amd64"
			;;
		aarch64|arm64)
			echo "arm64"
			;;
		*)
			echo "Unsupported host architecture: $(uname -m)" >&2
			exit 1
			;;
	esac
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--build)
			BUILD_BEFORE_ASSEMBLY=1
			shift
			;;
		--skip-packages)
			BUILD_LINUX_PACKAGES=0
			shift
			;;
		--version-label)
			VERSION_LABEL="${2:?missing value for --version-label}"
			shift 2
			;;
		--target-arch)
			TARGET_ARCH="${2:?missing value for --target-arch}"
			shift 2
			;;
		--help|-h)
			usage
			exit 0
			;;
		*)
			echo "Unknown option: $1" >&2
			usage >&2
			exit 1
			;;
	esac
done

if [[ -z "${TARGET_ARCH}" ]]; then
	TARGET_ARCH="$(detect_target_arch)"
fi

case "${TARGET_ARCH}" in
	amd64)
		:
		;;
	arm64)
		:
		;;
	*)
		echo "Unsupported target arch: ${TARGET_ARCH}" >&2
		exit 1
		;;
esac

if ((BUILD_BEFORE_ASSEMBLY)); then
	"${PROJECT_DIR}/scripts/bazel_in_docker.sh" build //:typesense-server
fi

if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
	"${PROJECT_DIR}/scripts/bazel_in_docker.sh" --build-image-only
fi

mkdir -p "${CACHE_DIR}"

BUILD_PACKAGES_ENV=0
if ((BUILD_LINUX_PACKAGES)); then
	BUILD_PACKAGES_ENV=1
fi

docker run \
	--rm \
	--user 0:0 \
	--entrypoint /bin/bash \
	-e "VERSION_LABEL=${VERSION_LABEL}" \
	-e "TARGET_ARCH=${TARGET_ARCH}" \
	-e "BUILD_LINUX_PACKAGES=${BUILD_PACKAGES_ENV}" \
	-e "TYPESENSE_BAZEL_CACHE_DIR=${CACHE_DIR}" \
	-e "HOST_UID=$(id -u)" \
	-e "HOST_GID=$(id -g)" \
	-v "${PROJECT_DIR}:${WORKDIR}" \
	-v "${CACHE_DIR}:${CACHE_DIR}" \
	-w "${WORKDIR}" \
	"${IMAGE}" \
	-lc '
set -euo pipefail

RELEASE_DIR="${PWD}/release/linux-${TARGET_ARCH}"
ARTIFACT_DIR="${PWD}/artifacts"
DEBUG_DIR="${ARTIFACT_DIR}/debug"
PACKAGE_DIR="${ARTIFACT_DIR}/packages"
SERVER_BINARY="${PWD}/bazel-bin/typesense-server"
ARTIFACT_BASENAME="typesense-server-${VERSION_LABEL}-linux-${TARGET_ARCH}"
DEBUG_BASENAME="${ARTIFACT_BASENAME}.debug"
TARBALL="${ARTIFACT_DIR}/${ARTIFACT_BASENAME}.tar.gz"
DEBUG_TARBALL="${ARTIFACT_DIR}/${DEBUG_BASENAME}.tar.gz"

if [[ ! -f "${SERVER_BINARY}" ]]; then
	echo "typesense-server binary not found at ${SERVER_BINARY}" >&2
	echo "Build it first with scripts/bazel_in_docker.sh build //:typesense-server or pass --build." >&2
	exit 1
fi

if [[ "${BUILD_LINUX_PACKAGES}" == "1" ]]; then
	apt-get update >/tmp/release-linux-artifacts.apt-update.log
	apt-get install -y --no-install-recommends alien rpm dpkg-dev >/tmp/release-linux-artifacts.apt-install.log
fi

rm -rf "${RELEASE_DIR}"
mkdir -p "${RELEASE_DIR}" "${DEBUG_DIR}" "${PACKAGE_DIR}"
rm -f "${TARBALL}" "${TARBALL}.sha256.txt" "${DEBUG_TARBALL}" "${DEBUG_TARBALL}.sha256.txt"

if ldd "${SERVER_BINARY}" | grep -q "libonnxruntime\\.so\\.1"; then
	echo "typesense-server unexpectedly depends on libonnxruntime.so.1" >&2
	exit 1
fi

bash api_tests/scripts/prepare_runtime_bundle.sh "${RELEASE_DIR}" "${SERVER_BINARY}"

chmod u+w "${RELEASE_DIR}/typesense-server"
objcopy --only-keep-debug "${RELEASE_DIR}/typesense-server" "${DEBUG_DIR}/${DEBUG_BASENAME}"
chmod u+w "${DEBUG_DIR}/${DEBUG_BASENAME}"
strip --strip-debug "${RELEASE_DIR}/typesense-server"
objcopy --add-gnu-debuglink="${DEBUG_DIR}/${DEBUG_BASENAME}" "${RELEASE_DIR}/typesense-server"

python3 - <<PY
import hashlib
from pathlib import Path

release_dir = Path(r"${RELEASE_DIR}")
binary = release_dir / "typesense-server"
(release_dir / "typesense-server.md5.txt").write_text(hashlib.md5(binary.read_bytes()).hexdigest() + "\n")
PY

HELP_OUTPUT="${RELEASE_DIR}/typesense-server-help.txt"
set +e
"${RELEASE_DIR}/typesense-server" --help >"${HELP_OUTPUT}" 2>&1
STATUS=$?
set -e
cat "${HELP_OUTPUT}"
if [[ "${STATUS}" -ne 0 && "${STATUS}" -ne 1 ]]; then
	exit "${STATUS}"
fi
grep -Eq "^usage:|^Command line usage:" "${HELP_OUTPUT}"

tar -czf "${TARBALL}" -C "${RELEASE_DIR}" .
python3 - <<PY
import hashlib
from pathlib import Path

archive = Path(r"${TARBALL}")
Path(str(archive) + ".sha256.txt").write_text(f"{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n")
PY

tar -czf "${DEBUG_TARBALL}" -C "${DEBUG_DIR}" "${DEBUG_BASENAME}"
python3 - <<PY
import hashlib
from pathlib import Path

archive = Path(r"${DEBUG_TARBALL}")
Path(str(archive) + ".sha256.txt").write_text(f"{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n")
PY

python3 - <<PY
import hashlib
import tarfile
from pathlib import Path

archive = Path(r"${TARBALL}")
with tarfile.open(archive, "r:gz") as tf:
    members = {member.name.lstrip("./"): member for member in tf.getmembers() if member.isfile()}
    if "typesense-server" not in members:
        raise SystemExit("typesense-server missing from packaged artifact")
    if "typesense-server.md5.txt" not in members:
        raise SystemExit("typesense-server.md5.txt missing from packaged artifact")
    binary_bytes = tf.extractfile(members["typesense-server"]).read()
    md5_bytes = tf.extractfile(members["typesense-server.md5.txt"]).read().decode().strip()
    digest = hashlib.md5(binary_bytes).hexdigest()
    if digest != md5_bytes:
        raise SystemExit(f"binary md5 mismatch: expected {md5_bytes}, got {digest}")
PY

tar -tzf "${DEBUG_TARBALL}" | grep -Fx "${DEBUG_BASENAME}"
readelf -S "${RELEASE_DIR}/typesense-server" | grep -q "\\.gnu_debuglink"

if [[ "${BUILD_LINUX_PACKAGES}" == "1" ]]; then
	TSV="${VERSION_LABEL}" \
	ARCH="${TARGET_ARCH}" \
	RELEASE_ARTIFACT_DIR="${ARTIFACT_DIR}" \
	RELEASE_PACKAGE_DIR="${PACKAGE_DIR}" \
	bash debian-pkg/generate_deb_rpm.sh
fi

chown -R "${HOST_UID}:${HOST_GID}" "${RELEASE_DIR}" "${ARTIFACT_DIR}"

ls -lh "${TARBALL}" "${TARBALL}.sha256.txt" "${DEBUG_TARBALL}" "${DEBUG_TARBALL}.sha256.txt"
if [[ "${BUILD_LINUX_PACKAGES}" == "1" ]]; then
	find "${PACKAGE_DIR}" -maxdepth 1 -type f \( -name "typesense-server-${VERSION_LABEL}-*.deb" -o -name "typesense-server-*.rpm" \) -print0 | xargs -0 ls -lh
fi
'
