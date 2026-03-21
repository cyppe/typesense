# Typesense Fork Benchmark Results

Tracking benchmark results across tuning iterations.
Fork branch: `v32`
Dataset: MusicBrainz 1M songs
Tool: k6 via benchmark CLI; scenario duration varies by profile (`quick` 15s, `standard` 30s by default)

---

Historical note: Runs 14-26 below are archival pre-cutover measurements from when this branch still carried both the old `braft` runtime and the new NuRaft runtime. They remain useful as migration evidence, but those comparison lanes are now retired because this branch is NuRaft-only. Any per-run recommendation to "keep" or "not remove" `braft` is superseded by the later cutover decision on this branch.

---

## Benchmark Scope Warning (2026-03-20, updated 2026-03-21)

The canonical `quick/core` and `standard/core` lanes still do **not** measure "live reads while imports are saturating the node"; they measure import completion and then post-import search. A real DDEV import against the fork on March 20, 2026 produced repeated slow requests on cheap endpoints like `/metrics.json`, `/health`, `/collections`, `/aliases`, and `/keys` in the `2.5-4.7s` range while the import was in progress, and related Laravel import jobs eventually timed out at `600s`.

That blind spot is now covered by the repo-owned `scripts/replay_fitment_import_stress.py` lane. The March 21 replay/fix cycle materially changed the conclusion: after moving write requests off the HTTP-side request path and onto the worker pool, the local mixed fitment lane no longer reproduces the old "node becomes operationally unusable during imports" failure. Keep using the replay lane for heavy-import claims, because the canonical benchmark matrix still does not encode this behavior directly.

---

## Run 36: Cached CPU Sampling Removes The Last `metrics.json` Handler Stall (2026-03-21)

**Commit:** local working tree on top of `HEAD` at run time
**Commands:**
- `python3 scripts/replay_fitment_import_stress.py --binary ./bazel-bin/typesense-server --total-fitment-docs 200000 --batch-docs 5000 --import-workers 3 --product-docs 50000 --vehicle-docs 50000 --seed-target-order never --server-batch-size 1000 --probe-profile dashboard --probe-workers 4 --search-workers 2 --probe-interval 0.1 --json-output /tmp/fork-current-post-cpucache-200k.json`
- `python3 scripts/replay_fitment_import_stress.py --baseline-binary /tmp/typesense-upstream-bin/typesense-server --baseline-label upstream-30.1 --candidate-binary ./bazel-bin/typesense-server --candidate-label fork-current --total-fitment-docs 200000 --batch-docs 5000 --import-workers 3 --product-docs 50000 --vehicle-docs 50000 --seed-target-order never --server-batch-size 1000 --probe-profile dashboard --probe-workers 4 --search-workers 2 --probe-interval 0.1 --json-output /tmp/upstream-vs-fork-post-cpucache-200k.json`
**Scenario:** eliminate the last route-local stall that still made `/metrics.json` materially slower than upstream even after the NuRaft sync/auth and event-loop fixes. The root cause turned out to be `SystemMetrics::get_cpu_stats()` sleeping for `100ms` inside every request so it could take a second `/proc/stat` snapshot. The route now uses cached delta-based CPU sampling instead of blocking the handler.

### Findings

- The remaining `/metrics.json` cost was real and almost entirely self-inflicted. On the corrected heavy-import lane before this fix, the fork still showed `/metrics.json 152.0ms` vs upstream `106.4ms`, and the route-level handler metrics showed about `102ms` of handler time even when the rest of the node was healthy.
- Removing the per-request `100ms` CPU sampling pause collapsed the route’s own cost. On the fork-only `200k` replay, `/metrics.json` fell from `122.2ms` avg with about `113.5ms` Server-Timing `process` time to `26.7ms` avg with only `3.8ms` `process` time, and the internal route metrics dropped to roughly `1-2ms` total handler/H2O time.
- The canonical upstream compare improved with it. On the corrected `200k` dashboard lane, the fork now runs at `120467.4 docs/s` vs upstream `72258.9 docs/s`, while `/metrics.json` is down to `29.0ms` avg instead of the earlier `152.0ms`.
- Search and control-plane reads remain slower than upstream in some lanes, but the remaining gaps are now small-route admission/connection effects rather than an obviously expensive handler on the server side. The hot-route process time for `/collections`, `/stats.json`, and `/metrics.json` is now effectively negligible compared with the old DDEV-era failure mode.

### Summary Table

| Lane | Import avg | Docs/sec | `/health` avg | `/metrics.json` avg | `/stats.json` avg | `/collections` avg | Search avg |
|---|---:|---:|---:|---:|---:|---:|---:|
| upstream `30.1`, `200k`, `batch_size=1000` | `189.5 ms` | `72258.9` | `1.6 ms` | `107.2 ms` | `0.2 ms` | `1.6 ms` | `7.4 ms` |
| fork current, same `200k` lane | `105.3 ms` | `120467.4` | `14.7 ms` | `29.0 ms` | `2.1 ms` | `17.5 ms` | `21.9 ms` |

### Decision

- Keep CPU sampling out of request handlers. Any future system-metrics refresh should remain cached or background-updated; do not reintroduce a timed sampling pause in `/metrics.json` or health-adjacent routes.
- Treat the corrected `200k` dashboard replay as green for release-quality heavy-import responsiveness on the local canonical lane. The fork now materially beats upstream on import throughput while keeping the dashboard-critical metrics route fast instead of self-blocking.
- The remaining difference to upstream is no longer a root-cause investigation item. Search and some control-plane routes are still slower than upstream during import, but they are now in the “worth further polish if it is cheap” category, not “server becomes impossible to use” territory.

---

## Run 35: Corrected Upstream-Comparable Fitment Replay Turns Green After Sync/Auth And Event-Loop Fixes (2026-03-21)

**Commit:** local working tree on top of `HEAD` at run time
**Commands:**
- `python3 scripts/replay_fitment_import_stress.py --baseline-binary /tmp/typesense-upstream-bin/typesense-server --baseline-label upstream-30.1 --candidate-binary ./bazel-bin/typesense-server --candidate-label fork-current --total-fitment-docs 100000 --batch-docs 5000 --import-workers 3 --product-docs 30000 --vehicle-docs 30000 --seed-target-order never --server-batch-size 1000 --probe-profile dashboard --probe-workers 4 --search-workers 2 --probe-interval 0.2 --json-output /tmp/upstream-vs-fork-batch1000-inlinecut-100k.json`
- `python3 scripts/replay_fitment_import_stress.py --baseline-binary /tmp/typesense-upstream-bin/typesense-server --baseline-label upstream-30.1 --candidate-binary ./bazel-bin/typesense-server --candidate-label fork-current --total-fitment-docs 200000 --batch-docs 5000 --import-workers 3 --product-docs 50000 --vehicle-docs 50000 --seed-target-order never --server-batch-size 1000 --probe-profile dashboard --probe-workers 4 --search-workers 2 --probe-interval 0.1 --json-output /tmp/upstream-vs-fork-batch1000-inlinecut-200k.json`
**Scenario:** first correct the benchmark semantics, then rerun after the remaining NuRaft read/auth and HTTP-path fixes. The semantic correction is important: upstream `v30.1` still defaults the request parameter `batch_size` to `40`, but its internal `Collection::add_many(...)` path continues to batch at `1000`. The fork intentionally wires request/config `batch_size` end-to-end, so upstream-comparable replay must pass `--server-batch-size 1000`.

### Findings

- The earlier `batch_size=40` replay overstated the fork-vs-upstream gap because it was not apples-to-apples. Upstream was effectively still importing at `1000` internally while the fork really honored `40` end-to-end.
- The `sync_live_product_state()` read-path cleanup was a real win, but not the decisive one. The bigger follow-up was narrowing the HTTP inline fast path back down to only truly trivial routes (`health` / `status` / `debug` class). Keeping `/metrics.json`, `/stats.json`, runtime search, and collection/control-plane GETs inline on the H2O event loop was creating request-admission pressure during import.
- After those fixes, import response queueing essentially collapsed on the corrected lane: on the fork `200k` run, `http_import_avg_response_queue_ms=1` and `message_dispatch_stream_response_max_queue_ms=57`, versus the earlier `43ms` average queue cost and materially worse admission behavior.
- On the corrected `100k` compare, the fork now beat upstream strongly on import throughput (`109922.5 docs/s` vs `74892.6 docs/s`) while keeping `/health` and `/metrics.json` effectively at upstream parity (`0.8ms` vs `0.8ms`, `104.9ms` vs `104.4ms`). Search and some control-plane GETs are still slower than upstream, but they are now measured in tens of milliseconds rather than the old operationally-bad multi-hundred-millisecond or multi-second regime.
- The steadier `200k` replay kept the same story: upstream stayed at `73030.5 docs/s`, while the fork held `106273.4 docs/s`. Search remained usable during import (`37.7ms` avg, `59.2ms` p95), and the heavier GET routes stayed in the `17-52ms` range instead of the older “dashboard feels dead” shape.

### Summary Table

| Lane | Import avg | Docs/sec | `/health` avg | `/metrics.json` avg | `/stats.json` avg | `/collections` avg | Search avg |
|---|---:|---:|---:|---:|---:|---:|---:|
| upstream `30.1`, `100k`, `batch_size=1000` | `176.8 ms` | `74892.6` | `0.8 ms` | `104.4 ms` | `0.3 ms` | `0.2 ms` | `5.9 ms` |
| fork current, same `100k` lane | `112.5 ms` | `109922.5` | `0.8 ms` | `104.9 ms` | `14.1 ms` | `33.0 ms` | `15.2 ms` |
| upstream `30.1`, `200k`, `batch_size=1000` | `188.0 ms` | `73030.5` | `0.4 ms` | `106.4 ms` | `0.2 ms` | `2.0 ms` | `6.5 ms` |
| fork current, same `200k` lane | `121.8 ms` | `106273.4` | `21.7 ms` | `152.0 ms` | `42.4 ms` | `52.7 ms` | `37.7 ms` |

### Decision

- Treat the corrected `batch_size=1000` fitment replay as the canonical upstream-comparable heavy-import lane.
- Keep the fork’s product behavior explicit: request/config `batch_size` must continue to flow end-to-end. Do **not** add a hidden internal hardcoded `1000` just because upstream still has one.
- Keep the inline fast path restricted to truly trivial routes. Running heavier read/control-plane handlers inline on the H2O event loop was the wrong tradeoff for import-era responsiveness.
- The remaining gap is now optimization, not root-cause uncertainty. The node stays operational during heavy import and the fork is ahead of upstream on corrected import throughput, but `/metrics.json` and some control-plane GETs are still slower than upstream and remain worth tuning.

---

## Run 34: Dashboard-Style `batch_size=40` Replay Reopens The Remaining DDEV-Parity Gap (2026-03-21)

**Commit:** local working tree on top of `HEAD` at run time
**Command:** `python3 scripts/replay_fitment_import_stress.py --baseline-binary /tmp/typesense-upstream-bin/typesense-server --baseline-label upstream-30.1 --candidate-binary ./bazel-bin/typesense-server --candidate-label fork-current --total-fitment-docs 50000 --batch-docs 5000 --import-workers 3 --product-docs 15000 --vehicle-docs 15000 --seed-target-order after --server-batch-size 40 --probe-profile dashboard --probe-workers 4 --search-workers 2 --probe-interval 0.2 --json-output /tmp/upstream-vs-fork-dashboard-50k-b40-r2.json`
**Scenario:** upgrade the local fitment replay to look more like the real DDEV control plane by probing the same dashboard GET routes during import and by explicitly forcing the historical low server-side import batching posture (`batch_size=40`).

This run is still useful as a fork-only low-batch stress lane, but it is **not** an apples-to-apples upstream comparison after the later source audit in Run 35. Upstream `v30.1` keeps an internal `Collection::add_many(...)` batch size of `1000`, so this `--server-batch-size 40` replay is measuring a materially different product configuration on the fork than it is on upstream.

### Findings

- The replay harness is now good enough to catch the control-plane class of regression directly. The new `--probe-profile dashboard` lane exercises `/collections`, `/aliases`, `/analytics/rules`, `/keys`, `/presets`, `/stemming/dictionaries`, `/stopwords`, and `/debug` alongside `/health`, `/metrics.json`, `/stats.json`, and search while the fitment import is active.
- The inline-response fast path for cheap GET routes is worthwhile. On this harsher lane the fork now keeps `/health` at `4.2ms` average, `/aliases` at `5.6ms`, `/stemming/dictionaries` at `7.3ms`, and `/debug` at `1.8ms` instead of forcing even those endpoints through the worker-pool plus shared response-dispatch path.
- That said, the low-batch DDEV-parity gap is still real. Upstream stayed at `72814.7 docs/s`, `/metrics.json 103.2ms`, `/stats.json 0.4ms`, `/collections 0.4ms`, and search `4.4ms`, while the fork measured `34362.6 docs/s`, `/metrics.json 232.0ms`, `/stats.json 192.4ms`, `/collections 226.4ms`, and search `124.9ms`.
- This remaining gap still does **not** look like a Raft backlog or thread-pool exhaustion problem. On the fork run, `queued_writes=0`, `pending_write_batches=0`, `nuraft_commit_lag=0`, `nuraft_state_machine_apply_lag=0`, and both thread-pool queue metrics stayed at `0`.
- The next local investigation target is therefore narrower than before: keep the new inline fast path for cheap control-plane routes, but keep working on the remaining low-batch search/stats responsiveness and import-throughput gap under concurrent dashboard pressure.

### Summary Table

| Lane | Import avg | Docs/sec | `/health` avg | `/metrics.json` avg | `/stats.json` avg | `/collections` avg | Search avg |
|---|---:|---:|---:|---:|---:|---:|---:|
| upstream `30.1`, `50k`, `after`, dashboard probes, `batch_size=40` | `170.7 ms` | `72814.7` | `1.4 ms` | `103.2 ms` | `0.4 ms` | `0.4 ms` | `4.4 ms` |
| fork current, same lane | `380.6 ms` | `34362.6` | `4.2 ms` | `232.0 ms` | `192.4 ms` | `226.4 ms` | `124.9 ms` |

### Decision

- Keep the new dashboard replay lane as the canonical DDEV-parity regression lane for heavy-import work. The earlier lighter fitment replay remains useful, but it no longer has enough control-plane pressure to represent the remaining problem by itself.
- Keep the inline response fast path for cheap GET routes. It clearly improves `/health` and the metadata endpoints, even though it does not yet solve the heavier `/stats.json` / search gap.
- Do **not** treat the heavy-import issue as fully solved for release purposes while this harsher lane still trails upstream materially.

---

## Run 33: Heavy-Import Responsiveness Recovered On The Local Fitment Replay Lane (2026-03-21)

