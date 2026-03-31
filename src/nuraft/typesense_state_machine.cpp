#include "nuraft/typesense_state_machine.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include <libnuraft/buffer.hxx>
#include <libnuraft/buffer_serializer.hxx>

#include <iostream>

#include "nuraft/nuraft_applied_request_store.h"
#include "nuraft/nuraft_request_envelope.h"
#include "nuraft/nuraft_route_classifier.h"
#include "logger.h"

namespace {

constexpr uint32_t kLogicalSnapshotTransferFormatVersion = 2;

// Keep each logical snapshot object comfortably below the large RocksDB SST
// sizes seen in real recovery snapshots so NuRaft does not have to move a
// whole checkpoint file in one RPC-sized payload.
constexpr uint64_t kLogicalSnapshotChunkBytes = 8ULL * 1024ULL * 1024ULL;

struct SnapshotTransferFile {
    std::string relative_path;
    uint64_t size = 0;
};

struct SnapshotTransferChunk {
    size_t file_index = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
};

struct SnapshotTransferCtx {
    std::string snapshot_dir;
    std::vector<SnapshotTransferFile> files;
    std::vector<SnapshotTransferChunk> chunks;
    bool has_main_db_checkpoint = false;
    nuraft::ptr<nuraft::snapshot> snap;
};

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

bool has_db_prefix(const std::string& relative_path) {
    return relative_path == "db" || relative_path.rfind("db/", 0) == 0;
}

// Build a stable flat list of all files in a directory tree.
void list_files_recursive(const std::string& base_dir,
                          std::vector<SnapshotTransferFile>& out) {
    for (auto& entry : std::filesystem::recursive_directory_iterator(base_dir)) {
        if (entry.is_regular_file()) {
            auto rel = std::filesystem::relative(entry.path(), base_dir).string();
            out.push_back({rel, static_cast<uint64_t>(entry.file_size())});
        }
    }

    std::sort(out.begin(), out.end(), [](const SnapshotTransferFile& lhs, const SnapshotTransferFile& rhs) {
        return lhs.relative_path < rhs.relative_path;
    });
}

void build_snapshot_chunks(const std::vector<SnapshotTransferFile>& files,
                           std::vector<SnapshotTransferChunk>& chunks) {
    chunks.clear();
    for (size_t file_index = 0; file_index < files.size(); ++file_index) {
        const auto& file = files[file_index];
        if (file.size == 0) {
            chunks.push_back({file_index, 0, 0});
            continue;
        }

        for (uint64_t offset = 0; offset < file.size; offset += kLogicalSnapshotChunkBytes) {
            const uint64_t remaining = file.size - offset;
            chunks.push_back({file_index, offset, std::min<uint64_t>(remaining, kLogicalSnapshotChunkBytes)});
        }
    }
}

std::string incoming_snapshot_dir(const NuRaftStateLayout& layout, uint64_t log_index) {
    return layout.snapshot_dir + "/snapshot-incoming-" + std::to_string(log_index);
}

}  // namespace

struct TypesenseStateMachine::IncomingSnapshotCtx {
    std::string snapshot_dir;
    std::unordered_map<std::string, uint64_t> expected_file_sizes;
    bool has_main_db_checkpoint = false;
};

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
//   obj_id 0: transfer format version + has_db flag + file manifest
//   obj_id 1..N: chunk objects (path + file offset + full file size + chunk bytes)
//
// This keeps large RocksDB checkpoint files bounded during live NuRaft transfer.

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
            ctx->has_main_db_checkpoint = desc.includes_main_db_checkpoint;
            list_files_recursive(ctx->snapshot_dir, ctx->files);
            build_snapshot_chunks(ctx->files, ctx->chunks);
        }
    }

    if (obj_id == 0) {
        // Send file manifest.
        size_t total = sizeof(nuraft::int32) + sizeof(nuraft::byte) + sizeof(nuraft::int32);
        for (const auto& file : ctx->files) {
            total += sizeof(nuraft::int32) + file.relative_path.size() + sizeof(nuraft::ulong);
        }
        data_out = nuraft::buffer::alloc(total);
        data_out->put(static_cast<nuraft::int32>(kLogicalSnapshotTransferFormatVersion));
        data_out->put(static_cast<nuraft::byte>(ctx->has_main_db_checkpoint ? 1 : 0));
        data_out->put(static_cast<nuraft::int32>(ctx->files.size()));
        for (const auto& file : ctx->files) {
            data_out->put(file.relative_path.c_str(), file.relative_path.size());
            data_out->put(static_cast<nuraft::ulong>(file.size));
        }
        data_out->pos(0);
        is_last_obj = ctx->chunks.empty();
        return 0;
    }

    // obj_id 1..N maps to chunk_list[obj_id - 1].
    const size_t chunk_idx = static_cast<size_t>(obj_id - 1);
    if (chunk_idx >= ctx->chunks.size()) {
        is_last_obj = true;
        data_out = nuraft::buffer::alloc(0);
        return 0;
    }

    const auto& chunk = ctx->chunks[chunk_idx];
    const auto& file = ctx->files[chunk.file_index];
    const std::string file_path = ctx->snapshot_dir + "/" + file.relative_path;

    std::string content;
    if (chunk.size > 0) {
        std::ifstream ifs(file_path, std::ios::binary);
        if (!ifs.is_open()) {
            return -1;
        }
        ifs.seekg(static_cast<std::streamoff>(chunk.offset));
        content.resize(static_cast<size_t>(chunk.size));
        ifs.read(content.data(), static_cast<std::streamsize>(chunk.size));
        if (static_cast<uint64_t>(ifs.gcount()) != chunk.size) {
            return -1;
        }
    }

    const auto& name = file.relative_path;
    const char* content_ptr = content.empty() ? "" : content.data();
    size_t total = sizeof(nuraft::int32) + name.size() +
                   sizeof(nuraft::ulong) + sizeof(nuraft::ulong) +
                   sizeof(nuraft::int32) + content.size();
    data_out = nuraft::buffer::alloc(total);
    data_out->put(name.c_str(), name.size());
    data_out->put(static_cast<nuraft::ulong>(chunk.offset));
    data_out->put(static_cast<nuraft::ulong>(file.size));
    data_out->put(content_ptr, content.size());
    data_out->pos(0);

    is_last_obj = (chunk_idx + 1 >= ctx->chunks.size());
    return 0;
}

