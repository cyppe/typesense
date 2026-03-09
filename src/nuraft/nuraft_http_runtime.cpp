#include "nuraft/nuraft_http_runtime.h"

#include <exception>
#include <utility>

#include "core_api.h"
#include "json.hpp"
#include "nuraft/nuraft_request_journal.h"
#include "nuraft/nuraft_route_classifier.h"
#include "nuraft/nuraft_snapshot_coordinator.h"
#include "nuraft/nuraft_static_cluster.h"
#include "nuraft/nuraft_state_initializer.h"
#include "nuraft/nuraft_state_machine_sink.h"
#include "typesense_server_utils.h"
#include "tsconfig.h"

namespace {

constexpr const char* kCollectionPrefix = "state/collections/";
constexpr const char* kDocumentPrefix = "state/documents/";

NuRaftHttpRuntimeService* current_runtime_service() {
    return (server == nullptr) ? nullptr : dynamic_cast<NuRaftHttpRuntimeService*>(server->get_replication_state());
}

bool create_snapshot_response(const std::shared_ptr<http_req>& request, const std::shared_ptr<http_res>& response) {
    NuRaftHttpRuntimeService* runtime = current_runtime_service();
    if (runtime == nullptr) {
        response->set_500("NuRaft runtime service is not attached.");
        return true;
    }

    std::string snapshot_path;
    const auto it = request->params.find("snapshot_path");
    if (it != request->params.end()) {
        snapshot_path = it->second;
    }
    runtime->do_snapshot(snapshot_path, request, response);
    return true;
}

bool vote_response(const std::shared_ptr<http_req>& request, const std::shared_ptr<http_res>& response) {
    static_cast<void>(request);
    NuRaftHttpRuntimeService* runtime = current_runtime_service();
    if (runtime == nullptr) {
        response->set_500("NuRaft runtime service is not attached.");
        return true;
    }

    nlohmann::json body = {
        {"success", runtime->trigger_vote()},
    };
    response->set_body(200, body.dump());
    return true;
}

bool get_runtime_collections(const std::shared_ptr<http_req>& request, const std::shared_ptr<http_res>& response) {
    static_cast<void>(request);
    NuRaftHttpRuntimeService* runtime = current_runtime_service();
    if (runtime == nullptr) {
        response->set_500("NuRaft runtime service is not attached.");
        return true;
    }

    nlohmann::json body;
    std::string error;
    if (!runtime->list_collections(body, error)) {
        response->set_500(error);
        return true;
    }

    response->set_body(200, body.dump());
    return true;
}

bool get_runtime_collection(const std::shared_ptr<http_req>& request, const std::shared_ptr<http_res>& response) {
    NuRaftHttpRuntimeService* runtime = current_runtime_service();
    if (runtime == nullptr) {
        response->set_500("NuRaft runtime service is not attached.");
        return true;
    }

    const auto it = request->params.find("collection");
    if (it == request->params.end() || it->second.empty()) {
        response->set_400("Missing collection path parameter.");
        return true;
    }

    std::string encoded;
    std::string error;
    if (!runtime->read_collection(it->second, encoded, error)) {
        response->set_500(error);
        return true;
    }
    if (encoded.empty()) {
        response->set_404("Not Found");
        return true;
    }

    response->set_body(200, encoded);
    return true;
}

bool get_runtime_document(const std::shared_ptr<http_req>& request, const std::shared_ptr<http_res>& response) {
    NuRaftHttpRuntimeService* runtime = current_runtime_service();
    if (runtime == nullptr) {
        response->set_500("NuRaft runtime service is not attached.");
        return true;
    }

    const auto collection_it = request->params.find("collection");
    const auto id_it = request->params.find("id");
    if (collection_it == request->params.end() || collection_it->second.empty() ||
        id_it == request->params.end() || id_it->second.empty()) {
        response->set_400("Missing collection or id path parameter.");
        return true;
    }

    std::string encoded;
    std::string error;
    if (!runtime->read_document(collection_it->second, id_it->second, encoded, error)) {
        response->set_500(error);
        return true;
    }
    if (encoded.empty()) {
        response->set_404("Not Found");
        return true;
    }

    response->set_body(200, encoded);
    return true;
}

bool write_placeholder(const std::shared_ptr<http_req>& request, const std::shared_ptr<http_res>& response) {
    static_cast<void>(request);
    static_cast<void>(response);
    return true;
}

}  // namespace

