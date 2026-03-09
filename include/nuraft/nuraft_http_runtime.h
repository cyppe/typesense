#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "http_server.h"
#include "nuraft_replication_controller.h"
#include "replication/replication_service.h"

struct NuRaftHttpServerOptions {
    NuRaftPrototypeOptions startup_options;
    std::string listen_address = "127.0.0.1";
    uint32_t listen_port = 8108;
    std::string api_key = "xyz";
    std::string cluster_data_dirs;
    std::string install_snapshot_path;
    uint32_t cluster_leader_api_port = 0;
};

class NuRaftHttpRuntimeService : public ReplicationService {
public:
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

private:
    bool append_and_apply(const std::string& request_json,
                          uint64_t& appended_index,
                          bool& forwarded_to_leader,
                          int32_t& target_server_id,
                          std::string& error);
    bool apply_local_pending(uint64_t& applied_count, std::string& error);
    bool read_materialized_entries(std::vector<std::pair<std::string, std::string>>& entries,
                                   std::string& error) const;
    void send_response(const std::shared_ptr<http_req>& request,
                       const std::shared_ptr<http_res>& response) const;

    HttpServer* server_;
    NuRaftHttpServerOptions options_;
    NuRaftIdentity identity_;
    NuRaftBootstrapConfig bootstrap_config_;
    NuRaftStateLayout layout_;
    std::map<int32_t, std::string> cluster_data_dirs_;
    std::atomic<int32_t> preferred_leader_server_id_;
    std::atomic<bool> initialized_;
    mutable std::mutex mutex_;
};

bool nuraft_http_runtime_auth(std::map<std::string, std::string>& params,
                              std::vector<nlohmann::json>& embedded_params_vec,
                              const std::string& body,
                              const route_path& rpath,
                              const std::string& auth_key);

void register_nuraft_http_runtime_routes(HttpServer* server);
