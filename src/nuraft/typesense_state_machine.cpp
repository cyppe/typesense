#include "nuraft/typesense_state_machine.h"

#include <cstring>
#include <filesystem>
#include <fstream>

#include <libnuraft/buffer.hxx>
#include <libnuraft/buffer_serializer.hxx>

#include <iostream>

#include "nuraft/nuraft_applied_request_store.h"
#include "nuraft/nuraft_request_envelope.h"
#include "nuraft/nuraft_route_classifier.h"
#include "logger.h"

namespace {

void update_atomic_max(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current,
                                         value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
}

uint64_t steady_clock_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Build a flat list of all files in a directory tree (relative paths).
void list_files_recursive(const std::string& base_dir,
                          const std::string& rel_prefix,
                          std::vector<std::string>& out) {
    for (auto& entry : std::filesystem::recursive_directory_iterator(base_dir)) {
        if (entry.is_regular_file()) {
            auto rel = std::filesystem::relative(entry.path(), base_dir).string();
            out.push_back(rel);
        }
    }
}

}  // namespace

TypesenseStateMachine::TypesenseStateMachine(
    const NuRaftStateLayout& layout,
    NuRaftKvStateMachineSink* kv_sink,
    TypesenseCommitCallback commit_callback,
    TypesenseSnapshotAppliedCallback snapshot_applied_callback)
    : layout_(layout),
      kv_sink_(kv_sink),
      snapshot_coordinator_(layout),
      commit_callback_(std::move(commit_callback)),
      snapshot_applied_callback_(std::move(snapshot_applied_callback)),
      last_commit_index_(0) {
    // Recover last commit index from the KV sink's applied index.
    uint64_t last_applied = 0;
    std::string error;
    if (kv_sink_ && kv_sink_->read_last_applied_index(last_applied, error)) {
        last_commit_index_.store(last_applied);
    }

    // Load last snapshot descriptor if available.
    NuRaftSnapshotDescriptor desc;
    if (snapshot_coordinator_.read_last_snapshot(desc, error) && !desc.snapshot_id.empty()) {
        auto cfg = nuraft::cs_new<nuraft::cluster_config>();
        last_snapshot_ptr_ = nuraft::cs_new<nuraft::snapshot>(
            desc.last_log_index,
            0,  // term (not tracked in descriptor, NuRaft will update)
            cfg);
    }
}

TypesenseStateMachine::~TypesenseStateMachine() = default;

nuraft::ptr<nuraft::buffer> TypesenseStateMachine::commit(
    const nuraft::ulong log_idx, nuraft::buffer& data) {
    // Deserialize the NuRaftRequestEnvelope from the committed buffer.
    std::string raw(reinterpret_cast<const char*>(data.data_begin()), data.size());
    NuRaftRequestEnvelope envelope;
    std::string error;

    if (!NuRaftRequestEnvelope::deserialize(raw, envelope, error)) {
        std::cerr << "TypesenseStateMachine::commit: failed to deserialize "
                        << "envelope at index " << log_idx << ": " << error << "\n";
        last_commit_index_.store(log_idx);
        return nullptr;
    }

    // Convert to NuRaftAppliedRequest via the existing from_log_entry helper.
    NuRaftLogEntry log_entry;
    log_entry.index = log_idx;
    log_entry.envelope = envelope;

    NuRaftAppliedRequest applied;
    if (!NuRaftAppliedRequest::from_log_entry(log_entry, applied, error)) {
        std::cerr << "TypesenseStateMachine::commit: from_log_entry failed "
                        << "at index " << log_idx << ": " << error << "\n";
        last_commit_index_.store(log_idx);
        return nullptr;
    }

    if (kv_sink_) {
        std::vector<NuRaftAppliedRequest> batch{applied};
        if (!kv_sink_->apply_all(batch, error)) {
            std::cerr << "TypesenseStateMachine::commit: KV sink apply failed "
                            << "at index " << log_idx << ": " << error << "\n";
        }
    }

    last_commit_index_.store(log_idx);

    // Notify the runtime so it can mirror to CollectionManager.
    if (commit_callback_) {
        commit_callback_(log_idx, applied);
    }

    return nullptr;
}