NuRaftHttpRuntimeService::NuRaftHttpRuntimeService(HttpServer* server, NuRaftHttpServerOptions options)
    : server_(server),
      options_(std::move(options)),
      layout_(NuRaftStateLayout::from_data_dir(options_.startup_options.data_dir)),
      preferred_leader_server_id_(0),
      initialized_(false) {}

bool NuRaftHttpRuntimeService::initialize(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!NuRaftStateInitializer::initialize(options_.startup_options, identity_, bootstrap_config_, error)) {
        return false;
    }

    if (!options_.install_snapshot_path.empty()) {
        NuRaftSnapshotDescriptor descriptor;
        NuRaftSnapshotCoordinator coordinator(layout_);
        if (!coordinator.install_snapshot(options_.install_snapshot_path, descriptor, error)) {
            return false;
        }

        NuRaftMetadataStore metadata_store(layout_);
        if (!metadata_store.read_identity(identity_, error) ||
            !metadata_store.read_bootstrap_config(bootstrap_config_, error)) {
            return false;
        }
    }

    if (!options_.cluster_data_dirs.empty()) {
        if (!NuRaftStaticCluster::parse_data_dir_map(options_.cluster_data_dirs, cluster_data_dirs_, error)) {
            return false;
        }
        preferred_leader_server_id_.store(static_cast<int32_t>(options_.cluster_leader_api_port == 0 ?
                                                               options_.startup_options.api_port :
                                                               options_.cluster_leader_api_port));
    } else {
        cluster_data_dirs_.clear();
        cluster_data_dirs_.emplace(identity_.server_id, options_.startup_options.data_dir);
        preferred_leader_server_id_.store(identity_.server_id);
    }

    uint64_t applied_count = 0;
    if (!apply_local_pending(applied_count, error)) {
        return false;
    }

    initialized_.store(true);
    error.clear();
    return true;
}

void NuRaftHttpRuntimeService::send_response(const std::shared_ptr<http_req>& request,
                                             const std::shared_ptr<http_res>& response) const {
    if (!response->is_alive) {
        return;
    }

    response->wait();
    auto* req_res = new async_req_res_t(request, response, true);
    server_->get_message_dispatcher()->send_message(HttpServer::STREAM_RESPONSE_MESSAGE, req_res);
}

bool NuRaftHttpRuntimeService::append_and_apply(const std::string& request_json,
                                                uint64_t& appended_index,
                                                bool& forwarded_to_leader,
                                                int32_t& target_server_id,
                                                std::string& error) {
    appended_index = 0;
    forwarded_to_leader = false;
    target_server_id = identity_.server_id;

    if (!options_.cluster_data_dirs.empty()) {
        if (!NuRaftStaticCluster::append_request(bootstrap_config_,
                                                 cluster_data_dirs_,
                                                 preferred_leader_server_id_.load(),
                                                 identity_.server_id,
                                                 request_json,
                                                 appended_index,
                                                 forwarded_to_leader,
                                                 target_server_id,
                                                 error)) {
            return false;
        }

        std::vector<NuRaftStaticClusterNodeStatus> statuses;
        return NuRaftStaticCluster::replicate_and_apply(bootstrap_config_,
                                                        cluster_data_dirs_,
                                                        preferred_leader_server_id_.load(),
                                                        "kv",
                                                        statuses,
                                                        error);
    }

    NuRaftRequestJournal request_journal(layout_);
    if (!request_journal.initialize(error)) {
        return false;
    }
    if (!request_journal.append_request_json(request_json, appended_index, error)) {
        return false;
    }

    uint64_t applied_count = 0;
    return apply_local_pending(applied_count, error);
}

bool NuRaftHttpRuntimeService::apply_local_pending(uint64_t& applied_count, std::string& error) {
    auto sink = std::make_unique<NuRaftKvStateMachineSink>(layout_);
    NuRaftPrototypeStateMachine state_machine(layout_, std::move(sink));
    if (!state_machine.initialize(error)) {
        return false;
    }

    std::vector<NuRaftLogEntry> applied_entries;
    if (!state_machine.apply_pending(applied_entries, error)) {
        return false;
    }

    applied_count = applied_entries.size();
    error.clear();
    return true;
}

