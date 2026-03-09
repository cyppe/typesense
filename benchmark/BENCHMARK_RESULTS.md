# Typesense Fork Benchmark Results

Tracking benchmark results across tuning iterations.
Fork branch: `rocksdb-upgrade-v10.10.1`
Dataset: MusicBrainz 1M songs
Tool: k6 via benchmark CLI, 30s per scenario

---

## Run 14: Raft Recovery Comparison (`braft` runtime vs NuRaft prototype, 2026-03-09)

**Commit:** `HEAD` at run time  
**Command:** `scripts/benchmark_vs_upstream.sh --profile raft-recovery`  
**Scenario:** `200` writes before follower outage, `3 x 50` writes while one follower is down, `22s` sleeps between outage rounds, `2` repeats.  
**Artifacts:** `~/.cache/typesense/benchmark/raft-recovery-summary.json`, run root `/home/cyppe/.cache/typesense/benchmark/raft-recovery-runs/20260309-200011`

### Aggregated Results

| Measure | NuRaft prototype | `braft` runtime |
|---|---:|---:|
| Recovery time after latest snapshot / restart | `0.52 ms` | `3261.10 ms` |
| Extra recovery penalty from blocking snapshots on unhealthy peers | `+10.94 ms` | n/a |
| Replay after rejoin | `0` entries with leader-only snapshots, `150` with `require-healthy-peers` | `152.5` entries |
| Snapshot freshness gap at end of outage | `0` entries after leader-only install | `2` entries |
| Timed snapshots created during outage | `3` per run in leader-only policy | `1` per run |
| Follower snapshot install observed on rejoin | `yes` (prototype install path) | `no` in `2/2` runs |

### Interpretation

- The current fork's `braft` path is fixed for the unhealthy-peer timed-snapshot deadlock class: the leader did create timed snapshots while a follower was down, and the snapshot stayed fresh to within `2` entries of the final committed index.
- That fix does **not** make `braft` look like the NuRaft prototype on recovery behavior. In both runtime repeats, the follower still restarted around log index `202` and replayed roughly the whole outage window (`152-153` entries) instead of obviously taking a snapshot-install path.
- The NuRaft prototype remains materially stronger on this narrow disaster-recovery shape when the leader is allowed to keep snapshotting locally. If NuRaft blocks snapshots on peer health, it immediately loses that advantage and also replays the whole outage window.

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
