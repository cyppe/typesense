# Worker Thread Architecture — Handoff for New Agent

## Your Mission

Design and implement a **background mirror worker thread** that eliminates `sync_live_product_state()` from the Typesense NuRaft fork. This is the last major architectural improvement needed for production-grade cluster performance.

## Context

This fork replaces upstream Typesense's braft/brpc with NuRaft for Raft consensus. The current architecture has a performance bottleneck: `sync_live_product_state()` replays KV sink entries to CollectionManager under an **exclusive lock** on `mutex_`, blocking all concurrent reads and writes for ~300ms during active imports.

The current mitigation (Phase 1) moved sync from the h2o event loop to the thread pool. This prevents HTTP I/O blocking but sync still contends with the mutex during every write and read.

## What You Need to Understand

### The Two-Layer State Architecture

The fork maintains two representations of the committed state:

1. **KV materialized state sink** (RocksDB) — written by the NuRaft state machine `commit()` on the dedicated NuRaft commit thread. Always up-to-date on all nodes.

2. **CollectionManager** (in-memory search index) — the actual Typesense product state. Updated by:
   - `mirror_single_node_typesense_state()` in `write()` (originating node only)
   - `replay_live_product_state()` called from `sync_live_product_state()` (all other nodes, on-demand)

`live_product_state_applied_index_` tracks what CollectionManager has applied. The sync function reads entries from the KV sink after this index and replays them to CollectionManager.

### The Core Problem

`write()` applies its entry to CollectionManager AND needs the handler response for the HTTP reply. On non-originating nodes (followers receiving Raft-replicated entries), there is no `write()` call — CollectionManager is only updated when `sync_live_product_state()` runs (triggered by reads or subsequent writes).

### What Was Already Tried (and Failed)

**Attempt 1**: Advance `live_product_state_applied_index_` in the commit callback. **Failed** because sync uses this index to know what to replay — advancing it prematurely made sync think CollectionManager was caught up when it wasn't.

**Attempt 2**: Worker thread that processes entries from KV sink and applies to CollectionManager. **Failed** because of the double-application race: the worker and `write()` on the originating node both try to apply the same entry. The second application fails with "already exists" (409) for creates, "not found" for deletes, etc. Checking `live_product_state_applied_index_` before applying is racy — the worker can apply between `append_via_raft()` returning and `write()` reaching the mirror call.

**Attempt 3**: Skip the mirror in `write()` if the worker already applied (check applied index >= committed index). **Failed** because `write()` needs the handler response for HTTP — returning `request->body` or reading back from CollectionManager doesn't produce the correct response format for every route type (especially imports, analytics, and unknown routes).

## Key Files to Study

### Fork NuRaft Implementation
- `src/nuraft/nuraft_http_runtime.cpp` — **THE MAIN FILE**. Contains `write()`, `sync_live_product_state()`, `mirror_single_node_typesense_state()`, `process_document_import_write()`, route registration
- `include/nuraft/nuraft_http_runtime.h` — class declaration with all members
- `src/nuraft/typesense_state_machine.cpp` — the `commit()` method (line 72) where KV sink writes happen and the commit callback fires
- `src/nuraft/nuraft_state_machine_sink.cpp` — the KV sink RocksDB operations
- `src/http_server.cpp` — HTTP request pipeline, auth, thread pool dispatch

### NuRaft Library Reference
- `/home/cyppe/Projects/forks/NuRaft/examples/example_common.hxx` — canonical usage patterns
- `/home/cyppe/Projects/forks/NuRaft/examples/calculator/calc_state_machine.hxx` — example state machine (commit applies state directly)
- `/home/cyppe/Projects/forks/NuRaft/include/libnuraft/raft_server.hxx` — `wait_for_state_machine_commit()` (line ~952), `get_committed_log_idx()`
- `/home/cyppe/Projects/forks/NuRaft/include/libnuraft/callback.hxx` — `StateMachineExecution` callback (fires after every commit)
- `/home/cyppe/Projects/forks/NuRaft/src/handle_commit.cxx` — how NuRaft calls `commit()` on the background commit thread

### Upstream Typesense (for comparison)
- `/home/cyppe/Projects/forks/original-typesense/typesense/src/raft_server.cpp` — upstream uses braft. `on_apply()` (line 511) is their equivalent of the commit callback. They apply state AND build the HTTP response in the same callback, using `ReplicationClosure` to deliver it back to the waiting client thread.