bool NuRaftHttpRuntimeService::read_materialized_entries(
    std::vector<std::pair<std::string, std::string>>& entries,
    std::string& error) const {
    NuRaftKvStateMachineSink sink(layout_);
    return sink.read_materialized_entries(entries, error);
}

void NuRaftHttpRuntimeService::write(const std::shared_ptr<http_req>& request,
                                     const std::shared_ptr<http_res>& response) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    if (!initialized_.load()) {
        response->set_500("NuRaft runtime service is not initialized.");
        send_response(request, response);
        return;
    }

    const NuRaftRouteKind route_kind = NuRaftRouteClassifier::classify(request->route_hash);
    if (route_kind != NuRaftRouteKind::kCollectionCreate &&
        route_kind != NuRaftRouteKind::kCollectionDrop &&
        route_kind != NuRaftRouteKind::kDocumentWrite &&
        route_kind != NuRaftRouteKind::kDocumentDelete &&
        route_kind != NuRaftRouteKind::kDocumentImport) {
        response->set_422("Unsupported NuRaft runtime write route.");
        send_response(request, response);
        return;
    }

    uint64_t appended_index = 0;
    bool forwarded_to_leader = false;
    int32_t target_server_id = identity_.server_id;
    if (!append_and_apply(request->to_json(), appended_index, forwarded_to_leader, target_server_id, error)) {
        response->set_500(error);
        send_response(request, response);
        return;
    }

    nlohmann::json response_body = {
        {"success", true},
        {"appended_index", appended_index},
        {"forwarded_to_leader", forwarded_to_leader},
        {"target_server_id", target_server_id},
    };

    if (request->http_method == "POST" && !request->body.empty()) {
        try {
            response_body["result"] = nlohmann::json::parse(request->body);
        } catch (const std::exception&) {
            response_body["result_raw"] = request->body;
        }
        response->set_body(201, response_body.dump());
    } else {
        response->set_body(200, response_body.dump());
    }

    send_response(request, response);
}

bool NuRaftHttpRuntimeService::is_read_caught_up() const {
    return initialized_.load();
}

bool NuRaftHttpRuntimeService::is_write_caught_up() const {
    return initialized_.load();
}

bool NuRaftHttpRuntimeService::is_alive() const {
    return initialized_.load();
}

uint64_t NuRaftHttpRuntimeService::node_state() const {
    return initialized_.load() ? 1 : 0;
}

nlohmann::json NuRaftHttpRuntimeService::get_status() {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json status = {
        {"state", initialized_.load() ? "running" : "initializing"},
        {"server_id", identity_.server_id},
        {"is_leader", is_leader()},
        {"leader_url", get_leader_url()},
        {"read_caught_up", is_read_caught_up()},
        {"write_caught_up", is_write_caught_up()},
        {"queued_writes", 0},
    };

    std::string error;
    std::vector<NuRaftStaticClusterNodeStatus> statuses;
    if (NuRaftStaticCluster::collect_status(bootstrap_config_,
                                            cluster_data_dirs_,
                                            preferred_leader_server_id_.load(),
                                            statuses,
                                            error)) {
        for (const auto& node_status : statuses) {
            if (node_status.server_id == identity_.server_id) {
                status["last_index"] = node_status.last_log_index;
                status["committed_index"] = node_status.last_log_index;
                status["known_applied_index"] = node_status.last_applied_index;
                status["applying_index"] = 0;
                break;
            }
        }
    } else {
        status["error"] = error;
    }

    return status;
}

void NuRaftHttpRuntimeService::do_snapshot(const std::string& snapshot_path,
                                           const std::shared_ptr<http_req>& req,
                                           const std::shared_ptr<http_res>& res) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    NuRaftKvStateMachineSink sink(layout_);
    NuRaftSnapshotDescriptor descriptor;
    NuRaftSnapshotCoordinator coordinator(layout_);
    if (!coordinator.create_snapshot(snapshot_path, &sink, descriptor, error)) {
        res->set_500(error);
        send_response(req, res);
        return;
    }

    nlohmann::json body = {
        {"success", true},
        {"snapshot_id", descriptor.snapshot_id},
        {"last_log_index", descriptor.last_log_index},
        {"last_applied_index", descriptor.last_applied_index},
    };
    res->set_body(201, body.dump());
    send_response(req, res);
}