nuraft::ptr<nuraft::buffer> TypesenseStateMachine::pre_commit(
    const nuraft::ulong /*log_idx*/, nuraft::buffer& /*data*/) {
    return nullptr;  // No optimistic execution needed.
}

void TypesenseStateMachine::rollback(
    const nuraft::ulong /*log_idx*/, nuraft::buffer& /*data*/) {
    // No rollback needed since we don't do pre_commit.
}

void TypesenseStateMachine::create_snapshot(
    nuraft::snapshot& s,
    nuraft::async_result<bool>::handler_type& when_done) {
    const auto start = std::chrono::steady_clock::now();
    snapshot_in_progress_.store(true, std::memory_order_relaxed);
    last_snapshot_log_index_.store(s.get_last_log_idx(), std::memory_order_relaxed);

    NuRaftSnapshotDescriptor desc;
    std::string error;
    bool success = false;
    {
        std::lock_guard<std::mutex> guard(snapshot_mutex_);

        if (kv_sink_) {
            std::string export_path;  // Empty = don't export to external path.
            success = snapshot_coordinator_.create_snapshot(export_path, kv_sink_, desc, error);
        }

        if (success) {
            auto cfg = nuraft::cs_new<nuraft::cluster_config>();
            last_snapshot_ptr_ = nuraft::cs_new<nuraft::snapshot>(
                s.get_last_log_idx(),
                s.get_last_log_term(),
                cfg);
            cumulative_snapshots_.fetch_add(1, std::memory_order_relaxed);
            last_snapshot_applied_index_.store(desc.last_applied_index, std::memory_order_relaxed);
            last_snapshot_completed_at_ms_.store(steady_clock_now_ms(), std::memory_order_relaxed);
        }
    }

    const uint64_t total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    last_snapshot_total_ms_.store(total_ms, std::memory_order_relaxed);
    last_snapshot_success_.store(success, std::memory_order_relaxed);
    update_atomic_max(max_snapshot_total_ms_, total_ms);
    snapshot_in_progress_.store(false, std::memory_order_relaxed);

    if (success) {
        TS_LOG(INFO) << "NuRaft snapshot created: snapshot_id=" << desc.snapshot_id
                     << ", log_index=" << s.get_last_log_idx()
                     << ", applied_index=" << desc.last_applied_index
                     << ", total_ms=" << total_ms;
    } else {
        cumulative_snapshot_failures_.fetch_add(1, std::memory_order_relaxed);
        TS_LOG(WARNING) << "NuRaft snapshot creation failed: log_index=" << s.get_last_log_idx()
                        << ", total_ms=" << total_ms
                        << ", error=" << error;
    }

    nuraft::ptr<std::exception> except(nullptr);
    // Invoke the NuRaft completion callback after releasing snapshot_mutex_.
    // The callback re-enters last_snapshot(), and calling it under the mutex
    // can deadlock manual snapshot requests forever.
    when_done(success, except);
}

bool TypesenseStateMachine::apply_snapshot(nuraft::snapshot& s) {
    NuRaftSnapshotDescriptor desc;
    std::string error;
    {
        std::lock_guard<std::mutex> guard(snapshot_mutex_);

        // The snapshot data was received via save_logical_snp_obj.
        // The actual state is in the snapshot directory.
        if (!snapshot_coordinator_.read_last_snapshot(desc, error)) {
            std::cerr << "TypesenseStateMachine: apply_snapshot failed to read descriptor: "
                            << error << "\n";
            return false;
        }

        auto cfg = nuraft::cs_new<nuraft::cluster_config>();
        last_snapshot_ptr_ = nuraft::cs_new<nuraft::snapshot>(
            s.get_last_log_idx(),
            s.get_last_log_term(),
            cfg);
        last_commit_index_.store(s.get_last_log_idx());
        last_snapshot_log_index_.store(s.get_last_log_idx(), std::memory_order_relaxed);
        last_snapshot_applied_index_.store(desc.last_applied_index, std::memory_order_relaxed);
        last_snapshot_success_.store(true, std::memory_order_relaxed);
        last_snapshot_completed_at_ms_.store(steady_clock_now_ms(), std::memory_order_relaxed);
    }

    if (snapshot_applied_callback_) {
        snapshot_applied_callback_(s.get_last_log_idx(), desc);
    }

    std::cerr << "TypesenseStateMachine: applied snapshot at index "
                 << s.get_last_log_idx() << "\n";
    return true;
}