void TypesenseStateMachine::save_logical_snp_obj(
    nuraft::snapshot& s,
    nuraft::ulong& obj_id,
    nuraft::buffer& data,
    bool is_first_obj,
    bool is_last_obj) {
    if (is_first_obj && obj_id == 0) {
        data.pos(0);

        const auto format_version = static_cast<uint32_t>(data.get_int());
        if (format_version != kLogicalSnapshotTransferFormatVersion) {
            std::cerr << "TypesenseStateMachine: unsupported snapshot transfer manifest version "
                      << format_version << " at index " << s.get_last_log_idx() << "\n";
            obj_id = 1;
            return;
        }

        auto incoming = std::make_unique<IncomingSnapshotCtx>();
        incoming->snapshot_dir = incoming_snapshot_dir(layout_, s.get_last_log_idx());
        incoming->has_main_db_checkpoint = data.get_byte() != 0;

        const size_t file_count = static_cast<size_t>(data.get_int());
        for (size_t i = 0; i < file_count; ++i) {
            size_t name_len = 0;
            const char* name_ptr = reinterpret_cast<const char*>(data.get_bytes(name_len));
            incoming->expected_file_sizes.emplace(std::string(name_ptr, name_len), data.get_ulong());
        }

        std::error_code ec;
        std::filesystem::remove_all(incoming->snapshot_dir, ec);
        ec.clear();
        std::filesystem::create_directories(incoming->snapshot_dir, ec);
        if (ec) {
            std::cerr << "TypesenseStateMachine: failed to initialize incoming snapshot dir at index "
                      << s.get_last_log_idx() << ": " << ec.message() << "\n";
            obj_id = 1;
            return;
        }

        {
            std::lock_guard<std::mutex> guard(incoming_snapshot_mutex_);
            incoming_snapshot_ctx_ = std::move(incoming);
        }

        obj_id = 1;
        return;
    }

    if (data.size() == 0) {
        return;
    }

    IncomingSnapshotCtx incoming;
    {
        std::lock_guard<std::mutex> guard(incoming_snapshot_mutex_);
        if (!incoming_snapshot_ctx_) {
            std::cerr << "TypesenseStateMachine: missing incoming snapshot context at index "
                      << s.get_last_log_idx() << "\n";
            obj_id = obj_id + 1;
            return;
        }
        incoming = *incoming_snapshot_ctx_;
    }

    // Parse: name_len + name + file_offset + file_size + chunk_len + chunk.
    data.pos(0);
    size_t name_len = 0;
    const char* name_ptr = reinterpret_cast<const char*>(data.get_bytes(name_len));
    std::string rel_name(name_ptr, name_len);
    const uint64_t file_offset = data.get_ulong();
    const uint64_t file_size = data.get_ulong();

    size_t chunk_len = 0;
    const char* chunk_ptr = reinterpret_cast<const char*>(data.get_bytes(chunk_len));

    // Write file into snapshot staging directory.
    const std::string file_path = incoming.snapshot_dir + "/" + rel_name;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(file_path).parent_path(), ec);
    if (ec) {
        std::cerr << "TypesenseStateMachine: failed to create incoming snapshot parent for '"
                  << rel_name << "' at index " << s.get_last_log_idx() << ": "
                  << ec.message() << "\n";
        obj_id = obj_id + 1;
        return;
    }

    {
        std::fstream file;
        if (file_offset == 0) {
            file.open(file_path, std::ios::binary | std::ios::out | std::ios::trunc);
        } else {
            file.open(file_path, std::ios::binary | std::ios::in | std::ios::out);
        }

        if (!file.is_open()) {
            std::cerr << "TypesenseStateMachine: failed to open incoming snapshot file '"
                      << rel_name << "' at index " << s.get_last_log_idx() << "\n";
            obj_id = obj_id + 1;
            return;
        }

        file.seekp(static_cast<std::streamoff>(file_offset));
        if (!file) {
            std::cerr << "TypesenseStateMachine: failed to seek incoming snapshot file '"
                      << rel_name << "' to offset " << file_offset << " at index "
                      << s.get_last_log_idx() << "\n";
            obj_id = obj_id + 1;
            return;
        }

        if (chunk_len > 0) {
            file.write(chunk_ptr, static_cast<std::streamsize>(chunk_len));
        }

        if (!file) {
            std::cerr << "TypesenseStateMachine: failed to write incoming snapshot file '"
                      << rel_name << "' at index " << s.get_last_log_idx() << "\n";
            obj_id = obj_id + 1;
            return;
        }
    }

    const auto expected = incoming.expected_file_sizes.find(rel_name);
    if (expected == incoming.expected_file_sizes.end() || expected->second != file_size) {
        std::cerr << "TypesenseStateMachine: incoming snapshot file '" << rel_name
                  << "' did not match the manifest at index " << s.get_last_log_idx() << "\n";
        obj_id = obj_id + 1;
        return;
    }

    bool installed_snapshot = false;
    NuRaftSnapshotDescriptor installed_descriptor;
    if (is_last_obj) {
        bool saw_db_file = false;
        for (const auto& entry : incoming.expected_file_sizes) {
            const auto snapshot_file = std::filesystem::path(incoming.snapshot_dir) / entry.first;
            saw_db_file = saw_db_file || has_db_prefix(entry.first);

            std::error_code size_ec;
            if (!std::filesystem::exists(snapshot_file)) {
                std::cerr << "TypesenseStateMachine: incoming snapshot is missing expected file '"
                          << entry.first << "' at index " << s.get_last_log_idx() << "\n";
                obj_id = obj_id + 1;
                std::lock_guard<std::mutex> guard(incoming_snapshot_mutex_);
                incoming_snapshot_ctx_.reset();
                return;
            }

            const uint64_t actual_size = std::filesystem::file_size(snapshot_file, size_ec);
            if (size_ec || actual_size != entry.second) {
                std::cerr << "TypesenseStateMachine: incoming snapshot file '" << entry.first
                          << "' had size " << actual_size << " but expected " << entry.second
                          << " at index " << s.get_last_log_idx() << "\n";
                obj_id = obj_id + 1;
                std::lock_guard<std::mutex> guard(incoming_snapshot_mutex_);
                incoming_snapshot_ctx_.reset();
                return;
            }
        }

        if (incoming.has_main_db_checkpoint) {
            if (!saw_db_file) {
                std::cerr << "TypesenseStateMachine: incoming snapshot manifest expected db files but none were advertised at index "
                          << s.get_last_log_idx() << "\n";
                obj_id = obj_id + 1;
                std::lock_guard<std::mutex> guard(incoming_snapshot_mutex_);
                incoming_snapshot_ctx_.reset();
                return;
            }

            if (!std::filesystem::is_directory(std::filesystem::path(incoming.snapshot_dir) / "db")) {
                std::cerr << "TypesenseStateMachine: incoming snapshot is missing the main db checkpoint directory at index "
                          << s.get_last_log_idx() << "\n";
                obj_id = obj_id + 1;
                std::lock_guard<std::mutex> guard(incoming_snapshot_mutex_);
                incoming_snapshot_ctx_.reset();
                return;
            }
        }

        std::lock_guard<std::mutex> guard(snapshot_mutex_);
        // Install the snapshot.
        std::string error;
        if (!snapshot_coordinator_.install_snapshot(incoming.snapshot_dir, installed_descriptor, error)) {
            std::cerr << "TypesenseStateMachine: failed to install incoming snapshot at index "
                      << s.get_last_log_idx() << ": " << error << "\n";
            obj_id = obj_id + 1;
            std::lock_guard<std::mutex> incoming_guard(incoming_snapshot_mutex_);
            incoming_snapshot_ctx_.reset();
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

        std::lock_guard<std::mutex> incoming_guard(incoming_snapshot_mutex_);
        incoming_snapshot_ctx_.reset();
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