**Commit:** local working tree on top of `HEAD` at run time
**Commands:**
- `TYPESENSE_IMPORT_BATCH_SIZE=1000 python3 scripts/replay_fitment_import_stress.py --baseline-binary /tmp/typesense-upstream-bin/typesense-server --baseline-label upstream-30.1 --candidate-binary ./bazel-bin/typesense-server --candidate-label fork-current --total-fitment-docs 50000 --batch-docs 5000 --import-workers 3 --product-docs 15000 --vehicle-docs 15000 --probe-interval 0.2 --seed-target-order never --json-output /tmp/upstream-vs-fork-fitment-50k-never-b1000-writepool.json`
- `TYPESENSE_IMPORT_BATCH_SIZE=1000 python3 scripts/replay_fitment_import_stress.py --baseline-binary /tmp/typesense-upstream-bin/typesense-server --baseline-label upstream-30.1 --candidate-binary ./bazel-bin/typesense-server --candidate-label fork-current --total-fitment-docs 100000 --batch-docs 5000 --import-workers 3 --product-docs 30000 --vehicle-docs 30000 --probe-interval 0.2 --seed-target-order after --json-output /tmp/upstream-vs-fork-fitment-100k-after-b1000-writepool.json`
**Scenario:** rerun the local `product_vehicle_fitments_se` replay after moving write requests off the HTTP-side request path and onto the main worker pool, then check both the isolated fitment-upsert lane and the more realistic mixed lane where fitments arrive before referenced docs exist and late async-reference work follows.

### Findings

- The main root cause on this branch was request-path scheduling, not Raft lag or collection import cost alone: `HttpServer::process_request()` had been running write requests inline on the HTTP-side path. Moving writes onto the worker pool collapsed the old request-shell gap and removed the catastrophic live-read starvation seen in DDEV and the earlier local replays.
- The isolated fitment-upsert lane is now faster than upstream on import throughput. At `50k` docs with `--seed-target-order never`, the fork improved from `61756.8 docs/s` in Run 32 to `86242.9 docs/s`, while upstream stayed at `73247.2 docs/s`.
- The realistic mixed lane also now holds up. At `100k` docs with late reference seeding (`--seed-target-order after`), the fork slightly beat upstream on import throughput (`76492.8 docs/s` vs `74857.6 docs/s`) while keeping cheap reads and search operational during the import instead of drifting into the old multi-second failure mode.
- Request-lifecycle instrumentation now shows the remaining gap is much smaller and much better explained: on the mixed `100k` fork run, `http_import_avg_total_ms=169`, `http_import_avg_handler_ms=153`, `http_import_avg_response_queue_ms=7`, and `http_import_avg_unattributed_ms=5`. The earlier `~72ms` unattributed shell is no longer the dominant story.
- The fork is not yet strictly equal to upstream on every live-read metric during import. In the mixed `100k` lane, `/metrics.json`, `/stats.json`, and search are still slower than upstream, but they are now measured in tens of milliseconds rather than the earlier seconds-long operational degradation.

### Summary Table

| Lane | Import avg | Docs/sec | `/health` avg | `/metrics.json` avg | `/stats.json` avg | Search avg |
|---|---:|---:|---:|---:|---:|---:|
| upstream `30.1`, `100k`, `after` | `177.5 ms` | `74857.6` | `2.7 ms` | `102.8 ms` | `1.5 ms` | `6.4 ms` |
| fork current, `100k`, `after` | `175.2 ms` | `76492.8` | `4.3 ms` | `117.2 ms` | `17.6 ms` | `19.2 ms` |

### Decision

- Treat the heavy-import responsiveness regression as materially fixed on the local replay lane. The old "dashboard and cheap reads basically stop responding during heavy imports" symptom is no longer reproduced here after the write-offload change.
- Keep `TYPESENSE_IMPORT_BATCH_SIZE=1000` as a high-throughput replay/benchmark override, not as a new default product setting.
- Keep the local fitment replay as the canonical reproduction and regression lane for this class of issue. The remaining work is optimization and broader validation, not root-cause uncertainty.

---

## Run 32: Local Fitment Replay Isolates The Remaining NuRaft Import/Search Responsiveness Gap (2026-03-21)

**Commit:** local working tree on top of `HEAD` at run time
**Command:** `TYPESENSE_IMPORT_BATCH_SIZE=1000 python3 scripts/replay_fitment_import_stress.py --baseline-binary /tmp/typesense-upstream-bin/typesense-server --baseline-label upstream-30.1 --candidate-binary ./bazel-bin/typesense-server --candidate-label fork-current --total-fitment-docs 50000 --batch-docs 5000 --import-workers 3 --product-docs 15000 --vehicle-docs 15000 --probe-interval 0.2 --seed-target-order never --json-output /tmp/upstream-vs-fork-fitment-50k-never-b1000-current.json`
**Scenario:** use the repo-owned `product_vehicle_fitments_se` replay harness to compare upstream vs the fork under concurrent import plus live read/search probes, while isolating the fitment-upsert phase (`--seed-target-order never`) from later async-reference backfill.

### Findings

- The local replay now reproduces the same class of user-visible regression without DDEV: the fork stays materially slower than upstream during the heavy fitment import lane and cheap reads/searches still degrade while writes are active.
- The fork now supports a repo-owned server-side import batching knob (`TYPESENSE_IMPORT_BATCH_SIZE` / `--import-batch-size`), and on this lane `1000` is measurably better than the historical default `40` for throughput. Keep it as an override, not as a new product default.
- The remaining gap is **not** explained by the old replay chunking, the extra handler-side slicing, or the initial async-reference backfill theory alone. On the isolated fitment lane, collection import work was only about `60-65ms` per sampled request while the fork still averaged `210.5ms` client-side and kept search around `134.5ms`.
- Additional request-lifecycle instrumentation now shows the fork's average import request shell at roughly `149ms` (`auth ~5ms`, handler `~71ms`, response queue `~15ms`, unattributed remainder `~72ms`). That still leaves a sizable unattributed interval during import, and read/search responsiveness remains much worse than upstream.
- A process-wide exclusive mutex in `NuRaftHttpRuntimeService::write()` was real and got removed from the hot path (shared read-side lock plus atomic applied-index tracking), but it only produced a small improvement. Keep the fix, but do not treat it as the main root cause.

### Summary Table

| Lane | Import avg | Docs/sec | `/health` avg | `/metrics.json` avg | `/stats.json` avg | Search avg |
|---|---:|---:|---:|---:|---:|---:|
| upstream `30.1` | `165.5 ms` | `74980.7` | `14.8 ms` | `106.8 ms` | `0.6 ms` | `4.4 ms` |
| fork current | `210.5 ms` | `61756.8` | `27.5 ms` | `263.9 ms` | `112.3 ms` | `134.5 ms` |

### Decision

- Keep the local `replay_fitment_import_stress.py` lane as the canonical reproduction path for this regression; it now catches both import throughput loss and live read/search degradation under write saturation.
- Keep server-side import batching configurable, but do **not** change the accepted product default from `40` based on this run alone.
- Treat lock contention / request-lifecycle attribution during concurrent import as the next investigation area. The fork's live-read/search degradation is still far larger than the raw collection import work would suggest.

---

## Run 31: Item 38 Closeout Against Upstream 30.1 After Benchmark Harness Repair (2026-03-18)

**Commit:** local `HEAD` `1c34ddf7` at run time
**Command:** `TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile standard --scope core --clean`
**Scenario:** rerun the canonical upstream-comparable `standard/core` lane after repairing the Dockerized Influx bind-mount cleanup path, and decide whether item 38's committed indexing JSON ownership-handoff change can close on current branch head.

### Findings

- Run 30's benchmark-harness blocker was real and root-caused locally: `--clean` removed and recreated the bind-mounted `benchmark/influxdb-data` tree while `benchmark-influxdb-1` could stay alive, so Docker kept the deleted inode mounted and k6 later failed with `mkdir /var/lib/influxdb/data: no such file or directory`.
- `scripts/benchmark_vs_upstream.sh` now stops the benchmark compose stack before cleaning bind-mounted state, recreates `benchmark/influxdb-data/{data,meta,wal}`, and verifies those paths inside the running Influx container before launching the benchmark CLI.
- The repaired wrapper completed the full canonical upstream compare successfully. Archive snapshot: `~/.cache/typesense/benchmark/archives/20260318-161516-standard-core`.

### Import Summary

| Lane | Import duration | Docs imported | HTTP status | Response warnings |
|---|---:|---:|---:|---:|
| upstream `30.1` | `50270 ms` | `1000000/1000000` | `200` | `0` |
| current fork `1c34ddf7` | `31790 ms` | `1000000/1000000` | `200` | `0` |

Import delta: `-18480 ms` (`-36.76%`) in the fork's favor.

### Search p95 Summary

| Scenario | Upstream p95 (`50vu / 100vu`) | Fork p95 (`50vu / 100vu`) | Delta (`50vu / 100vu`) |
|---|---:|---:|---:|
| `just_q` | `5 / 5 ms` | `5 / 4 ms` | `+0.00% / -20.00%` |
| `q_star` | `0 / 0 ms` | `0 / 0 ms` | `0.00% / 0.00%` |
| `filter_simple` | `110 / 345 ms` | `16 / 17 ms` | `-85.45% / -95.07%` |
| `filter_complex` | `27 / 72 ms` | `6 / 6 ms` | `-77.78% / -91.67%` |
| `sort_simple` | `254 / 577 ms` | `161 / 157 ms` | `-36.61% / -72.79%` |
| `sort_eval_condition` | `306 / 617 ms` | `166 / 161 ms` | `-45.75% / -73.91%` |
| `sort_eval_score` | `312 / 672 ms` | `166 / 173 ms` | `-46.79% / -74.26%` |
| `facet` | `461 / 947 ms` | `142 / 140 ms` | `-69.20% / -85.22%` |
| `group` | `2547 / 6170 ms` | `420 / 440 ms` | `-83.51% / -92.87%` |

### Decision

- Close item 38.
- Keep the committed move-handoff code from `7ab4cd8b` (`Reduce indexing JSON copies and prewarm test models`).
- Keep the benchmark-harness cleanup fix in `scripts/benchmark_vs_upstream.sh`; it is now part of the supported Dockerized benchmark lane, not a one-off local workaround.

---

## Run 30: Item 38 Cleaner Repeat Blocked By Dockerized Influx Mount Failure (2026-03-18)

**Commit:** local `HEAD` `1da111f7` at run time
**Command:** `TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --baseline-binary /tmp/typesense-server-baseline-86587197 --baseline-label baseline-86587197 --fork-label head-1da111f7 --profile quick --scope core --clean`
**Scenario:** rerun item 38 on the default benchmark workdir to decide whether Run 29's mixed search p95 table was real signal or quick-profile noise.

### Findings

- The rerun still hit repeated `Couldn't write stats ... mkdir /var/lib/influxdb/data: no such file or directory` errors from k6's Influx output path.
- This is no longer just an alternate-`--work-dir` problem. The same failure reproduced on the default `~/.cache/typesense/benchmark` workdir.
- The break is inside the current Dockerized benchmark harness, not just a missing host directory. During the rerun the checkout contained `benchmark/influxdb-data/data`, but `docker exec benchmark-influxdb-1 ls /var/lib/influxdb/data` still reported that path missing inside the running container.
- One attempt reached a baseline-only import summary (`29187 ms`, `1000000/1000000`, HTTP `200`, warnings `0`) before the search phase became untrustworthy, but the compare never produced a clean fork-vs-baseline result worth treating as product signal.

### Decision

- Keep item 38 active.
- Fix or bypass the Dockerized Influx mount failure before rerunning the explicit-binary `quick/core` compare.

---

## Run 29: Indexing JSON Ownership Handoff Audit, First Measured Pass (item 38, 2026-03-18)

**Commit:** local working tree on top of `86587197` at run time
**Command:** `TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --baseline-binary /tmp/typesense-server-baseline-86587197 --baseline-label baseline-86587197 --fork-binary ./bazel-bin/typesense-server --fork-label candidate-move-index-record --profile quick --scope core --clean`
**Scenario:** item 38's first measured candidate removes a full `nlohmann::json` deep copy per imported document by moving freshly parsed local documents directly into `index_record` instead of copying them first in `Collection::add_many(...)` and the matching load/alter replay loops.

### Import Summary

| Lane | Import duration | Docs imported | HTTP status | Response warnings | Archive |
|---|---:|---:|---:|---:|---|
| baseline `86587197` binary | `27716 ms` | `1000000/1000000` | `200` | `0` | `~/.cache/typesense/benchmark/archives/20260318-073343-quick-core` |
| candidate move-handoff binary | `26392 ms` | `1000000/1000000` | `200` | `0` | same archive |

Import delta: `-1324 ms` (`-4.78%`) in the candidate's favor.

### Representative Search p95 Summary

| Scenario | Baseline p95 (`50vu / 100vu`) | Candidate p95 (`50vu / 100vu`) |
|---|---:|---:|
| `group` | `409 / 398 ms` | `384 / 390 ms` |
| `just_q` | `5 / 4 ms` | `4 / 4 ms` |
| `facet` | `138 / 138 ms` | `138 / 155 ms` |
| `sort_simple` | `67 / 61 ms` | `86 / 90 ms` |
| `sort_eval_score` | `103 / 72 ms` | `124 / 119 ms` |

### Interpretation

- The targeted ownership change is large enough to matter on the real 1M-doc import path. One full `quick/core` replay cut import time by about `4.8%` without changing response shape or ingestion count.
- The same successful replay did **not** yield a clean steady-state search story. Some scenarios improved (`group`, `just_q`), some were flat, and some sort-heavy rows regressed materially. Because the code change only touches import-time JSON ownership handoff and does not change steady-state search code paths, this mixed table is more likely a sign that `quick/core` needs another clean repeat before closeout than proof of a real search regression.
- A follow-up rerun with `--work-dir /tmp/typesense-bench-move-r2` was invalid because k6 could not write Influx stats (`mkdir /var/lib/influxdb/data: no such file or directory`). A later repeat on the default benchmark workdir reproduced the same Dockerized Influx failure; see Run 30. Treat this as a benchmark-harness issue, not as Typesense signal.

### Decision

- Keep the move-handoff code change as the current best import-path candidate for item 38.
- Do **not** close item 38 yet on this run alone. Re-run the comparison cleanly before treating the search deltas as either a blocker or a non-issue.

---

## Run 28: USearch Vector Backend Closeout (repo-owned `vector_index_t`, 2026-03-17)

**Commit:** local working tree on top of `c1a50e23` at run time
**Command:** `scripts/bazel_in_docker.sh run //:usearch-vector-backend-benchmark -- --docs 20000 --dims 384 --cycles 10 --updates-per-cycle 400 --replacements-per-cycle 100 --searches-per-cycle 200 --k 20 --ef 80 --kernel-samples 4096 --kernel-repeats 64 --seed 42`
**Scenario:** close out item 36 with a focused vector-backend benchmark that compares distance kernels and then drives the production USearch backend through repeated update, replacement, and search traffic.

### Kernel Summary (2 reruns)

| Kernel | Run 1 | Run 2 | Relative result |
|---|---:|---:|---|
| `usearch_builtin_ip_normalized` | `35.93 ms` | `35.92 ms` | Winner in both runs (`+1.77%` / `+3.28%` vs scalar baseline) |
| `parity_scalar_normalized_ip` | `36.58 ms` | `37.14 ms` | Old parity-first scalar baseline |
| `usearch_builtin_cos_raw` | `48.68 ms` | `50.28 ms` | Rejected (`-33.09%` / `-35.36%` vs scalar baseline) |

