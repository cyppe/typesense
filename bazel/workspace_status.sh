#!/usr/bin/env bash

set -euo pipefail

workspace_dir="${BUILD_WORKSPACE_DIRECTORY:-$(pwd)}"
cd "${workspace_dir}"

# Bazel builds run inside the repo-mounted CI container, where git can reject
# the workspace as an unsafe directory because the UID differs from the host.
# Mark this workspace safe so provenance stays available in Dockerized builds.
git config --global --add safe.directory "${workspace_dir}" >/dev/null 2>&1 || true

git_sha="unknown"
git_short_sha="unknown"
git_ref="(detached)"
git_exact_tag="(none)"
git_tree_status="unknown"

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git_sha="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
    git_short_sha="$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"

    git_ref="${TYPESENSE_BUILD_SOURCE_REF:-}"
    if [[ -z "${git_ref}" ]]; then
        git_ref="$(git symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
    fi
    if [[ -z "${git_ref}" ]]; then
        git_ref="$(git describe --all --exact-match HEAD 2>/dev/null || true)"
    fi
    if [[ -z "${git_ref}" ]]; then
        git_ref="(detached)"
    fi

    git_exact_tag="$(git describe --tags --exact-match HEAD 2>/dev/null || true)"
    if [[ -z "${git_exact_tag}" ]]; then
        git_exact_tag="(none)"
    fi

    git update-index -q --refresh >/dev/null 2>&1 || true
    if git diff-index --quiet HEAD -- >/dev/null 2>&1; then
        git_tree_status="clean"
    else
        git_tree_status="dirty"
    fi
fi

printf 'STABLE_TYPESENSE_GIT_SHA %s\n' "${git_sha}"
printf 'STABLE_TYPESENSE_GIT_SHORT_SHA %s\n' "${git_short_sha}"
printf 'STABLE_TYPESENSE_GIT_REF %s\n' "${git_ref}"
printf 'STABLE_TYPESENSE_GIT_EXACT_TAG %s\n' "${git_exact_tag}"
printf 'STABLE_TYPESENSE_GIT_TREE_STATUS %s\n' "${git_tree_status}"