nuraft::ptr<nuraft::snapshot> TypesenseStateMachine::last_snapshot() {
    std::lock_guard<std::mutex> guard(snapshot_mutex_);
    return last_snapshot_ptr_;
}

nuraft::ulong TypesenseStateMachine::last_commit_index() {
    return last_commit_index_.load();
}

uint64_t TypesenseStateMachine::get_last_commit_index() const {
    return last_commit_index_.load();
}

TypesenseSnapshotMetricsSnapshot TypesenseStateMachine::get_snapshot_metrics() const {
    TypesenseSnapshotMetricsSnapshot snapshot;
    snapshot.snapshot_in_progress = snapshot_in_progress_.load(std::memory_order_relaxed);
    snapshot.last_snapshot_success = last_snapshot_success_.load(std::memory_order_relaxed);
    snapshot.last_snapshot_log_index = last_snapshot_log_index_.load(std::memory_order_relaxed);
    snapshot.last_snapshot_applied_index = last_snapshot_applied_index_.load(std::memory_order_relaxed);
    snapshot.last_snapshot_completed_at_ms = last_snapshot_completed_at_ms_.load(std::memory_order_relaxed);
    snapshot.last_snapshot_total_ms = last_snapshot_total_ms_.load(std::memory_order_relaxed);
    snapshot.max_snapshot_total_ms = max_snapshot_total_ms_.load(std::memory_order_relaxed);
    snapshot.cumulative_snapshots = cumulative_snapshots_.load(std::memory_order_relaxed);
    snapshot.cumulative_snapshot_failures = cumulative_snapshot_failures_.load(std::memory_order_relaxed);
    return snapshot;
}

// --- Logical snapshot transfer (object-based) ---
//
// Object layout for transfer:
//   obj_id 0: file count (4-byte int32) + file list (each: 4-byte name_len + name)
//   obj_id 1..N: file content chunks (4-byte name_len + name + 4-byte data_len + data)
//
// This allows incremental transfer of the full snapshot directory.

int TypesenseStateMachine::read_logical_snp_obj(
    nuraft::snapshot& s,
    void*& user_snp_ctx,
    nuraft::ulong obj_id,
    nuraft::ptr<nuraft::buffer>& data_out,
    bool& is_last_obj) {
    std::lock_guard<std::mutex> guard(snapshot_mutex_);

    auto* ctx = static_cast<SnapshotTransferCtx*>(user_snp_ctx);
    if (!ctx) {
        // First call: build file list from the latest snapshot.
        ctx = new SnapshotTransferCtx();
        user_snp_ctx = ctx;
        ctx->snap = nuraft::cs_new<nuraft::snapshot>(
            s.get_last_log_idx(), s.get_last_log_term(),
            nuraft::cs_new<nuraft::cluster_config>());

        // Find snapshot directory.
        NuRaftSnapshotDescriptor desc;
        std::string error;
        if (snapshot_coordinator_.read_last_snapshot(desc, error) &&
            !desc.snapshot_id.empty()) {
            ctx->snapshot_dir = layout_.snapshot_dir + "/" + desc.snapshot_id;
            list_files_recursive(ctx->snapshot_dir, "", ctx->file_list);
        }
    }

    if (obj_id == 0) {
        // Send file manifest.
        size_t total = sizeof(nuraft::int32);
        for (auto& f : ctx->file_list) {
            total += sizeof(nuraft::int32) + f.size();
        }
        data_out = nuraft::buffer::alloc(total);
        data_out->put(static_cast<nuraft::int32>(ctx->file_list.size()));
        for (auto& f : ctx->file_list) {
            data_out->put(f.c_str(), f.size());
        }
        data_out->pos(0);
        is_last_obj = ctx->file_list.empty();
        return 0;
    }

    // obj_id 1..N maps to file_list[obj_id - 1].
    size_t file_idx = static_cast<size_t>(obj_id - 1);
    if (file_idx >= ctx->file_list.size()) {
        is_last_obj = true;
        data_out = nuraft::buffer::alloc(0);
        return 0;
    }

    std::string file_path = ctx->snapshot_dir + "/" + ctx->file_list[file_idx];
    std::string content;
    {
        std::ifstream ifs(file_path, std::ios::binary);
        if (ifs.is_open()) {
            content.assign(std::istreambuf_iterator<char>(ifs),
                           std::istreambuf_iterator<char>());
        }
    }

    auto& name = ctx->file_list[file_idx];
    size_t total = sizeof(nuraft::int32) + name.size() +
                   sizeof(nuraft::int32) + content.size();
    data_out = nuraft::buffer::alloc(total);
    data_out->put(name.c_str(), name.size());
    data_out->put(static_cast<const char*>(content.data()), content.size());
    data_out->pos(0);

    is_last_obj = (file_idx + 1 >= ctx->file_list.size());
    return 0;
}