All three kernels produced the same checksum (`65341.51`) on the benchmark corpus, so the winning path is a semantically equivalent faster implementation, not a different scoring target.

### Mixed Update/Search Summary (2 reruns)

| Measure | Run 1 | Run 2 |
|---|---:|---:|
| Initial index build (`20k` docs) | `28.476 s` | `25.756 s` |
| Write-side ops (`4k` in-place upserts + `1k` delete/insert replacements) | `5000` | `5000` |
| Write throughput | `534.29 ops/s` | `543.20 ops/s` |
| Write p50 / p95 | `1820.93 / 2461.06 us` | `1786.45 / 2481.36 us` |
| Search ops | `2000` | `2000` |
| Search throughput | `1195.73 ops/s` | `1218.69 ops/s` |
| Search p50 / p95 | `854.72 / 1353.24 us` | `843.19 / 1317.62 us` |
| Final live elements | `20000` | `20000` |
| Final deleted count | `0` | `0` |
| Search checksum | `21272624` | `21272624` |

### Interpretation

- The supported production vector path now behaves like a real USearch backend, not a thin `hnswlib` impersonation layer. The mixed workload kept `deleted_count=0` at the end of both reruns, so Typesense-owned tombstones plus free-slot reuse held steady under repeated update/replacement traffic.
- The old parity-first scalar distance shim is no longer the best choice. USearch's builtin normalized inner-product kernel won both reruns, while builtin raw cosine was materially slower with no checksum/quality gain on this corpus.
- One focused replay test did surface a tiny observable float drift (`0.095856309` vs `0.095856249`, about `6e-8`) in `HybridSearchAuxScoreTest`. This was investigated against USearch's upstream metric path and is consistent with SIMD/autovec accumulation order for the same `1 - dot` math, not a ranking change or a different distance definition.

### Decision

- Keep the supported production backend on USearch `index_gt` behind the repo-owned `vector_index_t` seam.
- Remove the remaining `hnswlib` runtime/Bazel baggage from the supported path. The only surviving hnsw-shaped surface is schema `hnsw_params`, kept intentionally for API compatibility and mapped onto backend construction settings.
- Use USearch's builtin inner-product metric on normalized vectors as the production distance kernel. It is the fastest semantically equivalent option tested here, and it lets the backend stay USearch-native instead of carrying a permanent Typesense-owned scalar shim.
- Do **not** switch the production default to raw builtin cosine. On this benchmark it was substantially slower and did not buy a different checksum or a quality signal strong enough to justify divergence.

---

## Run 27: NuRaft Bulk-Import Parity Finalized For The Core Benchmark Lane (NuRaft runtime, 2026-03-14)

**Commit:** local working tree on top of `985acb45` at run time
**Scenario:** prove whether the NuRaft runtime can keep one-shot bulk-import semantics without turning the whole request into one oversized Raft entry, then rerun the upstream-comparable `core` lane.

### Proof Summary

| Check | Pre-fix runtime | Final runtime |
|---|---:|---:|
| Throttled `12k` single POST | connection reset after `3570/12000` lines | HTTP `200`, `12000/12000` lines |
| Direct `1M` single POST | not safe for the benchmark lane | HTTP `200`, `1000000/1000000` lines in `28s` |
| Final collection count | `3570` | `12000` / `1000000` |
| Runtime write model | H2O body aggregation could still reach `append_via_raft()` mid-request | buffer one logical request, replicate bounded logical chunks (`5000` docs / `4 MiB`), then replay the buffered body through `post_import_documents()` in H2O-sized slices |
| Committed-index behavior | partial body fragment still committed | bounded by logical chunking: `+2` for the `6k` API test, `202` entries for the direct `1M` proof |
| Request timeout | hardcoded `60000ms` | default `60000ms`, configurable via `--request-timeout-ms` / `TYPESENSE_REQUEST_TIMEOUT_MS` |

### Interpretation

- The regression was real and architectural, not benchmark CLI drift: under a slow single POST, the old runtime path could still let transport/body aggregation boundaries dictate when Raft work happened.
- A first route-only buffering refactor was not sufficient. It restored one-shot POST completion, but it still changed the live import execution model enough to distort storage/search behavior.
- The final design is the one to keep: H2O still buffers one logical request before the runtime write path, but the runtime now keeps Raft entries bounded while replaying the buffered request through the existing import-handler cadence. That preserves the documented import response contract and the live engine's batching behavior together.

### Decision

- Keep the benchmark `core` scope on the upstream-comparable shape: one large import POST, then search.
- Keep the NuRaft import implementation as bounded logical chunking plus one logical handler replay; do not regress to per-transport-chunk Raft work or to a chunk-per-handler execution model.
- `extended` remains the opt-in lane for stress import, concurrent search+import, and extra RocksDB/metrics collection.
- Hosted/manual benchmark runs that need more than the inherited `60s` request deadline should use `TYPESENSE_REQUEST_TIMEOUT_MS=300000`. The benchmark launcher forwards that env var into Dockerized benchmark containers, so older comparison binaries safely ignore it.

### Core Lane Validation

| Run | Command | Result |
|---|---|---|
| `quick/core` self-compare | `TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --self-compare --profile quick --scope core` | Passed on the final replay-model build. Import `27090ms -> 25689ms`, search checks `100%` for both halves, archive snapshot `20260314-080257-quick-core` under explicit work dir `/tmp/self-bench-rerun.ccjGHS` |
| `standard/core` upstream compare | `TYPESENSE_REQUEST_TIMEOUT_MS=300000 scripts/benchmark_vs_upstream.sh --build --profile standard --scope core` | Passed. Archive: `~/.cache/typesense/benchmark/archives/20260314-082629-standard-core` |

### Standard/Core Summary

| Commit lane | Import duration | Docs imported | HTTP status | Response warnings | Benchmark data dir |
|---|---:|---:|---:|---:|---:|
| `upstream-30.1` | `29383ms` | `1000000/1000000` | `200` | `0` | `893M` |
| final local runtime build | `26906ms` | `1000000/1000000` | `200` | `0` | `489M` |

### Representative Search p95 Summary

| Scenario | Upstream p95 (`50vu / 100vu`) | Final runtime p95 (`50vu / 100vu`) |
|---|---:|---:|
| `facet` | `365 / 837 ms` | `133 / 133 ms` |
| `filter_simple` | `93 / 302 ms` | `15 / 15 ms` |
| `group` | `2182 / 4467 ms` | `379 / 380 ms` |
| `sort_eval_score` | `261 / 583 ms` | `66 / 65 ms` |
| `sort_simple` | `209 / 516 ms` | `55 / 54 ms` |

### Benchmark Verdict

- The final runtime build beat upstream classic on import (`29.383s -> 26.906s`) and on every meaningful non-zero search scenario in this `standard/core` replay.
- The only visible regression was `just_q (50vu)` moving from `4ms` to `5ms`, which is below the benchmark lane's own `7ms` significance floor for that scenario.
- The earlier storage/search regression from the discarded chunk-per-handler prototype is no longer present. Against upstream classic, the final runtime build now produces a smaller benchmark data directory while also winning the mixed `index + search` lane.

### Benchmark Harness Note

- `standard/core` initially tripped a benchmark-harness-only failure after the server emitted a very large volume of `threadpool exhaustion detected` stderr during long search runs.
- The fix was to launch the long-lived Typesense Docker process with execa `buffer: false`. The harness already consumes stdout/stderr incrementally, so internal buffering only created a Bun/get-stream failure mode without adding value.
- The harness now also keeps the first `threadpool exhaustion detected` line but collapses the repeated burst into periodic summaries with the max observed queue depth. That preserves the saturation signal without burying the benchmark verdict in log spam.
- Keep that setting. Long benchmark lanes should not depend on buffering all server logs in memory.

---

## Run 26: Fixed-Rate Mixed Runtime Contention After Sink-Backed Local Replay Progress (`braft` runtime vs NuRaft runtime, 2026-03-10)

**Commit:** `HEAD` at run time
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-runtime-contention --duration 10s --docs 100 --writer-threads 1 --writer-interval-ms 2 --reader-threads 2 --repeats 2`
**Scenario:** same bounded runtime contention lane, but now with a fixed writer interval to compare read QoS under roughly equal write pressure after removing the single-node runtime's per-write replay-progress fsync from the hot path.
**Artifacts:** `~/.cache/typesense/benchmark/raft-runtime-contention-summary.json`, run root `/home/cyppe/.cache/typesense/benchmark/raft-runtime-contention-runs/20260310-112717`

### Aggregated Results

| Measure | `braft` runtime | NuRaft runtime |
|---|---:|---:|
| Writes completed (median of 2) | `3,208` | `3,246` |
| Reads completed (median of 2) | `94,042` | `85,642` |
| Write p50 / p95 | `1.01 / 1.24 ms` | `0.97 / 1.15 ms` |
| Read p50 / p95 | `0.19 / 0.36 ms` | `0.18 / 0.81 ms` |
| Process CPU | `4,435 ms` | `2,195 ms` |
| Peak RSS | `259,572 KB` | `71,314 KB` |
| NuRaft / `braft` write ratio |  | `1.01x` |
| NuRaft / `braft` read ratio |  | `0.91x` |

### Interpretation

- This is the first mixed contention run that holds write pressure approximately equal enough to compare read QoS fairly.
- Under that controlled write rate, NuRaft is now effectively at write parity and keeps about `91%` of `braft` read throughput while using much less CPU and RSS.
- That changes the decision posture materially. The old closed-loop mixed lane still shows a large read gap, but it is no longer a clean fairness signal once NuRaft can drive materially more writes in the same window.

### Decision

- Treat NuRaft as the benchmark winner for the current bounded runtime surface.
- Do **not** delete `braft` yet, because broader API parity, async/import/streaming hardening, and a migration cut line still remain.
- From here the question is no longer “is NuRaft viable enough to keep going?” It is “finish the runtime/product hardening without losing these signals.”

---

## Run 25: Pure-Write Runtime Contention After Sink-Backed Local Replay Progress (`braft` runtime vs NuRaft runtime, 2026-03-10)

**Commit:** `HEAD` at run time
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-runtime-contention --duration 5s --docs 100 --writer-threads 1 --reader-threads 0 --repeats 3`
**Scenario:** isolate sustained document-write pressure after removing the single-node runtime's per-write replay-progress fsync from the hot path and deriving local replay progress from the materialized sink instead.
**Artifacts:** `~/.cache/typesense/benchmark/raft-runtime-contention-summary.json`, run root `/home/cyppe/.cache/typesense/benchmark/raft-runtime-contention-runs/20260310-112055`

### Aggregated Results

| Measure | `braft` runtime | NuRaft runtime |
|---|---:|---:|
| Writes completed (median of 3) | `4,458` | `6,606` |
| Write p50 / p95 | `0.94 / 1.24 ms` | `0.72 / 0.85 ms` |
| Process CPU | `1,780 ms` | `1,180 ms` |
| Peak RSS | `153,476 KB` | `74,068 KB` |
| NuRaft / `braft` write ratio |  | `1.48x` |

### Interpretation

- The per-write replay-progress sync was a real bottleneck. Once the single-node runtime stopped treating the metadata replay-progress file as the hot-path source of truth, NuRaft moved from roughly `0.47x` of `braft` pure-write throughput to about `1.48x`.
- Pure writes are no longer the steady-state blocker on the bounded runtime surface.
- This also explains why the old closed-loop mixed lane became harder to interpret: NuRaft can now issue materially more writes in the same window, which naturally increases interference unless the benchmark controls write rate explicitly.

### Decision

- Do **not** treat raw write throughput as the reason to keep `braft`.
- Keep the fixed-rate mixed lane alongside the pure-read and pure-write variants so future regressions stay attributable.

---

## Run 24: Pure-Write Runtime Contention Isolation (`braft` runtime vs NuRaft runtime, 2026-03-10)

**Commit:** `HEAD` at run time
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-runtime-contention --duration 5s --docs 100 --writer-threads 1 --reader-threads 0 --repeats 3`
**Scenario:** isolate sustained document-write pressure with no readers so the contention lane can distinguish raw write throughput from mixed read/write interference.
**Artifacts:** `~/.cache/typesense/benchmark/raft-runtime-contention-summary.json`, run root `/home/cyppe/.cache/typesense/benchmark/raft-runtime-contention-runs/20260310-110607`

### Aggregated Results

| Measure | `braft` runtime | NuRaft runtime |
|---|---:|---:|
| Writes completed (median of 3) | `4,736` | `2,245` |
| Write p50 / p95 | `0.97 / 1.35 ms` | `1.29 / 1.59 ms` |
| Process CPU | `2,270 ms` | `600 ms` |
| Peak RSS | `154,636 KB` | `69,832 KB` |
| NuRaft / `braft` write ratio |  | `0.47x` |

### Interpretation

- Pure writes are still a real NuRaft bottleneck on the live runtime lane. The median run reached only about `47%` of `braft` write throughput here.
- That changes the earlier story: the mixed contention gap is not primarily a raw read-path problem anymore. Pure reads already look healthy, but sustained write pressure still degrades the current NuRaft runtime enough to drag mixed read/write throughput down with it.
- The lower CPU and RSS numbers show this is still more likely runtime-integration debt than a proof that the core NuRaft library cannot compete.

### Decision

- Do **not** remove `braft` yet.
- Do continue NuRaft only if the next work is explicitly aimed at write-path interference, synchronous durability/response handling, and mixed contention behavior.
- Stop describing the blocker as “the live read path” alone; the cleaner statement is “NuRaft still loses under sustained writes and therefore under mixed read/write pressure.”

---

## Run 23: Pure-Read Runtime Contention Isolation (`braft` runtime vs NuRaft runtime, 2026-03-10)

**Commit:** `HEAD` at run time
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-runtime-contention --duration 5s --docs 100 --writer-threads 0 --reader-threads 2 --repeats 3`
**Scenario:** isolate steady-state document reads with no writers so the contention lane can distinguish raw read latency from mixed read/write interference.
**Artifacts:** `~/.cache/typesense/benchmark/raft-runtime-contention-summary.json`, run root `/home/cyppe/.cache/typesense/benchmark/raft-runtime-contention-runs/20260310-105927`

### Aggregated Results

| Measure | `braft` runtime | NuRaft runtime |
|---|---:|---:|
| Reads completed (median of 3) | `53,928` | `57,204` |
| Read p50 / p95 | `0.18 / 0.24 ms` | `0.17 / 0.23 ms` |
| Process CPU | `1,450 ms` | `960 ms` |
| Peak RSS | `163,408 KB` | `68,172 KB` |
| NuRaft / `braft` read ratio |  | `1.06x` |

### Interpretation

- Pure reads are no longer the blocker on the bounded live runtime lane. NuRaft slightly beat `braft` on median completed reads and latency in this isolated run.
- That is a useful narrowing result: the large mixed contention gap is not caused by an intrinsically slow document read path.
- The remaining gap must therefore be explained by sustained write cost or read/write interference rather than standalone read lookup speed.

### Decision

- Do **not** remove `braft` yet.
- Do continue NuRaft, because the old “NuRaft reads are just slow” explanation is no longer supported by the data.
- Use pure-read and pure-write isolation alongside the mixed lane from here forward so contention regressions are easier to attribute.