bool NuRaftHttpRuntimeService::trigger_vote() {
    preferred_leader_server_id_.store(identity_.server_id);
    return true;
}

bool NuRaftHttpRuntimeService::reset_peers() {
    return false;
}

void NuRaftHttpRuntimeService::persist_applying_index() {}

int64_t NuRaftHttpRuntimeService::get_num_queued_writes() {
    return 0;
}

bool NuRaftHttpRuntimeService::is_leader() {
    return preferred_leader_server_id_.load() == identity_.server_id;
}

std::string NuRaftHttpRuntimeService::get_leader_url() const {
    NuRaftPeerAddress leader;
    std::string error;
    if (!NuRaftStaticCluster::discover_leader(bootstrap_config_,
                                              preferred_leader_server_id_.load(),
                                              leader,
                                              error)) {
        return "";
    }

    return leader.leader_url(bootstrap_config_.api_uses_ssl);
}

void NuRaftHttpRuntimeService::decr_pending_writes() {}

bool NuRaftHttpRuntimeService::list_collections(nlohmann::json& result, std::string& error) const {
    std::vector<std::pair<std::string, std::string>> entries;
    if (!read_materialized_entries(entries, error)) {
        return false;
    }

    result = nlohmann::json::array();
    for (const auto& entry : entries) {
        if (entry.first.rfind(kCollectionPrefix, 0) != 0) {
            continue;
        }
        result.push_back(nlohmann::json::parse(entry.second));
    }

    error.clear();
    return true;
}

bool NuRaftHttpRuntimeService::read_collection(const std::string& collection,
                                               std::string& encoded,
                                               std::string& error) const {
    encoded.clear();
    std::vector<std::pair<std::string, std::string>> entries;
    if (!read_materialized_entries(entries, error)) {
        return false;
    }

    const std::string key = std::string(kCollectionPrefix) + collection;
    for (const auto& entry : entries) {
        if (entry.first == key) {
            encoded = entry.second;
            break;
        }
    }

    error.clear();
    return true;
}

bool NuRaftHttpRuntimeService::read_document(const std::string& collection,
                                             const std::string& document_id,
                                             std::string& encoded,
                                             std::string& error) const {
    encoded.clear();
    std::vector<std::pair<std::string, std::string>> entries;
    if (!read_materialized_entries(entries, error)) {
        return false;
    }

    const std::string key = std::string(kDocumentPrefix) + collection + "/" + document_id;
    for (const auto& entry : entries) {
        if (entry.first == key) {
            encoded = entry.second;
            break;
        }
    }

    error.clear();
    return true;
}

bool nuraft_http_runtime_auth(std::map<std::string, std::string>& params,
                              std::vector<nlohmann::json>& embedded_params_vec,
                              const std::string& body,
                              const route_path& rpath,
                              const std::string& auth_key) {
    static_cast<void>(params);
    static_cast<void>(embedded_params_vec);
    static_cast<void>(body);

    if (rpath.handler == get_health) {
        return true;
    }

    const std::string configured_api_key = Config::get_instance().get_api_key();
    return configured_api_key.empty() || configured_api_key == auth_key;
}

void register_nuraft_http_runtime_routes(HttpServer* server) {
    server->get("/health", get_health);
    server->get("/status", get_status);

    server->get("/collections", get_runtime_collections);
    server->get("/collections/:collection", get_runtime_collection);
    server->get("/collections/:collection/documents/:id", get_runtime_document);

    server->post("/collections", write_placeholder);
    server->del("/collections/:collection", write_placeholder);
    server->post("/collections/:collection/documents", write_placeholder);
    server->post("/collections/:collection/documents/import", write_placeholder, true, true);

    server->post("/operations/snapshot", create_snapshot_response, false, true);
    server->post("/operations/vote", vote_response);
}
