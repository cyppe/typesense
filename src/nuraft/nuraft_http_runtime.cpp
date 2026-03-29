#include "nuraft/nuraft_http_runtime.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <random>
#include <set>
#include <thread>
#include <utility>

#include "analytics_manager.h"
#include "core_api.h"
#include "json.hpp"
#include "collection_manager.h"
#include "nuraft/nuraft_route_classifier.h"
#include "nuraft/nuraft_snapshot_coordinator.h"
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
constexpr size_t kDocumentImportRaftChunkMaxBytes = 4 * 1024 * 1024;

// Generate a unique document ID for auto-ID documents before Raft serialization.
// Uses hex timestamp + random to avoid collisions with Typesense's seq_id-based decimal IDs.
std::string generate_auto_document_id() {
    static thread_local std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count() ^
        reinterpret_cast<uint64_t>(&rng));  // NOLINT
    const uint64_t ts = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const uint64_t rand_val = rng();
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(ts),
                  static_cast<unsigned long long>(rand_val));
    return std::string(buf, 32);
}

// Inject auto-generated IDs into JSONL import body lines that lack an "id" field.
bool inject_auto_ids_into_import_body(std::string& body) {
    if (body.empty()) return true;

    std::string result;
    result.reserve(body.size() + 256);
    size_t pos = 0;
    bool modified = false;

    while (pos < body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        std::string line = body.substr(pos, nl - pos);
        pos = nl + 1;

        if (!line.empty()) {
            try {
                nlohmann::json doc = nlohmann::json::parse(line);
                if (doc.is_object() && !doc.contains("id")) {
                    doc["id"] = generate_auto_document_id();
                    line = doc.dump();
                    modified = true;
                }
            } catch (const std::exception&) {
                // Leave unparseable lines as-is; they will fail downstream.
            }
        }

        if (!result.empty()) result.push_back('\n');
        result += line;
    }

    if (modified) {
        body = std::move(result);
    }
    return true;
}
uint64_t elapsed_ms_since(const std::chrono::steady_clock::time_point& start_time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();
}

bool is_expected_missing_collection_mirror_skip(NuRaftRouteKind route_kind, const std::string& error) {
    return route_kind == NuRaftRouteKind::kCollectionDrop &&
           error.find("No collection with name `") != std::string::npos;
}

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

NuRaftHttpRuntimeService* current_runtime_service() {
    return (server == nullptr) ? nullptr : dynamic_cast<NuRaftHttpRuntimeService*>(server->get_replication_state());
}