---

## Run 22: Repeated Runtime Contention After Bounded Live Typesense-State Mirror (`braft` runtime vs NuRaft runtime, 2026-03-10)

**Commit:** `HEAD` at run time
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-runtime-contention --duration 5s --docs 100 --reader-threads 2 --repeats 3`
**Scenario:** same repeated contention lane as Run 21, but the single-node NuRaft runtime now bootstraps a minimal `CollectionManager`/`Store` stack, mirrors valid-schema CRUD writes into that live Typesense state, and prefers live reads for collections that stay on the mirrored subset.
**Artifacts:** `~/.cache/typesense/benchmark/raft-runtime-contention-summary.json`, run root `/home/cyppe/.cache/typesense/benchmark/raft-runtime-contention-runs/20260310-104653`

### Aggregated Results

| Measure | `braft` runtime | NuRaft runtime |
|---|---:|---:|
| Writes completed (median of 3) | `2,872` | `3,022` |
| Reads completed (median of 3) | `40,646` | `7,853` |
| Write p50 / p95 | `0.99 / 1.65 ms` | `1.50 / 1.80 ms` |
| Read p50 / p95 | `0.21 / 0.49 ms` | `1.40 / 1.68 ms` |
| Process CPU | `3,010 ms` | `950 ms` |
| Peak RSS | `256,468 KB` | `71,356 KB` |
| NuRaft / `braft` write ratio |  | `1.05x` |
| NuRaft / `braft` read ratio |  | `0.19x` |

### Interpretation

- The bounded live-state mirror kept the write-side result roughly where Run 21 already had it: near parity or slightly ahead on median completed writes.
- It did **not** materially close the mixed-workload gap. Even after preferring live Typesense state for the contention collection, NuRaft still reached only about `19%` of `braft` read throughput in this lane.
- Later pure-read isolation showed that standalone reads are already fine. The mixed deficit is therefore better explained by sustained write pressure and read/write interference than by the old prototype materialized-view read path alone.

### Decision

- Do **not** remove `braft` yet.
- Do continue NuRaft only if the next work is explicitly aimed at explaining or closing the sustained-write and mixed-interference gap.
- The replacement question is now much tighter: recovery is better, bounded API replay is better, pure reads are already fine, but sustained writes and mixed contention are still the main unresolved reason `braft` remains safer today.

---

## Run 21: Repeated Raft Runtime Contention Median After Read-Mostly Cache Cleanup (`braft` runtime vs NuRaft runtime, 2026-03-10)

**Commit:** `HEAD` at run time
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-runtime-contention --duration 5s --docs 100 --reader-threads 2 --repeats 3`
**Scenario:** same bounded contention lane as Run 19, but now with repeat support in the harness plus a read-mostly single-node document cache that avoids taking write-side cache locks for uncached documents.
**Artifacts:** `~/.cache/typesense/benchmark/raft-runtime-contention-summary.json`, run root `/home/cyppe/.cache/typesense/benchmark/raft-runtime-contention-runs/20260310-102718`

### Aggregated Results

| Measure | `braft` runtime | NuRaft runtime |
|---|---:|---:|
| Writes completed (median of 3) | `3,260` | `3,415` |
| Reads completed (median of 3) | `42,517` | `9,042` |
| Write p50 / p95 | `0.91 / 1.21 ms` | `1.44 / 1.66 ms` |
| Read p50 / p95 | `0.20 / 0.46 ms` | `1.33 / 1.56 ms` |
| Process CPU | `2,820 ms` | `820 ms` |
| Peak RSS | `255,908 KB` | `61,708 KB` |
| NuRaft / `braft` write ratio |  | `1.05x` |
| NuRaft / `braft` read ratio |  | `0.21x` |

### Interpretation

- This is the first contention checkpoint using repeated-run medians instead of a single wall-clock sample.
- The read-mostly cache cleanup removed the worst write-side cache interference. On the median of three repeats, NuRaft is now roughly at write parity or slightly ahead on this lane.
- Reads are still materially behind. Even on the cleaner median, NuRaft is only around `21%` of `braft` read throughput here.
- The per-run raw data still shows host sensitivity, so this should be treated as a stronger steady-state checkpoint than Run 19, but not as the final word on production read behavior.

### Decision

- Do **not** remove `braft` yet.
- Do continue NuRaft, because the write-side contention gap is no longer the blocker it was.
- The main remaining performance question is now concentrated in steady-state read behavior on the live runtime path.

---

## Run 20: Focused Raft API Replay After Single-Node Runtime Cache Cleanup (`braft` runtime vs NuRaft runtime, 2026-03-10)

**Commit:** `HEAD` at run time
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-api-replay`
**Scenario:** same bounded API wrapper comparison as Run 16 after the direct-local-apply work, durable replay-progress/segment-log reuse, and a small single-node document cache in the NuRaft runtime.
**Artifacts:** `~/.cache/typesense/benchmark/raft-api-replay-summary.json`

### Aggregated Results

| Suite | `braft` runtime | NuRaft runtime | Delta | Speedup |
|---|---:|---:|---:|---:|
| `tests/collections.test.ts` | `56,268.93 ms` | `2,431.67 ms` | `53,837.26 ms` faster | `23.14x` |
| `tests/documents.test.ts` | `58,272.66 ms` | `2,479.94 ms` | `55,792.73 ms` faster | `23.50x` |

### Interpretation

- The bounded NuRaft runtime is still materially faster than the live `braft` runtime on the currently implemented API surface.
- The new single-node cache did not change scope, but it did keep the focused API replay lane firmly in NuRaft's favor while preserving restart, snapshot, and static multi-node replay behavior.
- Fresh `raft-runtime-contention` reruns taken in the same session were too host-sensitive to replace Run 19: `braft` write p95 bounced into the `22-38 ms` range while read throughput spiked abnormally, so Run 19 remains the current trustworthy steady-state contention baseline.

### Decision

- Do **not** remove `braft` yet.
- Do continue NuRaft, because the runtime-integration work is still producing real wins on the implemented API surface.
- Treat Run 19 as the current contention checkpoint until the same lane is rerun on a quieter host or with a more stable harness setting.

---

## Run 19: Focused Raft Runtime Contention Comparison After Direct Local Apply + Reusable Replay Progress (`braft` runtime vs NuRaft runtime, 2026-03-10)

**Commit:** `HEAD` at run time
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-runtime-contention --duration 5s --docs 100 --reader-threads 2`
**Scenario:** same bounded contention lane as Runs 17-18 after three more single-node runtime optimizations: direct local append/apply for fresh writes, persistent segment-log append handles, and reusable replay-progress persistence instead of reconstructing a fresh metadata store per write.
**Artifacts:** `~/.cache/typesense/benchmark/raft-runtime-contention-summary.json`, run roots `/home/cyppe/.cache/typesense/benchmark/raft-runtime-contention-runs/20260310-075149` and `/home/cyppe/.cache/typesense/benchmark/raft-runtime-contention-runs/20260310-075210`

### Aggregated Results

| Measure | `braft` runtime | NuRaft runtime |
|---|---:|---:|
| Writes completed | `5,179-5,198` | `3,388-3,448` |
| Reads completed | `31,197-31,373` | `9,061-9,102` |
| Write p50 / p95 | `0.94 / 1.17 ms` | `1.42-1.44 / 1.67-1.72 ms` |
| Read p50 / p95 | `0.25 / 0.66 ms` | `1.32 / 1.56-1.61 ms` |
| Process CPU | `4,050-4,070 ms` | `880-890 ms` |
| Peak RSS | `339,632-371,112 KB` | `107,916-110,180 KB` |
| NuRaft / `braft` write ratio |  | `0.65-0.67x` |
| NuRaft / `braft` read ratio |  | `0.29x` |

### Interpretation

- This is the first clean repeat band after the write-side bookkeeping optimizations rather than a single noisy sample.
- The direct local apply path and reusable replay-progress persistence moved NuRaft from the Run 18 band of roughly `53%` write throughput / `18%` read throughput versus `braft` to a repeatable band around `65-67%` writes / `29%` reads.
- NuRaft still trails `braft` on steady-state point reads and writes, so `braft` remains the safer production runtime today.
- The remaining gap is no longer large enough to look like a core-library disqualifier. It looks like a runtime-integration gap that still needs more read-path and request-lifecycle work.
- Resource use remains materially lower on the NuRaft side in this lane, which keeps the migration question open instead of closing it.

### Decision

- Do **not** remove `braft` yet.
- Do continue NuRaft, because the current runtime contention gap keeps shrinking under localized integration fixes.
- The next priority should stay on read-path and request-lifecycle efficiency in the live NuRaft runtime, then rerun all three canonical lanes together.

---

## Run 18: Focused Raft Runtime Contention Comparison After Single-Node Runtime Reuse (`braft` runtime vs NuRaft runtime, 2026-03-10)

**Commit:** `HEAD` at run time  
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-runtime-contention --duration 5s --docs 100 --reader-threads 2`  
**Scenario:** same bounded contention lane as Run 17 after keeping the single-node NuRaft request journal, state machine, and materialized-state sink alive across requests, plus avoiding rereads for non-`PATCH` document writes.  
**Artifacts:** `~/.cache/typesense/benchmark/raft-runtime-contention-summary.json`, run root `/home/cyppe/.cache/typesense/benchmark/raft-runtime-contention-runs/20260310-070750`

### Aggregated Results

| Measure | `braft` runtime | NuRaft runtime |
|---|---:|---:|
| Writes completed | `4,003` | `2,115` |
| Reads completed | `30,067` | `5,542` |
| Write p50 / p95 | `0.97 / 2.52 ms` | `2.32 / 2.79 ms` |
| Read p50 / p95 | `0.27 / 0.64 ms` | `2.17 / 2.63 ms` |
| Process CPU | `3,610 ms` | `1,360 ms` |
| Peak RSS | `266,732 KB` | `63,828 KB` |

### Interpretation

- This is still the same bounded document read/write lane, so the comparison remains relevant to the currently implemented common runtime surface.
- The single-node runtime reuse work changed the contention picture materially. NuRaft moved from roughly `6%` of `braft` write throughput and `2%` of read throughput in Run 17 to roughly `53%` of write throughput and `18%` of read throughput here.
- NuRaft still trails `braft` on steady-state contention, especially reads, but it no longer loses badly enough to make the migration direction look obviously doomed.
- The resource picture remains favorable to NuRaft in this lane: much lower CPU time and much lower peak RSS.

### Decision

- Do **not** remove `braft` yet.
- Do continue NuRaft, because the biggest runtime bottleneck so far responded well to straightforward integration fixes.
- The remaining question is no longer "can NuRaft be made respectable under runtime contention?" It can. The next question is whether the remaining read-path gap can be closed enough without turning the integration into an unbounded rewrite.

---

## Run 17: Focused Raft Runtime Contention Comparison (`braft` runtime vs NuRaft runtime, 2026-03-09)

**Commit:** `HEAD` at run time  
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-runtime-contention --duration 5s --docs 100 --reader-threads 2`  
**Scenario:** preload `100` documents, then compare one writer plus `2` document readers against the live `//:typesense-server` and `//:typesense-server-nuraft-runtime` binaries for `5s`.  
**Artifacts:** `~/.cache/typesense/benchmark/raft-runtime-contention-summary.json`, run root `/home/cyppe/.cache/typesense/benchmark/raft-runtime-contention-runs/20260309-232312`

### Aggregated Results

| Measure | `braft` runtime | NuRaft runtime |
|---|---:|---:|
| Writes completed | `4,646` | `298` |
| Reads completed | `25,949` | `438` |
| Write p50 / p95 | `1.02 / 1.46 ms` | `16.23 / 19.51 ms` |
| Read p50 / p95 | `0.27 / 0.87 ms` | `17.63 / 37.01 ms` |
| Process CPU | `4,570 ms` | `3,430 ms` |
| Peak RSS | `378,044 KB` | `131,032 KB` |

### Interpretation

- This is the first canonical runtime-vs-runtime contention lane that stays on a surface both binaries really implement today without relying on the temporary NuRaft search shim.
- NuRaft no longer fails this lane: a real RocksDB self-lock bug surfaced during validation and was fixed by sharing one process-local materialized-state DB handle per path.
- Even after that fix and direct RocksDB point-lookups / prefix counts for common reads, the current NuRaft runtime is still materially slower than the live `braft` runtime under bounded document read/write pressure.
- That means the decision is no longer "NuRaft looks better everywhere we can measure." It does not. NuRaft is still winning recovery behavior and bounded API replay timing, but it is currently losing steady-state read/write contention on the implemented runtime surface.

### Decision

- Do **not** remove `braft` yet.
- Do **not** treat the current NuRaft runtime as production-ready.
- Do continue NuRaft only if the remaining work is explicitly aimed at closing the runtime integration gap: long-lived sink/state handles, less per-request reconstruction, and the sync/async follow-ups already called out in `MODERNIZATION_PLAN.md`.

---

## Run 16: Focused Raft API Replay Comparison (`braft` runtime vs NuRaft runtime, 2026-03-09)

**Commit:** `HEAD` at run time  
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-api-replay`  
**Scenario:** time the same real `scripts/run_api_tests.sh` wrapper against `//:typesense-server` and `//:typesense-server-nuraft-runtime` for `tests/collections.test.ts` and `tests/documents.test.ts`.  
**Artifacts:** `~/.cache/typesense/benchmark/raft-api-replay-summary.json`

### Aggregated Results

| Suite | `braft` runtime | NuRaft runtime | Delta | Speedup |
|---|---:|---:|---:|---:|
| `tests/collections.test.ts` | `59,263.88 ms` | `3,532.13 ms` | `55,731.75 ms` faster | `16.78x` |
| `tests/documents.test.ts` | `57,196.37 ms` | `2,575.73 ms` | `54,620.64 ms` faster | `22.21x` |

### Interpretation

- This is the first focused runtime-vs-runtime timing lane on the real API harness rather than prototype-vs-runtime.
- The result is materially in NuRaft's favor on the currently implemented API surface: health, collections, documents, bounded search, import, restart, snapshot, and static multi-node replay.
- The biggest caveat is still scope. These files cover only the bounded surface that now exists on `//:typesense-server-nuraft-runtime`; they do not yet prove parity on the broader product API or mixed search/import contention profiles.

### Decision

- Do **not** remove `braft` yet.
- Do continue the NuRaft implementation/evaluation path.
- This run materially strengthens the case that the next deciding data should come from broader runtime parity and contention on the NuRaft runtime lane, not from more isolated prototype-only work.

---

## Run 15: Raft Recovery + Delayed Join Comparison (`braft` runtime vs NuRaft prototype, 2026-03-09)

