#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include <libnuraft/nuraft.hxx>

#include "http_server.h"
#include "nuraft_state_initializer.h"
#include "nuraft_state_machine_sink.h"
#include "typesense_log_store.h"
#include "typesense_state_machine.h"
#include "typesense_state_manager.h"
#include "replication/replication_service.h"

struct NuRaftRaftParams {
    // Heartbeat and election timing.
    uint32_t heart_beat_interval_ms = 100;
    uint32_t election_timeout_lower_bound_ms = 200;
    uint32_t election_timeout_upper_bound_ms = 400;

    // Log compaction: how many log entries to keep after the last snapshot.
    uint32_t reserved_log_items = 5000;

    // Client request timeout (blocking mode wait).
    uint32_t client_req_timeout_ms = 3000;

    // Auto-forwarding: followers forward writes to the leader automatically.
    bool auto_forwarding = true;
    uint32_t auto_forwarding_req_timeout_ms = 5000;

    // Snapshot distance: number of commits between automatic snapshots.
    uint32_t snapshot_distance = 100000;

    // Leadership expiry: step down if no quorum acknowledgment within this time.
    // 0 = disabled (NuRaft default). Recommended: 5000ms for production multi-node.
    uint32_t leadership_expiry_ms = 5000;

    // ASIO transport thread pool size.
    uint32_t asio_thread_pool_size = 4;
};

struct NuRaftHttpServerOptions {
    NuRaftPrototypeOptions startup_options;
    std::string listen_address = "127.0.0.1";
    uint32_t listen_port = 8108;
    uint32_t request_timeout_ms = 60000;
    std::string api_key = "xyz";
    NuRaftRaftParams raft_params;
};

enum class NuRaftWriteRouteMode {
    kMirrorWorker,
    kOriginHandlerAfterRaft,
    kLocalOnly,
};

class NuRaftHttpRuntimeService : public ReplicationService {
public:
    struct MirroredWriteResult {
        bool handler_ok = false;
        bool should_apply = false;
        uint32_t status_code = 0;
        std::string body;
        std::string content_type_header;
        std::string error;
        uint64_t applied_index = 0;
    };

    explicit NuRaftHttpRuntimeService(HttpServer* server, NuRaftHttpServerOptions options);

    bool initialize(std::string& error);

    void write(const std::shared_ptr<http_req>& request,
               const std::shared_ptr<http_res>& response) override;
    bool is_read_caught_up() const override;
    bool is_write_caught_up() const override;
    bool is_alive() const override;
    uint64_t node_state() const override;
    nlohmann::json get_status() override;
    void do_snapshot(const std::string& snapshot_path,
                     const std::shared_ptr<http_req>& req,
                     const std::shared_ptr<http_res>& res) override;
    bool trigger_vote() override;
    bool reset_peers() override;
    void persist_applying_index() override;
    int64_t get_num_queued_writes() override;
    bool is_leader() override;
    std::string get_leader_url() const override;
    void decr_pending_writes() override;

    bool list_collections(nlohmann::json& result, std::string& error) const;
    bool read_collection(const std::string& collection, std::string& encoded, std::string& error) const;
    bool read_document(const std::string& collection,
                       const std::string& document_id,
                       std::string& encoded,
                       std::string& error) const;
    bool count_collection_documents(const std::string& collection, size_t& count, std::string& error) const;
    bool search_documents(const std::string& collection,
                          const std::map<std::string, std::string>& params,
                          nlohmann::json& result,
                          std::string& error) const;
    bool is_single_node_mode() const;
    bool wait_for_live_product_state(uint32_t timeout_ms, std::string& error);
    void shutdown();

private:
    void start_mirror_worker();
    void stop_mirror_worker();
    bool initialize_raft_server(std::string& error);
    bool process_document_import_write(const std::shared_ptr<http_req>& request,
                                       const std::shared_ptr<http_res>& response,
                                       uint64_t& committed_index,
                                       bool& forwarded_to_leader,
                                       std::string& error);
    bool append_via_raft(const std::string& request_payload,
                         uint16_t payload_encoding,
                         const http_req& request,
                         uint64_t& committed_index,
                         bool& forwarded_to_leader,
                         std::string& error);
    bool cache_enabled() const;
    bool read_materialized_value(const std::string& key,
                                 std::string& value,
                                 bool& found,
                                 std::string& error) const;
    bool read_materialized_prefix(const std::string& prefix,
                                  std::vector<std::pair<std::string, std::string>>& entries,
                                  std::string& error) const;
    bool count_materialized_prefix(const std::string& prefix, size_t& count, std::string& error) const;
    bool read_materialized_entries(std::vector<std::pair<std::string, std::string>>& entries,
                                   std::string& error) const;
    void update_single_node_document_cache(const http_req& request, NuRaftRouteKind route_kind);
    void invalidate_single_node_collection_cache(const std::string& collection);
    bool prefers_materialized_reads(const std::string& collection) const;
    void send_response(const std::shared_ptr<http_req>& request,
                       const std::shared_ptr<http_res>& response) const;
    void advance_live_product_state_applied_index(uint64_t applied_index);
    bool wait_for_applied_index(uint64_t target_index, uint32_t timeout_ms);
    uint64_t allocate_response_token();
    void enqueue_mirrored_request(const NuRaftAppliedRequest& request);
    void mirror_worker_loop();
    MirroredWriteResult apply_mirrored_request(const NuRaftAppliedRequest& applied_request);
    bool wait_for_mirrored_result(uint64_t response_token,
                                  uint32_t timeout_ms,
                                  MirroredWriteResult& result);
    void store_mirrored_result(uint64_t response_token, MirroredWriteResult result);

