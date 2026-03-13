# AGENTS.md

This file is for AI coding agents working in this repository.

## Read This First

1. `MODERNIZATION_PLAN.md` - the source of truth for modernization work, active priorities, blockers, and lessons learned.
2. `TESTING_RUNBOOK.md` - the default build/test/replay commands.
3. `benchmark/BENCHMARK_RESULTS.md` - benchmark decisions, accepted defaults, and rejected tuning ideas.
4. `bazel/PATCH_DEBT.md` - current external patch inventory and what is still worth reducing.
5. `README.md` - user-facing build context and contribution basics.

If your task changes modernization status, benchmark policy, or the canonical workflow, update the matching document in the same change.

## Repo Intent

- This fork is focused on keeping Typesense modern across Bazel, dependencies, CI, tests, and performance tooling.
- The repo is Docker-first for reproducibility.
- Prefer CI-parity flows over host-specific shortcuts.
- Avoid adding duplicate scripts, duplicate docs, or alternate entrypoints unless there is a strong reason.

## Ground Rules

- Do not treat old tribal knowledge as current truth; prefer the files listed above.
- Do not add a new "temporary" script if an existing script can be extended.
- Do not duplicate the same command matrix across multiple docs; keep one canonical location and link to it.
- Do not revert user changes you did not make.
- Do not attempt blocked work from `MODERNIZATION_PLAN.md` unless the blocker is actually resolved.

## Canonical Entry Points

- Build/test wrapper: `scripts/bazel_in_docker.sh`
- Local toolchain check: `scripts/check_local_toolchain.sh`
- Benchmark comparison: `scripts/benchmark_vs_upstream.sh`
- API test runner: `scripts/run_api_tests.sh`
- API runtime bundle prep: `api_tests/scripts/prepare_runtime_bundle.sh`
- API migration binary download: `api_tests/scripts/download_migration_binary.sh`
- C++ suite replay: `test/scripts/replay_typesense_test.sh`
- Model prewarm: `test/scripts/prewarm_e5_small_model.sh`

Before adding a new helper script, check whether one of these should become the single supported entrypoint instead.

## Default Commands

`TESTING_RUNBOOK.md` owns the canonical build/test/replay/API command matrix.

Use these short defaults unless a task explicitly needs a different lane:

- Build server: `scripts/bazel_in_docker.sh build //:typesense-server`
- Run API suite: `scripts/run_api_tests.sh -- --no-secrets --download-migration-binary`
- Replay one C++ test: `test/scripts/replay_typesense_test.sh FilterTest.FilterTreeIteratorTimeout`
- Benchmark comparison: `TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile standard --scope core`

`run_api_tests.sh` defaults to Dockerized Bun via the repo's Ubuntu-based API image so agents do not need Bun installed on the host; use `--host-bun` only as an escape hatch.

## Docs Ownership

- `MODERNIZATION_PLAN.md`: active priorities, blockers, done/not-done state, lessons learned.
- `TESTING_RUNBOOK.md`: the canonical test/build/replay workflow.
- `benchmark/BENCHMARK_RESULTS.md`: benchmark outcomes, accepted defaults, rejected experiments, hardware guidance.
- `README.md`: concise user-facing setup guidance, not the full modernization backlog.
- `benchmark/README.md`: benchmark CLI-specific usage only; keep it aligned with the actual CLI commands.

If the same command appears in multiple places, one file should be the owner and the others should mostly point to it.

## Modernization-Specific Rules

- `MODERNIZATION_PLAN.md` says agents must update it when they complete modernization work. Follow that rule.
- Keep the Priority Queue current.
- Add only short, high-value lessons learned.
- Do not copy patch-debt detail into other docs; link `bazel/PATCH_DEBT.md` instead.
- Respect known blockers, especially Protobuf 34 / brpc compatibility.

## Dependency Hygiene

- Prefer the newest stable version that is realistic for this repo.
- Prefer Bun over pnpm or npm for repo-managed JS tooling unless a tool is Bun-incompatible.
- Verify upstream compatibility before bumping major versions.
- Record why a dependency is intentionally not upgraded yet.
- For Bazel externals, check whether a patch can be removed before carrying it forward.
- Generate patch files from the exact upstream content; do not hand-write patch hunks.

## Testing Expectations

- For C++ changes, run at least the targeted test and the relevant broader suite when practical.
- For build or dependency changes, run `scripts/bazel_in_docker.sh build //:typesense-server`.
- For test infrastructure changes, use the replay helpers and CI-parity commands from `TESTING_RUNBOOK.md`.
- For benchmark-affecting changes, update `benchmark/BENCHMARK_RESULTS.md` if you are making a decision based on new numbers.

## Benchmark Policy

- Treat `benchmark/BENCHMARK_RESULTS.md` as the decision log.
- Current accepted default posture is conservative: keep broadly safe defaults and document higher-throughput overrides.
- Validate mixed read/write tradeoffs, not only pure import wins.
- Prefer `scripts/benchmark_vs_upstream.sh` for local comparison so agents do not have to reconstruct the flow.

## Known Gotchas

- glog still cannot be fully removed because brpc/braft depend on it indirectly.
- Patch droppability must be verified with a full Dockerized Bazel build, not by inspection alone.
- `max-indexing-concurrency=16` is a valid high-CPU override, not a universal default.
- Import `batch_size` is wired end-to-end and should not silently regress back to a hardcoded internal batch size.
- Tests must remain parallel-safe: isolated temp dirs, dynamic ports, no shared writable paths.

## When You Change Tooling Or Docs

- Remove or redirect obsolete scripts instead of leaving shadow entrypoints behind.
- Update any stale references in workflows, README files, and runbooks.
- Keep script help text accurate.
- Prefer one obvious command per task class: build, test, replay, benchmark.

## Good Final Deliverables

A good change in this repo usually includes:

- the code or script change,
- the matching doc update,
- verification with the canonical command,
- and a note in `MODERNIZATION_PLAN.md` when modernization status changed.