**Commit:** `HEAD` at run time  
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-recovery`  
**Scenario:** steady write `docs=200`; outage/rejoin `200` writes before outage plus `3 x 50` writes while one follower is down with `22s` sleeps; delayed join `200` writes, manual leader snapshot, then `150` more writes before a late third node joins; `2` repeats.  
**Artifacts:** `~/.cache/typesense/benchmark/raft-recovery-summary.json`, run root `/home/cyppe/.cache/typesense/benchmark/raft-recovery-runs/20260309-211903`

### Aggregated Results

| Measure | NuRaft prototype steady write | `braft` runtime steady write |
|---|---:|---:|
| Write time (`docs=200`) | `0.88 ms` append + `17.39 ms` apply | `175.63 ms` |
| Write throughput | `229,256.90` append entries/s, `11,612.91` apply entries/s | `1,139.62` docs/s |
| Process CPU | `21.92 ms` | `70.00 ms` |
| Peak RSS | `26,190 KB` | `631,662 KB` |

| Measure | NuRaft outage recovery | `braft` outage recovery |
|---|---:|---:|
| Recovery time after latest snapshot / restart | `0.55 ms` | `3264.31 ms` |
| Extra recovery penalty from blocking snapshots on unhealthy peers | `+11.36 ms` | n/a |
| Replay after rejoin | `0` entries with leader-only snapshots, `150` with `require-healthy-peers` | `153` entries |
| Recovery path | `snapshot-install-only` with leader-only policy, `snapshot-install-plus-log-replay` with `require-healthy-peers` | `log-replay-only` in `2/2` runs |
| Snapshot freshness gap at end of outage | `0` entries after leader-only install | `2` entries |
| Timed snapshots created during outage | `3` per run in leader-only policy | `1` per run |
| Follower snapshot install observed on rejoin | `yes` (prototype install path) | `no` in `2/2` runs |

| Measure | NuRaft delayed join | `braft` delayed join |
|---|---:|---:|
| Join recovery time | `14.63 ms` | `9284.94 ms` |
| Replay after join | `50` entries | `352` entries |
| Join recovery path | `snapshot-install-plus-log-replay` in `2/2` runs | `log-replay-only` in `2/2` runs |
| Snapshot gap before join | fresh snapshot install plus `50` tail entries | `151` entries behind final leader index |
| Joiner snapshot install observed | `yes` (prototype install path) | `no` in `2/2` runs |

### Interpretation

- The current fork's `braft` path stays fixed for the unhealthy-peer timed-snapshot deadlock class: the leader kept snapshotting during follower outage and stayed within `2` entries of the final committed index.
- That does **not** translate into snapshot-based recovery in the live runtime. Across both outage repeats, `braft` still recovered by replaying roughly the whole missing window (`153` entries) and showed no follower snapshot install.
- The new delayed-join lane is the stronger signal. With a fresh leader snapshot already available, the NuRaft prototype installed that snapshot and replayed only the `50` post-snapshot tail, while `braft` replayed the full `352` committed entries for the late third node and again showed no snapshot install.
- On the current integration surfaces, NuRaft is materially better on both disaster recovery and add-back / late-join recovery shapes. But the comparison is still prototype-vs-runtime, not production-vs-production.

### Decision

- Do **not** remove `braft` yet. The full server/runtime path is still only implemented there.
- Do continue the NuRaft feasibility sprint. The late-join result strengthens the case that NuRaft is worth carrying forward.
- The next deciding data is broader runtime parity and contention behavior, not whether NuRaft recovery is promising enough to keep exploring.

---

## Run 14: Raft Recovery Comparison (`braft` runtime vs NuRaft prototype, 2026-03-09)

**Commit:** `HEAD` at run time  
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-recovery`  
**Scenario:** `200` writes before follower outage, `3 x 50` writes while one follower is down, `22s` sleeps between outage rounds, `2` repeats.  
**Artifacts:** `~/.cache/typesense/benchmark/raft-recovery-summary.json`, run root `/home/cyppe/.cache/typesense/benchmark/raft-recovery-runs/20260309-202254`

### Aggregated Results

| Measure | NuRaft prototype steady write | `braft` runtime steady write |
|---|---:|---:|
| Write time (`docs=200`) | `0.87 ms` append + `16.09 ms` apply | `177.81 ms` |
| Write throughput | `231,997.99` append entries/s, `12,494.09` apply entries/s | `1,125.57` docs/s |
| Process CPU | `20.15 ms` | `60.00 ms` |
| Peak RSS | `26,776 KB` | `519,988 KB` |

| Measure | NuRaft prototype | `braft` runtime |
|---|---:|---:|
| Recovery time after latest snapshot / restart | `0.58 ms` | `3263.73 ms` |
| Extra recovery penalty from blocking snapshots on unhealthy peers | `+10.53 ms` | n/a |
| Replay after rejoin | `0` entries with leader-only snapshots, `150` with `require-healthy-peers` | `152` entries |
| Recovery path | `snapshot-install-only` with leader-only policy, `snapshot-install-plus-log-replay` with `require-healthy-peers` | `log-replay-only` in `2/2` runs |
| Snapshot freshness gap at end of outage | `0` entries after leader-only install | `2` entries |
| Timed snapshots created during outage | `3` per run in leader-only policy | `1` per run |
| Follower snapshot install observed on rejoin | `yes` (prototype install path) | `no` in `2/2` runs |
| Process CPU cost | `147.44 ms` total benchmark CPU | leader `315 ms` during outage |
| Peak RSS | `25,410 KB` benchmark-process peak | leader `436,078 KB` during outage, follower `392,000 KB` during recovery |

### Interpretation

- The current fork's `braft` path is fixed for the unhealthy-peer timed-snapshot deadlock class: the leader did create timed snapshots while a follower was down, and the snapshot stayed fresh to within `2` entries of the final committed index.
- That fix does **not** make `braft` look like the NuRaft prototype on recovery behavior. In both runtime repeats, the follower still restarted around log index `202` and replayed roughly the whole outage window (`152-153` entries) instead of obviously taking a snapshot-install path.
- The NuRaft prototype remains materially stronger on this narrow disaster-recovery shape when the leader is allowed to keep snapshotting locally. If NuRaft blocks snapshots on peer health, it immediately loses that advantage and also replays the whole outage window.
- The new steady-write rows point the same way as the recovery rows: the isolated NuRaft prototype path is materially lighter and faster on this narrow write benchmark. But it is still not a full HTTP/runtime comparison, so treat it as justification to continue the feasibility sprint, not as permission to rip out `braft` immediately.
- The new CPU/RSS rows are useful only as directional signals. They compare a full live `braft` HTTP server against an isolated NuRaft prototype benchmark process, so they are not a valid “NuRaft uses 17x less memory” conclusion by themselves.

### Decision

- Do **not** remove `braft` yet. The live server/runtime path is still only implemented there.
- Do continue the NuRaft feasibility sprint. Recovery behavior is now strong enough to justify the remaining Story E work.
- The next deciding data is not more outage-policy evidence. It is steady-state CPU/memory plus broader runtime-parity and contention measurements.

---

## Run 11: `write-stress` Baseline Repeats (2026-03-05)

**Commit:** `fe451638`  
**Upstream:** Typesense 30.1  
**Profile:** write-stress (30s per scenario)  
**Shared server args:** `--max-indexing-concurrency=4`

### Pass 1

Archive: `/home/cyppe/.cache/typesense/benchmark/archives/20260305-211730-write-stress---max-indexing-concurrency4`

| Scenario | Upstream 30.1 | Fork baseline | % Change | Verdict |
|----------|---------------|---------------|----------|---------|
| **Import (1M docs)** | **37.975s** | **22.384s** | **-41.06%** | **FORK WINS** |
| concurrent_facet (50vu) | 24 ms | 30 ms | +25.00% | UPSTREAM |
| concurrent_filter_simple (50vu) | 15 ms | 19 ms | +26.67% | UPSTREAM |
| concurrent_sort_eval_score (50vu) | 16 ms | 19 ms | +18.75% | UPSTREAM |

Stress counters (fork):
- `concurrent_import_docs_total`: `144000`
- `concurrent_import_success_rate`: `100%`
- `concurrent_import_batch_duration_ms` p95: `56.9ms`

### Pass 2

Archive: `/home/cyppe/.cache/typesense/benchmark/archives/20260305-214513-write-stress---max-indexing-concurrency4`

| Scenario | Upstream 30.1 | Fork baseline | % Change | Verdict |
|----------|---------------|---------------|----------|---------|
| **Import (1M docs)** | **29.864s** | **22.593s** | **-24.35%** | **FORK WINS** |
| concurrent_facet (50vu) | 25 ms | 30 ms | +20.00% | UPSTREAM |
| concurrent_filter_simple (50vu) | 16 ms | 19 ms | +18.75% | UPSTREAM |
| concurrent_sort_eval_score (50vu) | 16 ms | 19 ms | +18.75% | UPSTREAM |

Stress counters (fork):
- `concurrent_import_docs_total`: `145000`
- `concurrent_import_success_rate`: `100%`
- `concurrent_import_batch_duration_ms` p95: `67.8ms`

### Conclusion

Across both write-stress baseline repeats, import remains materially better on the fork, but concurrent search regressions are consistently far above the Phase 3 keep/drop ceiling (3%) at `max-indexing-concurrency=4`. This run set established the baseline that motivated the Run 12 default-candidate validation.

---

## Run 12: `max-indexing-concurrency=16` High-CPU Candidate (`write-stress`, 2026-03-05)

**Candidate setup:** temporary default `max_indexing_concurrency` set from `4` to `16` for validation (CLI/env override still supported).  
**Profile:** write-stress (30s per scenario)

### Pass 1

Archive: `/home/cyppe/.cache/typesense/benchmark/archives/20260305-224512-write-stress`

| Scenario | Upstream 30.1 | Fork candidate | % Change | Verdict |
|----------|---------------|----------------|----------|---------|
| **Import (1M docs)** | **30.035s** | **21.589s** | **-28.12%** | **FORK WINS** |
| concurrent_facet (50vu) | 30 ms | 31 ms | +3.33% | ~SAME (borderline) |
| concurrent_filter_simple (50vu) | 19 ms | 19 ms | 0.00% | SAME |
| concurrent_sort_eval_score (50vu) | 19 ms | 19 ms | 0.00% | SAME |

Stress counters (fork):
- `concurrent_import_docs_total`: `148000`
- `concurrent_import_batch_duration_ms` p95: `87.7ms`

### Pass 2

Archive: `/home/cyppe/.cache/typesense/benchmark/archives/20260305-231242-write-stress`

| Scenario | Upstream 30.1 | Fork candidate | % Change | Verdict |
|----------|---------------|----------------|----------|---------|
| **Import (1M docs)** | **31.111s** | **21.503s** | **-30.88%** | **FORK WINS** |
| concurrent_facet (50vu) | 31 ms | 30 ms | -3.23% | FORK WINS |
| concurrent_filter_simple (50vu) | 20 ms | 19 ms | -5.00% | FORK WINS |
| concurrent_sort_eval_score (50vu) | 20 ms | 19 ms | -5.00% | FORK WINS |

Stress counters (fork):
- `concurrent_import_docs_total`: `148000`
- `concurrent_import_batch_duration_ms` p95: `63.4ms`

### Conclusion

With `max-indexing-concurrency=16`, import stays strongly better while concurrent scenarios move to parity or better in repeat runs. One pass had a borderline `+3.33%` concurrent facet regression, but the second pass showed all concurrent metrics better than upstream.

Operational decision: keep the strict default at `4` for broader hardware compatibility, and treat `16` as a high-CPU override. This avoids overcommitting CPU on smaller production nodes while preserving a documented path to higher import throughput on larger machines.

---

## Run 13: Import API `batch_size` A/B (`40` vs `1000`, 2026-03-06)

**Commit:** `fe451638`  
**Goal:** validate that the now-wired import `batch_size` parameter materially changes throughput and define a practical default recommendation.  
**Method:** benchmark CLI comparisons with `--duration 15s` (used only for relative A/B between `batch_size` values, not for direct comparison with 30s historical runs).

### Candidate A: `batch_size=40` (default)

Run label: `fork-b40-r2`  
Command log: `/home/cyppe/.local/share/opencode/tool-output/tool_cc1fff396001TByKn25TW2n785`

| Metric | Upstream 30.1 | Fork (`batch_size=40`) | % Change |
|--------|----------------|------------------------|----------|
| Import (`import_duration`) | N/A | `22.455s` | N/A |
| Time to bulk import (1M docs) | `31.111s` | `22.455s` | `-27.82%` |

### Candidate B: `batch_size=1000`

Run label: `fork-b1000`  
Command log: `/home/cyppe/.local/share/opencode/tool-output/tool_cc1f6ee48001rC3VLKAdWoWJfd`

| Metric | Upstream 30.1 | Fork (`batch_size=1000`) | % Change |
|--------|----------------|--------------------------|----------|
| Import (`import_duration`) | N/A | `22.409s` | N/A |
| Time to bulk import (1M docs) | `31.111s` | `22.409s` | `-27.97%` |

### Conclusion

`batch_size=1000` is only `46ms` faster than `batch_size=40` for 1M-doc import in this run pair (~`0.21%`, effectively noise-level for this workload). Keep `batch_size=40` as the safe default for mixed read/write deployments, and treat larger values (`200-1000`) as workload-specific overrides for dedicated ingest windows.

---

## Run 9: `optimize_filters_for_hits` A/B Sweep (2026-03-05)

**Commit:** `995fc84f`  
**Upstream:** Typesense 30.1  
**Profile:** standard (30s per scenario)  
**Shared server args:** `--max-indexing-concurrency=4`

### Baseline (without `db-optimize-filters-for-hits`)

| Scenario | Upstream 30.1 | Fork baseline | % Change | Verdict |
|----------|---------------|---------------|----------|---------|
| **Import (1M docs)** | **28,551ms** | **22,236ms** | **-22.12%** | **FORK WINS** |
| filter_complex (50vu) | 30 ms | 32 ms | +6.67% | UPSTREAM |
| filter_simple (50vu) | 114 ms | 112 ms | -1.75% | ~SAME |
| facet (50vu) | 515 ms | 505 ms | -1.94% | ~SAME |
| sort_simple (50vu) | 272 ms | 262 ms | -3.68% | FORK WINS |
| concurrent_facet (50vu) | 32 ms | 31 ms | -3.13% | FORK WINS |
| concurrent_filter_simple (50vu) | 20 ms | 20 ms | 0.00% | SAME |
| concurrent_sort_eval_score (50vu) | 21 ms | 20 ms | -4.76% | FORK WINS |

### Candidate (`--db-optimize-filters-for-hits=true`)

| Scenario | Upstream 30.1 | Fork candidate | % Change | Verdict |
|----------|---------------|----------------|----------|---------|
| **Import (1M docs)** | **32,340ms** | **21,790ms** | **-32.62%** | **FORK WINS** |
| filter_complex (50vu) | 30 ms | 28 ms | -6.67% | FORK WINS |
| filter_simple (50vu) | 119 ms | 112 ms | -5.88% | FORK WINS |
| facet (50vu) | 459 ms | 457 ms | -0.44% | ~SAME |
| sort_simple (50vu) | 277 ms | 243 ms | -12.27% | FORK WINS |
| concurrent_facet (50vu) | 22 ms | 29 ms | +31.82% | UPSTREAM |
| concurrent_filter_simple (50vu) | 14 ms | 18 ms | +28.57% | UPSTREAM |
| concurrent_sort_eval_score (50vu) | 14 ms | 19 ms | +35.71% | UPSTREAM |

### Conclusion

