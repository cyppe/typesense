#include "nuraft/nuraft_http_runtime.h"

#include <exception>
#include <set>
#include <utility>

#include "analytics_manager.h"
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
#include "search_analytics.h"
#include "tokenizer.h"
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

bool find_registered_route(HttpServer* server,
                           uint64_t route_hash,
                           route_path*& route,
                           std::string& error) {
    route = nullptr;
    if (server == nullptr) {
        error = "NuRaft runtime server is not attached.";
        return false;
    }

    if (!server->get_route(route_hash, &route) || route == nullptr || route->handler == nullptr) {
        error = "NuRaft runtime could not resolve registered route handler.";
        return false;
    }

    error.clear();
    return true;
}

bool invoke_registered_handler(HttpServer* server,
                               const std::shared_ptr<http_req>& request,
                               const std::shared_ptr<http_res>& response,
                               std::string& error) {
    route_path* route = nullptr;
    if (!find_registered_route(server, request->route_hash, route, error)) {
        return false;
    }

    if (route->handler(request, response)) {
        error.clear();
        return true;
    }

    if (response->status_code == 0) {
        error = "Registered route handler failed without setting an HTTP response.";
        return false;
    }

    error.clear();
    return false;
}

bool should_replay_live_product_state(const NuRaftAppliedRequest& applied_request) {
    switch (applied_request.route_kind) {
        case NuRaftRouteKind::kCollectionCreate:
        case NuRaftRouteKind::kCollectionDrop:
        case NuRaftRouteKind::kDocumentWrite:
        case NuRaftRouteKind::kDocumentDelete:
        case NuRaftRouteKind::kDocumentImport:
        case NuRaftRouteKind::kUnknown:
            return true;
        default:
            return false;
    }
}

std::shared_ptr<http_req> build_replay_request(const NuRaftAppliedRequest& applied_request,
                                               const route_path& route) {
    auto request = std::make_shared<http_req>();
    request->http_method = !applied_request.metadata.empty() ? applied_request.metadata : route.http_method;
    request->path_without_query = "/" + StringUtils::join(route.path_parts, "/");
    request->route_hash = applied_request.route_hash;
    request->params = applied_request.params;
    request->body = applied_request.body;
    request->metadata = applied_request.metadata;
    request->first_chunk_aggregate = applied_request.first_chunk_aggregate;
    request->last_chunk_aggregate = applied_request.last_chunk_aggregate;
    request->start_ts = applied_request.start_ts;
    request->log_index = applied_request.log_index;
    request->is_binary_body = applied_request.is_binary_body;
    request->api_auth_key = Config::get_instance().get_api_key();
    request->client_ip = "127.0.0.1";
    request->is_write = true;
    return request;
}

