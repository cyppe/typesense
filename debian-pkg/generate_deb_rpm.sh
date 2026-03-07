#!/bin/bash

# TSV is passed as an environment variable to the script

if [ -z "${TSV:-}" ]; then
	echo '$TSV is not provided. Quitting.'
	exit 1
fi

if [ -z "${ARCH:-}" ]; then
	echo '$ARCH is not provided. Quitting.'
	exit 1
fi

ARTIFACT_SUFFIX="${ARTIFACT_SUFFIX:-}"

RPM_ARCH=$ARCH
if [ "$ARCH" == "amd64" ]; then
	RPM_ARCH="x86_64"
elif [ "$ARCH" == "arm64" ]; then
	RPM_ARCH="aarch64"
fi

set -ex
CURR_DIR=$(dirname "$0" | while read -r a; do cd "$a" && pwd && break; done)
ARTIFACT_BASENAME="typesense-server-${TSV}-linux-${ARCH}${ARTIFACT_SUFFIX}.tar.gz"
RELEASE_PACKAGE_DIR="${RELEASE_PACKAGE_DIR:-${CURR_DIR}/../artifacts/packages}"
mkdir -p "${RELEASE_PACKAGE_DIR}"

if [ -n "${RELEASE_TARBALL_PATH:-}" ]; then
	RELEASE_TARBALL="${RELEASE_TARBALL_PATH}"
else
	RELEASE_TARBALL=""
	for candidate_dir in "${RELEASE_ARTIFACT_DIR:-${CURR_DIR}/../artifacts}" "${CURR_DIR}/../bazel-bin"; do
		candidate_path="${candidate_dir}/${ARTIFACT_BASENAME}"
		if [ -f "${candidate_path}" ]; then
			RELEASE_TARBALL="${candidate_path}"
			break
		fi
	done
fi

if [ -z "${RELEASE_TARBALL}" ]; then
	echo "Release tarball not found for ${ARTIFACT_BASENAME}." >&2
	echo "Set RELEASE_TARBALL_PATH or RELEASE_ARTIFACT_DIR to point at the built release artifact." >&2
	exit 1
fi

RELEASE_SHA256_PATH="${RELEASE_SHA256_PATH:-${RELEASE_TARBALL}.sha256.txt}"
if [ -f "${RELEASE_SHA256_PATH}" ]; then
	expected_sha256=$(awk '{print $1}' "${RELEASE_SHA256_PATH}")
	actual_sha256=$(sha256sum "${RELEASE_TARBALL}" | cut -d' ' -f1)
	if [ "${expected_sha256}" != "${actual_sha256}" ]; then
		echo "Release tarball checksum mismatch for ${RELEASE_TARBALL}." >&2
		exit 1
	fi
fi

rm -rf /tmp/typesense-deb-build && mkdir /tmp/typesense-deb-build
cp -r $CURR_DIR/typesense-server /tmp/typesense-deb-build

# Download Typesense, extract and make it executable

#curl -o /tmp/typesense-server-$TSV.tar.gz https://dl.typesense.org/releases/$TSV/typesense-server-$TSV-linux-${ARCH}.tar.gz
rm -rf /tmp/typesense-server-$TSV && mkdir /tmp/typesense-server-$TSV
tar -xzf "${RELEASE_TARBALL}" -C /tmp/typesense-server-$TSV

downloaded_hash=$(md5sum /tmp/typesense-server-$TSV/typesense-server | cut -d' ' -f1)
original_hash=$(cat /tmp/typesense-server-$TSV/typesense-server.md5.txt)

if [ "$downloaded_hash" == "$original_hash" ]; then
	mkdir -p /tmp/typesense-deb-build/typesense-server/usr/bin
	cp /tmp/typesense-server-$TSV/typesense-server /tmp/typesense-deb-build/typesense-server/usr/bin
else
	>&2 echo "Typesense server binary is corrupted. Quitting."
	exit 1
fi

rm -rf /tmp/typesense-server-$TSV /tmp/typesense-server-$TSV.tar.gz

sed -i "s/\$VERSION/$TSV/g" $(find /tmp/typesense-deb-build -maxdepth 10 -type f)
sed -i "s/\$ARCH/$ARCH/g" $(find /tmp/typesense-deb-build -maxdepth 10 -type f)