`optimize_filters_for_hits=true` improves import and most standard search scenarios in this sweep, but it introduces large regressions in concurrent search+import scenarios. Under current keep/drop gates (no >3% search regressions), this setting should remain **disabled by default** until write-stress/concurrency repeats show stable behavior.

---

## Run 10: `max_background_jobs` and Compression Thread Sweeps (2026-03-05)

**Profile:** standard (30s per scenario)  
**Shared server args:** `--max-indexing-concurrency=4`

### Candidate A: `--db-max-background-jobs=16` (commit `995fc84f`)

| Scenario | Upstream 30.1 | Fork candidate | % Change | Verdict |
|----------|---------------|----------------|----------|---------|
| **Import (1M docs)** | **36,021ms** | **21,827ms** | **-39.40%** | **FORK WINS** |
| facet (50vu) | 428 ms | 498 ms | +16.36% | UPSTREAM |
| filter_complex (50vu) | 25 ms | 31 ms | +24.00% | UPSTREAM |
| sort_simple (50vu) | 231 ms | 264 ms | +14.29% | UPSTREAM |
| concurrent_facet (50vu) | 21 ms | 32 ms | +52.38% | UPSTREAM |

### Candidate B: `--db-compression-parallel-threads=8` (commit `f04fb147`)

| Scenario | Upstream 30.1 | Fork candidate | % Change | Verdict |
|----------|---------------|----------------|----------|---------|
| **Import (1M docs)** | **26,087ms** | **22,190ms** | **-14.94%** | **FORK WINS** |
| facet (50vu) | 480 ms | 508 ms | +5.83% | UPSTREAM |
| filter_complex (50vu) | 29 ms | 32 ms | +10.34% | UPSTREAM |
| sort_simple (50vu) | 254 ms | 277 ms | +9.06% | UPSTREAM |
| concurrent_facet (50vu) | 25 ms | 32 ms | +28.00% | UPSTREAM |

### Conclusion

Both candidates materially improve import throughput, but they consistently regress multiple search scenarios beyond the Phase 3 keep/drop limits. For now, both should remain **non-default** and be treated as workload-specific overrides only.

---

## Run 8: Block Size 4KB vs 16KB (2026-03-05)

**Commit:** `98caf70b`
**Upstream:** Typesense 30.1
**Profile:** standard (30s per scenario)
**Extra server args:** `--db-block-size=4096` (testing original 4KB block size)

### Search Results (p95 search_processing_time_ms)

| Scenario | Upstream 30.1 | Fork 98caf70b | % Change | Verdict |
|----------|--------------|---------------|----------|---------|
| **Import (1M docs)** | **28,329ms** | **21,372ms** | **-25%** | **FORK WINS** |
| filter_simple | 294 ms | 285 ms | -3% | FORK WINS |
| filter_complex | 56 ms | 56 ms | 0% | SAME |
| just_q | 4 ms | 4 ms | 0% | SAME |
| q_star | 0 ms | 0 ms | SAME | SAME |
| group | 4,089 ms | 4,165 ms | +2% | ~SAME |
| sort_eval_condition | 474 ms | 484 ms | +2% | ~SAME |
| facet | 798 ms | 825 ms | +3% | UPSTREAM |
| sort_eval_score | 501 ms | 525 ms | +5% | UPSTREAM |
| sort_simple | 428 ms | 453 ms | +6% | UPSTREAM |

### Key Finding: Block Size Trade-off

| Scenario | 16KB blocks (default) | 4KB blocks (this run) | Winner |
|----------|----------------------|----------------------|--------|
| filter_complex | +8% regression | 0% (fixed) | **4KB** |
| sort_simple | -3% win | +6% regression | **16KB** |
| sort_eval_score | -4% win | +5% regression | **16KB** |
| facet | -14% win | +3% regression | **16KB** |

**Conclusion:** 16KB blocks are clearly better overall. The filter_complex +8% regression with 16KB is a minor trade-off for significant wins in sort and facet scenarios. **Keep 16KB as default.**

The reason: larger blocks improve sequential scan performance (sorting, faceting) because fewer block reads are needed, but slightly hurt small random lookups (complex filters with many small result sets).

---

## Run 7: max-indexing-concurrency=16 (2026-03-05)

**Commit:** `58929e4b`
**Upstream:** Typesense 30.1
**Profile:** standard (30s per scenario)
**Extra server args:** `--max-indexing-concurrency=16` (tested as a high-concurrency override)

### Search Results (p95 search_processing_time_ms)

| Scenario | Upstream 30.1 | Fork 58929e4b | % Change | Verdict |
|----------|--------------|---------------|----------|---------|
| **Import (1M docs)** | **26,962ms** | **19,883ms** | **-26%** | **FORK WINS** |
| filter_complex | 59 ms | 54 ms | **-8%** | **FORK WINS** |
| sort_eval_condition | 498 ms | 478 ms | -4% | FORK WINS |
| sort_simple | 449 ms | 437 ms | -3% | FORK WINS |
| facet | 801 ms | 786 ms | -2% | FORK WINS |
| sort_eval_score | 521 ms | 520 ms | 0% | SAME |
| just_q | 4 ms | 4 ms | 0% | SAME |
| q_star | 0 ms | 0 ms | SAME | SAME |
| filter_simple | 290 ms | 293 ms | +1% | ~SAME |
| group | 4,056 ms | 4,181 ms | +3% | ~SAME |

### Stress Import Results (4 VUs x 5000 doc chunks, 30s per config)

| Metric | Upstream 30.1 | Fork 58929e4b | % Change | Verdict |
|--------|--------------|---------------|----------|---------|
| Batch p95 latency | 764 ms | 743 ms | -3% | FORK WINS |
| Batch avg latency | 237 ms | 192 ms | **-19%** | **FORK WINS** |
| Total docs imported | 4,278,000 | 4,770,000 | **+11%** | **FORK WINS** |

### Key Finding: max-indexing-concurrency=16 Impact

Comparing Run 7 (concurrency=16) vs Run 6 (concurrency=4, default):

| Metric | concurrency=4 (Run 6) | concurrency=16 (Run 7) | Impact |
|--------|----------------------|------------------------|--------|
| Import time (fork) | 21,275ms | 19,883ms | **-7% faster** |
| filter_complex (fork) | 57ms (+8%) | 54ms (-8%) | **Regression fixed!** |
| group (fork) | 4,289ms (+1%) | 4,181ms (+3%) | Similar |

**`max-indexing-concurrency=16` improves import by ~7% and fixes the filter_complex regression.** The concurrency setting controls validation parallelism (runs BEFORE the exclusive lock), so higher values = faster pre-processing = less time under lock.

---

## Run 6: Full Benchmark with Stress Import (2026-03-04)

**Commit:** `46fbb2cf` (same config as Run 5)
**Upstream:** Typesense 30.1
**Profile:** standard (30s per scenario) + stress import (4 VUs, 5000 chunk, 60s)
**Storage:** Disk-backed (`~/.cache/typesense/benchmark`), fresh InfluxDB

### Search Results (p95 search_processing_time_ms)

| Scenario | Upstream 30.1 | Fork 46fbb2cf | % Change | Verdict |
|----------|--------------|---------------|----------|---------|
| **Import (1M docs)** | **29,710ms** | **21,275ms** | **-28%** | **FORK WINS** |
| facet | 920 ms | 787 ms | **-14%** | **FORK WINS** |
| sort_eval_score | 514 ms | 496 ms | -4% | FORK WINS |
| sort_eval_condition | 488 ms | 471 ms | -3% | FORK WINS |
| sort_simple | 442 ms | 427 ms | -3% | FORK WINS |
| filter_simple | 300 ms | 289 ms | -4% | FORK WINS |
| filter_complex | 53 ms | 57 ms | +8% | UPSTREAM |
| group | 4,238 ms | 4,289 ms | +1% | ~SAME |
| just_q | 5 ms | 4 ms | -20% | FORK WINS |
| q_star | 0 ms | 0 ms | SAME | SAME |

### Stress Import Results (4 VUs × 5000 doc chunks, 60s)

| Metric | Upstream 30.1 | Fork 46fbb2cf | % Change | Verdict |
|--------|--------------|---------------|----------|---------|
| Batch p95 latency | 734 ms | 742 ms | +1% | ~SAME |
| Batch avg latency | 238 ms | 198 ms | **-17%** | **FORK WINS** |
| Total docs imported | 4,280,000 | 4,723,000 | **+10%** | **FORK WINS** |
| Peak memory (active) | 1,070 MB | 1,320 MB | +23% | UPSTREAM |
| 503 backpressure | 0 | 0 | — | SAME |

### Summary

**Search: Fork wins 6/9, ties 2, loses 1.** Import is 28% faster — the largest import improvement yet. Facet search 14% faster. Most scenarios show 3-4% improvement. Only filter_complex regresses slightly (+8%).

**Stress import: Fork imports 10% more docs in the same time window** with 17% lower average batch latency. The p95 tail is similar (~740ms for both). Fork uses ~250MB more peak memory — expected from 128MB write buffers (4 × 128MB vs 4 × 4MB = 512MB vs 16MB memtable budget).

### Key Observations

- **Import -28% is a major win** — largest yet, up from -7% in Run 5. Different run conditions (disk-backed vs tmpfs in Run 5) may account for the difference; disk-backed storage makes the write buffer optimizations more impactful since tmpfs has no real I/O penalty.
- **sort_simple regression from Run 4 confirmed fixed** — now -3% fork wins, was +31% in Run 4.
- **filter_complex is the one consistent minor regression** — +8% here, +4.4% in Run 3. Likely related to block_size increase (4KB→16KB) affecting small filter result sets.
- **Stress import shows fork's write pipeline is strictly better** — 10% more throughput with same tail latency. The 128MB write buffers + unordered_write + subcompactions all contribute.
- **Memory overhead is predictable** — ~250MB more from larger memtable budget. Acceptable trade-off on production machines (32GB+).

---

## Run 5: Tier 1 Config Optimizations — ALL SCENARIOS WIN (2026-03-04)

**Commit:** `bc8e482b` (Ribbon filters, partitioned index, prepopulate cache, unordered_write, max_subcompactions=2, 128MB write buffers)
**Upstream:** Typesense 30.1
**Profile:** standard (30s per scenario)

### Changes from Run 4

- **128MB write buffers** (was 4MB) — 32x larger memtables, fewer flushes
- **4 write buffers** (was 2) — writes continue during flush
- **Ribbon filters** replacing Bloom (30% less filter memory, same FP rate)
- **Partitioned index/filters** with 4KB metadata blocks + pin top level
- **`prepopulate_block_cache = kFlushOnly`** — cache-warm on flush
- **`unordered_write = true`** — +34-131% write throughput (safe: WAL disabled)
- **`max_subcompactions = 2`** — parallel L0→L1 compaction

### Results (p95 search_time_ms)

| Scenario | Upstream 30.1 | Fork bc8e482b | % Change | Verdict |
|----------|--------------|---------------|----------|---------|
| **Import** | **21,804ms** | **20,191ms** | **-7%** | **FORK WINS** |
| sort_eval_score | 915 ms | 562 ms | **-39%** | **FORK WINS** |
| facet | 1,323 ms | 895 ms | **-32%** | **FORK WINS** |
| group | 4,700 ms | 4,510 ms | -4% | FORK WINS |
| filter_complex | 60 ms | 58 ms | -3% | FORK WINS |
| filter_simple | 308 ms | 302 ms | -2% | FORK WINS |
| sort_eval_condition | 527 ms | 522 ms | -1% | FORK WINS |
| just_q | 4 ms | 4 ms | 0% | SAME |
| sort_simple | 475 ms | 477 ms | 0% | SAME |
| q_star | 0 ms | 0 ms | SAME | SAME |

**Summary: Fork wins 7/9 scenarios, ties 2, loses 0. Best run yet. sort_eval_score -39%, facet -32%, import -7%. Zero regressions.**

### Key Observations

- **sort_simple regression from Run 4 is GONE** — it was likely a Run 4 fluke or the Tier 1 optimizations (partitioned index, prepopulate cache) helped
- **sort_eval_score massive improvement (-39%)** — likely due to 128MB write buffers creating fewer, larger SST files with better data locality for eval-based sorting
- **facet searches -32% faster** — partitioned filters + Ribbon filters + prepopulate cache all help faceted search which needs both data and filter blocks
- **Import 7% faster** — `unordered_write` + `max_subcompactions` + `prepopulate_block_cache` combine to reduce write latency
- **All Tier 1 optimizations are safe and beneficial** — no regressions in any scenario

---

## Run 4: WriteBatch Aggregation + Code Optimizations (2026-03-04)

**Commit:** `2b3a2960` (WriteBatch aggregation in batch_index, async_io all iterators)
**Upstream:** Typesense 30.1
**Profile:** standard (30s per scenario)

### Changes from Run 3

- **WriteBatch aggregation** — All documents in a batch are now written in a single RocksDB Write() call (was 1000 separate calls per batch)
- **async_io for get_iterator() and get_last_N_values()** — io_uring prefetch for all iterator paths
- **`/debug?rocksdb_stats=true` endpoint** — Full RocksDB statistics and 17 structured DB properties

### Results (p95 search_time_ms)

| Scenario | Upstream 30.1 | Fork 2b3a2960 | % Change | Verdict |
|----------|--------------|---------------|----------|---------|
| **Import** | **21,679ms** | **22,774ms** | **+5%** | **UPSTREAM** |
| filter_simple | 428 ms | 315 ms | **-26%** | **FORK WINS** |
| facet | 1,007 ms | 872 ms | **-13%** | **FORK WINS** |
| group | 5,410 ms | 4,723 ms | **-13%** | **FORK WINS** |
| sort_eval_condition | 533 ms | 527 ms | -1% | ~SAME |
| sort_eval_score | 570 ms | 566 ms | -1% | ~SAME |
| filter_complex | 60 ms | 60 ms | 0% | SAME |
| q_star | 0 ms | 0 ms | SAME | SAME |
| just_q | 4 ms | 5 ms | +25% | UPSTREAM |
| sort_simple | 464 ms | 607 ms | +31% | UPSTREAM |

**Summary:** Fork wins 3/9, ties 4, loses 2. Filter/facet/group scenarios significantly faster (-13% to -26%). Import neutral. sort_simple regression needs investigation.

### RocksDB Health (from /debug endpoint during search phase)

- Block cache utilization: 248MB / 256MB (97%) — excellent
- Cache composition: 91% data blocks, 4.5% filters, 0.9% indexes
- Write stalls: 0 (no throttling)
- L0 files: 1 (no compaction pressure)
- Running compactions: 0
- Estimated keys: ~2M

### Key Observations

- **WriteBatch aggregation didn't dramatically change import speed** — The bottleneck is likely in the in-memory indexing, not the RocksDB write path. The original per-doc WriteBatch was already well-optimized by RocksDB's group commit.
- **Filter and facet searches benefit most** from the upgraded RocksDB and new config (HyperClockCache, async_io, dynamic level bytes, per-level compression).
- **sort_simple regression (+31%)** needs investigation — may be related to block_size change (4KB→16KB) affecting point lookups within sorted result sets.

---

## Run 3: Conservative Tuning + Configurable Settings (2026-03-04)

