#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <libnuraft/nuraft.hxx>

#include "nuraft_snapshot_coordinator.h"
#include "nuraft_applied_request_store.h"
#include "nuraft_state_layout.h"
#include "nuraft_state_machine_sink.h"

// Callback invoked after each committed log entry is applied to the state machine.
// Parameters: log_index, decoded request.
using TypesenseCommitCallback = std::function<void(uint64_t log_index,
                                                   const NuRaftAppliedRequest& request)>;

// NuRaft state machine for Typesense.
//
// Applies committed Raft log entries (NuRaftRequestEnvelope payloads) to
// the NuRaftKvStateMachineSink. Handles snapshot creation via RocksDB
// checkpoints and logical snapshot transfer between nodes.
class TypesenseStateMachine : public nuraft::state_machine {
public:
    TypesenseStateMachine(const NuRaftStateLayout& layout,
                          NuRaftKvStateMachineSink* kv_sink,
                          TypesenseCommitCallback commit_callback = nullptr);
    ~TypesenseStateMachine() override;

    // nuraft::state_machine interface
    nuraft::ptr<nuraft::buffer> commit(const nuraft::ulong log_idx,
                                       nuraft::buffer& data) override;
    nuraft::ptr<nuraft::buffer> pre_commit(const nuraft::ulong log_idx,
                                           nuraft::buffer& data) override;
    void rollback(const nuraft::ulong log_idx,
                  nuraft::buffer& data) override;

    void create_snapshot(nuraft::snapshot& s,
                         nuraft::async_result<bool>::handler_type& when_done) override;
    bool apply_snapshot(nuraft::snapshot& s) override;
    nuraft::ptr<nuraft::snapshot> last_snapshot() override;
    nuraft::ulong last_commit_index() override;

    // Logical snapshot transfer (object-based).
    void save_logical_snp_obj(nuraft::snapshot& s,
                              nuraft::ulong& obj_id,
                              nuraft::buffer& data,
                              bool is_first_obj,
                              bool is_last_obj) override;
    int read_logical_snp_obj(nuraft::snapshot& s,
                             void*& user_snp_ctx,
                             nuraft::ulong obj_id,
                             nuraft::ptr<nuraft::buffer>& data_out,
                             bool& is_last_obj) override;
    void free_user_snp_ctx(void*& user_snp_ctx) override;

    uint64_t get_last_commit_index() const;

private:
    NuRaftStateLayout layout_;
    NuRaftKvStateMachineSink* kv_sink_;
    NuRaftSnapshotCoordinator snapshot_coordinator_;
    TypesenseCommitCallback commit_callback_;

    std::atomic<uint64_t> last_commit_index_;
    mutable std::mutex snapshot_mutex_;
    nuraft::ptr<nuraft::snapshot> last_snapshot_ptr_;

    // Snapshot transfer state.
    struct SnapshotTransferCtx {
        std::string snapshot_dir;
        std::vector<std::string> file_list;
        nuraft::ptr<nuraft::snapshot> snap;
    };
};
