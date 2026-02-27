#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_FILE="${TYPESENSE_CLANG_WARNING_LOG:-${ROOT_DIR}/tmp/clang-warning-guardrail.log}"
TOTAL_WARNING_MAX="${TYPESENSE_CLANG_WARNING_MAX:-0}"
TRACKED_WARNING_MAX="${TYPESENSE_CLANG_TRACKED_WARNING_MAX:-0}"
SPARSEPP_WARNING_MAX="${TYPESENSE_CLANG_SPARSEPP_WARNING_MAX:-0}"

mkdir -p "$(dirname "${LOG_FILE}")"

echo "Running clang warning guardrail build..."
"${ROOT_DIR}/scripts/bazel_in_docker.sh" build //:typesense-server \
	--repo_env=CC=clang \
	--repo_env=CXX=clang++ \
	--cxxopt=-Wno-enum-constexpr-conversion \
	--cxxopt=-Wno-unused-command-line-argument \
	--linkopt=-Wno-unused-command-line-argument \
	--per_file_copt=.*com_github_brpc_brpc.*@-Wno-deprecated-declarations \
	--per_file_copt=.*com_github_brpc_braft.*@-Wno-deprecated-declarations \
	--per_file_copt=.*com_github_brpc_brpc.*@-Wno-vla-cxx-extension \
	--per_file_copt=.*com_github_brpc_braft.*@-Wno-vla-cxx-extension \
	--per_file_copt=.*com_github_brpc_brpc.*@-Wno-macro-redefined \
	--per_file_copt=.*com_github_brpc_braft.*@-Wno-macro-redefined \
	--per_file_copt=.*com_github_brpc_brpc.*@-Wno-invalid-offsetof \
	--per_file_copt=.*com_github_brpc_braft.*@-Wno-invalid-offsetof \
	--per_file_copt=.*onnx_runtime_extensions.*@-Wno-pessimizing-move \
	--per_file_copt=.*clip_tokenizer.*@-Wno-pessimizing-move \
	--per_file_copt=.*clip_tokenizer.*@-Wno-unused-variable \
	--per_file_copt='.*glog.*@-Wno-#pragma-messages' \
	--per_file_copt=.*quicly.*@-Wno-unused-but-set-variable \
	2>&1 | tee "${LOG_FILE}" | python3 -c 'import sys; ignore="OpenJDK 64-Bit Server VM warning: Options -Xverify:none and -noverify were deprecated in JDK 13 and will likely be removed in a future release."; [sys.stdout.write(line) for line in sys.stdin if line.rstrip("\n") != ignore]'

read -r TOTAL_WARNINGS TRACKED_WARNINGS SPARSEPP_WARNINGS < <(
	python3 - "${LOG_FILE}" <<'PY'
import pathlib
import re
import sys

log_path = pathlib.Path(sys.argv[1])
text = log_path.read_text(encoding="utf-8", errors="replace")

ignored_warning_lines = {
    "OpenJDK 64-Bit Server VM warning: Options -Xverify:none and -noverify were deprecated in JDK 13 and will likely be removed in a future release.",
}
total = sum(
    1
    for line in text.splitlines()
    if "warning:" in line and line.strip() not in ignored_warning_lines
)
tracked = len(re.findall(r"^(src|include|test|api_tests)/[^:\n]+:\d+:\d+: warning:", text, re.MULTILINE))
sparsepp = len(re.findall(r"^include/sparsepp\.h:\d+:\d+: warning:", text, re.MULTILINE))

print(total, tracked, sparsepp)
PY
)

echo "Clang warning summary: total=${TOTAL_WARNINGS}, tracked=${TRACKED_WARNINGS}, sparsepp=${SPARSEPP_WARNINGS}"
echo "Guardrail thresholds: total<=${TOTAL_WARNING_MAX}, tracked<=${TRACKED_WARNING_MAX}, sparsepp<=${SPARSEPP_WARNING_MAX}"

if ((TOTAL_WARNINGS > TOTAL_WARNING_MAX)); then
	echo "ERROR: Total clang warnings exceeded budget (${TOTAL_WARNINGS} > ${TOTAL_WARNING_MAX})."
	echo "See ${LOG_FILE} for details."
	exit 1
fi

if ((TRACKED_WARNINGS > TRACKED_WARNING_MAX)); then
	echo "ERROR: Tracked clang warnings exceeded budget (${TRACKED_WARNINGS} > ${TRACKED_WARNING_MAX})."
	echo "See ${LOG_FILE} for details."
	exit 1
fi

if ((SPARSEPP_WARNINGS > SPARSEPP_WARNING_MAX)); then
	echo "ERROR: sparsepp clang warnings exceeded budget (${SPARSEPP_WARNINGS} > ${SPARSEPP_WARNING_MAX})."
	echo "See ${LOG_FILE} for details."
	exit 1
fi

echo "Clang warning guardrail passed."