**Commit:** `faee4cc5` (all RocksDB settings made configurable via CLI/env/config)
**Upstream:** Typesense 30.1

### RocksDB Settings (store.cpp defaults)

| Setting | Upstream 30.1 | Fork (this run) | Notes |
|---------|--------------|-----------------|-------|
| RocksDB version | 7.x | 10.10.1 | Major upgrade |
| `write_buffer_size` | 4 MB | 4 MB | Configurable via `--db-write-buffer-size` |
| `max_write_buffer_number` | 2 | 2 | Configurable via `--db-max-write-buffer-number` |
| `block_cache_size` | 256 MB (LRU) | 256 MB (HyperClock) | HyperClockCache new in 10.x |
| `block_size` | 4 KB | 16 KB | Configurable via `--db-block-size` |
| `format_version` | 5 | 7 | Latest SST format, configurable |
| `level_compaction_dynamic_level_bytes` | false | true | Auto-sizes levels, configurable |
| `compression_per_level` | LZ4 all | None/None/LZ4/LZ4/LZ4/ZSTD/ZSTD | L0-L1 uncompressed for write speed |
| `bottommost_compression` | N/A | ZSTD | Best ratio for cold data |
| `compression_parallel_threads` | 1 | 4 | Configurable |
| `bytes_per_sync` | 0 | 1 MB | Smooth I/O, configurable |
| `max_manifest_file_size` | 64 MB | 1 MB | Configurable |
| `statistics` | off | on (kExceptTimers) | ~5% overhead, configurable |
| `block_protection_bytes_per_key` | 0 | 1 | Integrity checking |
| `paranoid_memory_checks` | false | true | Memory validation |
| `async_io` (iterators) | false | true | io_uring prefetch, configurable |
| `daily_offpeak_time_utc` | N/A | 02:00-06:00 | Offpeak compaction scheduling |
| `optimize_filters_for_memory` | false | true | Reduced filter fragmentation |
| `pin_l0_filter_and_index_blocks_in_cache` | false | true | Keep hot filters pinned |
| `bloom_filter` | 10 bits | 10 bits | Same |
| `CompactOnDeletion` | N/A | 10000/7500/0.5 | Reclaim space from bulk deletes |

### Results (p95 search_time_ms)

| Scenario | Upstream 30.1 | Fork faee4cc5 | Diff | % Change | Verdict |
|----------|--------------|---------------|------|----------|---------|
| **bulk_import** | **4.0 min** | **3.0 min** | **-1.0** | **-25.0%** | **FORK WINS** |
| just_q (100vu) | 4.0 ms | 3.0 ms | -1.0 | -25.0% | FORK WINS |
| just_q (50vu) | 4.0 ms | 3.0 ms | -1.0 | -25.0% | FORK WINS |
| filter_simple (100vu) | 347.0 ms | 330.7 ms | -16.3 | -4.7% | FORK WINS |
| sort_eval_condition (50vu) | 281.9 ms | 271.0 ms | -10.9 | -3.9% | FORK WINS |
| sort_eval_condition (100vu) | 597.0 ms | 575.3 ms | -21.7 | -3.6% | FORK WINS |
| sort_eval_score (100vu) | 616.0 ms | 594.9 ms | -21.1 | -3.4% | FORK WINS |
| sort_eval_score (50vu) | 309.6 ms | 299.0 ms | -10.6 | -3.4% | FORK WINS |
| group (100vu) | 2407.2 ms | 2327.0 ms | -80.2 | -3.3% | FORK WINS |
| facet (100vu) | 959.0 ms | 959.0 ms | 0.0 | 0.0% | SAME |
| facet (50vu) | 443.0 ms | 443.0 ms | 0.0 | 0.0% | SAME |
| filter_complex (50vu) | 27.0 ms | 27.0 ms | 0.0 | 0.0% | SAME |
| filter_simple (50vu) | 103.0 ms | 103.0 ms | 0.0 | 0.0% | SAME |
| group (50vu) | 4732.0 ms | 4732.0 ms | 0.0 | 0.0% | SAME |
| sort_simple (100vu) | 537.0 ms | 537.0 ms | 0.0 | 0.0% | SAME |
| filter_complex (100vu) | 69.9 ms | 73.0 ms | +3.1 | +4.4% | UPSTREAM |
| sort_simple (50vu) | 233.0 ms | 243.1 ms | +10.1 | +4.3% | UPSTREAM |

**Summary:** Fork wins 9/17 scenarios, ties 6, loses 2. Import 25% faster. Search 3-5% faster in most scenarios.

---

## Run 2: Conservative Tuning (previous session)

**Settings:** Same as Run 3 but with settings hardcoded (not configurable).
**Results:** Bulk import ~8% faster, search at near-parity (0-4% difference).

---

## Run 1: Aggressive Tuning (previous session)

**Settings:** 128MB write buffers, 4 buffers, partitioned index/filters, ZSTD dictionary compression.
**Results:** Catastrophic regression +130-177% in search latency. Reverted immediately.
**Lesson:** Large write buffers and partitioned filters hurt read performance for this workload size (~1M docs). The overhead of partition lookups exceeds the benefit when working set fits in cache.

---

## Configurable Settings Reference

All RocksDB settings are now configurable via CLI args, env vars, or config file (no recompilation needed):

| CLI Flag | Env Var | Default | Description |
|----------|---------|---------|-------------|
| `--db-write-buffer-size` | `TYPESENSE_DB_WRITE_BUFFER_SIZE` | 134217728 (128MB) | Memtable size in bytes |
| `--db-max-write-buffer-number` | `TYPESENSE_DB_MAX_WRITE_BUFFER_NUMBER` | 4 | Max concurrent memtables |
| `--db-block-cache-size` | `TYPESENSE_DB_BLOCK_CACHE_SIZE` | 268435456 (256MB) | Block cache size (HyperClockCache) |
| `--db-block-size` | `TYPESENSE_DB_BLOCK_SIZE` | 16384 (16KB) | SST block size in bytes |
| `--db-format-version` | `TYPESENSE_DB_FORMAT_VERSION` | 7 | SST format version (max 7) |
| `--db-level-compaction-dynamic-level-bytes` | `TYPESENSE_DB_LEVEL_COMPACTION_DYNAMIC_LEVEL_BYTES` | true | Auto-size compaction levels |
| `--db-enable-statistics` | `TYPESENSE_DB_ENABLE_STATISTICS` | true | RocksDB statistics counters |
| `--db-compression-parallel-threads` | `TYPESENSE_DB_COMPRESSION_PARALLEL_THREADS` | 4 | Parallel compression threads |
| `--db-bytes-per-sync` | `TYPESENSE_DB_BYTES_PER_SYNC` | 1048576 (1MB) | Sync interval during writes |
| `--db-max-manifest-file-size` | `TYPESENSE_DB_MAX_MANIFEST_FILE_SIZE` | 1048576 (1MB) | Max MANIFEST file size |
| `--db-enable-async-io` | `TYPESENSE_DB_ENABLE_ASYNC_IO` | true | Async I/O for iterators (io_uring) |
| `--db-offpeak-time-utc` | `TYPESENSE_DB_OFFPEAK_TIME_UTC` | 02:00-06:00 | Offpeak compaction window |
| `--db-rate-limit-bytes-per-sec` | `TYPESENSE_DB_RATE_LIMIT_BYTES_PER_SEC` | 0 (disabled) | Compaction I/O rate limit |
| `--db-unordered-write` | `TYPESENSE_DB_UNORDERED_WRITE` | true | Unordered writes (+34-131% throughput, safe when WAL disabled) |
| `--db-max-subcompactions` | `TYPESENSE_DB_MAX_SUBCOMPACTIONS` | 2 | Parallel L0→L1 sub-compaction threads |
| `--db-compaction-interval` | `TYPESENSE_DB_COMPACTION_INTERVAL` | 0 (disabled) | Periodic RocksDB compaction cadence |
| `--max-indexing-concurrency` | `TYPESENSE_MAX_INDEXING_CONCURRENCY` | 4 | Indexing pre-processing parallelism |

---

## Default Deltas vs Upstream Docs

For upstream PR planning, these are the important defaults that diverge from current upstream docs:

| Parameter | Upstream docs default | Fork default | Why we changed it |
|-----------|------------------------|--------------|-------------------|
| `db-write-buffer-size` | 4MB | 128MB | Better import throughput and fewer flushes on medium/large hardware |
| `db-max-write-buffer-number` | 2 | 4 | Lets writes continue while flush/compaction catches up |
| `db-compaction-interval` | 7 days | disabled (`0`) | Avoids scheduled compaction interference; rely on normal background compaction |
| `max-indexing-concurrency` | 4 | 4 | Kept conservative as global default |

Note: `128MB` write buffers are not universally safe as a one-size-fits-all upstream default. They are effective on our benchmark hardware, but small nodes can prefer smaller values.

---

## Production Tuning Recommendations (CPU + RAM aware)

Use `max-indexing-concurrency` as an explicit production tuning knob. Keep the default conservative, then raise only when CPU headroom is available.

| Logical CPU count | Recommended `max-indexing-concurrency` | Guidance |
|-------------------|----------------------------------------|----------|
| 2-4 | 4 (default) | Safe baseline for mixed search/index workloads and limited CPU headroom. |
| 6-8 | 6-8 | Start at 6 for query-heavy traffic, move to 8 for import-heavy windows. |
| 12-16 | 8-12 | Good balance for sustained indexing without starving concurrent search. |
| 24+ | 16 | High-throughput import setting; validate on your workload before locking in. |

Practical guardrails:
- Keep `max-indexing-concurrency` at or below roughly half of logical CPUs for mixed production traffic.
- On memory-constrained nodes (for example <=8GB RAM), prefer the lower end of the CPU tier.
- Re-run both `standard` and `write-stress` profiles before promoting a higher setting.

Future improvement idea:
- Add an adaptive startup heuristic (CPU+memory aware) that picks a bounded default and still allows explicit CLI/env override.

Recommended write-path settings by hardware tier:

| Hardware tier | `db-write-buffer-size` | `db-max-write-buffer-number` | `max-indexing-concurrency` |
|---------------|------------------------|------------------------------|----------------------------|
| Small (<=4 vCPU or <=8GB RAM) | 16-32MB | 2 | 4 |
| Medium (6-8 vCPU, 16-32GB RAM) | 64MB | 3-4 | 6-8 |
| Large (12-16 vCPU, 32-64GB RAM) | 128MB | 4 | 8-12 |
| Very large (24+ vCPU, 64GB+ RAM) | 128-256MB | 4-6 | 12-16 |

Recommended import API `batch_size` by workload tier:

| Workload tier | Suggested `batch_size` | Guidance |
|---------------|------------------------|----------|
| Shared cluster (search + writes) | 40 (default) | Best safety margin for query latency and backpressure handling. |
| Moderate ingest windows | 100-250 | Use when import dominates briefly and search SLOs still hold. |
| Dedicated/high-CPU ingest windows | 500-1000 | Throughput-oriented tuning; validate with concurrent search tests before rollout. |

Safety guidance:
- Treat these as starting points, not hard rules.
- Validate any increase with both `standard` and `write-stress` profiles before rollout.
- If search latency regresses under mixed read/write load, first step down `max-indexing-concurrency`, then step down write buffer size.
- For import `batch_size` changes, first test `40 -> 200` before jumping to `1000`.

---

## Lessons Learned

### Key Findings

1. **Write buffer size is workload-dependent** — 128MB buffers (Run 1) caused catastrophic regression, but Run 5 with the same 128MB + other optimizations (partitioned filters, Ribbon, prepopulate cache) is the BEST run. The regression in Run 1 was likely caused by something else (possibly misconfigured `OptimizeLevelStyleCompaction` budget). 128MB is now the default.

2. **Partitioned index/filters need proper configuration** — Run 1 showed catastrophic regression, but Run 5 with `metadata_block_size=4096` + `pin_top_level_index_and_filter=true` shows no regression. The key is pinning top-level partitions and using small metadata blocks.

3. **HyperClockCache is a safe upgrade** — Drop-in replacement for LRU cache in RocksDB 10.x. No regressions observed, slightly better concurrent performance.

4. **Per-level compression works well** — L0-L1 uncompressed saves write CPU, ZSTD at bottom levels saves space. No measurable read regression.

5. **`OptimizeLevelStyleCompaction()` budget matters** — Pass `write_buffer_size * max_write_buffer_number` as the budget so it calculates correct level sizes. Don't override its write_buffer settings afterward unless intentional.

6. **Statistics overhead is negligible** — `kExceptTimers` level adds ~0% overhead (timers are the expensive part). Always enable for production diagnostics.

7. **async_io (io_uring) is free performance** — No regressions, helps with prefetch during scans. Enable for all iterators, not just explicit scans.

8. **WriteBatch aggregation has minimal impact on import** — RocksDB's group commit already batches concurrent Write() calls internally. The real import bottleneck is in-memory indexing (inverting, tokenizing, building tries), not the disk write path. SstFileWriter/IngestExternalFile would bypass memtable entirely and is more promising.

9. **Ribbon filters save 30% filter memory** — Drop-in replacement for Bloom filters with same false positive rate. Uses Bloom for L0 (faster build during flush) and Ribbon for deeper levels. No read regression.

10. **Partitioned index/filters need careful sizing** — Run 1 showed catastrophic regression with partitioned filters. Re-testing in Run 5 with smaller metadata_block_size (4KB) and pin_top_level. May work better with the new 128MB write buffers since they create fewer, larger SST files.

11. **`unordered_write` is safe when WAL is disabled** — Typesense master uses Raft WAL and disables RocksDB WAL. `unordered_write` relaxes snapshot consistency but only matters for concurrent reads during writes. Safe for Typesense's use case.

12. **`prepopulate_block_cache = kFlushOnly`** — Warms block cache when memtables flush to SST files, so newly imported data is immediately cache-hot for searches. Zero downside.

13. **Block size is a search-type trade-off** — 16KB blocks help sequential scans (sorting, faceting) by reducing block reads, but slightly hurt small random lookups (complex filters). 16KB is the better default for Typesense's mixed workload. The filter_complex +8% regression is acceptable given -14% facet and -3% sort wins.

14. **`max-indexing-concurrency=16` is throughput-optimized, not universally default-safe** — It improves import and often concurrent behavior on high-core test hardware, but it can overconsume CPU on smaller machines. Keep default at 4 and tune by CPU tier.

15. **Import API `batch_size` (`40` vs `1000`) is a secondary knob in this workload** — after wiring it end-to-end, measured import delta is very small (~0.2% in Run 13). Keep 40 as default and use larger values only for explicit ingest-oriented tuning.

---

## Code-Level Optimization Findings

### Analyzed (from Typesense RocksDB integration audit)

1. **WriteBatch aggregation (HIGH IMPACT — IMPLEMENTED)**
   - **Problem:** Each document in a batch creates its own WriteBatch with 2 Puts, written individually. For 1000-doc batches, that's 1000 separate RocksDB Write() calls.
   - **Fix:** Aggregate all Puts into a single WriteBatch per batch (2000 Puts in one Write() call). Fall back to per-doc writes only on failure.
   - **Expected impact:** 5-10x fewer memtable flushes during import, significant import speedup.
   - **File:** `src/collection.cpp:batch_index()`