bool get_runtime_collections(const std::shared_ptr<http_req>& request, const std::shared_ptr<http_res>& response) {
    NuRaftHttpRuntimeService* runtime = current_runtime_service();
    if (runtime == nullptr) {
        response->set_500("NuRaft runtime service is not attached.");
        return true;
    }

    if (!CollectionManager::get_instance().get_collection_names().empty()) {
        return get_collections(request, response);
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

    if (CollectionManager::get_instance().get_collection(it->second) != nullptr) {
        return get_collection_summary(request, response);
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

    if (runtime->is_single_node_mode() &&
        CollectionManager::get_instance().get_collection(collection_it->second) != nullptr) {
        return get_fetch_document(request, response);
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

    // When the collection is live in CollectionManager, use the real search
    // pipeline (in-memory index, analytics, presets, stopwords, etc.).
    if (CollectionManager::get_instance().get_collection(collection_it->second) != nullptr) {
        nlohmann::json embedded_params = nlohmann::json::object();
        std::string results_json_str;
        Option<bool> search_op = CollectionManager::do_search(request->params, embedded_params,
                                                               results_json_str, request->conn_ts);
        if (!search_op.ok()) {
            response->set_body(search_op.code(), R"({"message":")" + search_op.error() + R"("})");
        } else {
            response->set_body(200, results_json_str);
        }
        return true;
    }
    // Materialized KV fallback for collections not yet replayed.
    nlohmann::json body;
    std::string error;
    if (!runtime->search_documents(collection_it->second, request->params, body, error)) {
        response->set_500(error);
        return true;
    }

    if (runtime->is_single_node_mode() && Config::get_instance().get_enable_search_analytics()) {
        const auto user_it = request->params.find(http_req::USER_HEADER);
        if (user_it != request->params.end() && !user_it->second.empty()) {
            const auto query_it = request->params.find("q");
            const std::string normalized_query =
                (query_it == request->params.end()) ? "" : Tokenizer::normalize_ascii_no_spaces(query_it->second);
            if (!normalized_query.empty()) {
                search_internal_event_t internal_event = {
                    SearchAnalytics::LOG_TYPE,
                    collection_it->second,
                    normalized_query,
                    normalized_query,
                    user_it->second,
                    request->params.count("filter_by") != 0 ? request->params.at("filter_by") : "",
                    request->params.count("analytics_tag") != 0 ? request->params.at("analytics_tag") : "",
                };
                AnalyticsManager::get_instance().add_internal_event(internal_event);

                const bool found_hits = body.contains("found") && body["found"].is_number_unsigned() &&
                                        body["found"].get<size_t>() != 0;
                internal_event.type = found_hits ? SearchAnalytics::POPULAR_QUERIES_TYPE :
                                                   SearchAnalytics::NO_HIT_QUERIES_TYPE;
                AnalyticsManager::get_instance().add_internal_event(internal_event);
            }
        }
    }

    response->set_body(200, body.dump());
    return true;
}

}  // namespace

NuRaftHttpRuntimeService::NuRaftHttpRuntimeService(HttpServer* server, NuRaftHttpServerOptions options)
    : server_(server),
      options_(std::move(options)),
      layout_(NuRaftStateLayout::from_data_dir(options_.startup_options.data_dir)),
      preferred_leader_server_id_(0),
      initialized_(false),
      live_product_state_applied_index_(0) {}

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

bool replay_live_product_state(HttpServer* server,
                               const NuRaftKvStateMachineSink& sink,
                               uint64_t& replayed_through_index,
                               std::string& error) {
    if (server == nullptr) {
        error = "NuRaft runtime server is not attached.";
        return false;
    }

    std::vector<NuRaftAppliedRequest> applied_requests;
    if (!sink.read_all(applied_requests, error) || applied_requests.empty()) {
        return error.empty();
    }

    uint64_t latest_index = replayed_through_index;
    for (const auto& applied_request : applied_requests) {
        latest_index = std::max(latest_index, applied_request.index);
        if (applied_request.index <= replayed_through_index ||
            !should_replay_live_product_state(applied_request)) {
            continue;
        }

        route_path* route = nullptr;
        if (!find_registered_route(server, applied_request.route_hash, route, error)) {
            TS_LOG(WARNING) << "NuRaft replay: could not find route for hash="
                            << applied_request.route_hash << " kind=" << static_cast<int>(applied_request.route_kind);
            return false;
        }

        auto request = build_replay_request(applied_request, *route);
        auto response = std::make_shared<http_res>(nullptr);

        if (applied_request.route_kind != NuRaftRouteKind::kUnknown) {
            if (!mirror_single_node_typesense_state(request, applied_request.route_kind, error)) {
                TS_LOG(WARNING) << "NuRaft startup replay mirror failed for route '"
                                << request->http_method << " " << request->path_without_query
                                << "': " << error;
                error.clear();
            }
        } else {
            if (!invoke_registered_handler(server, request, response, error)) {
                if (response->status_code == 0) {
                    return false;
                }

                TS_LOG(WARNING) << "NuRaft startup replay handler returned "
                                << response->status_code << " for route '"
                                << request->http_method << " " << request->path_without_query
                                << "': " << response->body;
                error.clear();
            }
        }
    }

    replayed_through_index = latest_index;
    error.clear();
    return true;
}

bool NuRaftHttpRuntimeService::sync_live_product_state(std::string& error) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        // A write is in progress; state will be consistent after it completes.
        error.clear();
        return true;
    }
    if (!initialized_.load()) {
        error.clear();
        return true;
    }

    if (materialized_state_sink_ != nullptr) {
        return replay_live_product_state(server_,
                                         *materialized_state_sink_,
                                         live_product_state_applied_index_,
                                         error);
    }

    NuRaftKvStateMachineSink sink(layout_);
    if (!replay_live_product_state(server_, sink,
                                   live_product_state_applied_index_,
                                   error)) {
        TS_LOG(WARNING) << "NuRaft multi-node sync replay deferred: " << error;
        error.clear();
    }
    return true;
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
    materialized_state_sink_ = std::make_unique<NuRaftKvStateMachineSink>(layout_);
    if (options_.cluster_data_dirs.empty()) {
        local_request_journal_ = std::make_unique<NuRaftRequestJournal>(layout_);
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

    if (materialized_state_sink_ != nullptr) {
        uint64_t last_applied_index = 0;
        if (!materialized_state_sink_->read_last_applied_index(last_applied_index, error)) {
            local_request_journal_.reset();
            materialized_state_sink_.reset();
            return false;
        }

        if (options_.cluster_data_dirs.empty() &&
            !sync_local_replay_progress(last_applied_index, error)) {
            local_request_journal_.reset();
            materialized_state_sink_.reset();
            return false;
        }

        if (materialized_state_sink_ != nullptr &&
            !replay_live_product_state(server_,
                                       *materialized_state_sink_,
                                       live_product_state_applied_index_,
                                       error)) {
            local_request_journal_.reset();
            materialized_state_sink_.reset();
            return false;
        }
    } else {
        NuRaftKvStateMachineSink sink(layout_);
        if (!replay_live_product_state(server_,
                                       sink,
                                       live_product_state_applied_index_,
                                       error)) {
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
    route_path* route = nullptr;
    const bool has_registered_route = find_registered_route(server_, request->route_hash, route, error);
    const bool supports_generic_registered_write =
        route_kind == NuRaftRouteKind::kUnknown &&
        has_registered_route;

    if (!supports_generic_registered_write &&
        route_kind != NuRaftRouteKind::kCollectionCreate &&
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

    if (supports_generic_registered_write) {
        error.clear();
        const bool handler_ok = invoke_registered_handler(server_, request, response, error);
        if (!handler_ok && response->status_code == 0) {
            response->set_500(error);
        }
        // Only advance the replay index in single-node mode. In multi-node mode,
        // classified writes (collection create, document import, etc.) are not applied
        // by write() — they rely on sync_live_product_state() replay. Advancing the
        // index here would cause those entries to be skipped.
        if (options_.cluster_data_dirs.empty() &&
            appended_index > live_product_state_applied_index_) {
            live_product_state_applied_index_ = appended_index;
        }
        send_response(request, response);
        return;
    }

    // Apply classified writes to CollectionManager inline. In single-node mode
    // this is the primary application path. In multi-node mode, this ensures the
    // handling process has the write reflected immediately (e.g., counter increments
    // via document import need to be visible when reading back the document).
    if (!mirror_single_node_typesense_state(request, route_kind, error)) {
        TS_LOG(WARNING) << "NuRaft runtime skipped live Typesense state mirror: " << error;
        error.clear();
    }
    // Advance the replay index so sync_live_product_state() doesn't re-apply
    // the entry that mirror_single_node_typesense_state() just applied.
    if (appended_index > live_product_state_applied_index_) {
        live_product_state_applied_index_ = appended_index;
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

bool NuRaftHttpRuntimeService::is_single_node_mode() const {
    return options_.cluster_data_dirs.empty();
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

    if (options_.cluster_data_dirs.empty() && materialized_state_sink_ != nullptr) {
        uint64_t last_applied_index = 0;
        std::string local_error;
        if (materialized_state_sink_->read_last_applied_index(last_applied_index, local_error)) {
            uint64_t last_index = 0;
            if (local_request_journal_ != nullptr) {
                std::vector<NuRaftLogEntry> entries;
                if (local_request_journal_->replay(entries, local_error)) {
                    last_index = entries.empty() ? 0 : entries.back().index;
                }
            }

            status["last_index"] = last_index;
            status["committed_index"] = last_index;
            status["known_applied_index"] = last_applied_index;
            status["applying_index"] = 0;
            return status;
        }

        status["error"] = local_error;
        return status;
    }

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
    if (!prefers_materialized_reads(collection)) {
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
    if (!prefers_materialized_reads(collection)) {
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

    // Sync replayed product state before handling the request.
    // In single-node mode, writes already apply via mirror_single_node_typesense_state,
    // so read-only requests can skip the sync. In multi-node mode, followers must
    // always sync to replay entries received from the leader.
    const bool is_read_only = (rpath.http_method == "GET") ||
                               (rpath.handler == post_create_event) ||
                               (rpath.handler == post_multi_search);
    NuRaftHttpRuntimeService* runtime = current_runtime_service();
    const bool needs_sync = !is_read_only ||
                             (runtime != nullptr && !runtime->is_single_node_mode());
    if (needs_sync && runtime != nullptr) {
        std::string error;
        if (!runtime->sync_live_product_state(error)) {
            TS_LOG(WARNING) << "NuRaft runtime failed to sync live product state before handling request: " << error;
        }
    }

    const std::string configured_api_key = Config::get_instance().get_api_key();
    return configured_api_key.empty() || configured_api_key == auth_key;
}

void register_nuraft_http_runtime_routes(HttpServer* server) {
    // Keep this as the one central route table for the NuRaft-backed server.
    // The old typesense_server.cpp route list was useful because it let you scan
    // most of the product surface in one place. Preserve that property here.

    server->get("/collections/:collection/documents/search", search_runtime_documents);
    server->post("/multi_search", post_multi_search, false, true);

    server->post("/collections/:collection/documents", post_add_document);
    server->del("/collections/:collection/documents", del_remove_documents, false, true);
    server->post("/collections/:collection/documents/import", post_import_documents, true, true);
    server->get("/collections/:collection/documents/export", get_export_documents, false, true);
    server->get("/collections/:collection/documents/:id", get_runtime_document);
    server->patch("/collections/:collection/documents/:id", patch_update_document);
    server->patch("/collections/:collection/documents", patch_update_documents);
    server->del("/collections/:collection/documents/:id", del_remove_document);

    server->post("/collections", post_create_collection);
    server->patch("/collections/:collection", patch_update_collection);
    server->get("/collections", get_runtime_collections);
    server->del("/collections/:collection", del_drop_collection);
    server->get("/collections/:collection", get_runtime_collection);

    server->get("/aliases", get_aliases);
    server->get("/aliases/:alias", get_alias);
    server->put("/aliases/:alias", put_upsert_alias);
    server->del("/aliases/:alias", del_alias);

    server->get("/keys", get_keys);
    server->get("/keys/:id", get_key);
    server->post("/keys", post_create_key);
    server->del("/keys/:id", del_key);

    server->get("/presets", get_presets);
    server->get("/presets/:name", get_preset);
    server->put("/presets/:name", put_upsert_preset);
    server->del("/presets/:name", del_preset);

    server->get("/stopwords", get_stopwords);
    server->get("/stopwords/:name", get_stopword);
    server->put("/stopwords/:name", put_upsert_stopword);
    server->del("/stopwords/:name", del_stopword);

    server->get("/synonym_sets", get_synonym_sets);
    server->get("/synonym_sets/:name", get_synonym_set);
    server->put("/synonym_sets/:name", put_synonym_set);
    server->del("/synonym_sets/:name", del_synonym_set);
    server->get("/synonym_sets/:name/items", get_synonym_set_items);
    server->get("/synonym_sets/:name/items/:id", get_synonym_set_item);
    server->put("/synonym_sets/:name/items/:id", put_synonym_set_item);
    server->del("/synonym_sets/:name/items/:id", del_synonym_set_item);

    server->get("/curation_sets", get_curation_sets);
    server->get("/curation_sets/:name", get_curation_set);
    server->put("/curation_sets/:name", put_curation_set);
    server->del("/curation_sets/:name", del_curation_set);
    server->get("/curation_sets/:name/items", get_curation_set_items);
    server->get("/curation_sets/:name/items/:id", get_curation_set_item);
    server->put("/curation_sets/:name/items/:id", put_curation_set_item);
    server->del("/curation_sets/:name/items/:id", del_curation_set_item);

    server->get("/analytics/rules", get_analytics_rules);
    server->get("/analytics/rules/:name", get_analytics_rule);
    server->post("/analytics/rules", post_create_analytics_rules);
    server->put("/analytics/rules/:name", put_upsert_analytics_rules);
    server->del("/analytics/rules/:name", del_analytics_rules);
    server->post("/analytics/events", post_create_event);
    server->post("/analytics/aggregate_events", post_write_analytics_to_db);
    server->get("/analytics/events", get_analytics_events);
    server->post("/analytics/flush", post_analytics_flush);
    server->get("/analytics/status", get_analytics_status);

    server->post("/stemming/dictionaries/import", post_import_stemming_dictionary, true, true);
    server->get("/stemming/dictionaries", get_stemming_dictionaries);
    server->get("/stemming/dictionaries/:id", get_stemming_dictionary);
    server->del("/stemming/dictionaries/:id", del_stemming_dictionary);

    server->get("/metrics.json", get_metrics_json);
    server->get("/stats.json", get_stats_json);
    server->get("/debug", get_debug);
    server->get("/health", get_health);
    server->get("/health_with_rusage", get_health_with_resource_usage);
    server->post("/health", post_health);
    server->get("/status", get_status);

    server->post("/operations/snapshot", post_snapshot, false, true);
    server->post("/operations/vote", post_vote, false, false);
    server->post("/operations/cache/clear", post_clear_cache, false, false);
    server->post("/operations/db/compact", post_compact_db, false, false);
    server->post("/operations/reset_peers", post_reset_peers, false, false);
    server->get("/operations/schema_changes", get_schema_changes);

    server->post("/conversations/models", post_conversation_model);
    server->get("/conversations/models", get_conversation_models);
    server->get("/conversations/models/:id", get_conversation_model);
    server->put("/conversations/models/:id", put_conversation_model);
    server->del("/conversations/models/:id", del_conversation_model);

    server->post("/personalization/models", post_personalization_model);
    server->get("/personalization/models", get_personalization_models);
    server->get("/personalization/models/:id", get_personalization_model);
    server->del("/personalization/models/:id", del_personalization_model);
    server->put("/personalization/models/:id", put_personalization_model);

    server->get("/limits", get_rate_limits);
    server->get("/limits/active", get_active_throttles);
    server->get("/limits/exceeds", get_limit_exceed_counts);
    server->get("/limits/:id", get_rate_limit);
    server->post("/limits", post_rate_limit);
    server->put("/limits/:id", put_rate_limit);
    server->del("/limits/:id", del_rate_limit);
    server->del("/limits/active/:id", del_throttle);
    server->del("/limits/exceeds/:id", del_exceed);
    server->post("/config", post_config, false, false);

    server->post("/proxy", post_proxy);
    server->post("/proxy_sse", post_proxy_sse, false, true);

    server->post("/nl_search_models", post_nl_search_model);
    server->get("/nl_search_models", get_nl_search_models);
    server->get("/nl_search_models/:id", get_nl_search_model);
    server->put("/nl_search_models/:id", put_nl_search_model);
    server->del("/nl_search_models/:id", delete_nl_search_model);
}