bool apply_typesense_write_handler(const std::shared_ptr<http_req>& request,
                                   bool (*handler)(const std::shared_ptr<http_req>&,
                                                   const std::shared_ptr<http_res>&),
                                   std::string& error,
                                   std::shared_ptr<http_res> captured_response = nullptr) {
    auto response = captured_response ? captured_response : std::make_shared<http_res>(nullptr);
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

std::shared_ptr<http_req> build_request_copy(const http_req& source) {
    auto request = std::make_shared<http_req>();
    request->http_method = source.http_method;
    request->path_without_query = source.path_without_query;
    request->route_hash = source.route_hash;
    request->params = source.params;
    request->embedded_params_vec = source.embedded_params_vec;
    request->api_auth_key = source.api_auth_key;
    request->metadata = source.metadata;
    request->start_ts = source.start_ts;
    request->conn_ts = source.conn_ts;
    request->overloaded = source.overloaded;
    request->log_index = source.log_index;
    request->client_ip = source.client_ip;
    request->is_binary_body = source.is_binary_body;
    request->is_write = source.is_write.load();
    request->async_res_set_headers_callback = source.async_res_set_headers_callback;
    request->async_res_write_callback = source.async_res_write_callback;
    request->async_res_done_callback = source.async_res_done_callback;
    return request;
}

std::shared_ptr<http_req> build_import_chunk_request(const http_req& source,
                                                     std::string body,
                                                     bool first_chunk,
                                                     bool last_chunk) {
    auto request = build_request_copy(source);
    request->first_chunk_aggregate = first_chunk;
    request->last_chunk_aggregate = last_chunk;
    request->chunk_len = body.size();
    request->body = std::move(body);
    return request;
}

NuRaftAppliedRequest build_applied_request(const http_req& request) {
    NuRaftAppliedRequest applied_request;
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
    return applied_request;
}

template <typename ConsumeChunk>
bool for_each_import_body_chunk(const std::string& body,
                                size_t max_docs_per_chunk,
                                ConsumeChunk&& consume_chunk,
                                std::string& error) {
    if (body.empty()) {
        return consume_chunk(std::string(), true, true, error);
    }

    bool first_chunk = true;
    size_t chunk_start = 0;
    size_t cursor = 0;
    size_t docs_in_chunk = 0;
    size_t chunk_bytes = 0;

    while (cursor < body.size()) {
        const size_t line_start = cursor;
        const size_t newline = body.find('\n', cursor);
        const size_t next_cursor = newline == std::string::npos ? body.size() : newline + 1;
        const size_t line_bytes = next_cursor - line_start;

        if (docs_in_chunk > 0 &&
            (docs_in_chunk >= max_docs_per_chunk ||
             chunk_bytes + line_bytes > kDocumentImportRaftChunkMaxBytes)) {
            if (!consume_chunk(body.substr(chunk_start, line_start - chunk_start), first_chunk, false, error)) {
                return false;
            }
            first_chunk = false;
            chunk_start = line_start;
            docs_in_chunk = 0;
            chunk_bytes = 0;
            continue;
        }

        cursor = next_cursor;
        docs_in_chunk += 1;
        chunk_bytes += line_bytes;
    }

    if (docs_in_chunk == 0) {
        error = "NuRaft import chunking produced an empty final chunk.";
        return false;
    }

    return consume_chunk(body.substr(chunk_start, cursor - chunk_start), first_chunk, true, error);
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
      initialized_(false) {}

bool NuRaftHttpRuntimeService::cache_enabled() const {
    return materialized_state_sink_ != nullptr;
}

bool mirror_single_node_typesense_state(const std::shared_ptr<http_req>& request,
                                        NuRaftRouteKind route_kind,
                                        std::string& error,
                                        std::shared_ptr<http_res> captured_response = nullptr) {
    switch (route_kind) {
        case NuRaftRouteKind::kCollectionCreate:
            return apply_typesense_write_handler(request, post_create_collection, error, captured_response);
        case NuRaftRouteKind::kCollectionDrop:
            return apply_typesense_write_handler(request, del_drop_collection, error, captured_response);
        case NuRaftRouteKind::kDocumentWrite:
            if (request->http_method == "PATCH") {
                if (request->params.count("id") == 0 || request->params.at("id").empty()) {
                    return apply_typesense_write_handler(request, patch_update_documents, error, captured_response);
                }
                return apply_typesense_write_handler(request, patch_update_document, error, captured_response);
            }
            return apply_typesense_write_handler(request, post_add_document, error, captured_response);
        case NuRaftRouteKind::kDocumentDelete:
            if (request->params.count("id") == 0 || request->params.at("id").empty()) {
                return apply_typesense_write_handler(request, del_remove_documents, error, captured_response);
            }
            return apply_typesense_write_handler(request, del_remove_document, error, captured_response);
        case NuRaftRouteKind::kDocumentImport:
            return apply_typesense_write_handler(request, post_import_documents, error, captured_response);
        default:
            error.clear();
            return true;
    }
}

// Replay only kUnknown routes (aliases, presets, stopwords, synonyms,
// curations, analytics, etc.) from the full materialized state history.
// These are NOT loaded by collection_manager.load() and need explicit replay.
// Classified routes (collection/document operations) are already in RocksDB.
bool replay_unknown_routes_from_history(HttpServer* server,
                                        const NuRaftKvStateMachineSink& sink,
                                        std::string& error) {
    if (server == nullptr) {
        error = "NuRaft runtime server is not attached.";
        return false;
    }

    std::vector<NuRaftAppliedRequest> applied_requests;
    if (!sink.read_all(applied_requests, error) || applied_requests.empty()) {
        return error.empty();
    }

    size_t replayed_count = 0;
    size_t skipped_count = 0;
    auto last_progress_log = std::chrono::steady_clock::now();

    for (const auto& applied_request : applied_requests) {
        if (applied_request.route_kind != NuRaftRouteKind::kUnknown) {
            ++skipped_count;
            continue;
        }

        route_path* route = nullptr;
        if (!find_registered_route(server, applied_request.route_hash, route, error)) {
            TS_LOG(WARNING) << "NuRaft unknown-route replay: could not find route for hash="
                            << applied_request.route_hash;
            return false;
        }

        auto request = build_replay_request(applied_request, *route);
        auto response = std::make_shared<http_res>(nullptr);

        if (!invoke_registered_handler(server, request, response, error)) {
            if (response->status_code == 0) {
                return false;
            }
            TS_LOG(WARNING) << "NuRaft unknown-route replay handler returned "
                            << response->status_code << " for route '"
                            << request->http_method << " " << request->path_without_query
                            << "': " << response->body;
            error.clear();
        }

        ++replayed_count;
        const auto now = std::chrono::steady_clock::now();
        if (replayed_count % 1000 == 0 ||
            std::chrono::duration_cast<std::chrono::seconds>(now - last_progress_log).count() >= 10) {
            TS_LOG(INFO) << "NuRaft unknown-route replay progress: replayed=" << replayed_count
                         << " skipped=" << skipped_count
                         << " total=" << applied_requests.size();
            last_progress_log = now;
        }
    }

    TS_LOG(INFO) << "NuRaft unknown-route replay complete: replayed=" << replayed_count
                 << " skipped=" << skipped_count
                 << " total=" << applied_requests.size();
    error.clear();
    return true;
}

// Overload for startup: reads ALL entries, skips classified routes below
// skip_classified_through_index (already in RocksDB), replays kUnknown
// routes from the full history (not loaded by collection_manager.load()).
bool replay_live_product_state(HttpServer* server,
                               const NuRaftKvStateMachineSink& sink,
                               uint64_t& replayed_through_index,
                               uint64_t skip_classified_through_index,
                               std::string& error) {
    if (server == nullptr) {
        error = "NuRaft runtime server is not attached.";
        return false;
    }

    std::vector<NuRaftAppliedRequest> applied_requests;
    if (!sink.read_all(applied_requests, error) || applied_requests.empty()) {
        return error.empty();
    }

    TS_LOG(INFO) << "NuRaft startup replay: " << applied_requests.size()
                 << " applied request(s) to evaluate"
                 << " (skip_classified_through_index=" << skip_classified_through_index << ")";

    size_t replayed_count = 0;
    size_t skipped_count = 0;
    auto last_progress_log = std::chrono::steady_clock::now();

    uint64_t latest_index = replayed_through_index;
    for (const auto& applied_request : applied_requests) {
        latest_index = std::max(latest_index, applied_request.index);
        if (!should_replay_live_product_state(applied_request)) {
            ++skipped_count;
            continue;
        }
        // Skip bulk document imports that are already loaded from RocksDB by
        // collection_manager.load(). Imports are the dominant cost during startup
        // replay (millions of documents, async reference helper fan-out).
        // All other route kinds are replayed to restore metadata linkages.
        if (applied_request.route_kind == NuRaftRouteKind::kDocumentImport &&
            applied_request.index <= skip_classified_through_index) {
            ++skipped_count;
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

        ++replayed_count;
        const auto now = std::chrono::steady_clock::now();
        if (replayed_count % 1000 == 0 ||
            std::chrono::duration_cast<std::chrono::seconds>(now - last_progress_log).count() >= 10) {
            TS_LOG(INFO) << "NuRaft startup replay progress: replayed=" << replayed_count
                         << " skipped=" << skipped_count
                         << " total=" << applied_requests.size();
            last_progress_log = now;
        }
    }

    TS_LOG(INFO) << "NuRaft startup replay complete: replayed=" << replayed_count
                 << " skipped=" << skipped_count
                 << " total=" << applied_requests.size();

    replayed_through_index = latest_index;
    error.clear();
    return true;
}

// Overload for sync path: reads only entries after replayed_through_index.
// All route kinds (classified and kUnknown) in the delta are replayed.
bool replay_live_product_state(HttpServer* server,
                               const NuRaftKvStateMachineSink& sink,
                               uint64_t& replayed_through_index,
                               std::string& error) {
    if (server == nullptr) {
        error = "NuRaft runtime server is not attached.";
        return false;
    }

    std::vector<NuRaftAppliedRequest> applied_requests;
    if (!sink.read_all_after(replayed_through_index, applied_requests, error) || applied_requests.empty()) {
        return error.empty();
    }

    uint64_t latest_index = replayed_through_index;
    for (const auto& applied_request : applied_requests) {
        latest_index = std::max(latest_index, applied_request.index);
        if (!should_replay_live_product_state(applied_request)) {
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
                // "already exists" is expected when a follower that received the original
                // write (and applied it locally) later replays the same entry from Raft.
                const bool is_expected_duplicate =
                    error.find("already exists") != std::string::npos;
                if (!is_expected_duplicate) {
                    TS_LOG(WARNING) << "NuRaft sync replay mirror failed for route '"
                                    << request->http_method << " " << request->path_without_query
                                    << "': " << error;
                }
                error.clear();
            }
        } else {
            if (!invoke_registered_handler(server, request, response, error)) {
                if (response->status_code == 0) {
                    return false;
                }
                TS_LOG(WARNING) << "NuRaft sync replay handler returned "
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
    const auto sync_start = std::chrono::steady_clock::now();
    cumulative_sync_calls_.fetch_add(1, std::memory_order_relaxed);
    last_sync_replay_ms_.store(0, std::memory_order_relaxed);

    auto finish_sync_metrics = [&](uint64_t replay_ms) {
        const uint64_t total_ms = elapsed_ms_since(sync_start);
        cumulative_sync_total_ms_.fetch_add(total_ms, std::memory_order_relaxed);
        last_sync_total_ms_.store(total_ms, std::memory_order_relaxed);
        max_sync_total_ms_.store(std::max(max_sync_total_ms_.load(std::memory_order_relaxed), total_ms),
                                 std::memory_order_relaxed);
        last_sync_replay_ms_.store(replay_ms, std::memory_order_relaxed);
        if (replay_ms != 0) {
            cumulative_sync_replay_ms_.fetch_add(replay_ms, std::memory_order_relaxed);
        }
    };

    auto state_is_caught_up = [&](uint64_t local_applied_index) {
        return live_product_state_applied_index_.load(std::memory_order_relaxed) >= local_applied_index;
    };
    auto wait_for_inflight_import_apply = [&](uint64_t target_index) {
        if (active_import_requests_.load(std::memory_order_relaxed) == 0 ||
            inflight_import_target_index_.load(std::memory_order_relaxed) < target_index) {
            return false;
        }

        std::unique_lock<std::mutex> progress_lock(live_state_progress_mutex_);
        live_state_progress_cv_.wait_for(progress_lock, std::chrono::milliseconds(100), [&] {
            return state_is_caught_up(target_index) ||
                   active_import_requests_.load(std::memory_order_relaxed) == 0;
        });
        return state_is_caught_up(target_index);
    };

    uint64_t local_applied_index = 0;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (!initialized_.load()) {
            error.clear();
            cumulative_sync_fast_path_hits_.fetch_add(1, std::memory_order_relaxed);
            finish_sync_metrics(0);
            return true;
        }

        if (raft_state_machine_ != nullptr && materialized_state_sink_ != nullptr) {
            local_applied_index = raft_state_machine_->get_last_commit_index();
            if (state_is_caught_up(local_applied_index)) {
                error.clear();
                cumulative_sync_fast_path_hits_.fetch_add(1, std::memory_order_relaxed);
                finish_sync_metrics(0);
                return true;
            }
        }
    }

    if (wait_for_inflight_import_apply(local_applied_index)) {
        error.clear();
        cumulative_sync_fast_path_hits_.fetch_add(1, std::memory_order_relaxed);
        finish_sync_metrics(0);
        return true;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!initialized_.load()) {
        error.clear();
        cumulative_sync_fast_path_hits_.fetch_add(1, std::memory_order_relaxed);
        finish_sync_metrics(0);
        return true;
    }

    if (raft_state_machine_ != nullptr && materialized_state_sink_ != nullptr) {
        local_applied_index = raft_state_machine_->get_last_commit_index();
        if (state_is_caught_up(local_applied_index)) {
            error.clear();
            cumulative_sync_fast_path_hits_.fetch_add(1, std::memory_order_relaxed);
            finish_sync_metrics(0);
            return true;
        }
    }

    if (materialized_state_sink_ != nullptr) {
        uint64_t replayed_through_index = live_product_state_applied_index_.load(std::memory_order_relaxed);
        const auto replay_start = std::chrono::steady_clock::now();
        const bool ok = replay_live_product_state(server_,
                                                  *materialized_state_sink_,
                                                  replayed_through_index,
                                                  error);
        const uint64_t replay_ms = elapsed_ms_since(replay_start);
        if (ok) {
            advance_live_product_state_applied_index(replayed_through_index);
            cumulative_sync_replay_calls_.fetch_add(1, std::memory_order_relaxed);
        }
        finish_sync_metrics(replay_ms);
        return ok;
    }

    NuRaftKvStateMachineSink sink(layout_);
    uint64_t replayed_through_index = live_product_state_applied_index_.load(std::memory_order_relaxed);
    const auto replay_start = std::chrono::steady_clock::now();
    if (!replay_live_product_state(server_, sink,
                                   replayed_through_index,
                                   error)) {
        TS_LOG(WARNING) << "NuRaft sync replay deferred: " << error;
        error.clear();
    } else {
        advance_live_product_state_applied_index(replayed_through_index);
        cumulative_sync_replay_calls_.fetch_add(1, std::memory_order_relaxed);
    }
    finish_sync_metrics(elapsed_ms_since(replay_start));
    return true;
}

bool NuRaftHttpRuntimeService::initialize(std::string& error) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    TS_LOG(INFO) << "NuRaft init: state initializer starting (data_dir="
                 << options_.startup_options.data_dir
                 << " local_host=" << options_.startup_options.local_host
                 << " peer_port=" << options_.startup_options.peer_port
                 << " api_port=" << options_.startup_options.api_port
                 << " nodes_config=" << (options_.startup_options.nodes_config.empty()
                                         ? "(single-node)" : options_.startup_options.nodes_config)
                 << ")";
    if (!NuRaftStateInitializer::initialize(options_.startup_options, identity_, bootstrap_config_, error)) {
        TS_LOG(ERROR) << "NuRaft init: state initializer failed: " << error;
        return false;
    }
    TS_LOG(INFO) << "NuRaft init: identity server_id=" << identity_.server_id
                 << " peer_endpoint=" << identity_.peer_endpoint
                 << " self=" << bootstrap_config_.self.host
                 << ":" << bootstrap_config_.self.peer_port
                 << ":" << bootstrap_config_.self.api_port
                 << " peers=" << bootstrap_config_.peers.size();

    materialized_state_sink_ = std::make_unique<NuRaftKvStateMachineSink>(layout_);

    uint64_t last_applied_index = 0;
    if (!materialized_state_sink_->read_last_applied_index(last_applied_index, error)) {
        TS_LOG(ERROR) << "NuRaft init: failed to read materialized state: " << error;
        materialized_state_sink_.reset();
        return false;
    }
    TS_LOG(INFO) << "NuRaft init: materialized state last_applied_index=" << last_applied_index;

    // Product state (collections, documents, aliases, presets, stopwords,
    // synonyms, curations, analytics) is already loaded from the main RocksDB
    // store by collection_manager.load(). The main store has WAL enabled so
    // all writes from the leader's product handlers are durable. Just advance
    // the applied-index to match the materialized state so the sync path
    // starts delta replay from the correct point.
    advance_live_product_state_applied_index(last_applied_index);
    TS_LOG(INFO) << "NuRaft init: skipped startup replay (state loaded from main store), "
                 << "advanced applied index to " << last_applied_index;

    {
        std::unique_lock<std::shared_mutex> cache_lock(document_cache_mutex_);
        document_cache_.clear();
    }
    {
        std::unique_lock<std::shared_mutex> preference_lock(read_preference_mutex_);
        materialized_read_preferred_collections_.clear();
    }

    if (!initialize_raft_server(error)) {
        TS_LOG(ERROR) << "NuRaft init: raft server initialization failed: " << error;
        materialized_state_sink_.reset();
        return false;
    }

    // Request leadership and wait for the election to settle. In single-node
    // mode the node must become leader before it can accept writes.
    TS_LOG(INFO) << "NuRaft init: requesting leadership...";
    raft_server_->request_leadership();
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (raft_server_->is_leader()) {
            TS_LOG(INFO) << "NuRaft init: leadership acquired in " << (attempt * 100) << " ms.";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!raft_server_->is_leader()) {
        TS_LOG(WARNING) << "NuRaft init: failed to acquire leadership after 5 seconds.";
    }

    initialized_.store(true);
    TS_LOG(INFO) << "NuRaft runtime initialization complete.";
    error.clear();
    return true;
}

void NuRaftHttpRuntimeService::send_response(const std::shared_ptr<http_req>& request,
                                             const std::shared_ptr<http_res>& response) const {
    if (!response->is_alive) {
        return;
    }

    const auto wait_start = std::chrono::steady_clock::now();
    response->wait();
    request->add_response_pre_dispatch_wait_us(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - wait_start).count());
    auto* req_res = new async_req_res_t(request, response, true);
    request->mark_response_dispatch();
    server_->send_message(HttpServer::STREAM_RESPONSE_MESSAGE, req_res);
}

void NuRaftHttpRuntimeService::advance_live_product_state_applied_index(uint64_t applied_index) {
    uint64_t current = live_product_state_applied_index_.load(std::memory_order_relaxed);
    while (current < applied_index &&
           !live_product_state_applied_index_.compare_exchange_weak(current,
                                                                    applied_index,
                                                                    std::memory_order_relaxed,
                                                                    std::memory_order_relaxed)) {
    }

    if (current < applied_index) {
        live_state_progress_cv_.notify_all();
    }
}

void NuRaftHttpRuntimeService::write(const std::shared_ptr<http_req>& request,
                                     const std::shared_ptr<http_res>& response) {
    struct handler_scope_t {
        std::shared_ptr<http_req> request;
        explicit handler_scope_t(const std::shared_ptr<http_req>& req): request(req) {
            if (request->handler_start_ts_us.load(std::memory_order_relaxed) == 0) {
                request->mark_handler_start();
            }
        }

        ~handler_scope_t() {
            if (request->handler_end_ts_us.load(std::memory_order_relaxed) == 0) {
                request->mark_handler_end();
            }
        }
    } handler_scope(request);

    std::shared_lock<std::shared_mutex> lock(mutex_);
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
    const bool delegates_to_registered_import_handler =
        route_kind == NuRaftRouteKind::kDocumentImport &&
        has_registered_route &&
        route != nullptr;

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

    // Pre-generate document IDs for auto-ID documents before Raft serialization.
    // The KV materialized sink requires an "id" field to store documents, but
    // Typesense's Collection::add_doc() normally generates IDs after Raft commit.
    if (route_kind == NuRaftRouteKind::kDocumentWrite &&
        request->http_method != "PATCH" && request->http_method != "DELETE") {
        auto id_it = request->params.find("id");
        const bool has_url_id = id_it != request->params.end() && !id_it->second.empty();
        if (!has_url_id) {
            nlohmann::json parsed_body;
            if (parse_json_if_present(request->body, parsed_body) &&
                parsed_body.is_object() && !parsed_body.contains("id")) {
                parsed_body["id"] = generate_auto_document_id();
                request->body = parsed_body.dump();
            }
        }
    }

    uint64_t committed_index = 0;
    bool forwarded_to_leader = false;
    if (delegates_to_registered_import_handler) {
        if (!process_document_import_write(request, response, committed_index, forwarded_to_leader, error)) {
            if (response->status_code == 0) {
                response->set_500(error);
            }
            send_response(request, response);
            return;
        }
        const uint64_t applied_index = live_product_state_applied_index_.load(std::memory_order_relaxed);
        if (committed_index > applied_index) {
            advance_live_product_state_applied_index(committed_index);
        }
        send_response(request, response);
        return;
    }

    const std::string request_payload = build_applied_request(*request).encode_binary();
    if (!append_via_raft(request_payload,
                         NuRaftRequestEnvelope::kAppliedRequestBinaryEncoding,
                         *request,
                         committed_index,
                         forwarded_to_leader,
                         error)) {
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
        const uint64_t applied_index = live_product_state_applied_index_.load(std::memory_order_relaxed);
        if (committed_index > applied_index) {
            advance_live_product_state_applied_index(committed_index);
        }

        if (route != nullptr && (route->async_req || route->async_res)) {
            return;
        }

        send_response(request, response);
        return;
    }

    // Run the real Typesense handler and capture its response. This gives us
    // standard API responses (correct created_at, document IDs, etc.) instead
    // of the manually constructed Raft metadata that breaks client compatibility.
    auto handler_response = std::make_shared<http_res>(nullptr);
    if (!mirror_single_node_typesense_state(request, route_kind, error, handler_response)) {
        if (is_expected_missing_collection_mirror_skip(route_kind, error)) {
            TS_LOG(INFO) << "NuRaft runtime skipped live Typesense state mirror for missing collection drop: "
                         << error;
        } else {
            TS_LOG(WARNING) << "NuRaft runtime skipped live Typesense state mirror: " << error;
        }
        error.clear();
    }
    const uint64_t applied_index = live_product_state_applied_index_.load(std::memory_order_relaxed);
    if (committed_index > applied_index) {
        advance_live_product_state_applied_index(committed_index);
    }

    update_single_node_document_cache(*request, route_kind);

    // Use the real handler response directly (standard Typesense API format).
    if (handler_response->status_code != 0) {
        response->set_body(handler_response->status_code, handler_response->body);
        response->content_type_header = handler_response->content_type_header;
    } else {
        // Handler didn't set a response (e.g., unknown route that succeeded).
        response->set_body(request->http_method == "POST" ? 201 : 200, "{}");
    }

    send_response(request, response);
}

bool NuRaftHttpRuntimeService::process_document_import_write(
    const std::shared_ptr<http_req>& request,
    const std::shared_ptr<http_res>& response,
    uint64_t& committed_index,
    bool& forwarded_to_leader,
    std::string& error) {
    committed_index = 0;
    forwarded_to_leader = false;
    active_import_requests_.fetch_add(1, std::memory_order_relaxed);
    const auto finish_import_request = [&]() {
        active_import_requests_.fetch_sub(1, std::memory_order_relaxed);
        live_state_progress_cv_.notify_all();
    };

    std::string aggregated_response_body;
    std::string response_content_type = "text/plain; charset=utf-8";
    uint32_t response_status_code = 200;
    const auto import_start = std::chrono::steady_clock::now();
    uint64_t logical_chunks = 0;
    uint64_t replay_chunks = 0;
    uint64_t append_ms = 0;
    uint64_t replay_ms = 0;
    uint64_t docs_estimate = 0;
    if (!request->body.empty()) {
        docs_estimate = 1;
        docs_estimate += static_cast<uint64_t>(std::count(request->body.begin(), request->body.end(), '\n'));
        if (!request->body.empty() && request->body.back() == '\n' && docs_estimate > 0) {
            docs_estimate--;
        }
    }
    size_t logical_chunk_max_docs = Config::get_instance().get_import_batch_size();
    const auto batch_size_it = request->params.find("batch_size");
    if (batch_size_it != request->params.end() &&
        StringUtils::is_uint32_t(batch_size_it->second)) {
        logical_chunk_max_docs = std::stoul(batch_size_it->second);
    }
    logical_chunk_max_docs = std::max<size_t>(1, logical_chunk_max_docs);

    const auto record_import_metrics = [&](uint64_t total_ms) {
        cumulative_import_requests_.fetch_add(1, std::memory_order_relaxed);
        cumulative_import_bytes_.fetch_add(request->body.size(), std::memory_order_relaxed);
        cumulative_import_docs_estimate_.fetch_add(docs_estimate, std::memory_order_relaxed);
        last_import_request_bytes_.store(request->body.size(), std::memory_order_relaxed);
        last_import_docs_estimate_.store(docs_estimate, std::memory_order_relaxed);
        last_import_logical_chunks_.store(logical_chunks, std::memory_order_relaxed);
        last_import_replay_chunks_.store(replay_chunks, std::memory_order_relaxed);
        last_import_append_ms_.store(append_ms, std::memory_order_relaxed);
        last_import_replay_ms_.store(replay_ms, std::memory_order_relaxed);
        last_import_total_ms_.store(total_ms, std::memory_order_relaxed);
        last_import_response_bytes_.store(aggregated_response_body.size(), std::memory_order_relaxed);
        last_import_docs_per_sec_.store(total_ms == 0 ? docs_estimate : (docs_estimate * 1000ULL) / total_ms,
                                        std::memory_order_relaxed);
        last_import_bytes_per_sec_.store(total_ms == 0 ? request->body.size() :
                                         (static_cast<uint64_t>(request->body.size()) * 1000ULL) / total_ms,
                                         std::memory_order_relaxed);
        max_import_total_ms_.store(std::max(max_import_total_ms_.load(std::memory_order_relaxed), total_ms),
                                   std::memory_order_relaxed);
    };

    const auto consume_chunk = [&](std::string chunk_body,
                                   bool first_chunk,
                                   bool last_chunk,
                                   std::string& chunk_error) -> bool {
        // Inject auto-IDs into import docs missing "id" before Raft serialization.
        inject_auto_ids_into_import_body(chunk_body);
        auto chunk_request = build_import_chunk_request(*request, std::move(chunk_body), first_chunk, last_chunk);
        const std::string request_payload = build_applied_request(*chunk_request).encode_binary();

        uint64_t chunk_committed_index = 0;
        bool chunk_forwarded_to_leader = false;
        const auto chunk_append_start = std::chrono::steady_clock::now();
        if (!append_via_raft(request_payload,
                             NuRaftRequestEnvelope::kAppliedRequestBinaryEncoding,
                             *chunk_request,
                             chunk_committed_index,
                             chunk_forwarded_to_leader,
                             chunk_error)) {
            append_ms += elapsed_ms_since(chunk_append_start);
            return false;
        }
        append_ms += elapsed_ms_since(chunk_append_start);

        committed_index = chunk_committed_index;
        forwarded_to_leader = forwarded_to_leader || chunk_forwarded_to_leader;
        uint64_t current_target = inflight_import_target_index_.load(std::memory_order_relaxed);
        while (current_target < chunk_committed_index &&
               !inflight_import_target_index_.compare_exchange_weak(current_target,
                                                                    chunk_committed_index,
                                                                    std::memory_order_relaxed,
                                                                    std::memory_order_relaxed)) {
        }
        logical_chunks++;

        auto chunk_response = std::make_shared<http_res>(nullptr);
        const auto chunk_replay_start = std::chrono::steady_clock::now();
        const bool handler_ok = invoke_registered_handler(server_, chunk_request, chunk_response, chunk_error);
        replay_ms += elapsed_ms_since(chunk_replay_start);
        if (!handler_ok && chunk_response->status_code == 0) {
            return false;
        }

        if (!chunk_response->content_type_header.empty()) {
            response_content_type = chunk_response->content_type_header;
        }
        if (chunk_response->status_code != 0) {
            response_status_code = chunk_response->status_code;
        }
        if (!chunk_response->body.empty()) {
            if (!aggregated_response_body.empty()) {
                aggregated_response_body.push_back('\n');
            }
            aggregated_response_body += chunk_response->body;
        }
        if (chunk_response->status_code >= 400) {
            chunk_error.clear();
            return false;
        }

        advance_live_product_state_applied_index(chunk_committed_index);
        replay_chunks++;
        return true;
    };

    // Buffer the HTTP request once, then feed raft bounded logical import chunks
    // keyed by the same request start_ts. This avoids per-transport-chunk raft
    // commits without storing the full 1M-document body in one raft entry.
    if (!for_each_import_body_chunk(request->body, logical_chunk_max_docs, consume_chunk, error)) {
        const uint64_t total_ms = elapsed_ms_since(import_start);
        record_import_metrics(total_ms);
        finish_import_request();
        if (response_status_code >= 400) {
            response->set_content(response_status_code,
                                  response_content_type,
                                  error.empty() ? aggregated_response_body : error,
                                  true);
        }
        return false;
    }
    const uint64_t total_ms = elapsed_ms_since(import_start);
    record_import_metrics(total_ms);
    finish_import_request();

    if (total_ms >= 2000) {
        TS_LOG(INFO) << "NuRaft import timing: bytes=" << request->body.size()
                     << " logical_chunks=" << logical_chunks
                     << " replay_chunks=" << replay_chunks
                     << " append_ms=" << append_ms
                     << " replay_ms=" << replay_ms
                     << " total_ms=" << total_ms;
    }

    response->set_content(response_status_code, response_content_type, aggregated_response_body, true);
    error.clear();
    return true;
}

bool NuRaftHttpRuntimeService::is_read_caught_up() const {
    return initialized_.load();
}

bool NuRaftHttpRuntimeService::is_single_node_mode() const {
    return bootstrap_config_.peers.empty();
}

bool NuRaftHttpRuntimeService::is_write_caught_up() const {
    return initialized_.load();
}

bool NuRaftHttpRuntimeService::is_alive() const {
    if (!initialized_.load() || !raft_server_) return false;
    // Single-node: always alive once initialized.
    if (bootstrap_config_.peers.empty()) return true;
    // Multi-node: alive if we know who the leader is.
    return raft_server_->get_leader() >= 0;
}

uint64_t NuRaftHttpRuntimeService::node_state() const {
    if (!initialized_.load()) return 0;
    // Match upstream braft State enum: 1 = STATE_LEADER, 4 = STATE_FOLLOWER.
    if (raft_server_ && raft_server_->is_leader()) return 1;
    return 4;
}

nlohmann::json NuRaftHttpRuntimeService::get_status() {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const uint64_t state_machine_applied_index = raft_state_machine_ != nullptr ?
        raft_state_machine_->get_last_commit_index() : 0;
    const auto snapshot_metrics = raft_state_machine_ != nullptr ?
        raft_state_machine_->get_snapshot_metrics() : TypesenseSnapshotMetricsSnapshot{};
    const uint64_t sync_calls = cumulative_sync_calls_.load(std::memory_order_relaxed);
    const uint64_t sync_replay_calls = cumulative_sync_replay_calls_.load(std::memory_order_relaxed);
    nlohmann::json status = {
        {"state", initialized_.load() ? "running" : "initializing"},
        {"server_id", identity_.server_id},
        {"snapshot_distance", options_.raft_params.snapshot_distance},
        {"is_leader", is_leader()},
        {"leader_url", get_leader_url()},
        {"read_caught_up", is_read_caught_up()},
        {"write_caught_up", is_write_caught_up()},
        {"queued_writes", 0},
        {"active_import_requests", active_import_requests_.load(std::memory_order_relaxed)},
        {"cumulative_import_requests", cumulative_import_requests_.load(std::memory_order_relaxed)},
        {"cumulative_import_bytes", cumulative_import_bytes_.load(std::memory_order_relaxed)},
        {"cumulative_import_docs_estimate", cumulative_import_docs_estimate_.load(std::memory_order_relaxed)},
        {"last_import_request_bytes", last_import_request_bytes_.load(std::memory_order_relaxed)},
        {"last_import_docs_estimate", last_import_docs_estimate_.load(std::memory_order_relaxed)},
        {"last_import_logical_chunks", last_import_logical_chunks_.load(std::memory_order_relaxed)},
        {"last_import_replay_chunks", last_import_replay_chunks_.load(std::memory_order_relaxed)},
        {"last_import_append_ms", last_import_append_ms_.load(std::memory_order_relaxed)},
        {"last_import_replay_ms", last_import_replay_ms_.load(std::memory_order_relaxed)},
        {"last_import_total_ms", last_import_total_ms_.load(std::memory_order_relaxed)},
        {"last_import_response_bytes", last_import_response_bytes_.load(std::memory_order_relaxed)},
        {"last_import_docs_per_sec", last_import_docs_per_sec_.load(std::memory_order_relaxed)},
        {"last_import_bytes_per_sec", last_import_bytes_per_sec_.load(std::memory_order_relaxed)},
        {"max_import_total_ms", max_import_total_ms_.load(std::memory_order_relaxed)},
        {"sync_cumulative_calls", sync_calls},
        {"sync_cumulative_fast_path_hits", cumulative_sync_fast_path_hits_.load(std::memory_order_relaxed)},
        {"sync_cumulative_replay_calls", sync_replay_calls},
        {"sync_last_total_ms", last_sync_total_ms_.load(std::memory_order_relaxed)},
        {"sync_last_replay_ms", last_sync_replay_ms_.load(std::memory_order_relaxed)},
        {"sync_avg_total_ms", sync_calls == 0 ? 0 :
                              cumulative_sync_total_ms_.load(std::memory_order_relaxed) / sync_calls},
        {"sync_avg_replay_ms", sync_replay_calls == 0 ? 0 :
                               cumulative_sync_replay_ms_.load(std::memory_order_relaxed) / sync_replay_calls},
        {"sync_max_total_ms", max_sync_total_ms_.load(std::memory_order_relaxed)},
        {"snapshot_in_progress", snapshot_metrics.snapshot_in_progress},
        {"last_snapshot_success", snapshot_metrics.last_snapshot_success},
        {"last_snapshot_log_index", snapshot_metrics.last_snapshot_log_index},
        {"last_snapshot_applied_index", snapshot_metrics.last_snapshot_applied_index},
        {"last_snapshot_total_ms", snapshot_metrics.last_snapshot_total_ms},
        {"max_snapshot_total_ms", snapshot_metrics.max_snapshot_total_ms},
        {"cumulative_snapshots", snapshot_metrics.cumulative_snapshots},
        {"cumulative_snapshot_failures", snapshot_metrics.cumulative_snapshot_failures},
    };

    if (raft_server_) {
        uint64_t committed_idx = raft_state_machine_ ?
            raft_state_machine_->get_last_commit_index() : 0;
        uint64_t last_idx = raft_server_->get_last_log_idx();
        status["last_index"] = last_idx;
        status["committed_index"] = committed_idx;
        const uint64_t known_applied_index = live_product_state_applied_index_.load(std::memory_order_relaxed);
        status["known_applied_index"] = known_applied_index;
        status["read_caught_up"] = initialized_.load() && known_applied_index >= committed_idx;
        status["applying_index"] = 0;
        status["raft_leader_id"] = raft_server_->get_leader();
        status["raft_term"] = raft_server_->get_term();
        status["state_machine_applied_index"] = state_machine_applied_index;
    }

    return status;
}

void NuRaftHttpRuntimeService::do_snapshot(const std::string& snapshot_path,
                                           const std::shared_ptr<http_req>& req,
                                           const std::shared_ptr<http_res>& res) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::string error;
    NuRaftKvStateMachineSink* snapshot_sink = materialized_state_sink_ != nullptr ? materialized_state_sink_.get() : nullptr;
    if (snapshot_sink == nullptr) {
        res->set_500("NuRaft runtime materialized state sink is not initialized.");
        send_response(req, res);
        return;
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
    if (raft_server_) {
        raft_server_->request_leadership();
        return true;
    }
    return false;
}

bool NuRaftHttpRuntimeService::reset_peers() {
    return false;
}

void NuRaftHttpRuntimeService::persist_applying_index() {}

int64_t NuRaftHttpRuntimeService::get_num_queued_writes() {
    return 0;
}

bool NuRaftHttpRuntimeService::is_leader() {
    if (raft_server_) {
        return raft_server_->is_leader();
    }
    return true;
}

std::string NuRaftHttpRuntimeService::get_leader_url() const {
    if (raft_server_) {
        int leader_id = raft_server_->get_leader();
        if (leader_id < 0) {
            return "";
        }
        for (const auto& peer : bootstrap_config_.peers) {
            if (peer.server_id() == leader_id) {
                return peer.leader_url(bootstrap_config_.api_uses_ssl);
            }
        }
        if (bootstrap_config_.self.server_id() == leader_id) {
            return bootstrap_config_.self.leader_url(bootstrap_config_.api_uses_ssl);
        }
        return "";
    }

    return bootstrap_config_.self.leader_url(bootstrap_config_.api_uses_ssl);
}

void NuRaftHttpRuntimeService::decr_pending_writes() {}

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
        // Invalidate per-document cache entries for this collection since an
        // import may have changed many documents.  Do NOT mark the collection
        // for materialized-state reads: imports containing $operations.increment
        // are stored raw in the materialized KV store but are resolved into
        // concrete field values by the live engine.  Preferring materialized
        // reads after an import would return unresolved $operations directives.
        invalidate_single_node_collection_cache(collection);
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
    if (rpath.handler == get_health) {
        return true;
    }

    // Sync replayed product state before handling the request.
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

    return handle_authentication(params, embedded_params_vec, body, rpath, auth_key);
}

void register_nuraft_http_runtime_routes(HttpServer* server) {
    server->get("/collections/:collection/documents/search", search_runtime_documents);
    server->post("/multi_search", post_multi_search, false, true);

    server->post("/collections/:collection/documents", post_add_document);
    server->del("/collections/:collection/documents", del_remove_documents, false, true);
    // Buffer one logical import request before entering the Raft write path so
    // transport chunking does not become log-entry granularity.
    server->post("/collections/:collection/documents/import", post_import_documents, false, true);
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
    server->patch("/keys/:id", patch_key);

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

// --- Real NuRaft consensus integration ---

bool NuRaftHttpRuntimeService::initialize_raft_server(std::string& error) {
    // Create state machine with commit callback for CollectionManager mirroring.
    raft_state_machine_ = nuraft::cs_new<TypesenseStateMachine>(
        layout_,
        materialized_state_sink_.get(),
        [](uint64_t log_idx, const NuRaftAppliedRequest& request) {
            (void)log_idx;
            (void)request;
        });

    // Create state manager.
    raft_state_manager_ = nuraft::cs_new<TypesenseStateManager>(
        layout_, identity_, bootstrap_config_);

    // Configure raft parameters from options (CLI args > ENV vars > defaults).
    const NuRaftRaftParams& rp = options_.raft_params;
    nuraft::raft_params params;
    params.heart_beat_interval_ = static_cast<int>(rp.heart_beat_interval_ms);
    params.election_timeout_lower_bound_ = static_cast<int>(rp.election_timeout_lower_bound_ms);
    params.election_timeout_upper_bound_ = static_cast<int>(rp.election_timeout_upper_bound_ms);
    params.reserved_log_items_ = static_cast<int>(rp.reserved_log_items);
    params.client_req_timeout_ = static_cast<int>(rp.client_req_timeout_ms);
    params.return_method_ = nuraft::raft_params::blocking;
    params.auto_forwarding_ = rp.auto_forwarding;
    params.auto_forwarding_req_timeout_ = static_cast<int>(rp.auto_forwarding_req_timeout_ms);
    params.snapshot_distance_ = static_cast<int>(rp.snapshot_distance);
    params.leadership_expiry_ = static_cast<int>(rp.leadership_expiry_ms);
    params.use_bg_thread_for_urgent_commit_ = true;

    // ASIO options for the NuRaft RPC transport.
    nuraft::asio_service::options asio_opts;
    asio_opts.thread_pool_size_ = static_cast<int>(rp.asio_thread_pool_size);

    TS_LOG(INFO) << "NuRaft params: heartbeat=" << rp.heart_beat_interval_ms
                 << "ms election=[" << rp.election_timeout_lower_bound_ms
                 << "," << rp.election_timeout_upper_bound_ms
                 << "]ms client_timeout=" << rp.client_req_timeout_ms
                 << "ms snapshot_distance=" << rp.snapshot_distance
                 << " reserved_logs=" << rp.reserved_log_items
                 << " leadership_expiry=" << rp.leadership_expiry_ms
                 << "ms auto_fwd=" << (rp.auto_forwarding ? "true" : "false")
                 << " auto_fwd_timeout=" << rp.auto_forwarding_req_timeout_ms
                 << "ms asio_threads=" << rp.asio_thread_pool_size;

    // Launch NuRaft server via raft_launcher.
    raft_launcher_ = std::make_unique<nuraft::raft_launcher>();
    int raft_port = static_cast<int>(bootstrap_config_.self.peer_port);

    raft_server_ = raft_launcher_->init(
        raft_state_machine_,
        raft_state_manager_,
        nullptr,  // logger (use NuRaft default)
        raft_port,
        asio_opts,
        params);

    if (!raft_server_) {
        error = "Failed to initialize NuRaft server on port " + std::to_string(raft_port);
        raft_launcher_.reset();
        return false;
    }

    error.clear();
    return true;
}

bool NuRaftHttpRuntimeService::append_via_raft(
    const std::string& request_payload,
    uint16_t payload_encoding,
    const http_req& request,
    uint64_t& committed_index,
    bool& forwarded_to_leader,
    std::string& error) {
    committed_index = 0;
    forwarded_to_leader = false;

    if (!raft_server_) {
        error = "NuRaft server is not running.";
        return false;
    }

    // Serialize the request into a NuRaft buffer.
    NuRaftRequestEnvelope envelope(request_payload, payload_encoding);
    std::string serialized = envelope.serialize();

    // Append to Raft — this blocks until committed by majority (blocking mode).
    // Retry on NOT_LEADER: covers the transient window where no leader is
    // elected yet (leader_ == -1) or a leadership transition is in flight.
    static constexpr int kMaxRetries = 5;
    static constexpr int kRetryBackoffMs = 200;

    for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
        auto buf_copy = nuraft::buffer::alloc(serialized.size());
        std::memcpy(buf_copy->data(), serialized.data(), serialized.size());

        const auto append_start = std::chrono::steady_clock::now();
        auto result = raft_server_->append_entries({buf_copy});
        const uint64_t append_wait_ms = elapsed_ms_since(append_start);
        if (result->get_accepted()) {
            committed_index = raft_state_machine_->get_last_commit_index();
            forwarded_to_leader = !raft_server_->is_leader();
            if (append_wait_ms >= 2000) {
                TypesenseSnapshotMetricsSnapshot snapshot_metrics;
                if (raft_state_machine_ != nullptr) {
                    snapshot_metrics = raft_state_machine_->get_snapshot_metrics();
                }
                TS_LOG(INFO) << "NuRaft append_entries slow: route_hash=" << request.route_hash
                             << ", payload_bytes=" << serialized.size()
                             << ", append_wait_ms=" << append_wait_ms
                             << ", committed_index=" << committed_index
                             << ", raft_last_log_idx=" << raft_server_->get_last_log_idx()
                             << ", raft_term=" << raft_server_->get_term()
                             << ", snapshot_in_progress=" << snapshot_metrics.snapshot_in_progress
                             << ", snapshot_last_log_index=" << snapshot_metrics.last_snapshot_log_index
                             << ", snapshot_last_applied_index=" << snapshot_metrics.last_snapshot_applied_index
                             << ", snapshot_last_total_ms=" << snapshot_metrics.last_snapshot_total_ms
                             << ", snapshot_max_total_ms=" << snapshot_metrics.max_snapshot_total_ms
                             << ", snapshot_cumulative=" << snapshot_metrics.cumulative_snapshots
                             << ", snapshot_failures=" << snapshot_metrics.cumulative_snapshot_failures;
            }
            error.clear();
            return true;
        }

        auto result_code = result->get_result_code();
        if (result_code != nuraft::cmd_result_code::NOT_LEADER ||
            attempt == kMaxRetries) {
            if (result_code == nuraft::cmd_result_code::NOT_LEADER) {
                error = "Not the leader after " + std::to_string(kMaxRetries) +
                        " retries. Leader is server " +
                        std::to_string(raft_server_->get_leader());
            } else {
                error = "NuRaft append_entries failed: result code " +
                        std::to_string(static_cast<int>(result_code));
            }
            return false;
        }

        TS_LOG(INFO) << "NuRaft append_entries: NOT_LEADER (attempt "
                     << (attempt + 1) << "/" << (kMaxRetries + 1)
                     << "), leader=" << raft_server_->get_leader()
                     << ", retrying in " << kRetryBackoffMs << "ms";
        std::this_thread::sleep_for(std::chrono::milliseconds(kRetryBackoffMs));
    }

    // Should not be reached — the loop returns on success or final failure.
    error = "NuRaft append_entries: unexpected exit from retry loop";
    return false;
}

void NuRaftHttpRuntimeService::shutdown() {
    if (raft_launcher_) {
        raft_launcher_->shutdown(5);
    }
    raft_server_.reset();
    raft_state_machine_.reset();
    raft_state_manager_.reset();
    raft_launcher_.reset();
}