    HttpServer* server_;
    NuRaftHttpServerOptions options_;
    NuRaftIdentity identity_;
    NuRaftBootstrapConfig bootstrap_config_;
    NuRaftStateLayout layout_;
    std::atomic<bool> initialized_;
    mutable std::unique_ptr<NuRaftKvStateMachineSink> materialized_state_sink_;
    mutable std::shared_mutex document_cache_mutex_;
    mutable std::unordered_map<std::string, std::string> document_cache_;
    mutable std::shared_mutex read_preference_mutex_;
    mutable std::unordered_set<std::string> materialized_read_preferred_collections_;
    std::atomic<uint64_t> live_product_state_applied_index_{0};
    std::atomic<uint64_t> inflight_import_target_index_{0};
    std::atomic<uint64_t> active_import_requests_{0};
    std::atomic<uint64_t> cumulative_import_requests_{0};
    std::atomic<uint64_t> cumulative_import_bytes_{0};
    std::atomic<uint64_t> cumulative_import_docs_estimate_{0};
    std::atomic<uint64_t> last_import_request_bytes_{0};
    std::atomic<uint64_t> last_import_docs_estimate_{0};
    std::atomic<uint64_t> last_import_logical_chunks_{0};
    std::atomic<uint64_t> last_import_apply_chunks_{0};
    std::atomic<uint64_t> last_import_append_ms_{0};
    std::atomic<uint64_t> last_import_apply_wait_ms_{0};
    std::atomic<uint64_t> last_import_total_ms_{0};
    std::atomic<uint64_t> last_import_response_bytes_{0};
    std::atomic<uint64_t> last_import_docs_per_sec_{0};
    std::atomic<uint64_t> last_import_bytes_per_sec_{0};
    std::atomic<uint64_t> max_import_total_ms_{0};
    std::atomic<uint64_t> cumulative_sync_calls_{0};
    std::atomic<uint64_t> cumulative_sync_fast_path_hits_{0};
    std::atomic<uint64_t> cumulative_sync_total_ms_{0};
    std::atomic<uint64_t> last_sync_total_ms_{0};
    std::atomic<uint64_t> max_sync_total_ms_{0};
    std::mutex live_state_progress_mutex_;
    std::condition_variable live_state_progress_cv_;
    std::atomic<uint64_t> next_response_token_{1};
    std::mutex mirrored_results_mutex_;
    std::condition_variable mirrored_results_cv_;
    std::unordered_map<uint64_t, MirroredWriteResult> mirrored_results_;
    std::mutex mirror_worker_mutex_;
    std::condition_variable mirror_worker_cv_;
    std::deque<NuRaftAppliedRequest> mirror_worker_queue_;
    bool mirror_worker_stopping_ = false;
    std::thread mirror_worker_thread_;
    mutable std::shared_mutex mutex_;

    // Real NuRaft consensus members.
    std::unique_ptr<nuraft::raft_launcher> raft_launcher_;
    nuraft::ptr<TypesenseStateMachine> raft_state_machine_;
    nuraft::ptr<TypesenseStateManager> raft_state_manager_;
    nuraft::ptr<nuraft::raft_server> raft_server_;
};

bool nuraft_http_runtime_auth(std::map<std::string, std::string>& params,
                              std::vector<nlohmann::json>& embedded_params_vec,
                              const std::string& body,
                              const route_path& rpath,
                              const std::string& auth_key);

bool nuraft_http_runtime_lookup_write_route_mode(uint64_t route_hash,
                                                 NuRaftWriteRouteMode& mode);

void register_nuraft_http_runtime_routes(HttpServer* server);