## Critical Invariants

1. `live_product_state_applied_index_` MUST only advance when CollectionManager has actually applied the entry
2. `write()` MUST return the correct HTTP response (handler output, not raw request body)
3. Import responses are per-line JSONL — can only come from running `post_import_documents`
4. The NuRaft `commit()` fires on a dedicated background thread — NOT the HTTP thread pool
5. `write()` uses `raft_params::blocking` mode — `append_entries()` blocks until commit completes
6. After `append_via_raft()` returns, the entry IS in the KV sink (commit already ran)
7. `wait_for_applied_index()` uses `live_state_progress_cv_` — all advancers must notify

## Suggested Approaches to Explore

### Approach A: Response Map + Worker Thread
The worker processes committed entries and stores handler responses in a `std::unordered_map<uint64_t, StoredResponse>`. `write()` waits for the worker (via `wait_for_applied_index`), then retrieves the stored response.

**Pros**: Clean separation, no double-application, no race
**Cons**: Memory management (responses must be cleaned up), import response aggregation across multiple chunks

### Approach B: Originator Reservation via Commit Callback
Use the commit callback (which fires synchronously during `append_entries()`) to check a pre-registered "reservation" from `write()`. If reserved, the callback signals `write()` to proceed (don't enqueue for worker). If not reserved (replication from another node), enqueue for worker.

**Pros**: No double-application
**Cons**: Reservation requires knowing committed_index before it's available; could use a thread-ID-based approach since `append_entries` and the commit callback always run from the same call chain

### Approach C: Upstream-Style ReplicationClosure
Model after upstream's `ReplicationClosure` pattern: `write()` creates a callback that receives the handler result. The commit/apply path (either worker or inline) invokes the callback with the result. This is essentially NuRaft's `cmd_result` pattern combined with the state machine commit.

**Pros**: Proven pattern (upstream uses it), clean
**Cons**: Requires restructuring the write path significantly

### Approach D: Make sync_live_product_state() Non-Blocking
Instead of replacing sync, make it faster:
- Use per-collection granular locks instead of the global `mutex_`
- Batch the replay (apply multiple entries in one lock acquisition)
- Use NuRaft's `wait_for_state_machine_commit()` to know when new entries are available
- Make the replay lock non-exclusive (readers can still proceed during replay if they don't access the same collection)

**Pros**: Minimal architectural change
**Cons**: Still a synchronous call in the hot path

## NuRaft Audit Key Findings (for your reference)

From a deep audit of NuRaft source and examples:

- **`commit()` runs on NuRaft's dedicated commit thread** — serialized, not concurrent
- **`wait_for_state_machine_commit(target_idx)`** — built-in async wait for a specific commit index, works on both leader and follower. The fork doesn't use this yet.
- **`StateMachineExecution` callback** (via `init_options.raft_callback_`) — fires after every state machine commit, on any node
- **`track_peers_sm_commit_idx_ = true`** — leader tracks follower state machine progress (already enabled)
- **Blocking mode**: `append_entries()` blocks until `commit()` completes, then returns. The commit fires on the commit thread, and the caller's thread blocks on an `EventAwaiter`
- **Auto-forwarding**: follower forwards to leader via ASIO RPC, blocks until leader commits
- **All NuRaft params are well-tuned** — heartbeat 100ms, election 200-400ms, leadership expiry 5000ms

## Testing

- Build: `scripts/bazel_in_docker.sh build //:typesense-server --jobs=3`
- C++ tests: `scripts/bazel_in_docker.sh test //:typesense-test --jobs=3 --test_output=errors`
- API tests (7 phases including 3-node cluster): `scripts/run_api_tests.sh --host-bun --skip-install -- --no-secrets`
- The key test: `api_tests/tests/nuraft_delete_recreate_import.test.ts` — tests delete→create→import race
- ddev cluster available at `ddev-generate-typesense-{1,2,3}` for real-world testing

## Definition of Done

1. `sync_live_product_state()` is no longer called from any hot path
2. CollectionManager is updated in real-time on all nodes (no manual replay)
3. `write()` returns correct HTTP responses for ALL route types (collections, documents, imports, analytics, aliases, presets, stopwords, synonyms, curations, stemming)
4. No double-application — each entry is applied to CollectionManager exactly once per node
5. All existing tests pass (C++ + API, all 7 phases)
6. No exclusive lock acquisition for catch-up replay
7. Search latency during imports drops significantly (no mutex contention from sync)
