#include "nuraft/nuraft_http_runtime.h"

#include <exception>
#include <set>
#include <utility>

#include "core_api.h"
#include "json.hpp"
#include "collection_manager.h"
#include "nuraft/nuraft_request_journal.h"
#include "nuraft/nuraft_route_classifier.h"
#include "nuraft/nuraft_snapshot_coordinator.h"
#include "nuraft/nuraft_static_cluster.h"
#include "nuraft/nuraft_state_initializer.h"
#include "nuraft/nuraft_state_machine_sink.h"
#include "nuraft/nuraft_metadata_store.h"
#include "typesense_server_utils.h"
#include "tsconfig.h"

namespace {

constexpr const char* kCollectionPrefix = "state/collections/";
constexpr const char* kDocumentPrefix = "state/documents/";

nlohmann::json normalize_collection_field(const nlohmann::json& field) {
    nlohmann::json normalized = field;
    normalized["facet"] = normalized.value("facet", false);
    normalized["index"] = normalized.value("index", true);
    normalized["infix"] = normalized.value("infix", false);
    normalized["locale"] = normalized.value("locale", "");
    normalized["optional"] = normalized.value("optional", false);
    normalized["sort"] = normalized.value("sort", true);
    normalized["stem"] = normalized.value("stem", false);
    normalized["stem_dictionary"] = normalized.value("stem_dictionary", "");
    normalized["store"] = normalized.value("store", true);
    return normalized;
}

bool normalize_collection_payload(const std::string& collection_name,
                                  const std::string& encoded,
                                  size_t num_documents,
                                  nlohmann::json& normalized,
                                  std::string& error) {
    try {
        normalized = nlohmann::json::parse(encoded);
    } catch (const std::exception& e) {
        error = std::string("Failed to parse NuRaft collection payload: ") + e.what();
        return false;
    }

    if (!normalized.is_object()) {
        error = "NuRaft collection payload must be a JSON object";
        return false;
    }

    normalized["name"] = normalized.value("name", collection_name);
    normalized["created_at"] = normalized.value("created_at", 0);
    normalized["default_sorting_field"] = normalized.value("default_sorting_field", "");
    normalized["enable_nested_fields"] = normalized.value("enable_nested_fields", false);
    normalized["num_documents"] = num_documents;
    normalized["symbols_to_index"] = normalized.value("symbols_to_index", nlohmann::json::array());
    normalized["token_separators"] = normalized.value("token_separators", nlohmann::json::array());

    nlohmann::json normalized_fields = nlohmann::json::array();
    if (normalized.contains("fields") && normalized["fields"].is_array()) {
        for (const auto& field : normalized["fields"]) {
            if (!field.is_object()) {
                error = "NuRaft collection fields must be JSON objects";
                return false;
            }
            normalized_fields.push_back(normalize_collection_field(field));
        }
    }
    normalized["fields"] = std::move(normalized_fields);

    error.clear();
    return true;
}

bool parse_json_if_present(const std::string& body, nlohmann::json& parsed) {
    try {
        parsed = nlohmann::json::parse(body);
        return true;
    } catch (const std::exception&) {
        parsed = nlohmann::json();
        return false;
    }
}

std::string ascii_lower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool contains_case_insensitive(const std::string& haystack, const std::string& needle) {
    return ascii_lower(haystack).find(ascii_lower(needle)) != std::string::npos;
}

bool extract_document_id_from_json(const nlohmann::json& parsed, std::string& document_id) {
    if (!parsed.is_object() || !parsed.contains("id")) {
        return false;
    }

    if (parsed["id"].is_string()) {
        document_id = parsed["id"].get<std::string>();
        return true;
    }
    if (parsed["id"].is_number_integer()) {
        document_id = std::to_string(parsed["id"].get<int64_t>());
        return true;
    }
    if (parsed["id"].is_number_unsigned()) {
        document_id = std::to_string(parsed["id"].get<uint64_t>());
        return true;
    }

    return false;
}

std::string document_materialized_key(const std::string& collection, const std::string& document_id) {
    return std::string(kDocumentPrefix) + collection + "/" + document_id;
}

void build_applied_request_from_http_request(const http_req& request,
                                             uint64_t index,
                                             NuRaftAppliedRequest& applied_request) {
    applied_request.index = index;
    applied_request.route_hash = request.route_hash;
    applied_request.route_kind = NuRaftRouteClassifier::classify(request.route_hash);
    applied_request.params = request.params;
    applied_request.metadata = request.metadata;
    applied_request.body = request.body;
    applied_request.first_chunk_aggregate = request.first_chunk_aggregate;
    applied_request.last_chunk_aggregate = request.last_chunk_aggregate.load();
    applied_request.start_ts = request.start_ts;
    applied_request.log_index = request.log_index;
    applied_request.is_binary_body = request.is_binary_body;
}

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

bool apply_typesense_write_handler(const std::shared_ptr<http_req>& request,
                                   bool (*handler)(const std::shared_ptr<http_req>&,
                                                   const std::shared_ptr<http_res>&),
                                   std::string& error) {
    auto response = std::make_shared<http_res>(nullptr);
    if (handler(request, response)) {
        error.clear();
        return true;
    }

    if (!response->body.empty()) {
        error = response->body;
    } else if (response->status_code != 0) {
        error = std::string("Typesense state mirror failed with HTTP ") +
                std::to_string(response->status_code) + " " +
                http_res::get_status_reason(response->status_code);
    } else {
        error = "Typesense state mirror failed without a response body";
    }
    return false;
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

bool search_runtime_documents(const std::shared_ptr<http_req>& request, const std::shared_ptr<http_res>& response) {
    NuRaftHttpRuntimeService* runtime = current_runtime_service();
    if (runtime == nullptr) {
        response->set_500("NuRaft runtime service is not attached.");
        return true;
    }

    const auto collection_it = request->params.find("collection");
    if (collection_it == request->params.end() || collection_it->second.empty()) {
        response->set_400("Missing collection path parameter.");
        return true;
    }

    nlohmann::json body;
    std::string error;
    if (!runtime->search_documents(collection_it->second, request->params, body, error)) {
        response->set_500(error);
        return true;
    }

    response->set_body(200, body.dump());
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

bool NuRaftHttpRuntimeService::cache_enabled() const {
    return options_.cluster_data_dirs.empty() && materialized_state_sink_ != nullptr;
}

bool mirror_single_node_typesense_state(const std::shared_ptr<http_req>& request,
                                        NuRaftRouteKind route_kind,
                                        std::string& error) {
    switch (route_kind) {
        case NuRaftRouteKind::kCollectionCreate:
            return apply_typesense_write_handler(request, post_create_collection, error);
        case NuRaftRouteKind::kCollectionDrop:
            return apply_typesense_write_handler(request, del_drop_collection, error);
        case NuRaftRouteKind::kDocumentWrite:
            if (request->http_method == "PATCH") {
                return apply_typesense_write_handler(request, patch_update_document, error);
            }
            return apply_typesense_write_handler(request, post_add_document, error);
        case NuRaftRouteKind::kDocumentDelete:
            return apply_typesense_write_handler(request, del_remove_document, error);
        case NuRaftRouteKind::kDocumentImport:
            return apply_typesense_write_handler(request, post_import_documents, error);
        default:
            error.clear();
            return true;
    }
}

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
    if (options_.cluster_data_dirs.empty()) {
        local_request_journal_ = std::make_unique<NuRaftRequestJournal>(layout_);
        materialized_state_sink_ = std::make_unique<NuRaftKvStateMachineSink>(layout_);
        if (!local_request_journal_->initialize(error)) {
            local_request_journal_.reset();
            materialized_state_sink_.reset();
            return false;
        }
    } else {
        local_request_journal_.reset();
        materialized_state_sink_.reset();
    }
    if (!apply_local_pending(applied_count, error)) {
        local_request_journal_.reset();
        materialized_state_sink_.reset();
        return false;
    }

    if (options_.cluster_data_dirs.empty()) {
        uint64_t last_applied_index = 0;
        if (!read_last_local_applied_index(last_applied_index, error) ||
            !sync_local_replay_progress(last_applied_index, error)) {
            local_request_journal_.reset();
            materialized_state_sink_.reset();
            return false;
        }
    }

    {
        std::unique_lock<std::shared_mutex> cache_lock(document_cache_mutex_);
        document_cache_.clear();
    }
    {
        std::unique_lock<std::shared_mutex> preference_lock(read_preference_mutex_);
        materialized_read_preferred_collections_.clear();
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
                                                const http_req& request,
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

    if (local_request_journal_ != nullptr) {
        if (!local_request_journal_->append_request_json(request_json, appended_index, error)) {
            return false;
        }
        NuRaftAppliedRequest applied_request;
        build_applied_request_from_http_request(request, appended_index, applied_request);
        if (!apply_single_local_append(applied_request, error)) {
            return false;
        }
        forwarded_to_leader = false;
        target_server_id = identity_.server_id;
        error.clear();
        return true;
    } else {
        NuRaftRequestJournal request_journal(layout_);
        if (!request_journal.initialize(error) ||
            !request_journal.append_request_json(request_json, appended_index, error)) {
            return false;
        }
    }

    uint64_t applied_count = 0;
    return apply_local_pending(applied_count, error);
}

bool NuRaftHttpRuntimeService::apply_single_local_append(const NuRaftAppliedRequest& applied_request,
                                                         std::string& error) {
    if (materialized_state_sink_ == nullptr) {
        error = "NuRaft runtime materialized state sink is not initialized.";
        return false;
    }

    if (!materialized_state_sink_->apply_all({applied_request}, error)) {
        return false;
    }

    error.clear();
    return true;
}

bool NuRaftHttpRuntimeService::apply_local_pending(uint64_t& applied_count, std::string& error) {
    if (local_request_journal_ != nullptr && materialized_state_sink_ != nullptr) {
        uint64_t last_applied_index = 0;
        if (!read_last_local_applied_index(last_applied_index, error)) {
            return false;
        }

        std::vector<NuRaftLogEntry> all_entries;
        if (!local_request_journal_->replay(all_entries, error)) {
            return false;
        }

        std::vector<NuRaftAppliedRequest> pending_requests;
        pending_requests.reserve(all_entries.size());
        for (const auto& entry : all_entries) {
            if (entry.index <= last_applied_index) {
                continue;
            }

            NuRaftAppliedRequest applied_request;
            if (!NuRaftAppliedRequest::from_log_entry(entry, applied_request, error)) {
                return false;
            }
            pending_requests.push_back(std::move(applied_request));
        }

        if (pending_requests.empty()) {
            applied_count = 0;
            error.clear();
            return true;
        }

        if (!materialized_state_sink_->apply_all(pending_requests, error)) {
            return false;
        }

        applied_count = pending_requests.size();
        error.clear();
        return true;
    }

    std::vector<NuRaftLogEntry> applied_entries;
    auto sink = std::make_unique<NuRaftKvStateMachineSink>(layout_);
    NuRaftPrototypeStateMachine state_machine(layout_, std::move(sink));
    if (!state_machine.initialize(error) || !state_machine.apply_pending(applied_entries, error)) {
        return false;
    }

    applied_count = applied_entries.size();
    error.clear();
    return true;
}

bool NuRaftHttpRuntimeService::read_last_local_applied_index(uint64_t& last_applied_index,
                                                             std::string& error) const {
    last_applied_index = 0;
    if (materialized_state_sink_ == nullptr) {
        error = "NuRaft runtime materialized state sink is not initialized.";
        return false;
    }

    return materialized_state_sink_->read_last_applied_index(last_applied_index, error);
}

bool NuRaftHttpRuntimeService::sync_local_replay_progress(uint64_t last_applied_index,
                                                          std::string& error) const {
    NuRaftMetadataStore metadata_store(layout_);
    if (!metadata_store.initialize(error)) {
        return false;
    }

    NuRaftReplayProgress progress;
    progress.last_applied_index = last_applied_index;
    return metadata_store.write_replay_progress(progress, error);
}

bool NuRaftHttpRuntimeService::read_materialized_value(const std::string& key,
                                                       std::string& value,
                                                       bool& found,
                                                       std::string& error) const {
    if (materialized_state_sink_ != nullptr) {
        return materialized_state_sink_->read_materialized_value(key, value, found, error);
    }

    NuRaftKvStateMachineSink sink(layout_);
    return sink.read_materialized_value(key, value, found, error);
}

bool NuRaftHttpRuntimeService::read_materialized_prefix(
    const std::string& prefix,
    std::vector<std::pair<std::string, std::string>>& entries,
    std::string& error) const {
    if (materialized_state_sink_ != nullptr) {
        return materialized_state_sink_->read_materialized_prefix(prefix, entries, error);
    }

    NuRaftKvStateMachineSink sink(layout_);
    return sink.read_materialized_prefix(prefix, entries, error);
}

bool NuRaftHttpRuntimeService::count_materialized_prefix(const std::string& prefix,
                                                         size_t& count,
                                                         std::string& error) const {
    if (materialized_state_sink_ != nullptr) {
        return materialized_state_sink_->count_materialized_prefix(prefix, count, error);
    }

    NuRaftKvStateMachineSink sink(layout_);
    return sink.count_materialized_prefix(prefix, count, error);
}

bool NuRaftHttpRuntimeService::read_materialized_entries(
    std::vector<std::pair<std::string, std::string>>& entries,
    std::string& error) const {
    if (materialized_state_sink_ != nullptr) {
        return materialized_state_sink_->read_materialized_entries(entries, error);
    }

    NuRaftKvStateMachineSink sink(layout_);
    return sink.read_materialized_entries(entries, error);
}

void NuRaftHttpRuntimeService::invalidate_single_node_collection_cache(const std::string& collection) {
    if (!cache_enabled()) {
        return;
    }

    const std::string prefix = std::string(kDocumentPrefix) + collection + "/";
    std::unique_lock<std::shared_mutex> cache_lock(document_cache_mutex_);
    for (auto it = document_cache_.begin(); it != document_cache_.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            it = document_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

bool NuRaftHttpRuntimeService::prefers_materialized_reads(const std::string& collection) const {
    std::shared_lock<std::shared_mutex> preference_lock(read_preference_mutex_);
    return materialized_read_preferred_collections_.find(collection) != materialized_read_preferred_collections_.end();
}

void NuRaftHttpRuntimeService::update_single_node_document_cache(const http_req& request,
                                                                 NuRaftRouteKind route_kind) {
    if (!cache_enabled()) {
        return;
    }

    const auto collection_it = request.params.find("collection");
    if (collection_it == request.params.end() || collection_it->second.empty()) {
        return;
    }
    const std::string& collection = collection_it->second;

    if (route_kind == NuRaftRouteKind::kCollectionDrop) {
        invalidate_single_node_collection_cache(collection);
        std::unique_lock<std::shared_mutex> preference_lock(read_preference_mutex_);
        materialized_read_preferred_collections_.erase(collection);
        return;
    }

    if (route_kind == NuRaftRouteKind::kDocumentImport) {
        invalidate_single_node_collection_cache(collection);
        std::unique_lock<std::shared_mutex> preference_lock(read_preference_mutex_);
        materialized_read_preferred_collections_.insert(collection);
        return;
    }

    std::string document_id;
    const auto id_it = request.params.find("id");
    if (id_it != request.params.end() && !id_it->second.empty()) {
        document_id = id_it->second;
    } else {
        nlohmann::json parsed_body;
        if (parse_json_if_present(request.body, parsed_body)) {
            extract_document_id_from_json(parsed_body, document_id);
        }
    }

    if (document_id.empty()) {
        return;
    }

    const std::string key = document_materialized_key(collection, document_id);
    if (route_kind == NuRaftRouteKind::kDocumentDelete) {
        std::unique_lock<std::shared_mutex> cache_lock(document_cache_mutex_);
        document_cache_.erase(key);
        return;
    }

    bool cache_contains_key = false;
    {
        std::shared_lock<std::shared_mutex> cache_lock(document_cache_mutex_);
        cache_contains_key = document_cache_.find(key) != document_cache_.end();
    }

    if (request.http_method != "PATCH") {
        if (!cache_contains_key) {
            return;
        }
        std::unique_lock<std::shared_mutex> cache_lock(document_cache_mutex_);
        document_cache_[key] = request.body;
        return;
    }

    nlohmann::json patch_body;
    if (!parse_json_if_present(request.body, patch_body) || !patch_body.is_object()) {
        return;
    }

    {
        std::shared_lock<std::shared_mutex> cache_lock(document_cache_mutex_);
        const auto existing = document_cache_.find(key);
        if (existing != document_cache_.end()) {
            nlohmann::json merged;
            if (parse_json_if_present(existing->second, merged) && merged.is_object()) {
                for (auto it = patch_body.begin(); it != patch_body.end(); ++it) {
                    merged[it.key()] = it.value();
                }
                cache_lock.unlock();
                std::unique_lock<std::shared_mutex> write_lock(document_cache_mutex_);
                document_cache_[key] = merged.dump();
                return;
            }
        }
    }

    if (!cache_contains_key) {
        return;
    }

    std::string stored_document;
    bool found = false;
    std::string error;
    if (!read_materialized_value(key, stored_document, found, error) || !found) {
        return;
    }

    std::unique_lock<std::shared_mutex> cache_lock(document_cache_mutex_);
    document_cache_[key] = stored_document;
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

    std::string previous_collection_body;
    std::string previous_document_body;
    if (route_kind == NuRaftRouteKind::kCollectionDrop) {
        const auto collection_it = request->params.find("collection");
        if (collection_it != request->params.end()) {
            std::string ignored_error;
            read_collection(collection_it->second, previous_collection_body, ignored_error);
        }
    } else if (route_kind == NuRaftRouteKind::kDocumentDelete) {
        const auto collection_it = request->params.find("collection");
        const auto id_it = request->params.find("id");
        if (collection_it != request->params.end() && id_it != request->params.end()) {
            std::string ignored_error;
            read_document(collection_it->second, id_it->second, previous_document_body, ignored_error);
        }
    }

    request->metadata = request->http_method;

    uint64_t appended_index = 0;
    bool forwarded_to_leader = false;
    int32_t target_server_id = identity_.server_id;
    const std::string request_json = request->to_json();
    if (!append_and_apply(request_json, *request, appended_index, forwarded_to_leader, target_server_id, error)) {
        response->set_500(error);
        send_response(request, response);
        return;
    }

    if (options_.cluster_data_dirs.empty() && !mirror_single_node_typesense_state(request, route_kind, error)) {
        TS_LOG(WARNING) << "NuRaft runtime skipped live Typesense state mirror: " << error;
        error.clear();
    }

    update_single_node_document_cache(*request, route_kind);

    nlohmann::json response_body = {
        {"success", true},
        {"appended_index", appended_index},
        {"forwarded_to_leader", forwarded_to_leader},
        {"target_server_id", target_server_id},
    };

    nlohmann::json top_level_result;
    bool has_top_level_result = false;
    if (route_kind == NuRaftRouteKind::kCollectionCreate) {
        if (!normalize_collection_payload("",
                                          request->body,
                                          0,
                                          top_level_result,
                                          error)) {
            response->set_500(error);
            send_response(request, response);
            return;
        }
        has_top_level_result = true;
    } else if (route_kind == NuRaftRouteKind::kCollectionDrop && !previous_collection_body.empty()) {
        const auto collection_it = request->params.find("collection");
        const std::string collection_name = collection_it == request->params.end() ? "" : collection_it->second;
        if (!normalize_collection_payload(collection_name,
                                          previous_collection_body,
                                          0,
                                          top_level_result,
                                          error)) {
            response->set_500(error);
            send_response(request, response);
            return;
        }
        has_top_level_result = true;
    } else if (route_kind == NuRaftRouteKind::kDocumentWrite) {
        nlohmann::json parsed_body;
        const bool parsed_request_body = parse_json_if_present(request->body, parsed_body) && parsed_body.is_object();
        if (request->http_method != "PATCH" && parsed_request_body) {
            top_level_result = parsed_body;
            has_top_level_result = true;
        } else {
            auto collection_it = request->params.find("collection");
            if (collection_it != request->params.end()) {
                std::string document_id;
                auto id_it = request->params.find("id");
                if (id_it != request->params.end()) {
                    document_id = id_it->second;
                } else if (parsed_request_body) {
                    extract_document_id_from_json(parsed_body, document_id);
                }

                if (!document_id.empty()) {
                    std::string stored_document;
                    if (read_document(collection_it->second, document_id, stored_document, error)) {
                        has_top_level_result = parse_json_if_present(stored_document, top_level_result) &&
                                               top_level_result.is_object();
                    } else {
                        response->set_500(error);
                        send_response(request, response);
                        return;
                    }
                }
            }
        }
    } else if (route_kind == NuRaftRouteKind::kDocumentImport &&
               parse_json_if_present(request->body, top_level_result) && top_level_result.is_object()) {
        has_top_level_result = true;
    } else if (route_kind == NuRaftRouteKind::kDocumentDelete &&
               parse_json_if_present(previous_document_body, top_level_result) && top_level_result.is_object()) {
        has_top_level_result = true;
    }

    if (request->http_method == "POST" && !request->body.empty()) {
        try {
            response_body["result"] = nlohmann::json::parse(request->body);
        } catch (const std::exception&) {
            response_body["result_raw"] = request->body;
        }
    }

    if (has_top_level_result) {
        if (!response_body.contains("result")) {
            response_body["result"] = top_level_result;
        }
        for (auto it = response_body.begin(); it != response_body.end(); ++it) {
            top_level_result[it.key()] = it.value();
        }
        response->set_body(request->http_method == "POST" ? 201 : 200, top_level_result.dump());
    } else if (request->http_method == "POST" && !request->body.empty()) {
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
    NuRaftKvStateMachineSink* snapshot_sink = materialized_state_sink_ != nullptr ? materialized_state_sink_.get() : &sink;
    if (materialized_state_sink_ != nullptr) {
        uint64_t last_applied_index = 0;
        if (!read_last_local_applied_index(last_applied_index, error) ||
            !sync_local_replay_progress(last_applied_index, error)) {
            res->set_500(error);
            send_response(req, res);
            return;
        }
    }
    NuRaftSnapshotDescriptor descriptor;
    NuRaftSnapshotCoordinator coordinator(layout_);
    if (!coordinator.create_snapshot(snapshot_path, snapshot_sink, descriptor, error)) {
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
    if (!read_materialized_prefix(kCollectionPrefix, entries, error)) {
        return false;
    }

    result = nlohmann::json::array();
    for (const auto& entry : entries) {
        if (entry.first.rfind(kCollectionPrefix, 0) != 0) {
            continue;
        }
        const std::string collection_name = entry.first.substr(std::string(kCollectionPrefix).size());
        size_t num_documents = 0;
        if (!count_collection_documents(collection_name, num_documents, error)) {
            return false;
        }

        nlohmann::json normalized_collection;
        if (!normalize_collection_payload(collection_name, entry.second, num_documents, normalized_collection, error)) {
            return false;
        }
        result.push_back(std::move(normalized_collection));
    }

    error.clear();
    return true;
}

bool NuRaftHttpRuntimeService::read_collection(const std::string& collection,
                                               std::string& encoded,
                                               std::string& error) const {
    encoded.clear();
    if (options_.cluster_data_dirs.empty() && !prefers_materialized_reads(collection)) {
        auto live_collection = CollectionManager::get_instance().get_collection(collection);
        if (live_collection != nullptr) {
            encoded = live_collection->get_summary_json().dump(-1, ' ', false, nlohmann::detail::error_handler_t::ignore);
            error.clear();
            return true;
        }
    }

    const std::string key = std::string(kCollectionPrefix) + collection;
    bool found = false;
    if (!read_materialized_value(key, encoded, found, error)) {
        return false;
    }

    if (found && !encoded.empty()) {
        size_t num_documents = 0;
        if (!count_collection_documents(collection, num_documents, error)) {
            return false;
        }

        nlohmann::json normalized_collection;
        if (!normalize_collection_payload(collection, encoded, num_documents, normalized_collection, error)) {
            return false;
        }
        encoded = normalized_collection.dump();
    }

    error.clear();
    return true;
}

bool NuRaftHttpRuntimeService::read_document(const std::string& collection,
                                             const std::string& document_id,
                                             std::string& encoded,
                                             std::string& error) const {
    encoded.clear();
    if (options_.cluster_data_dirs.empty() && !prefers_materialized_reads(collection)) {
        auto live_collection = CollectionManager::get_instance().get_collection(collection);
        if (live_collection != nullptr) {
            Option<nlohmann::json> live_document = live_collection->get(document_id);
            if (live_document.ok()) {
                encoded = live_document.get().dump(-1, ' ', false, nlohmann::detail::error_handler_t::ignore);
                error.clear();
                return true;
            }
            if (live_document.code() != 404) {
                error = live_document.error();
                return false;
            }
        }
    }

    const std::string key = document_materialized_key(collection, document_id);
    if (cache_enabled()) {
        std::shared_lock<std::shared_mutex> cache_lock(document_cache_mutex_);
        const auto cached = document_cache_.find(key);
        if (cached != document_cache_.end()) {
            encoded = cached->second;
            error.clear();
            return true;
        }
    }

    bool found = false;
    if (!read_materialized_value(key, encoded, found, error)) {
        return false;
    }

    if (found && cache_enabled()) {
        std::unique_lock<std::shared_mutex> cache_lock(document_cache_mutex_);
        document_cache_[key] = encoded;
    }

    error.clear();
    return true;
}

bool NuRaftHttpRuntimeService::count_collection_documents(const std::string& collection,
                                                          size_t& count,
                                                          std::string& error) const {
    const std::string prefix = std::string(kDocumentPrefix) + collection + "/";
    return count_materialized_prefix(prefix, count, error);
}

bool NuRaftHttpRuntimeService::search_documents(const std::string& collection,
                                                const std::map<std::string, std::string>& params,
                                                nlohmann::json& result,
                                                std::string& error) const {
    const auto q_it = params.find("q");
    const auto query_by_it = params.find("query_by");
    if (q_it == params.end() || query_by_it == params.end() ||
        q_it->second.empty() || query_by_it->second.empty()) {
        error = "NuRaft runtime search requires q and query_by parameters";
        return false;
    }

    std::set<std::string> query_fields;
    std::vector<std::string> fields;
    StringUtils::split(query_by_it->second, fields, ",");
    for (const auto& field : fields) {
        if (!field.empty()) {
            query_fields.insert(field);
        }
    }

    const std::string prefix = std::string(kDocumentPrefix) + collection + "/";
    std::vector<std::pair<std::string, std::string>> entries;
    if (!read_materialized_prefix(prefix, entries, error)) {
        return false;
    }

    nlohmann::json hits = nlohmann::json::array();
    for (const auto& entry : entries) {
        if (entry.first.rfind(prefix, 0) != 0) {
            continue;
        }

        nlohmann::json document;
        if (!parse_json_if_present(entry.second, document) || !document.is_object()) {
            continue;
        }

        bool matches = false;
        for (const auto& field : query_fields) {
            if (!document.contains(field)) {
                continue;
            }

            std::string field_value;
            if (document[field].is_string()) {
                field_value = document[field].get<std::string>();
            } else {
                field_value = document[field].dump();
            }

            if (contains_case_insensitive(field_value, q_it->second)) {
                matches = true;
                break;
            }
        }

        if (!matches) {
            continue;
        }

        hits.push_back({
            {"document", document},
        });
    }

    result = {
        {"found", hits.size()},
        {"facet_counts", nlohmann::json::array()},
        {"hits", std::move(hits)},
    };

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
    server->get("/collections/:collection/documents/search", search_runtime_documents);
    server->get("/collections/:collection/documents/:id", get_runtime_document);

    server->post("/collections", write_placeholder);
    server->del("/collections/:collection", write_placeholder);
    server->post("/collections/:collection/documents", write_placeholder);
    server->patch("/collections/:collection/documents/:id", write_placeholder);
    server->del("/collections/:collection/documents/:id", write_placeholder);
    server->post("/collections/:collection/documents/import", write_placeholder, true, true);

    server->post("/operations/snapshot", create_snapshot_response, false, true);
    server->post("/operations/vote", vote_response);
}