void TypesenseStateMachine::save_logical_snp_obj(
    nuraft::snapshot& s,
    nuraft::ulong& obj_id,
    nuraft::buffer& data,
    bool is_first_obj,
    bool is_last_obj) {
    if (is_first_obj && obj_id == 0) {
        // Receiving file manifest. Parse file list (we don't actually need it
        // here since each subsequent object self-describes its filename).
        obj_id = 1;
        return;
    }

    if (data.size() == 0) {
        return;
    }

    // Parse: name_len + name + data_len + data.
    data.pos(0);
    size_t name_len = 0;
    const char* name_ptr = reinterpret_cast<const char*>(data.get_bytes(name_len));
    std::string rel_name(name_ptr, name_len);

    size_t content_len = 0;
    const char* content_ptr = reinterpret_cast<const char*>(data.get_bytes(content_len));

    // Write file into snapshot staging directory.
    std::string snap_id = "snapshot-incoming-" + std::to_string(s.get_last_log_idx());
    std::string snap_dir = layout_.snapshot_dir + "/" + snap_id;
    std::string file_path = snap_dir + "/" + rel_name;
    std::filesystem::create_directories(
        std::filesystem::path(file_path).parent_path());
    {
        std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);
        if (ofs.is_open()) {
            ofs.write(content_ptr, static_cast<std::streamsize>(content_len));
        }
    }

    bool installed_snapshot = false;
    NuRaftSnapshotDescriptor installed_descriptor;
    if (is_last_obj) {
        std::lock_guard<std::mutex> guard(snapshot_mutex_);
        // Install the snapshot.
        std::string error;
        if (!snapshot_coordinator_.install_snapshot(snap_dir, installed_descriptor, error)) {
            std::cerr << "TypesenseStateMachine: failed to install incoming snapshot at index "
                      << s.get_last_log_idx() << ": " << error << "\n";
            obj_id = obj_id + 1;
            return;
        }

        auto cfg = nuraft::cs_new<nuraft::cluster_config>();
        last_snapshot_ptr_ = nuraft::cs_new<nuraft::snapshot>(
            s.get_last_log_idx(),
            s.get_last_log_term(),
            cfg);
        last_commit_index_.store(s.get_last_log_idx());
        last_snapshot_log_index_.store(s.get_last_log_idx(), std::memory_order_relaxed);
        last_snapshot_applied_index_.store(installed_descriptor.last_applied_index, std::memory_order_relaxed);
        last_snapshot_success_.store(true, std::memory_order_relaxed);
        last_snapshot_completed_at_ms_.store(steady_clock_now_ms(), std::memory_order_relaxed);
        installed_snapshot = true;
    }

    if (installed_snapshot && snapshot_applied_callback_) {
        snapshot_applied_callback_(s.get_last_log_idx(), installed_descriptor);
    }

    obj_id = obj_id + 1;
}

void TypesenseStateMachine::free_user_snp_ctx(void*& user_snp_ctx) {
    auto* ctx = static_cast<SnapshotTransferCtx*>(user_snp_ctx);
    delete ctx;
    user_snp_ctx = nullptr;
}
