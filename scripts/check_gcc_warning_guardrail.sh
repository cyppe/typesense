#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_FILE="${TYPESENSE_GCC_WARNING_LOG:-${ROOT_DIR}/tmp/gcc-warning-guardrail.log}"
TOTAL_WARNING_MAX="${TYPESENSE_GCC_WARNING_MAX:-0}"
TRACKED_WARNING_MAX="${TYPESENSE_GCC_TRACKED_WARNING_MAX:-0}"

mkdir -p "$(dirname "${LOG_FILE}")"

echo "Running GCC warning guardrail build..."
bazel_args=(build //:typesense-server)
if [[ -n "${TYPESENSE_ORT_PREBUILT_BUNDLE_DIR:-}" ]]; then
	bazel_args+=("--repo_env=TYPESENSE_ORT_PREBUILT_BUNDLE_DIR=${TYPESENSE_ORT_PREBUILT_BUNDLE_DIR}")
fi

"${ROOT_DIR}/scripts/bazel_in_docker.sh" "${bazel_args[@]}" \
	2>&1 | tee "${LOG_FILE}" | python3 -c 'import sys; ignore="OpenJDK 64-Bit Server VM warning: Options -Xverify:none and -noverify were deprecated in JDK 13 and will likely be removed in a future release."; [sys.stdout.write(line) for line in sys.stdin if line.rstrip("\n") != ignore]'

read -r TOTAL_WARNINGS TRACKED_WARNINGS < <(
	python3 - "${LOG_FILE}" <<'PY'
import pathlib
import re
import sys

log_path = pathlib.Path(sys.argv[1])
text = log_path.read_text(encoding="utf-8", errors="replace")

ignored_warning_lines = {
    "OpenJDK 64-Bit Server VM warning: Options -Xverify:none and -noverify were deprecated in JDK 13 and will likely be removed in a future release.",
}

warning_lines = [
    line for line in text.splitlines()
    if "warning:" in line and line.strip() not in ignored_warning_lines
]

tracked = len(re.findall(r"^(src|include|test|api_tests)/[^:\n]+:\d+:\d+: warning:", text, re.MULTILINE))

print(len(warning_lines), tracked)
PY
)

echo "GCC warning summary: total=${TOTAL_WARNINGS}, tracked=${TRACKED_WARNINGS}"
echo "Guardrail thresholds: total<=${TOTAL_WARNING_MAX}, tracked<=${TRACKED_WARNING_MAX}"

if ((TOTAL_WARNINGS > TOTAL_WARNING_MAX)); then
	echo "ERROR: Total GCC warnings exceeded budget (${TOTAL_WARNINGS} > ${TOTAL_WARNING_MAX})."
	echo "See ${LOG_FILE} for details."
	exit 1
fi

if ((TRACKED_WARNINGS > TRACKED_WARNING_MAX)); then
	echo "ERROR: Tracked GCC warnings exceeded budget (${TRACKED_WARNINGS} > ${TRACKED_WARNING_MAX})."
	echo "See ${LOG_FILE} for details."
	exit 1
fi

echo "GCC warning guardrail passed."
