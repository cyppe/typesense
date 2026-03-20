# GitHub Workflow Guide

This file explains what each GitHub Actions workflow in this repo is for and when to run it.
It is intentionally short. `TESTING_RUNBOOK.md` remains the owner for local replay commands and `MODERNIZATION_PLAN.md` remains the owner for workflow policy/state.

## Workflow Summary

### `tests.yml`

- Trigger: automatic on `push`, optional manual rerun.
- Purpose: primary gate for routine development.
- Coverage:
  - Dockerized Bazel server build
  - GCC warning guardrail
  - full `//:typesense-test`
  - API tests
  - TEI integration lane
- Run it:
  - always on normal branch updates
  - manually when you want the standard hosted confirmation for a SHA

### `clang-warning-guard.yml`

- Trigger: manual only.
- Purpose: clang-specific warning-budget enforcement without slowing every push.
- Coverage:
  - Dockerized clang build of `//:typesense-server`
  - clang warning log artifact on failure
- Run it:
  - after `.bazelrc` compiler-flag changes
  - after `scripts/bazel_in_docker.sh` or CI toolchain image changes
  - after Bazel external `BUILD` / patch changes
  - after warning-sensitive C++ changes where clang and GCC often differ

### `flake-detection.yml`

- Trigger: manual only.
- Purpose: targeted flake hunting for known-risk tests.
- Coverage:
  - 20x reruns of historically flaky C++ targets
  - 10x replay of the API documents lane
  - failure logs/artifacts for reproduction
- Run it:
  - after changing test harness plumbing
  - after touching known flaky areas
  - before merging a fix that claims to remove flakiness

### `sanitizer-testing.yml`

- Trigger: manual only.
- Purpose: memory/race validation beyond the normal push gate.
- Coverage:
  - ASAN/UBSAN full C++ suite
  - TSAN full C++ suite
- Run it:
  - after concurrency, lifetime, allocator, ownership, or low-level runtime changes
  - after dependency or toolchain changes that may affect sanitizer behavior

### `nightly-extended.yml`

- Trigger: manual only.
- Purpose: long-running confidence lane for endurance and repeatability.
- Coverage:
  - full C++ suite with `--runs_per_test=5`
  - full no-secrets API suite for 3 iterations
- Run it:
  - before high-confidence release decisions
  - after broad runtime refactors
  - when you need stronger “still stable under repetition” evidence than `tests.yml`

### `benchmark-testing.yml`

- Trigger: manual only.
- Purpose: same-branch performance guardrail.
- Coverage:
  - compares the latest successful `tests.yml` artifact on the branch against the previous successful `tests.yml` artifact on the same branch
  - persists benchmark Influx data between runs
- Run it:
  - after performance-sensitive runtime/indexing/search changes
  - when updating benchmark policy or documenting new benchmark decisions
- Do not use it:
  - as the upstream-vs-fork canonical comparison; use the documented local benchmark wrapper for that

### `ort-bundles.yml`

- Trigger: manual only.
- Purpose: prebuild reusable Linux CUDA-enabled ORT bundles.
- Coverage:
  - `linux-amd64`
  - `linux-arm64`
- Run it:
  - before `release-binaries.yml` when you want to avoid cold ORT rebuilds
  - before heavy hosted `tests.yml` validation on a branch that changed ORT/toolchain inputs

### `release-binaries.yml`

- Trigger: manual only.
- Purpose: full release artifact production and optional Docker publishing.
- Coverage:
  - Linux release tarballs/packages
  - Darwin release tarballs
  - Linux GPU deps artifacts
  - optional Docker Hub publish from validated Linux artifacts
- Run it:
  - for release proofs
  - after packaging or release-tooling changes
  - after `ort-bundles.yml` if you want the faster hosted Linux path

## Audit Verdict

The current workflow set is justified. The workflows are not duplicates; they answer different questions:

- `tests.yml`: “does this SHA pass the normal gate?”
- `clang-warning-guard.yml`: “is the clang warning surface still clean?”
- `flake-detection.yml`: “did we reintroduce flakiness in known-risk targets?”
- `sanitizer-testing.yml`: “did we break memory/race safety?”
- `nightly-extended.yml`: “does it stay stable under repetition and longer runtime?”
- `benchmark-testing.yml`: “did same-branch performance regress?”
- `ort-bundles.yml`: “can we pre-stage the heavy CUDA ORT dependency?”
- `release-binaries.yml`: “can we build/publish the real release outputs?”

The only mild overlap is `flake-detection.yml` vs `nightly-extended.yml`, but it is useful overlap:

- `flake-detection.yml` is faster and narrower for known flaky targets.
- `nightly-extended.yml` is broader and slower for higher-confidence endurance checks.
