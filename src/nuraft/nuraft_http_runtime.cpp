#include "nuraft/nuraft_http_runtime.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <random>
#include <set>
#include <thread>
#include <utility>

#include "analytics_manager.h"
#include "core_api.h"
#include "json.hpp"
#include "collection_manager.h"
#include "conversation_model_manager.h"
#include "nuraft/nuraft_route_classifier.h"
#include "nuraft/nuraft_snapshot_coordinator.h"
#include "nuraft/nuraft_state_initializer.h"
#include "nuraft/nuraft_state_machine_sink.h"
#include "nuraft/nuraft_metadata_store.h"
#include "natural_language_search_model_manager.h"
#include "personalization_model_manager.h"
#include "ratelimit_manager.h"
#include "search_analytics.h"
#include "stemmer_manager.h"
#include "stopwords_manager.h"
#include "tokenizer.h"
#include "typesense_server_utils.h"
#include "tsconfig.h"

namespace {

constexpr const char* kCollectionPrefix = "state/collections/";
constexpr const char* kDocumentPrefix = "state/documents/";
constexpr size_t kDocumentImportRaftChunkMaxBytes = 4 * 1024 * 1024;
constexpr uint64_t kStartupMaterializationGraceMs = 10000;
constexpr uint64_t kManualSnapshotTimeoutMs = 60000;
constexpr uint64_t kSnapshotSchedulerPollMs = 1000;

using HttpHandlerFn = bool (*)(const std::shared_ptr<http_req>&, const std::shared_ptr<http_res>&);

struct WriteRouteDefinition {
    const char* http_method;
    const char* path;
    HttpHandlerFn handler;
    NuRaftWriteRouteMode mode;
    bool async_req;
    bool async_res;
};

std::vector<std::string> split_route_path_parts(const std::string& path) {
    if (path.empty() || path == "/") {
        return {};
    }

    std::string trimmed = path;
    if (!trimmed.empty() && trimmed.front() == '/') {
        trimmed.erase(trimmed.begin());
    }

    std::vector<std::string> path_parts;
    StringUtils::split(trimmed, path_parts, "/");
    return path_parts;
}

uint64_t steady_clock_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint32_t load_test_mirror_worker_delay_ms() {
    const char* raw_value = std::getenv("TYPESENSE_TEST_NURAFT_MIRROR_APPLY_DELAY_MS");
    if (raw_value == nullptr || raw_value[0] == '\0') {
        return 0;
    }
    return static_cast<uint32_t>(std::strtoul(raw_value, nullptr, 10));
}

template <typename Callback>
void visit_nuraft_write_routes(Callback&& callback) {
    const auto emit = [&](const char* method,
                          const char* path,
                          HttpHandlerFn handler,
                          NuRaftWriteRouteMode mode,
                          bool async_req = false,
                          bool async_res = false) {
        callback(WriteRouteDefinition{method, path, handler, mode, async_req, async_res});
    };

    emit("POST", "/collections/:collection/documents", post_add_document, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/collections/:collection/documents", del_remove_documents, NuRaftWriteRouteMode::kMirrorWorker, false, true);
    emit("POST", "/collections/:collection/documents/import", post_import_documents, NuRaftWriteRouteMode::kMirrorWorker, false, true);
    emit("PATCH", "/collections/:collection/documents/:id", patch_update_document, NuRaftWriteRouteMode::kMirrorWorker);
    emit("PATCH", "/collections/:collection/documents", patch_update_documents, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/collections/:collection/documents/:id", del_remove_document, NuRaftWriteRouteMode::kMirrorWorker);

    emit("POST", "/collections", post_create_collection, NuRaftWriteRouteMode::kMirrorWorker);
    emit("PATCH", "/collections/:collection", patch_update_collection, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/collections/:collection", del_drop_collection, NuRaftWriteRouteMode::kMirrorWorker);

    emit("PUT", "/aliases/:alias", put_upsert_alias, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/aliases/:alias", del_alias, NuRaftWriteRouteMode::kMirrorWorker);

    emit("POST", "/keys", post_create_key, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/keys/:id", del_key, NuRaftWriteRouteMode::kMirrorWorker);
    emit("PATCH", "/keys/:id", patch_key, NuRaftWriteRouteMode::kMirrorWorker);

    emit("PUT", "/presets/:name", put_upsert_preset, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/presets/:name", del_preset, NuRaftWriteRouteMode::kMirrorWorker);

    emit("PUT", "/stopwords/:name", put_upsert_stopword, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/stopwords/:name", del_stopword, NuRaftWriteRouteMode::kMirrorWorker);

    emit("PUT", "/synonym_sets/:name", put_synonym_set, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/synonym_sets/:name", del_synonym_set, NuRaftWriteRouteMode::kMirrorWorker);
    emit("PUT", "/synonym_sets/:name/items/:id", put_synonym_set_item, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/synonym_sets/:name/items/:id", del_synonym_set_item, NuRaftWriteRouteMode::kMirrorWorker);

    emit("PUT", "/curation_sets/:name", put_curation_set, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/curation_sets/:name", del_curation_set, NuRaftWriteRouteMode::kMirrorWorker);
    emit("PUT", "/curation_sets/:name/items/:id", put_curation_set_item, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/curation_sets/:name/items/:id", del_curation_set_item, NuRaftWriteRouteMode::kMirrorWorker);

    emit("POST", "/analytics/rules", post_create_analytics_rules, NuRaftWriteRouteMode::kMirrorWorker);
    emit("PUT", "/analytics/rules/:name", put_upsert_analytics_rules, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/analytics/rules/:name", del_analytics_rules, NuRaftWriteRouteMode::kMirrorWorker);
    emit("POST", "/analytics/events", post_create_event, NuRaftWriteRouteMode::kLocalOnly);
    emit("POST", "/analytics/aggregate_events", post_write_analytics_to_db, NuRaftWriteRouteMode::kMirrorWorker);
    emit("POST", "/analytics/flush", post_analytics_flush, NuRaftWriteRouteMode::kLocalOnly);

    emit("POST", "/stemming/dictionaries/import", post_import_stemming_dictionary, NuRaftWriteRouteMode::kMirrorWorker, false, true);
    emit("DELETE", "/stemming/dictionaries/:id", del_stemming_dictionary, NuRaftWriteRouteMode::kMirrorWorker);

    emit("POST", "/health", post_health, NuRaftWriteRouteMode::kLocalOnly);

    emit("POST", "/operations/snapshot", post_snapshot, NuRaftWriteRouteMode::kLocalOnly);
    emit("POST", "/operations/vote", post_vote, NuRaftWriteRouteMode::kLocalOnly);
    emit("POST", "/operations/cache/clear", post_clear_cache, NuRaftWriteRouteMode::kLocalOnly);
    emit("POST", "/operations/db/compact", post_compact_db, NuRaftWriteRouteMode::kLocalOnly);
    emit("POST", "/operations/reset_peers", post_reset_peers, NuRaftWriteRouteMode::kLocalOnly);

    emit("POST", "/conversations/models", post_conversation_model, NuRaftWriteRouteMode::kMirrorWorker);
    emit("PUT", "/conversations/models/:id", put_conversation_model, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/conversations/models/:id", del_conversation_model, NuRaftWriteRouteMode::kMirrorWorker);

    emit("POST", "/personalization/models", post_personalization_model, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/personalization/models/:id", del_personalization_model, NuRaftWriteRouteMode::kMirrorWorker);
    emit("PUT", "/personalization/models/:id", put_personalization_model, NuRaftWriteRouteMode::kMirrorWorker);

    emit("POST", "/limits", post_rate_limit, NuRaftWriteRouteMode::kMirrorWorker);
    emit("PUT", "/limits/:id", put_rate_limit, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/limits/:id", del_rate_limit, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/limits/active/:id", del_throttle, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/limits/exceeds/:id", del_exceed, NuRaftWriteRouteMode::kMirrorWorker);
    emit("POST", "/config", post_config, NuRaftWriteRouteMode::kMirrorWorker);

    emit("POST", "/proxy", post_proxy, NuRaftWriteRouteMode::kLocalOnly);
    emit("POST", "/proxy_sse", post_proxy_sse, NuRaftWriteRouteMode::kLocalOnly, false, true);

    emit("POST", "/nl_search_models", post_nl_search_model, NuRaftWriteRouteMode::kMirrorWorker);
    emit("PUT", "/nl_search_models/:id", put_nl_search_model, NuRaftWriteRouteMode::kMirrorWorker);
    emit("DELETE", "/nl_search_models/:id", delete_nl_search_model, NuRaftWriteRouteMode::kMirrorWorker);
}

const std::unordered_map<uint64_t, NuRaftWriteRouteMode>& write_route_modes() {
    static const auto* modes = new std::unordered_map<uint64_t, NuRaftWriteRouteMode>([] {
        std::unordered_map<uint64_t, NuRaftWriteRouteMode> definitions;
        visit_nuraft_write_routes([&](const WriteRouteDefinition& route) {
            route_path route_path_obj(route.http_method,
                                      split_route_path_parts(route.path),
                                      route.handler,
                                      route.async_req,
                                      route.async_res);
            definitions.emplace(route_path_obj.route_hash(), route.mode);
        });
        return definitions;
    }());
    return *modes;
}

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

NuRaftAppliedRequest build_applied_request(const http_req& request,
                                           uint64_t origin_server_id = 0,
                                           uint64_t response_token = 0) {
    NuRaftAppliedRequest applied_request;
    applied_request.route_hash = request.route_hash;
    applied_request.route_kind = NuRaftRouteClassifier::classify(request.route_hash);
    applied_request.origin_server_id = origin_server_id;
    applied_request.response_token = response_token;
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

bool request_requires_strong_read_consistency(const std::shared_ptr<http_req>& req) {
    const auto consistency_it = req->params.find("read_consistency");
    if (consistency_it == req->params.end()) {
        return false;
    }

    return consistency_it->second == "strong" || consistency_it->second == "linearizable";
}

// Default reads should serve the latest locally applied product state instead of
// stalling behind the newest committed write. Callers that explicitly need the
// pre-existing behavior can request it with `read_consistency=strong`.
void maybe_wait_before_read(const std::shared_ptr<http_req>& req) {
    if (!request_requires_strong_read_consistency(req)) {
        return;
    }

    NuRaftHttpRuntimeService* runtime = current_runtime_service();
    if (runtime != nullptr) {
        std::string error;
        runtime->wait_for_live_product_state(5000, error);
    }
}

// Wrapper that waits for the live in-memory state before delegating to any
// standard read handler. Registered instead of the original handler for routes
// that read mutable state so the wait happens on the thread pool.
template<bool (*OriginalHandler)(const std::shared_ptr<http_req>&,
                                  const std::shared_ptr<http_res>&)>
bool synced_read_handler(const std::shared_ptr<http_req>& req,
                         const std::shared_ptr<http_res>& res) {
    maybe_wait_before_read(req);
    return OriginalHandler(req, res);
}

bool get_runtime_collections(const std::shared_ptr<http_req>& request, const std::shared_ptr<http_res>& response) {
    NuRaftHttpRuntimeService* runtime = current_runtime_service();
    if (runtime == nullptr) {
        response->set_500("NuRaft runtime service is not attached.");
        return true;
    }
    maybe_wait_before_read(request);

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
    maybe_wait_before_read(request);

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
    maybe_wait_before_read(request);

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
    maybe_wait_before_read(request);

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

bool nuraft_http_runtime_lookup_write_route_mode(uint64_t route_hash,
                                                 NuRaftWriteRouteMode& mode) {
    const auto& modes = write_route_modes();
    const auto it = modes.find(route_hash);
    if (it == modes.end()) {
        return false;
    }

    mode = it->second;
    return true;
}

NuRaftHttpRuntimeService::NuRaftHttpRuntimeService(HttpServer* server, NuRaftHttpServerOptions options)
    : server_(server),
      options_(std::move(options)),
      layout_(NuRaftStateLayout::from_data_dir(options_.startup_options.data_dir)),
      initialized_(false),
      test_mirror_worker_apply_delay_ms_(load_test_mirror_worker_delay_ms()) {}

bool NuRaftHttpRuntimeService::cache_enabled() const {
    return materialized_state_sink_ != nullptr;
}

bool NuRaftHttpRuntimeService::should_replicate_write(uint64_t route_hash, bool* known) const {
    NuRaftWriteRouteMode route_mode = NuRaftWriteRouteMode::kLocalOnly;
    const bool found = nuraft_http_runtime_lookup_write_route_mode(route_hash, route_mode);
    if (known != nullptr) {
        *known = found;
    }
    return found && route_mode == NuRaftWriteRouteMode::kMirrorWorker;
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

void NuRaftHttpRuntimeService::start_mirror_worker() {
    std::lock_guard<std::mutex> lock(mirror_worker_mutex_);
    if (mirror_worker_thread_.joinable()) {
        return;
    }
    mirror_worker_stopping_ = false;
    mirror_worker_thread_ = std::thread(&NuRaftHttpRuntimeService::mirror_worker_loop, this);
}

void NuRaftHttpRuntimeService::stop_mirror_worker() {
    {
        std::lock_guard<std::mutex> lock(mirror_worker_mutex_);
        mirror_worker_stopping_ = true;
    }
    mirror_worker_cv_.notify_all();

    if (mirror_worker_thread_.joinable()) {
        mirror_worker_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(mirrored_results_mutex_);
        mirrored_results_.clear();
    }
    mirrored_results_cv_.notify_all();
}

void NuRaftHttpRuntimeService::start_snapshot_scheduler() {
    const int interval_seconds = Config::get_instance().get_snapshot_interval_seconds();
    if (interval_seconds <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(snapshot_scheduler_mutex_);
    if (snapshot_scheduler_thread_.joinable()) {
        return;
    }
    snapshot_scheduler_stopping_ = false;
    snapshot_scheduler_thread_ = std::thread(&NuRaftHttpRuntimeService::snapshot_scheduler_loop, this);
}

void NuRaftHttpRuntimeService::stop_snapshot_scheduler() {
    {
        std::lock_guard<std::mutex> lock(snapshot_scheduler_mutex_);
        snapshot_scheduler_stopping_ = true;
    }
    snapshot_scheduler_cv_.notify_all();

    if (snapshot_scheduler_thread_.joinable()) {
        snapshot_scheduler_thread_.join();
    }
}

void NuRaftHttpRuntimeService::snapshot_scheduler_loop() {
    const int interval_seconds = Config::get_instance().get_snapshot_interval_seconds();
    if (interval_seconds <= 0) {
        return;
    }

    auto next_interval_check = std::chrono::steady_clock::now() +
                               std::chrono::seconds(interval_seconds);
    std::unique_lock<std::mutex> lock(snapshot_scheduler_mutex_);
    while (!snapshot_scheduler_stopping_) {
        if (snapshot_scheduler_cv_.wait_for(lock,
                                            std::chrono::milliseconds(kSnapshotSchedulerPollMs),
                                            [&] { return snapshot_scheduler_stopping_; })) {
            break;
        }

        lock.unlock();

        if (raft_server_ != nullptr && raft_server_->is_leader() && raft_state_machine_ != nullptr) {
            const auto snapshot_metrics = raft_state_machine_->get_snapshot_metrics();
            const uint64_t committed_index = raft_state_machine_->get_last_commit_index();
            const uint64_t last_snapshot_index = raft_server_->get_last_snapshot_idx();
            const auto peer_lag_metrics = get_snapshot_peer_lag_metrics(committed_index);
            const bool interval_due = std::chrono::steady_clock::now() >= next_interval_check;
            const bool lagging_peers_exceed_retained_window =
                peer_lag_metrics.lagging_peer_count > 0 &&
                committed_index > last_snapshot_index &&
                (committed_index - last_snapshot_index) > options_.raft_params.reserved_log_items;

            if (!snapshot_metrics.snapshot_in_progress &&
                committed_index > last_snapshot_index &&
                (interval_due || lagging_peers_exceed_retained_window)) {
                const uint64_t snapshot_index = raft_server_->create_snapshot();
                if (snapshot_index != 0) {
                    TS_LOG(INFO) << "NuRaft snapshot scheduler: created snapshot at committed_index="
                                 << committed_index << ", snapshot_index=" << snapshot_index
                                 << ", last_snapshot_index=" << last_snapshot_index
                                 << ", reason=" << (interval_due ? "interval" : "lagging_peer_window")
                                 << ", lagging_peer_count=" << peer_lag_metrics.lagging_peer_count
                                 << ", max_peer_log_gap=" << peer_lag_metrics.max_peer_log_gap;
                }
                next_interval_check = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(interval_seconds);
            } else if (interval_due) {
                next_interval_check = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(interval_seconds);
            }
        }

        lock.lock();
    }
}

NuRaftHttpRuntimeService::SnapshotPeerLagMetrics
NuRaftHttpRuntimeService::get_snapshot_peer_lag_metrics(uint64_t committed_index) const {
    SnapshotPeerLagMetrics metrics;
    if (raft_server_ == nullptr || !raft_server_->is_leader()) {
        return metrics;
    }

    const auto peer_infos = raft_server_->get_peer_info_all();
    for (const auto& peer_info : peer_infos) {
        const uint64_t peer_log_gap = committed_index > peer_info.last_log_idx_
                                          ? (committed_index - peer_info.last_log_idx_)
                                          : 0;
        const uint64_t peer_response_age_ms = peer_info.last_succ_resp_us_ / 1000;
        metrics.max_peer_log_gap = std::max(metrics.max_peer_log_gap, peer_log_gap);
        metrics.max_peer_response_age_ms = std::max(metrics.max_peer_response_age_ms,
                                                    peer_response_age_ms);
        if (peer_log_gap > options_.raft_params.reserved_log_items) {
            metrics.lagging_peer_count++;
        }
    }

    return metrics;
}

uint64_t NuRaftHttpRuntimeService::get_materialized_state_applied_index() const {
    if (materialized_state_sink_ == nullptr) {
        return 0;
    }

    uint64_t applied_index = 0;
    std::string error;
    if (!materialized_state_sink_->read_last_applied_index(applied_index, error)) {
        TS_LOG(WARNING) << "NuRaft runtime could not read materialized-state applied index: "
                        << error;
        return 0;
    }
    return applied_index;
}

uint64_t NuRaftHttpRuntimeService::allocate_response_token() {
    return next_response_token_.fetch_add(1, std::memory_order_relaxed);
}

void NuRaftHttpRuntimeService::enqueue_mirrored_request(const NuRaftAppliedRequest& request) {
    {
        std::lock_guard<std::mutex> lock(mirror_worker_mutex_);
        mirror_worker_queue_.push_back(request);
    }
    mirror_worker_cv_.notify_one();
}

void NuRaftHttpRuntimeService::store_mirrored_result(uint64_t response_token, MirroredWriteResult result) {
    {
        std::lock_guard<std::mutex> lock(mirrored_results_mutex_);
        mirrored_results_[response_token] = std::move(result);
    }
    mirrored_results_cv_.notify_all();
}

bool NuRaftHttpRuntimeService::wait_for_mirrored_result(uint64_t response_token,
                                                        uint32_t timeout_ms,
                                                        MirroredWriteResult& result) {
    std::unique_lock<std::mutex> lock(mirrored_results_mutex_);
    const bool ready = mirrored_results_cv_.wait_for(lock,
                                                     std::chrono::milliseconds(timeout_ms),
                                                     [&] {
                                                         return mirrored_results_.count(response_token) != 0;
                                                     });
    if (!ready) {
        return false;
    }

    const auto it = mirrored_results_.find(response_token);
    if (it == mirrored_results_.end()) {
        return false;
    }

    result = std::move(it->second);
    mirrored_results_.erase(it);
    return true;
}

bool NuRaftHttpRuntimeService::wait_for_live_product_state(uint32_t timeout_ms, std::string& error) {
    cumulative_sync_calls_.fetch_add(1, std::memory_order_relaxed);

    uint64_t target_index = 0;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (!initialized_.load()) {
            error.clear();
            cumulative_sync_fast_path_hits_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        target_index = raft_state_machine_ != nullptr ? raft_state_machine_->get_last_commit_index() : 0;
    }

    if (live_product_state_applied_index_.load(std::memory_order_relaxed) >= target_index) {
        error.clear();
        cumulative_sync_fast_path_hits_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const auto wait_start = std::chrono::steady_clock::now();
    const bool ok = wait_for_applied_index(target_index, timeout_ms);
    const uint64_t wait_ms = elapsed_ms_since(wait_start);
    cumulative_sync_total_ms_.fetch_add(wait_ms, std::memory_order_relaxed);
    last_sync_total_ms_.store(wait_ms, std::memory_order_relaxed);
    max_sync_total_ms_.store(std::max(max_sync_total_ms_.load(std::memory_order_relaxed), wait_ms),
                             std::memory_order_relaxed);

    if (!ok) {
        error = "Timed out waiting for live product state to reach committed index " +
                std::to_string(target_index);
        return false;
    }

    error.clear();
    return true;
}

NuRaftHttpRuntimeService::MirroredWriteResult
NuRaftHttpRuntimeService::apply_mirrored_request(const NuRaftAppliedRequest& applied_request) {
    MirroredWriteResult result;
    result.applied_index = applied_request.index;

    route_path* route = nullptr;
    std::string error;
    if (!find_registered_route(server_, applied_request.route_hash, route, error)) {
        result.error = error;
        return result;
    }

    if (route == nullptr) {
        result.error = "NuRaft runtime could not resolve registered route handler.";
        return result;
    }

    NuRaftWriteRouteMode route_mode = NuRaftWriteRouteMode::kLocalOnly;
    result.should_apply = nuraft_http_runtime_lookup_write_route_mode(applied_request.route_hash, route_mode) &&
                          route_mode == NuRaftWriteRouteMode::kMirrorWorker;
    if (!result.should_apply) {
        result.handler_ok = true;
        return result;
    }

    auto request = build_replay_request(applied_request, *route);
    auto response = std::make_shared<http_res>(nullptr);
    std::lock_guard<std::mutex> apply_lock(live_apply_mutex_);

    if (applied_request.route_kind != NuRaftRouteKind::kUnknown) {
        result.handler_ok = mirror_single_node_typesense_state(request,
                                                               applied_request.route_kind,
                                                               error,
                                                               response);
        if (!result.handler_ok && !is_expected_missing_collection_mirror_skip(applied_request.route_kind, error)) {
            TS_LOG(WARNING) << "NuRaft mirror worker apply failed for route '"
                            << request->http_method << " " << request->path_without_query
                            << "': " << error;
        }
        update_single_node_document_cache(*request, applied_request.route_kind);
    } else {
        result.handler_ok = invoke_registered_handler(server_, request, response, error);
        if (!result.handler_ok && response->status_code == 0) {
            TS_LOG(WARNING) << "NuRaft mirror worker apply failed for route '"
                            << request->http_method << " " << request->path_without_query
                            << "': " << error;
        }
    }

    result.status_code = response->status_code;
    result.body = response->body;
    result.content_type_header = response->content_type_header;
    result.error = error;
    return result;
}

void NuRaftHttpRuntimeService::mirror_worker_loop() {
    for (;;) {
        NuRaftAppliedRequest applied_request;
        {
            std::unique_lock<std::mutex> lock(mirror_worker_mutex_);
            mirror_worker_cv_.wait(lock, [&] {
                return mirror_worker_stopping_ || !mirror_worker_queue_.empty();
            });

            if (mirror_worker_queue_.empty()) {
                if (mirror_worker_stopping_) {
                    return;
                }
                continue;
            }

            applied_request = std::move(mirror_worker_queue_.front());
            mirror_worker_queue_.pop_front();
        }

        MirroredWriteResult result = apply_mirrored_request(applied_request);
        if (test_mirror_worker_apply_delay_ms_ != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(test_mirror_worker_apply_delay_ms_));
        }
        advance_live_product_state_applied_index(applied_request.index);

        if (applied_request.response_token != 0 &&
            applied_request.origin_server_id == static_cast<uint64_t>(identity_.server_id)) {
            result.applied_index = applied_request.index;
            store_mirrored_result(applied_request.response_token, std::move(result));
        }
    }
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
    const bool startup_materialization_tracking = !bootstrap_config_.peers.empty() && last_applied_index == 0;
    startup_materialization_tracking_.store(startup_materialization_tracking, std::memory_order_relaxed);
    startup_materialization_pending_.store(startup_materialization_tracking, std::memory_order_relaxed);
    startup_materialization_started_at_ms_.store(startup_materialization_tracking ? steady_clock_now_ms() : 0,
                                                 std::memory_order_relaxed);
    startup_materialization_base_index_.store(last_applied_index, std::memory_order_relaxed);

    // Product state (collections, documents, aliases, presets, stopwords,
    // synonyms, curations, analytics) is already loaded from the main RocksDB
    // store by collection_manager.load(). The main store has WAL enabled so
    // all writes from the product handlers are durable. Just advance the
    // applied index to match the materialized state so the background mirror
    // worker starts from the correct point.
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

    start_mirror_worker();

    if (!initialize_raft_server(error)) {
        TS_LOG(ERROR) << "NuRaft init: raft server initialization failed: " << error;
        stop_mirror_worker();
        materialized_state_sink_.reset();
        return false;
    }

    if (bootstrap_config_.peers.empty()) {
        // Only single-node mode needs to force leadership before serving.
        // In a multi-node cluster, forcing every recovering follower through a
        // 5-second leadership wait delays HTTP startup and obscures recovery.
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
    } else {
        TS_LOG(INFO) << "NuRaft init: multi-node startup, waiting for cluster election without forcing leadership.";
    }

    // Warm restarts can align the live applied index to the already-loaded
    // on-disk state machine index. Empty follower recovery must not do that:
    // the state machine can replay and commit far ahead of the mirror worker
    // during initialize_raft_server(), and copying that index here would mark
    // the node ready before CollectionManager has materialized the data.
    if (raft_server_ && !startup_materialization_tracking_.load(std::memory_order_relaxed)) {
        const uint64_t state_machine_applied =
            raft_state_machine_ != nullptr ? raft_state_machine_->get_last_commit_index() : 0;
        if (state_machine_applied > live_product_state_applied_index_.load(std::memory_order_relaxed)) {
            advance_live_product_state_applied_index(state_machine_applied);
            TS_LOG(INFO) << "NuRaft init: aligned applied index to state machine index "
                         << state_machine_applied;
        }
        startup_materialization_base_index_.store(
            live_product_state_applied_index_.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    refresh_startup_materialization_state();
    start_snapshot_scheduler();
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
        refresh_startup_materialization_state();
        live_state_progress_cv_.notify_all();
    }
}

bool NuRaftHttpRuntimeService::is_materialization_ready() const {
    const_cast<NuRaftHttpRuntimeService*>(this)->refresh_startup_materialization_state();
    if (!initialized_.load(std::memory_order_relaxed)) {
        return false;
    }

    if (raft_server_ != nullptr &&
        (raft_server_->is_catching_up() || raft_server_->is_receiving_snapshot())) {
        return false;
    }

    if (!startup_materialization_pending_.load(std::memory_order_relaxed)) {
        return true;
    }

    return materialization_lag() == 0;
}

uint64_t NuRaftHttpRuntimeService::materialization_lag() const {
    const uint64_t committed_index = get_materialized_state_applied_index();
    const uint64_t live_applied_index = live_product_state_applied_index_.load(std::memory_order_relaxed);
    return committed_index >= live_applied_index ? (committed_index - live_applied_index) : 0;
}

void NuRaftHttpRuntimeService::refresh_startup_materialization_state() {
    const uint64_t materialized_state_applied_index = get_materialized_state_applied_index();
    const uint64_t live_applied_index = live_product_state_applied_index_.load(std::memory_order_relaxed);
    const uint64_t committed_index = raft_server_ != nullptr ? raft_server_->get_committed_log_idx() : 0;
    const bool raft_is_recovering = raft_server_ != nullptr &&
        (raft_server_->is_catching_up() || raft_server_->is_receiving_snapshot());
    const uint64_t snapshot_reload_target_index =
        last_snapshot_reload_target_index_.load(std::memory_order_relaxed);
    const bool snapshot_reload_pending =
        snapshot_reload_in_progress_.load(std::memory_order_relaxed) ||
        snapshot_reload_target_index > live_applied_index;
    bool tracking = startup_materialization_tracking_.load(std::memory_order_relaxed);

    if (!tracking && (raft_is_recovering || snapshot_reload_pending)) {
        startup_materialization_tracking_.store(true, std::memory_order_relaxed);
        tracking = true;
    }

    if (materialized_state_applied_index > live_applied_index) {
        if (!tracking) {
            return;
        }
        startup_materialization_pending_.store(true, std::memory_order_relaxed);
        return;
    }

    if ((raft_is_recovering || snapshot_reload_pending) &&
        materialized_state_applied_index < committed_index) {
        if (!tracking) {
            return;
        }
        startup_materialization_pending_.store(true, std::memory_order_relaxed);
        return;
    }

    if (!tracking) {
        return;
    }

    const uint64_t base_index = startup_materialization_base_index_.load(std::memory_order_relaxed);
    if (live_applied_index <= base_index) {
        const uint64_t started_at_ms = startup_materialization_started_at_ms_.load(std::memory_order_relaxed);
        const uint64_t elapsed_ms = started_at_ms == 0 ? 0 : (steady_clock_now_ms() - started_at_ms);
        if (elapsed_ms < kStartupMaterializationGraceMs) {
            startup_materialization_pending_.store(true, std::memory_order_relaxed);
            return;
        }
    }

    startup_materialization_pending_.store(false, std::memory_order_relaxed);
    startup_materialization_tracking_.store(false, std::memory_order_relaxed);
}

bool NuRaftHttpRuntimeService::reload_live_product_state_from_snapshot(
    const NuRaftSnapshotDescriptor& descriptor,
    std::string& error) {
    const auto snapshot_db_dir = std::filesystem::path(layout_.snapshot_dir) /
                                 descriptor.snapshot_id / "db";
    if (!std::filesystem::is_directory(snapshot_db_dir)) {
        error = "NuRaft snapshot is missing the main Typesense db checkpoint at '" +
                snapshot_db_dir.string() + "'";
        return false;
    }

    std::lock_guard<std::mutex> apply_lock(live_apply_mutex_);

    CollectionManager& collection_manager = CollectionManager::get_instance();
    Store* store = collection_manager.get_store();
    if (store == nullptr) {
        error = "CollectionManager store is not initialized";
        return false;
    }

    StopwordsManager::get_instance().dispose();
    StemmerManager::get_instance().dispose();
    collection_manager.dispose();

    if (store->reload(true, snapshot_db_dir.string()) != 0) {
        error = "Failed to reload Typesense main store from snapshot checkpoint";
        return false;
    }

    Config& config = Config::get_instance();
    const size_t proc_count = std::max<size_t>(1, std::thread::hardware_concurrency());
    const size_t configured_parallel_collection_load = config.get_num_collections_parallel_load();
    const size_t num_collections_parallel_load =
        configured_parallel_collection_load == 0 ? (proc_count * 4) : configured_parallel_collection_load;
    const auto load_op = collection_manager.load(num_collections_parallel_load,
                                                 config.get_num_documents_parallel_load());
    if (!load_op.ok()) {
        error = load_op.error();
        return false;
    }

    RateLimitManager::getInstance()->clear_all();
    const auto rate_limit_init = RateLimitManager::getInstance()->init(store);
    if (!rate_limit_init.ok()) {
        error = rate_limit_init.error();
        return false;
    }

    ConversationModelManager::dispose();
    const auto conversation_model_init = ConversationModelManager::init(store);
    if (!conversation_model_init.ok()) {
        error = conversation_model_init.error();
        return false;
    }

    PersonalizationModelManager::dispose();
    const auto personalization_model_init = PersonalizationModelManager::init(store);
    if (!personalization_model_init.ok()) {
        error = personalization_model_init.error();
        return false;
    }

    NaturalLanguageSearchModelManager::dispose();
    const auto nl_model_init = NaturalLanguageSearchModelManager::init(store);
    if (!nl_model_init.ok()) {
        error = nl_model_init.error();
        return false;
    }

    {
        std::unique_lock<std::shared_mutex> cache_lock(document_cache_mutex_);
        document_cache_.clear();
    }
    {
        std::unique_lock<std::shared_mutex> preference_lock(read_preference_mutex_);
        materialized_read_preferred_collections_.clear();
    }

    advance_live_product_state_applied_index(descriptor.last_applied_index);
    error.clear();
    return true;
}

void NuRaftHttpRuntimeService::handle_applied_snapshot(
    uint64_t log_index,
    const NuRaftSnapshotDescriptor& descriptor) {
    const uint64_t previous_target = last_snapshot_reload_target_index_.load(std::memory_order_relaxed);
    if (descriptor.last_applied_index <= previous_target) {
        return;
    }
    last_snapshot_reload_target_index_.store(descriptor.last_applied_index, std::memory_order_relaxed);
    startup_materialization_tracking_.store(true, std::memory_order_relaxed);
    startup_materialization_pending_.store(true, std::memory_order_relaxed);
    startup_materialization_started_at_ms_.store(steady_clock_now_ms(), std::memory_order_relaxed);
    startup_materialization_base_index_.store(
        live_product_state_applied_index_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);

    bool expected = false;
    if (!snapshot_reload_in_progress_.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        return;
    }

    std::thread([this, log_index, descriptor]() {
        std::string error;
        if (!reload_live_product_state_from_snapshot(descriptor, error)) {
            TS_LOG(ERROR) << "NuRaft snapshot apply failed to reload live Typesense state at log_index="
                          << log_index << ": " << error;
        } else {
            TS_LOG(INFO) << "NuRaft snapshot apply reloaded live Typesense state at log_index="
                         << log_index << ", applied_index=" << descriptor.last_applied_index;
        }
        snapshot_reload_in_progress_.store(false, std::memory_order_relaxed);
        refresh_startup_materialization_state();
        live_state_progress_cv_.notify_all();
    }).detach();
}

bool NuRaftHttpRuntimeService::wait_for_applied_index(uint64_t target_index, uint32_t timeout_ms) {
    if (live_product_state_applied_index_.load(std::memory_order_relaxed) >= target_index) {
        return true;
    }
    std::unique_lock<std::mutex> lock(live_state_progress_mutex_);
    return live_state_progress_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        return live_product_state_applied_index_.load(std::memory_order_relaxed) >= target_index;
    });
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
    NuRaftWriteRouteMode route_mode = NuRaftWriteRouteMode::kLocalOnly;
    const bool has_explicit_route_mode =
        has_registered_route &&
        nuraft_http_runtime_lookup_write_route_mode(request->route_hash, route_mode);
    const bool delegates_to_mirror_worker = has_explicit_route_mode &&
                                            route_mode == NuRaftWriteRouteMode::kMirrorWorker;
    const bool delegates_to_registered_import_handler =
        delegates_to_mirror_worker &&
        route_kind == NuRaftRouteKind::kDocumentImport &&
        has_registered_route &&
        route != nullptr;

    if (!has_explicit_route_mode) {
        response->set_422("Unsupported NuRaft runtime write route.");
        send_response(request, response);
        return;
    }

    if (route_mode == NuRaftWriteRouteMode::kLocalOnly) {
        error.clear();
        const bool handler_ok = invoke_registered_handler(server_, request, response, error);
        if (!handler_ok && response->status_code == 0) {
            response->set_500(error);
        }
        send_response(request, response);
        return;
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
        send_response(request, response);
        return;
    }

    const uint64_t response_token = delegates_to_mirror_worker ? allocate_response_token() : 0;
    const std::string request_payload =
        build_applied_request(*request, identity_.server_id, response_token).encode_binary();
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

    // Wait for all preceding USER writes to be applied locally before running
    // the handler. Without this, CREATE and IMPORT can race: the import handler
    // runs before the create mirror completes, getting 404 "Collection not found".
    // Use a short non-blocking wait — if the predecessor hasn't been applied yet,
    // give it up to 5 seconds. This only matters when concurrent writes are
    // in flight (e.g., CREATE followed immediately by IMPORT).
    if (committed_index > 1 &&
        live_product_state_applied_index_.load(std::memory_order_relaxed) < committed_index - 1) {
        wait_for_applied_index(committed_index - 1, 5000);
    }

    if (!delegates_to_mirror_worker) {
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

    MirroredWriteResult mirrored_result;
    if (!wait_for_mirrored_result(response_token, options_.request_timeout_ms, mirrored_result)) {
        response->set_500("Timed out waiting for NuRaft mirror worker response.");
        send_response(request, response);
        return;
    }

    if (mirrored_result.status_code != 0) {
        response->set_body(mirrored_result.status_code, mirrored_result.body);
        response->content_type_header = mirrored_result.content_type_header;
    } else if (!mirrored_result.handler_ok && !mirrored_result.error.empty()) {
        response->set_500(mirrored_result.error);
    } else {
        const uint32_t code = request->http_method == "POST" ? 201 : 200;
        response->set_body(code, request->body);
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
    uint64_t apply_chunks = 0;
    uint64_t append_ms = 0;
    uint64_t apply_wait_ms = 0;
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
        last_import_apply_chunks_.store(apply_chunks, std::memory_order_relaxed);
        last_import_append_ms_.store(append_ms, std::memory_order_relaxed);
        last_import_apply_wait_ms_.store(apply_wait_ms, std::memory_order_relaxed);
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
        const uint64_t response_token = allocate_response_token();
        const std::string request_payload =
            build_applied_request(*chunk_request, identity_.server_id, response_token).encode_binary();

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

        // Wait for preceding entries (e.g., a collection CREATE) to be applied
        // before running the import handler which needs the collection to exist.
        {
            const uint64_t current_applied = live_product_state_applied_index_.load(std::memory_order_relaxed);
            if (chunk_committed_index > current_applied + 1) {
                wait_for_applied_index(chunk_committed_index - 1, 5000);
            }
        }

        auto chunk_response = std::make_shared<http_res>(nullptr);
        MirroredWriteResult chunk_result;
        const auto chunk_wait_start = std::chrono::steady_clock::now();
        const bool got_result = wait_for_mirrored_result(response_token, options_.request_timeout_ms, chunk_result);
        apply_wait_ms += elapsed_ms_since(chunk_wait_start);
        if (!got_result) {
            chunk_error = "Timed out waiting for NuRaft mirror worker import response.";
            return false;
        }
        if (!chunk_result.handler_ok && chunk_result.status_code == 0) {
            chunk_error = chunk_result.error.empty()
                              ? "NuRaft mirror worker import apply failed without an HTTP response."
                              : chunk_result.error;
            return false;
        }

        chunk_response->status_code = chunk_result.status_code;
        chunk_response->body = chunk_result.body;
        chunk_response->content_type_header = chunk_result.content_type_header;

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
        apply_chunks++;
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
                     << " apply_chunks=" << apply_chunks
                     << " append_ms=" << append_ms
                     << " apply_wait_ms=" << apply_wait_ms
                     << " total_ms=" << total_ms;
    }

    response->set_content(response_status_code, response_content_type, aggregated_response_body, true);
    error.clear();
    return true;
}

bool NuRaftHttpRuntimeService::is_read_caught_up() const {
    return is_materialization_ready();
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
    const uint64_t committed_idx = raft_server_ != nullptr ? raft_server_->get_committed_log_idx() : 0;
    const auto peer_lag_metrics = get_snapshot_peer_lag_metrics(committed_idx);
    const uint64_t sync_calls = cumulative_sync_calls_.load(std::memory_order_relaxed);
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
        {"last_import_apply_chunks", last_import_apply_chunks_.load(std::memory_order_relaxed)},
        {"last_import_append_ms", last_import_append_ms_.load(std::memory_order_relaxed)},
        {"last_import_apply_wait_ms", last_import_apply_wait_ms_.load(std::memory_order_relaxed)},
        {"last_import_total_ms", last_import_total_ms_.load(std::memory_order_relaxed)},
        {"last_import_response_bytes", last_import_response_bytes_.load(std::memory_order_relaxed)},
        {"last_import_docs_per_sec", last_import_docs_per_sec_.load(std::memory_order_relaxed)},
        {"last_import_bytes_per_sec", last_import_bytes_per_sec_.load(std::memory_order_relaxed)},
        {"max_import_total_ms", max_import_total_ms_.load(std::memory_order_relaxed)},
        {"sync_cumulative_calls", sync_calls},
        {"sync_cumulative_fast_path_hits", cumulative_sync_fast_path_hits_.load(std::memory_order_relaxed)},
        {"sync_last_total_ms", last_sync_total_ms_.load(std::memory_order_relaxed)},
        {"sync_avg_total_ms", sync_calls == 0 ? 0 :
                              cumulative_sync_total_ms_.load(std::memory_order_relaxed) / sync_calls},
        {"sync_max_total_ms", max_sync_total_ms_.load(std::memory_order_relaxed)},
        {"snapshot_in_progress", snapshot_metrics.snapshot_in_progress},
        {"last_snapshot_success", snapshot_metrics.last_snapshot_success},
        {"last_snapshot_log_index", snapshot_metrics.last_snapshot_log_index},
        {"last_snapshot_applied_index", snapshot_metrics.last_snapshot_applied_index},
        {"last_snapshot_completed_at_ms", snapshot_metrics.last_snapshot_completed_at_ms},
        {"last_snapshot_total_ms", snapshot_metrics.last_snapshot_total_ms},
        {"max_snapshot_total_ms", snapshot_metrics.max_snapshot_total_ms},
        {"cumulative_snapshots", snapshot_metrics.cumulative_snapshots},
        {"cumulative_snapshot_failures", snapshot_metrics.cumulative_snapshot_failures},
        {"snapshot_recovery_point_lag", committed_idx >= snapshot_metrics.last_snapshot_log_index ?
            (committed_idx - snapshot_metrics.last_snapshot_log_index) : 0},
        {"snapshot_lagging_peer_count", peer_lag_metrics.lagging_peer_count},
        {"snapshot_max_peer_log_gap", peer_lag_metrics.max_peer_log_gap},
        {"snapshot_max_peer_response_age_ms", peer_lag_metrics.max_peer_response_age_ms},
        {"materialization_ready", is_materialization_ready()},
        {"startup_materialization_pending", startup_materialization_pending_.load(std::memory_order_relaxed)},
        {"materialization_lag", materialization_lag()},
        {"raft_catching_up", raft_server_ != nullptr && raft_server_->is_catching_up()},
        {"raft_receiving_snapshot", raft_server_ != nullptr && raft_server_->is_receiving_snapshot()},
    };

    if (raft_server_) {
        uint64_t last_idx = raft_server_->get_last_log_idx();
        status["last_index"] = last_idx;
        status["committed_index"] = committed_idx;
        status["log_store_start_index"] = raft_server_->get_log_store() != nullptr ?
            raft_server_->get_log_store()->start_index() : 0;
        // Report state_machine_applied_index as known_applied_index. This reflects
        // what the KV sink has actually applied via NuRaft's commit thread.
        // The live_product_state_applied_index_ tracks the background mirror
        // worker and may lag behind the KV sink briefly.
        status["known_applied_index"] = state_machine_applied_index;
        status["read_caught_up"] = is_read_caught_up();
        status["live_product_applied_index"] = live_product_state_applied_index_.load(std::memory_order_relaxed);
        status["applying_index"] = 0;
        status["raft_leader_id"] = raft_server_->get_leader();
        status["raft_term"] = raft_server_->get_term();
        status["state_machine_applied_index"] = state_machine_applied_index;
        status["state_machine_caught_up"] = initialized_.load() && state_machine_applied_index >= committed_idx;
    }

    return status;
}

void NuRaftHttpRuntimeService::do_snapshot(const std::string& snapshot_path,
                                           const std::shared_ptr<http_req>& req,
                                           const std::shared_ptr<http_res>& res) {
    (void)req;
    std::string error;
    NuRaftKvStateMachineSink* snapshot_sink = materialized_state_sink_ != nullptr ? materialized_state_sink_.get() : nullptr;
    if (snapshot_sink == nullptr || raft_server_ == nullptr || raft_state_machine_ == nullptr) {
        res->set_500("NuRaft runtime materialized state sink is not initialized.");
        return;
    }

    auto raft_server = raft_server_;
    const uint64_t state_machine_index =
        raft_state_machine_ != nullptr ? raft_state_machine_->get_last_commit_index() : 0;
    const uint64_t snapshot_index =
        std::max<uint64_t>(state_machine_index,
                           raft_server != nullptr ? raft_server->get_committed_log_idx() : 0);
    if (snapshot_index == 0) {
        res->set_500("NuRaft internal snapshot creation failed.");
        return;
    }

    std::thread([raft_server]() {
        if (raft_server == nullptr) {
            return;
        }

        nuraft::raft_server::create_snapshot_options options;
        options.serialize_commit_ = true;
        const uint64_t created_index = raft_server->create_snapshot(options);
        if (created_index == 0) {
            TS_LOG(WARNING) << "NuRaft snapshot request: internal snapshot creation failed.";
        }
    }).detach();

    const uint64_t reserved_logs = options_.raft_params.reserved_log_items;
    const uint64_t expected_start_index =
        snapshot_index > reserved_logs ? (snapshot_index - reserved_logs + 1) : 1;

    const auto wait_for_compaction = [&](uint64_t timeout_ms) {
        const auto compaction_deadline = std::chrono::steady_clock::now() +
                                         std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < compaction_deadline) {
            if (raft_server != nullptr &&
                raft_server->get_last_snapshot_idx() >= snapshot_index &&
                raft_server->get_log_store() != nullptr &&
                raft_server->get_log_store()->start_index() >= expected_start_index) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    };

    const auto snapshot_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(kManualSnapshotTimeoutMs);
    while (std::chrono::steady_clock::now() < snapshot_deadline) {
        if (raft_server != nullptr && raft_server->get_last_snapshot_idx() >= snapshot_index) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (raft_server == nullptr || raft_server->get_last_snapshot_idx() < snapshot_index) {
        res->set_500("NuRaft internal snapshot creation timed out.");
        return;
    }

    if (!wait_for_compaction(2000) &&
        raft_server != nullptr &&
        raft_server->get_last_snapshot_idx() >= snapshot_index &&
        raft_server->get_log_store() != nullptr &&
        expected_start_index > 1) {
        // NuRaft's manual snapshot path created the snapshot, but the log-store
        // compaction did not become externally visible before timeout. Finish
        // the retention cut synchronously for the admin snapshot endpoint so a
        // wiped follower can immediately recover from the snapshot instead of
        // falling back to a full log replay.
        const bool compacted = raft_server->get_log_store()->compact(expected_start_index - 1);
        if (!compacted) {
            res->set_500("NuRaft snapshot compaction failed.");
            return;
        }
    }

    if (!wait_for_compaction(5000)) {
        res->set_500("NuRaft snapshot compaction did not complete before timeout.");
        return;
    }

    if (raft_server == nullptr ||
        raft_server->get_last_snapshot_idx() < snapshot_index ||
        raft_server->get_log_store() == nullptr ||
        raft_server->get_log_store()->start_index() < expected_start_index) {
        res->set_500("NuRaft snapshot compaction did not complete before timeout.");
        return;
    }

    NuRaftSnapshotCoordinator coordinator(layout_);
    NuRaftSnapshotDescriptor descriptor;
    if (!snapshot_path.empty()) {
        if (!coordinator.create_snapshot(snapshot_path, snapshot_sink, descriptor, error)) {
            res->set_500(error);
            return;
        }
    } else if (!coordinator.read_last_snapshot(descriptor, error)) {
        res->set_500(error);
        return;
    }

    TS_LOG(INFO) << "NuRaft snapshot request: internal snapshot ready at index=" << snapshot_index
                 << ", log_store_start_index="
                 << (raft_server->get_log_store() != nullptr ? raft_server->get_log_store()->start_index() : 0);

    nlohmann::json body = {
        {"success", true},
        {"snapshot_id", descriptor.snapshot_id},
        {"last_log_index", descriptor.last_log_index},
        {"last_applied_index", descriptor.last_applied_index},
    };
    res->set_body(201, body.dump());
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

    return handle_authentication(params, embedded_params_vec, body, rpath, auth_key);
}

void register_nuraft_http_runtime_routes(HttpServer* server) {
    server->get("/collections/:collection/documents/search", search_runtime_documents);
    server->post("/multi_search", post_multi_search, false, true);
    visit_nuraft_write_routes([server](const WriteRouteDefinition& route) {
        const std::string path(route.path);
        if (std::strcmp(route.http_method, "POST") == 0) {
            server->post(path, route.handler, route.async_req, route.async_res);
        } else if (std::strcmp(route.http_method, "PUT") == 0) {
            server->put(path, route.handler, route.async_req, route.async_res);
        } else if (std::strcmp(route.http_method, "PATCH") == 0) {
            server->patch(path, route.handler, route.async_req, route.async_res);
        } else if (std::strcmp(route.http_method, "DELETE") == 0) {
            server->del(path, route.handler, route.async_req, route.async_res);
        }
    });

    // Buffer one logical import request before entering the Raft write path so
    // transport chunking does not become log-entry granularity.
    server->get("/collections/:collection/documents/export", get_export_documents, false, true);
    server->get("/collections/:collection/documents/:id", get_runtime_document);
    server->get("/collections", get_runtime_collections);
    server->get("/collections/:collection", get_runtime_collection);

    // GET handlers that read mutable state serve the latest locally applied
    // product state by default. Opt-in strong reads (`read_consistency=strong`)
    // retain the old wait-for-live-state behavior on the thread pool rather
    // than on the h2o event loop. Write handlers still use
    // wait_for_applied_index() for predecessor ordering and per-entry mirrored
    // results for their own response.
    server->get("/aliases", synced_read_handler<get_aliases>);
    server->get("/aliases/:alias", synced_read_handler<get_alias>);

    server->get("/keys", synced_read_handler<get_keys>);
    server->get("/keys/:id", synced_read_handler<get_key>);

    server->get("/presets", synced_read_handler<get_presets>);
    server->get("/presets/:name", synced_read_handler<get_preset>);

    server->get("/stopwords", synced_read_handler<get_stopwords>);
    server->get("/stopwords/:name", synced_read_handler<get_stopword>);

    server->get("/synonym_sets", synced_read_handler<get_synonym_sets>);
    server->get("/synonym_sets/:name", synced_read_handler<get_synonym_set>);
    server->get("/synonym_sets/:name/items", synced_read_handler<get_synonym_set_items>);
    server->get("/synonym_sets/:name/items/:id", synced_read_handler<get_synonym_set_item>);

    server->get("/curation_sets", synced_read_handler<get_curation_sets>);
    server->get("/curation_sets/:name", synced_read_handler<get_curation_set>);
    server->get("/curation_sets/:name/items", synced_read_handler<get_curation_set_items>);
    server->get("/curation_sets/:name/items/:id", synced_read_handler<get_curation_set_item>);

    server->get("/analytics/rules", synced_read_handler<get_analytics_rules>);
    server->get("/analytics/rules/:name", synced_read_handler<get_analytics_rule>);
    server->get("/analytics/events", synced_read_handler<get_analytics_events>);
    server->get("/analytics/status", synced_read_handler<get_analytics_status>);

    server->get("/stemming/dictionaries", synced_read_handler<get_stemming_dictionaries>);
    server->get("/stemming/dictionaries/:id", synced_read_handler<get_stemming_dictionary>);

    server->get("/metrics.json", get_metrics_json);
    server->get("/stats.json", get_stats_json);
    server->get("/debug", get_debug);
    server->get("/health", get_health);
    server->get("/health_with_rusage", get_health_with_resource_usage);
    server->get("/status", get_status);

    server->get("/operations/schema_changes", get_schema_changes);

    server->get("/conversations/models", get_conversation_models);
    server->get("/conversations/models/:id", get_conversation_model);

    server->get("/personalization/models", get_personalization_models);
    server->get("/personalization/models/:id", get_personalization_model);

    server->get("/limits", get_rate_limits);
    server->get("/limits/active", get_active_throttles);
    server->get("/limits/exceeds", get_limit_exceed_counts);
    server->get("/limits/:id", get_rate_limit);

    server->get("/nl_search_models", get_nl_search_models);
    server->get("/nl_search_models/:id", get_nl_search_model);
}

// --- Real NuRaft consensus integration ---

bool NuRaftHttpRuntimeService::initialize_raft_server(std::string& error) {
    // Create state machine with a post-commit callback that queues each
    // committed request for the background mirror worker.
    raft_state_machine_ = nuraft::cs_new<TypesenseStateMachine>(
        layout_,
        materialized_state_sink_.get(),
        [this](uint64_t log_idx, const NuRaftAppliedRequest& request) {
            (void)log_idx;
            refresh_startup_materialization_state();
            enqueue_mirrored_request(request);
        },
        [this](uint64_t log_idx, const NuRaftSnapshotDescriptor& descriptor) {
            handle_applied_snapshot(log_idx, descriptor);
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
    // Track each peer's state machine commit index. Enables the leader to know
    // exactly how caught up each follower is, improving read consistency checks.
    params.track_peers_sm_commit_idx_ = true;

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
    stop_snapshot_scheduler();
    if (raft_launcher_) {
        raft_launcher_->shutdown(5);
    }
    stop_mirror_worker();
    raft_server_.reset();
    raft_state_machine_.reset();
    raft_state_manager_.reset();
    raft_launcher_.reset();
}
