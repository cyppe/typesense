# Typesense Modernization Program (Bazel 9 + Modern C++)

## Goals

- Keep the project on a current, supportable toolchain and dependency stack.
- Make Bazel 9.0 migration low-risk and repeatable.
- Reduce warning noise so real regressions are visible.
- Modernize C++ usage where it improves correctness, maintainability, and performance.
- Keep release safety high while changing internals aggressively.

## Current Baseline

- Bzlmod-first build on Bazel 8.6.0 is working.
- `bazel build //...` succeeds in the builder container.
- Repository rules are now pinned to explicit commits in `MODULE.bazel`.
- Test compilation issues from stricter JSON comparisons were fixed.

## Program Structure

Use 2-week sprints. Each sprint has stories with clear acceptance criteria.

---

## Sprint 1: Reproducibility and Build Hygiene

### Story S1-1: Fully deterministic dependency graph
- Ensure every non-BCR dependency is commit-pinned and hash-pinned where possible.
- Add a script/check that fails CI if `branch =` or `tag =` is used in `MODULE.bazel`.
- Acceptance: No floating refs; CI gate exists.

### Story S1-2: Lockfile stability policy
- Document when `MODULE.bazel.lock` can change and how to regenerate it.
- Add CI check for unexpected lockfile drift.
- Acceptance: predictable lockfile updates only.

### Story S1-3: Build profile matrix
- Add fast/opt/asan/ubsan build configs to `.bazelrc`.
- Ensure all profiles at least analyze successfully.
- Acceptance: profile matrix documented and runnable.

---

## Sprint 2: Bazel 9 Readiness

### Story S2-1: Bazel 9 dry-run lane
- Add non-blocking CI job for Bazel 9 (analysis first, then selected builds/tests).
- Track failures in a dedicated issue list.
- Acceptance: green analysis lane on Bazel 9 for core targets.

### Story S2-2: rules/protobuf compatibility pass
- Upgrade protobuf toward 33.x and align rules that depend on it.
- Remove compatibility patching where upstream fixes exist.
- Acceptance: core build/test targets pass with upgraded protobuf.

### Story S2-3: rules_cc and transitive rule updates
- Update rules_cc and related modules to Bazel 9-friendly versions.
- Replace deprecated APIs/labels triggered by Bazel warnings.
- Acceptance: no Bazel 9 blocker warnings in main build path.

---

## Sprint 3: Warning Debt Burn-Down (High Value)

### Story S3-1: Warning inventory and budget
- Collect warning counts by category/file from clean builds.
- Establish a warning budget and per-sprint reduction goal.
- Acceptance: warning dashboard and baseline committed.

### Story S3-2: Remove truly unused code/variables
- Delete dead locals, stale branches, and unreferenced helpers.
- For intentionally retained placeholders, mark with `[[maybe_unused]]` and a brief reason.
- Acceptance: warning count reduced significantly; no behavior changes.

### Story S3-3: Tighten compiler signal quality
- Enable stricter warnings where practical (especially in new/changed code).
- Keep `-Werror` scoped to avoid blocking legacy cleanup.
- Acceptance: changed code cannot introduce new warning classes.

Guideline for intentional unused artifacts:
- Keep only when there is a concrete operational/debug reason.
- Add one-line comment with trigger/usage context.
- Prefer `[[maybe_unused]]` over blanket disable flags.

---

## Sprint 4: C++ Modernization Pass

### Story S4-1: API surface cleanup
- Prefer `std::string_view`, `std::span`, and `enum class` in hot/shared APIs.
- Eliminate implicit narrowing and ambiguous conversions.
- Acceptance: targeted modules migrated with no perf regressions.

### Story S4-2: Safer ownership and error handling
- Reduce raw ownership where practical; use RAII/smart pointers consistently.
- Standardize error propagation patterns (Option/Status style consistency).
- Acceptance: fewer lifetime edge cases and clearer contracts.

### Story S4-3: Modern language feature adoption policy
- Decide C++20 baseline + selective C++23 features (where compiler support is stable).
- Document approved feature set for contributors.
- Acceptance: coding standard update merged.

---

## Sprint 5: Dependency Currency and Security

### Story S5-1: Library update wave
- Update core libraries (openssl, protobuf, onnxruntime, sentencepiece, etc.) to current supported versions.
- Track ABI/behavior risk per dependency.
- Acceptance: all updates validated by build + core tests.

### Story S5-2: CVE and license automation
- Add dependency scanning and license policy checks in CI.
- Introduce SLA for high/critical CVEs.
- Acceptance: automated report on every PR/main build.

---

## Sprint 6: Test System Modernization

### Story S6-1: Split monolithic test target
- Break `typesense-test` into logical suites (collection, join, curation, vector, etc.).
- Enable shardable, parallel CI runs.
- Acceptance: faster and more diagnosable CI tests.

### Story S6-2: Sanitizer test lanes
- Add ASAN/UBSAN test lanes for selected suites.
- Keep flaky/slow tests isolated and tagged.
- Acceptance: sanitizer lanes stable and actionable.

### Story S6-3: Deterministic test data and fixtures
- Remove hidden test-order dependencies and timing assumptions.
- Acceptance: repeatable test results across local/CI.

---

## Sprint 7: Tooling and Static Analysis

### Story S7-1: clang-tidy integration
- Add curated clang-tidy checks for changed files in CI.
- Provide auto-fix workflow for common findings.
- Acceptance: tidy warnings trend downward per sprint.

### Story S7-2: include hygiene
- Run include cleanup (IWYU-lite strategy) on targeted modules.
- Reduce transitive include dependence.
- Acceptance: improved compile times and fewer hidden dependencies.

### Story S7-3: Developer workflow polish
- Add scripts for one-command local verification (`build`, `test`, `sanitize`).
- Acceptance: onboarding and contributor loop shortened.

---

## Sprint 8: Performance and Release Hardening

### Story S8-1: Compiler/link optimization strategy
- Evaluate LTO/ThinLTO and optional PGO for release binaries.
- Acceptance: measurable perf/size improvements with safe rollback.

### Story S8-2: Build and runtime observability
- Add build-time metrics and runtime smoke benchmarks to release pipeline.
- Acceptance: regression detection before release.

### Story S8-3: Release gate definition
- Define required checks (build matrix, core tests, sanitizer subset, CVE scan).
- Acceptance: reproducible release checklist in repo.

---

## Cross-Sprint Operating Rules

- Do not add new floating dependency refs.
- New code should not increase warning count.
- Prefer upstream fixes over long-term local patching.
- Keep all modernization changes measurable (before/after metrics).
- Every sprint closes with:
  - build status snapshot,
  - warning delta,
  - dependency delta,
  - risk log updates.

## Suggested Metrics Dashboard

- Bazel build success by profile (`opt`, `asan`, `ubsan`, `test`).
- Warning count by type/file.
- Dependency freshness (latest, n-1, older).
- CI duration and flaky test rate.
- Binary size and key query latency benchmarks.

## Immediate Next Actions (Recommended)

1. Start Sprint 1 CI guardrails (`no branch/tag refs`, lockfile policy).
2. Open Bazel 9 dry-run lane and track blockers in one issue epic.
3. Begin warning inventory and remove low-risk unused variables in core/test code.
4. Plan protobuf 33.x upgrade as a dedicated short-lived branch with compatibility checklist.