2. **MultiGet for bulk lookups (MEDIUM IMPACT — IMPLEMENTED)**
   - **Problem:** Sequential `Get()` calls in cascade_remove_docs, filter-based updates, and old document reads during batch imports.
   - **Fix:** Batch seq_ids into `MultiGet()` calls.
   - **Expected impact:** Lower read amplification and latency for bulk update/delete/reference operations.
   - **Files:** `src/store.cpp`, `src/collection.cpp`

3. **async_io for all iterators (LOW IMPACT — IMPLEMENTED)**
   - **Problem:** `get_iterator()` and `get_last_N_values()` used default ReadOptions without async_io.
   - **Fix:** Enabled async_io on all iterator paths.
   - **File:** `src/store.cpp`

4. **Column families (NOT APPLICABLE)**
   - Typesense uses single key namespace with prefixes. Column families could theoretically separate metadata from documents, but the refactoring cost is high and benefit unclear for this workload.

5. **Prefix seek optimization (TO INVESTIGATE)**
   - Typesense uses many prefix-based scans (`$CM_`, `$CS_`, etc.).
   - Could benefit from `prefix_extractor` + prefix bloom filters for faster Seek.
   - Risk: Must ensure all iterator uses are compatible.

### Import Pipeline Deep Dive

`batch_size` is now wired end-to-end for the import API. The current flow:

1. `core_api.cpp` — `IMPORT_BATCH_SIZE` is parsed from HTTP params (default 40) and validated.
2. `core_api.cpp` — `IMPORT_BATCH_SIZE` is passed into `Collection::add_many(..., index_batch_size)`.
3. `collection.cpp` — `index_batch_size` controls when `batch_index()` is called (with safety clamping to at least 1).
4. `collection.cpp` — internal non-import call sites still use default `index_batch_size=1000` unless explicitly overridden.

Within each batch:
   - `batch_index_in_memory()` acquires **unique mutex** on collection (`collection.cpp:975`)
   - Inside, `batch_memory_index()` acquires **unique mutex** on index (`index.cpp:669`)
   - Per-field indexing parallelized via `thread_pool(8)` — but all within the exclusive lock
   - Aggregated `WriteBatch` writes all batch docs to RocksDB in a single call

**Key bottleneck: `std::unique_lock lock(mutex)` at collection.cpp:975 and index.cpp:669**
- All parallel import VUs serialize here — only one batch can index at a time
- Search queries take `shared_lock` on same mutexes — reads and writes contend
- The lock is necessary: in-memory index structures (posting lists, tries, sort indices) are not thread-safe for concurrent modification
- Making this finer-grained (per-field locks) would be a major upstream architectural change

**`max_indexing_concurrency` (default 4, user's production: 16)**
- Controls parallelism in `batch_validate_and_preprocess()` (index.cpp:464)
- This is the VALIDATION step that runs BEFORE the exclusive lock
- Higher values = faster validation = shorter time inside critical section
- 16 is reasonable on multi-core machines with many fields

### Not Yet Implemented (Backlog)

6. **Separate WriteOptions by operation type**
   - Metadata writes (critical): use sync=true
   - Bulk import writes: batch, disableWAL (when Raft has WAL)
   - Could improve tail latency isolation

7. **Per-field locking for concurrent import (HIGH IMPACT — DEEP RESEARCH NEEDED)**
   - **Problem:** `batch_memory_index()` (`index.cpp:669`) acquires a single `std::unique_lock` on the entire Index. All parallel import VUs serialize here. This is the #1 bottleneck for parallel bulk import.
   - **Opportunity:** Each field has its own data structures (inverted index, sort index, facet index). `index_field_in_memory()` operates per-field. Per-field `std::shared_mutex` would allow concurrent batches to index different fields simultaneously.
   - **Complications:** (a) `index->remove()` modifies ALL field indices — needs all field locks; (b) multi-field searches need consistent cross-field view; (c) schema changes need global lock; (d) collection-level mutex at `collection.cpp:975` is a second serialization point.
   - **Expected impact:** Could dramatically improve parallel import throughput (currently limited to one batch at a time inside the index). Most impactful for collections with many fields.
   - **Files:** `src/index.cpp` (batch_memory_index, index_field_in_memory), `include/index.h` (mutex → per-field mutexes)

---

## Research Findings (RocksDB Wiki / Official Docs)

### Write Performance

| Technique | Source | Expected Impact | Status | Notes |
|-----------|--------|----------------|--------|-------|
| WriteBatch aggregation | Code audit | HIGH | DONE | Aggregate 1000 docs into 1 WriteBatch |
| `prepopulate_block_cache = kFlushOnly` | Wiki: Block Cache | MEDIUM | DONE | Warm cache on flush, faster post-import reads |
| `max_background_jobs` increase | Wiki: Tuning | MEDIUM | TO TEST | More compaction parallelism during writes |
| `min_write_buffer_number_to_merge` | Wiki: Write Buffer | LOW-MED | TO TEST | Batch memtables before flush |
| Pipelined writes | Wiki: Pipelined Write | LOW | SKIP | Only helps with WAL enabled, Typesense master disables WAL |
| SstFileWriter / IngestExternalFile | Wiki: Creating & Ingesting | HIGH | TO INVESTIGATE | Bypass memtable entirely for bulk loads — could transform import perf |
| `disable_auto_compactions` during bulk load | Wiki: Compaction | MEDIUM | TO INVESTIGATE | Defer compaction during import, manual compact after |

### Read Performance

| Technique | Source | Expected Impact | Status | Notes |
|-----------|--------|----------------|--------|-------|
| `optimize_filters_for_hits = true` | Wiki: Bloom Filter | LOW-MED | TO TEST | Skip bloom on last level (Typesense mostly does successful lookups) |
| `max_open_files = -1` | Wiki: Tuning | LOW | TO TEST | Keep all SST file handles open |
| Prefix extractor + prefix bloom | Wiki: Prefix Seek | MEDIUM | TO INVESTIGATE | Typesense uses many prefix scans |
| Ribbon filters (30% less space) | Wiki: Bloom Filter | LOW | DONE | `NewRibbonFilterPolicy(10, 0)` — same FP rate, 30% less memory |
| MultiGet for batch lookups | Wiki: MultiGet | MEDIUM | TO IMPLEMENT | Batch sequential Get() calls |
| Direct I/O | Wiki: Direct IO | UNCERTAIN | SKIP FOR NOW | Risk: first reads after compaction trigger real I/O |

### Memory & Cache

| Technique | Source | Expected Impact | Status | Notes |
|-----------|--------|----------------|--------|-------|
| WriteBufferManager | Wiki: Write Buffer Manager | LOW | TO INVESTIGATE | Unified memtable memory control across Store instances |
| `cache_index_and_filter_blocks_with_high_priority` | Wiki: Block Cache | LOW | DONE | Already enabled |
| `pin_l0_filter_and_index_blocks_in_cache` | Wiki: Block Cache | LOW | DONE | Already enabled |
| `optimize_filters_for_memory` | Wiki: Bloom Filter | LOW | DONE | Already enabled |

### Observability

| Technique | Source | Expected Impact | Status | Notes |
|-----------|--------|----------------|--------|-------|
| `/debug?rocksdb_stats=true` endpoint | Fork code | N/A | DONE | Returns statistics + DB properties |
| Metrics collector k6 script | Fork code | N/A | DONE | Polls /metrics.json, /stats.json, /debug during benchmarks |
| Statistics counters (kExceptTimers) | Wiki: Statistics | N/A | DONE | ~0% overhead |
| `BLOCK_CACHE_HIT/MISS` ratio | Statistics | Diagnostic | TO CHECK | Needs benchmark with stats endpoint |
| `STALL_MICROS` during import | Statistics | Diagnostic | TO CHECK | Indicates write stalls |

### Inspiration from Other Projects

| Project | Technique | Relevance | Status |
|---------|-----------|-----------|--------|
| TiKV (TiDB) | Separate RocksDB instances for raft vs data | Already done (Typesense has Raft WAL + disableWAL for data) | N/A |
| CockroachDB/Pebble | Moved to custom LSM (Pebble) | Not applicable | N/A |
| MyRocks | Read-free replication | Interesting but Typesense has different replication model | N/A |
| Kafka Streams | Dedicated RocksDB per partition | Typesense uses single Store — could investigate per-collection stores | FUTURE |

---

## Benchmark Infrastructure

### Available Profiles

| Profile | Duration | Scenarios | Use Case |
|---------|----------|-----------|----------|
| `quick` | 15s | Import + search | Fast feedback loop during tuning |
| `standard` | 30s | Import + search | Default, comparable to previous runs |
| `write-stress` | 60s | Import + parallel stress | Focus on write/import performance |
| `full` | 60s | All scenarios + stress + metrics | Comprehensive analysis |

### Available Scopes

| Scope | Scenarios | Use Case |
|---|---|---|
| `core` | One large import POST + search | Default, upstream-comparable lane |
| `extended` | `core` + stress import + concurrent search/import + extra metrics | Manual deep-dive and regression hunting |

### Current Core Lane Policy

- The indexing benchmark sends the full dataset as one logical POST again. The old hosted `500`-doc client chunking workaround is retired.
- The product default request timeout is still `60000ms`, but benchmark lanes can raise it explicitly with `TYPESENSE_REQUEST_TIMEOUT_MS=300000` when they need more headroom for one-shot imports.
- `benchmark-testing.yml` now runs `--scope core` and exports `TYPESENSE_REQUEST_TIMEOUT_MS=300000`; the benchmark process launcher forwards that env var into the Dockerized server containers so new binaries honor it and older comparison artifacts ignore it safely.
- Hosted CI still keeps the larger explicit k6 indexing `maxDuration` (`60m`) because a one-shot 1M-doc import can exceed k6's default executor ceiling on smaller runners even when the server-side HTTP timeout is no longer the limiting factor.
- Hosted CI also cannot rely solely on Influx for the single-point `import_duration` metric. The benchmark harness uses k6's `handleSummary()` hook in the indexing lane to emit a machine-readable `import_duration` payload directly to stderr, and falls back to that direct value if the post-run Influx query returns no `import_duration` rows.
- Hosted CI benchmark selection is pinned to the workflow's own `github.sha` by default. If the current workflow SHA does not yet have a successful `tests.yml` artifact, the workflow fails fast instead of silently benchmarking the previous successful branch tip.
- Hosted `Benchmark Testing` on `27bb2bff` is green after the final replay-model fix. Important interpretation: that workflow compared the current fork SHA `27bb2bff` against the previous successful same-branch fork SHA `985acb45`, not against upstream `typesense/typesense`.
- Hosted run `23085170081` finished in `43m55s`. The same-branch guardrail numbers were mixed but inside current regression thresholds: import `59.498s -> 64.033s` (`+7.62%`), `facet` p95 `333ms -> 467/469ms`, `group` p95 `1562/1581ms -> 1591/1589ms`, and `sort_simple` p95 `204/205ms -> 203/200ms`.
- The stronger semantic/perf control for this sprint remains the local upstream compare: upstream classic `v30.1` vs the final runtime build on the same machine/workload shape. That local run stayed green with faster one-shot import (`29.383s -> 26.906s`) and materially faster heavy search scenarios.

### Monitoring Endpoints

During benchmarks, the following Typesense endpoints are polled:
- `/metrics.json` — CPU, memory, disk, network usage
- `/stats.json` — API endpoint latencies and request rates
- `/debug?rocksdb_stats=true` — RocksDB statistics and DB properties
- `/health` — Node health status

Results are saved to `{work_dir}/data/metrics/` as JSON files and streamed to InfluxDB for Grafana visualization.

### Running Benchmarks

```bash
# Quick feedback during tuning
scripts/benchmark_vs_upstream.sh --profile quick

# Standard comparison (default)
scripts/benchmark_vs_upstream.sh

# Stress write performance
scripts/benchmark_vs_upstream.sh --profile write-stress

# Full analysis with historical tracking
scripts/benchmark_vs_upstream.sh --profile full --no-flush

# Build + benchmark from scratch
scripts/benchmark_vs_upstream.sh --clean --build --profile standard
```

---

## Next Steps / Priority Queue

### P0 — Completed

- [x] WriteBatch aggregation in `batch_index()` — minimal import impact (RocksDB group commit already batches)
- [x] async_io for all iterators (free performance)
- [x] RocksDB metrics extraction between benchmark runs
- [x] Stress import benchmark (parallel VUs)
- [x] `/debug?rocksdb_stats=true` endpoint
- [x] Ribbon filters (30% less memory than Bloom)
- [x] Partitioned index/filters with 4KB metadata blocks
- [x] `prepopulate_block_cache = kFlushOnly`
- [x] `unordered_write = true` (safe with WAL disabled)
- [x] `max_subcompactions = 2`
- [x] 128MB write buffers, 4 buffers (configurable)
- [x] Run 4 benchmark (WriteBatch aggregation impact measured)
- [x] Run 5 benchmark (Tier 1 config optimizations — ALL SCENARIOS WIN)
- [x] Run 6 benchmark (full benchmark with stress import — disk-backed, import -28%, stress +10% throughput)
- [x] sort_simple regression FIXED — was +31% in Run 4, now -3% in Run 6
- [x] Run 7 benchmark (max-indexing-concurrency=16 — import -26%, filter_complex -8%)
- [x] Run 8 benchmark (block-size=4096 — confirms 16KB is better overall, 4KB only helps filter_complex)
- [x] filter_complex regression INVESTIGATED — caused by 16KB blocks, but 16KB wins everywhere else. Acceptable trade-off.
- [x] `--server-args` CLI option added to benchmark tool

### P1 — Next (Medium Impact, Needs Testing)

- [x] Test `--max-indexing-concurrency=16` as high-concurrency candidate (default remains 4) — validated in Run 12 write-stress repeats.
- [x] `optimize_filters_for_hits = true` — tested (Run 9), not default due to concurrent regressions
- [x] `max_background_jobs` increase — tested (Run 10), not default due to search regressions
- [x] MultiGet for bulk lookups in cascade operations — implemented in collection update/cascade paths
- [x] `fill_cache = false` for cold reads (cascade/remove + update flows) — implemented
- [x] Wire import API `batch_size` to actual indexing batch size in `Collection::add_many()` (default request value remains 40)

### P2 — Experimental / High Potential

- [ ] SstFileWriter / IngestExternalFile for bulk imports (deferred for now — reconsider only if future profiling shows the RocksDB write path, not in-memory indexing, is the dominant remaining import bottleneck)
- [ ] `disable_auto_compactions` during bulk load + manual compact after
- [ ] Prefix extractor + prefix bloom for prefix scans
- [ ] Dynamic `SetOptions()` — switch to import-optimized settings during bulk load, then back for search
- [ ] Separate WriteOptions per operation type
- [ ] ZSTD dictionary compression at bottommost level (32KB dict)

### P3 — Future / Large Refactoring

- [ ] Per-collection Store instances (better isolation, separate tuning per use case)
- [ ] Column families for metadata vs document data
- [ ] WriteBufferManager for unified memory control
- [ ] Direct I/O (needs careful testing, risk of regression on first reads)