dpkg-deb -Zgzip -z6 \
	-b /tmp/typesense-deb-build/typesense-server "/tmp/typesense-deb-build/typesense-server-${TSV}-${ARCH}${ARTIFACT_SUFFIX}.deb"

# Generate RPM

rm -rf /tmp/typesense-rpm-build && mkdir /tmp/typesense-rpm-build
cp "/tmp/typesense-deb-build/typesense-server-${TSV}-${ARCH}${ARTIFACT_SUFFIX}.deb" /tmp/typesense-rpm-build
cd /tmp/typesense-rpm-build && alien --scripts -k -r -g -v /tmp/typesense-rpm-build/typesense-server-${TSV}-${ARCH}${ARTIFACT_SUFFIX}.deb

sed -i 's#%dir "/"##' $(find /tmp/typesense-rpm-build/*/*.spec -maxdepth 10 -type f)
sed -i 's#%dir "/usr/bin/"##' $(find /tmp/typesense-rpm-build/*/*.spec -maxdepth 10 -type f)
sed -i 's/%config/%config(noreplace)/g' $(find /tmp/typesense-rpm-build/*/*.spec -maxdepth 10 -type f)
sed -i "s/^Release: 1/Release: 1${ARTIFACT_SUFFIX//-/.}/" $(find /tmp/typesense-rpm-build/*/*.spec -maxdepth 10 -type f)

SPEC_FILE=$(find /tmp/typesense-rpm-build -maxdepth 3 -type f -name '*.spec' | head -n 1)
if [ -z "${SPEC_FILE}" ]; then
	>&2 echo "Unable to locate generated RPM spec file."
	exit 1
fi
SPEC_ROOT=$(dirname "$SPEC_FILE")
RPM_BUILDROOT=$(mktemp -d /tmp/typesense-rpm-build/buildroot.XXXXXX)
cp -a "${SPEC_ROOT}/." "${RPM_BUILDROOT}/"
find "${RPM_BUILDROOT}" -maxdepth 1 -type f -name '*.spec' -delete
SPEC_FILE_COPY="${SPEC_FILE%.spec}-copy.spec"

cp $SPEC_FILE $SPEC_FILE_COPY

PRE_LINE=$(grep -n "%pre" $SPEC_FILE_COPY | cut -f1 -d: || true)
if [ -n "${PRE_LINE}" ]; then
	START_LINE=$(expr $PRE_LINE - 1)
	head -$START_LINE $SPEC_FILE_COPY >$SPEC_FILE
else
	cp $SPEC_FILE_COPY $SPEC_FILE
fi

echo "%prep" >>$SPEC_FILE
echo "cat >/tmp/find_requires.sh <<EOF
#!/bin/sh
%{__find_requires} | grep -v GLIBC_PRIVATE
exit 0
EOF" >>$SPEC_FILE

echo "chmod +x /tmp/find_requires.sh" >>$SPEC_FILE
echo "%define _use_internal_dependency_generator 0" >>$SPEC_FILE
echo "%define __find_requires /tmp/find_requires.sh" >>$SPEC_FILE

if [ -n "${PRE_LINE}" ]; then
	tail -n+$START_LINE $SPEC_FILE_COPY >>$SPEC_FILE
fi

rm $SPEC_FILE_COPY

cd "${SPEC_ROOT}" &&
	rpmbuild --target=${RPM_ARCH} --buildroot "${RPM_BUILDROOT}" -bb \
		$SPEC_FILE

cp "/tmp/typesense-deb-build/typesense-server-${TSV}-${ARCH}${ARTIFACT_SUFFIX}.deb" "${RELEASE_PACKAGE_DIR}"
GENERATED_RPM=$(find /tmp/typesense-rpm-build /root/rpmbuild/RPMS -type f -name '*.rpm' 2>/dev/null | head -n 1)
if [ -z "${GENERATED_RPM}" ]; then
	>&2 echo "Unable to locate generated RPM artifact."
	exit 1
fi
cp "${GENERATED_RPM}" "${RELEASE_PACKAGE_DIR}"
