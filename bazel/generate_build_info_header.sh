#!/usr/bin/env bash

set -euo pipefail

status_file="${1:?missing stable status file}"
output_path="${2:?missing output path}"

declare -A values=(
    [STABLE_TYPESENSE_GIT_SHA]="unknown"
    [STABLE_TYPESENSE_GIT_SHORT_SHA]="unknown"
    [STABLE_TYPESENSE_GIT_REF]="(detached)"
    [STABLE_TYPESENSE_GIT_EXACT_TAG]="(none)"
    [STABLE_TYPESENSE_GIT_TREE_STATUS]="unknown"
)

while IFS=' ' read -r key value; do
    if [[ -n "${key}" ]]; then
        values["${key}"]="${value}"
    fi
done < "${status_file}"

escape_c_string() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    printf '%s' "${value}"
}

cat >"${output_path}" <<EOF
#pragma once

#define TYPESENSE_BUILD_GIT_SHA "$(escape_c_string "${values[STABLE_TYPESENSE_GIT_SHA]}")"
#define TYPESENSE_BUILD_GIT_SHORT_SHA "$(escape_c_string "${values[STABLE_TYPESENSE_GIT_SHORT_SHA]}")"
#define TYPESENSE_BUILD_GIT_REF "$(escape_c_string "${values[STABLE_TYPESENSE_GIT_REF]}")"
#define TYPESENSE_BUILD_GIT_EXACT_TAG "$(escape_c_string "${values[STABLE_TYPESENSE_GIT_EXACT_TAG]}")"
#define TYPESENSE_BUILD_GIT_TREE_STATUS "$(escape_c_string "${values[STABLE_TYPESENSE_GIT_TREE_STATUS]}")"
EOF
