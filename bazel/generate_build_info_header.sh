#!/usr/bin/env bash

set -euo pipefail

status_file="${1:?missing stable status file}"
output_path="${2:?missing output path}"

git_sha="unknown"
git_short_sha="unknown"
git_ref="(detached)"
git_exact_tag="(none)"
git_tree_status="unknown"

while IFS=' ' read -r key value; do
    case "${key}" in
        STABLE_TYPESENSE_GIT_SHA)
            git_sha="${value}"
            ;;
        STABLE_TYPESENSE_GIT_SHORT_SHA)
            git_short_sha="${value}"
            ;;
        STABLE_TYPESENSE_GIT_REF)
            git_ref="${value}"
            ;;
        STABLE_TYPESENSE_GIT_EXACT_TAG)
            git_exact_tag="${value}"
            ;;
        STABLE_TYPESENSE_GIT_TREE_STATUS)
            git_tree_status="${value}"
            ;;
    esac
done < "${status_file}"

escape_c_string() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    printf '%s' "${value}"
}

cat >"${output_path}" <<EOF
#pragma once

#define TYPESENSE_BUILD_GIT_SHA "$(escape_c_string "${git_sha}")"
#define TYPESENSE_BUILD_GIT_SHORT_SHA "$(escape_c_string "${git_short_sha}")"
#define TYPESENSE_BUILD_GIT_REF "$(escape_c_string "${git_ref}")"
#define TYPESENSE_BUILD_GIT_EXACT_TAG "$(escape_c_string "${git_exact_tag}")"
#define TYPESENSE_BUILD_GIT_TREE_STATUS "$(escape_c_string "${git_tree_status}")"
EOF
