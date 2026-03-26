#include "nuraft/nuraft_state_machine_sink.h"

#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/utilities/checkpoint.h>
#include <yyjson.h>

#include "json.hpp"
#include "string_utils.h"

namespace {

std::mutex& materialized_db_registry_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, std::weak_ptr<rocksdb::DB>>& materialized_db_registry() {
    static std::unordered_map<std::string, std::weak_ptr<rocksdb::DB>> registry;
    return registry;
}

constexpr const char* kAppliedPrefix = "applied/";
constexpr const char* kStatePrefix = "state/";

std::string zero_padded_index(uint64_t index) {
    std::string value = std::to_string(index);
    if (value.size() < 20) {
        value.insert(0, 20 - value.size(), '0');
    }
    return value;
}

std::string applied_key(uint64_t index) {
    return std::string(kAppliedPrefix) + zero_padded_index(index);
}

std::string collection_key(const std::string& collection) {
    return std::string(kStatePrefix) + "collections/" + collection;
}

std::string document_key(const std::string& collection, const std::string& document_id) {
    return std::string(kStatePrefix) + "documents/" + collection + "/" + document_id;
}

std::string document_prefix(const std::string& collection) {
    return std::string(kStatePrefix) + "documents/" + collection + "/";
}

std::string import_request_id(const NuRaftAppliedRequest& request) {
    return zero_padded_index(request.start_ts != 0 ? request.start_ts : request.index);
}

std::string import_summary_key(const std::string& collection, const std::string& request_id) {
    return std::string(kStatePrefix) + "imports/" + collection + "/" + request_id;
}

std::string import_prefix(const std::string& collection) {
    return std::string(kStatePrefix) + "imports/" + collection + "/";
}

std::string import_buffer_key(const std::string& collection, const std::string& request_id) {
    return std::string(kStatePrefix) + "import_buffers/" + collection + "/" + request_id;
}

std::string import_buffer_prefix(const std::string& collection) {
    return std::string(kStatePrefix) + "import_buffers/" + collection + "/";
}

std::string delete_marker_key(const std::string& collection, uint64_t index) {
    return std::string(kStatePrefix) + "deletes/" + collection + "/" + zero_padded_index(index);
}

std::string delete_prefix_for_collection(const std::string& collection) {
    return std::string(kStatePrefix) + "deletes/" + collection + "/";
}

bool parse_json_body(const std::string& body, nlohmann::json& parsed, std::string& error) {
    try {
        parsed = nlohmann::json::parse(body);
        return true;
    } catch (const std::exception& e) {
        error = std::string("Failed to parse NuRaft request body: ") + e.what();
        return false;
    }
}

bool parse_json_object_and_extract_id(std::string_view body,
                                      std::string* document_id,
                                      std::string& error) {
    yyjson_read_err read_error{};
    yyjson_doc* document = yyjson_read_opts(const_cast<char*>(body.data()), body.size(), 0, nullptr, &read_error);
    if (document == nullptr) {
        error = std::string("Failed to parse NuRaft request body: ") +
                (read_error.msg != nullptr ? read_error.msg : "unknown yyjson parse error");
        return false;
    }

    yyjson_val* root = yyjson_doc_get_root(document);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(document);
        error = "NuRaft materialized import replay needs each line to be a complete JSON object";
        return false;
    }

    if (document_id != nullptr) {
        yyjson_val* id_value = yyjson_obj_get(root, "id");
        if (id_value == nullptr) {
            yyjson_doc_free(document);
            error = "NuRaft materialized sink needs a document id for this route";
            return false;
        }

        if (yyjson_is_str(id_value)) {
            document_id->assign(yyjson_get_str(id_value), yyjson_get_len(id_value));
        } else if (yyjson_is_sint(id_value)) {
            *document_id = std::to_string(yyjson_get_sint(id_value));
        } else if (yyjson_is_uint(id_value)) {
            *document_id = std::to_string(yyjson_get_uint(id_value));
        } else {
            yyjson_doc_free(document);
            error = "NuRaft materialized sink only supports string or integer document ids";
            return false;
        }
    }

    yyjson_doc_free(document);
    error.clear();
    return true;
}

bool resolve_collection_name(const NuRaftAppliedRequest& request,
                             std::string& collection,
                             std::string& error) {
    const auto it = request.params.find("collection");
    if (it != request.params.end() && !it->second.empty()) {
        collection = it->second;
        error.clear();
        return true;
    }

    if (request.route_kind == NuRaftRouteKind::kCollectionCreate) {
        nlohmann::json parsed;
        if (!parse_json_body(request.body, parsed, error)) {
            return false;
        }
        if (parsed.is_object() && parsed.contains("name") && parsed["name"].is_string()) {
            collection = parsed["name"].get<std::string>();
            error.clear();
            return true;
        }
    }

    error = "NuRaft materialized sink needs a collection name for this route";
    return false;
}

bool resolve_document_id(const NuRaftAppliedRequest& request,
                         std::string& document_id,
                         std::string& error) {
    const auto it = request.params.find("id");
    if (it != request.params.end() && !it->second.empty()) {
        document_id = it->second;
        error.clear();
        return true;
    }

    nlohmann::json parsed;
    if (!parse_json_body(request.body, parsed, error)) {
        return false;
    }
    if (!parsed.is_object() || !parsed.contains("id")) {
        error = "NuRaft materialized sink needs a document id for this route";
        return false;
    }

    if (parsed["id"].is_string()) {
        document_id = parsed["id"].get<std::string>();
        error.clear();
        return true;
    }
    if (parsed["id"].is_number_integer()) {
        document_id = std::to_string(parsed["id"].get<int64_t>());
        error.clear();
        return true;
    }
    if (parsed["id"].is_number_unsigned()) {
        document_id = std::to_string(parsed["id"].get<uint64_t>());
        error.clear();
        return true;
    }

    error = "NuRaft materialized sink only supports string or integer document ids";
    return false;
}

struct ImportReplaySession {
    uint64_t last_index = 0;
    uint64_t chunk_count = 0;
    uint64_t document_count = 0;
    bool complete = false;
    std::string pending_body;
};

nlohmann::json encode_import_session(const std::string& request_id,
                                    const ImportReplaySession& session) {
    return {
        {"request_id", request_id},
        {"last_index", session.last_index},
        {"chunk_count", session.chunk_count},
        {"document_count", session.document_count},
        {"pending_body_bytes", session.pending_body.size()},
        {"complete", session.complete},
    };
}

bool decode_import_session(const std::string& encoded,
                           ImportReplaySession& session,
                           std::string& error) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(encoded);
    } catch (const std::exception& e) {
        error = std::string("Failed to parse NuRaft import replay summary: ") + e.what();
        return false;
    }

    if (!parsed.is_object() ||
        !parsed.contains("last_index") || !parsed["last_index"].is_number_unsigned() ||
        !parsed.contains("chunk_count") || !parsed["chunk_count"].is_number_unsigned() ||
        !parsed.contains("document_count") || !parsed["document_count"].is_number_unsigned() ||
        !parsed.contains("complete") || !parsed["complete"].is_boolean()) {
        error = "NuRaft import replay summary is missing required fields";
        return false;
    }

    session.last_index = parsed["last_index"].get<uint64_t>();
    session.chunk_count = parsed["chunk_count"].get<uint64_t>();
    session.document_count = parsed["document_count"].get<uint64_t>();
    session.complete = parsed["complete"].get<bool>();
    session.pending_body.clear();
    error.clear();
    return true;
}

bool read_db_value(rocksdb::DB* db,
                   const std::string& key,
                   std::string& value,
                   bool& found,
                   std::string& error) {
    value.clear();
    const rocksdb::Status status = db->Get(rocksdb::ReadOptions(), key, &value);
    if (status.IsNotFound()) {
        found = false;
        error.clear();
        return true;
    }

    if (!status.ok()) {
        error = std::string("Failed to read NuRaft materialized state key '") + key + "': " +
                status.ToString();
        return false;
    }

    found = true;
    error.clear();
    return true;
}

bool read_import_session(rocksdb::DB* db,
                         const std::string& collection,
                         const std::string& request_id,
                         ImportReplaySession& session,
                         std::string& error) {
    session = ImportReplaySession();

    std::string encoded_summary;
    bool summary_found = false;
    if (!read_db_value(db, import_summary_key(collection, request_id), encoded_summary, summary_found, error)) {
        return false;
    }
    if (summary_found && !decode_import_session(encoded_summary, session, error)) {
        return false;
    }

    bool buffer_found = false;
    if (!read_db_value(db, import_buffer_key(collection, request_id), session.pending_body, buffer_found, error)) {
        return false;
    }
    if (!buffer_found) {
        session.pending_body.clear();
    }

    error.clear();
    return true;
}

bool parse_import_documents(const std::string& body,
                            bool last_chunk_aggregate,
                            std::vector<std::string>& documents,
                            std::vector<std::string>& document_ids,
                            std::string& pending_body,
                            std::string& error) {
    documents.clear();
    document_ids.clear();
    pending_body.clear();
    if (body.empty()) {
        error.clear();
        return true;
    }

    StringUtils::split(body, documents, "\n", false, false);
    if (!last_chunk_aggregate && !documents.empty()) {
        if (!parse_json_object_and_extract_id(documents.back(), nullptr, error)) {
            pending_body = documents.back();
            documents.pop_back();
            error.clear();
        }
    }

    document_ids.reserve(documents.size());
    for (const auto& document : documents) {
        std::string document_id;
        if (!parse_json_object_and_extract_id(document, &document_id, error)) {
            return false;
        }
        document_ids.emplace_back(std::move(document_id));
    }

    if (last_chunk_aggregate && !pending_body.empty()) {
        error = "NuRaft materialized import replay cannot end with a partial JSON document";
        return false;
    }

    error.clear();
    return true;
}

bool apply_import_mutation(rocksdb::DB* db,
                           rocksdb::WriteBatch& batch,
                           const NuRaftAppliedRequest& request,
                           std::map<std::string, ImportReplaySession>& import_sessions,
                           std::string& error) {
    std::string collection;
    if (!resolve_collection_name(request, collection, error)) {
        return false;
    }

    const std::string request_id = import_request_id(request);
    const std::string summary_key = import_summary_key(collection, request_id);
    const std::string buffer_key = import_buffer_key(collection, request_id);

    auto session_it = import_sessions.find(summary_key);
    if (session_it == import_sessions.end()) {
        ImportReplaySession loaded_session;
        if (!read_import_session(db, collection, request_id, loaded_session, error)) {
            return false;
        }
        session_it = import_sessions.emplace(summary_key, std::move(loaded_session)).first;
    }

    ImportReplaySession& session = session_it->second;
    const std::string combined_body = session.pending_body + request.body;
    std::vector<std::string> documents;
    std::vector<std::string> document_ids;
    std::string pending_body;
    if (!parse_import_documents(combined_body,
                                request.last_chunk_aggregate,
                                documents,
                                document_ids,
                                pending_body,
                                error)) {
        return false;
    }

    for (size_t i = 0; i < documents.size(); ++i) {
        batch.Put(document_key(collection, document_ids[i]), documents[i]);
    }

    session.last_index = request.index;
    session.chunk_count += 1;
    session.document_count += documents.size();
    session.pending_body = std::move(pending_body);
    session.complete = request.last_chunk_aggregate && session.pending_body.empty();

    if (session.pending_body.empty()) {
        batch.Delete(buffer_key);
    } else {
        batch.Put(buffer_key, session.pending_body);
    }
    batch.Put(summary_key, encode_import_session(request_id, session).dump());
    error.clear();
    return true;
}

bool delete_prefix(rocksdb::DB* db,
                   rocksdb::WriteBatch& batch,
                   const std::string& prefix,
                   std::string& error) {
    rocksdb::ReadOptions read_options;
    std::unique_ptr<rocksdb::Iterator> iterator(db->NewIterator(read_options));
    for (iterator->Seek(prefix); iterator->Valid() && iterator->key().starts_with(prefix); iterator->Next()) {
        batch.Delete(iterator->key());
    }

    if (!iterator->status().ok()) {
        error = std::string("Failed to scan NuRaft materialized state prefix: ") +
                iterator->status().ToString();
        return false;
    }

    error.clear();
    return true;
}

bool apply_materialized_mutation(rocksdb::DB* db,
                                 rocksdb::WriteBatch& batch,
                                 const NuRaftAppliedRequest& request,
                                 std::map<std::string, ImportReplaySession>& import_sessions,
                                 std::string& error) {
    std::string collection;
    std::string document_id;
    switch (request.route_kind) {
        case NuRaftRouteKind::kCollectionCreate:
            if (!resolve_collection_name(request, collection, error)) {
                return false;
            }
            batch.Put(collection_key(collection), request.body);
            break;
        case NuRaftRouteKind::kCollectionDrop:
            if (!resolve_collection_name(request, collection, error)) {
                return false;
            }
            batch.Delete(collection_key(collection));
            if (!delete_prefix(db, batch, document_prefix(collection), error) ||
                !delete_prefix(db, batch, import_prefix(collection), error) ||
                !delete_prefix(db, batch, import_buffer_prefix(collection), error) ||
                !delete_prefix(db, batch, delete_prefix_for_collection(collection), error)) {
                return false;
            }
            break;
        case NuRaftRouteKind::kDocumentWrite:
            if (!resolve_collection_name(request, collection, error) ||
                !resolve_document_id(request, document_id, error)) {
                return false;
            }
            if (request.metadata == "PATCH") {
                std::string existing_document;
                bool found = false;
                if (!read_db_value(db, document_key(collection, document_id), existing_document, found, error)) {
                    return false;
                }

                nlohmann::json patch_document;
                if (!parse_json_body(request.body, patch_document, error) || !patch_document.is_object()) {
                    error = "NuRaft materialized sink needs PATCH bodies to be JSON objects";
                    return false;
                }

                nlohmann::json merged_document = found ? nlohmann::json::parse(existing_document) : nlohmann::json::object();
                if (!merged_document.is_object()) {
                    error = "NuRaft materialized sink can only PATCH JSON object documents";
                    return false;
                }

                for (auto it = patch_document.begin(); it != patch_document.end(); ++it) {
                    merged_document[it.key()] = it.value();
                }
                batch.Put(document_key(collection, document_id), merged_document.dump());
            } else {
                batch.Put(document_key(collection, document_id), request.body);
            }
            break;
        case NuRaftRouteKind::kDocumentDelete:
            if (!resolve_collection_name(request, collection, error)) {
                return false;
            }
            if (resolve_document_id(request, document_id, error)) {
                batch.Delete(document_key(collection, document_id));
                error.clear();
            } else {
                batch.Put(delete_marker_key(collection, request.index), request.body);
                error.clear();
            }
            break;
        case NuRaftRouteKind::kDocumentImport:
            return apply_import_mutation(db, batch, request, import_sessions, error);
        case NuRaftRouteKind::kUnknown:
        default:
            error.clear();
            break;
    }

    return true;
}

bool read_prefix_entries(rocksdb::DB* db,
                         const std::string& prefix,
                         std::vector<std::pair<std::string, std::string>>& entries,
                         std::string& error) {
    entries.clear();
    rocksdb::ReadOptions read_options;
    std::unique_ptr<rocksdb::Iterator> iterator(db->NewIterator(read_options));
    for (iterator->Seek(prefix); iterator->Valid() && iterator->key().starts_with(prefix); iterator->Next()) {
        entries.emplace_back(iterator->key().ToString(), iterator->value().ToString());
    }

    if (!iterator->status().ok()) {
        error = std::string("Failed to scan NuRaft materialized state: ") + iterator->status().ToString();
        return false;
    }

    error.clear();
    return true;
}

bool count_prefix_entries(rocksdb::DB* db,
                          const std::string& prefix,
                          size_t& count,
                          std::string& error) {
    count = 0;
    rocksdb::ReadOptions read_options;
    std::unique_ptr<rocksdb::Iterator> iterator(db->NewIterator(read_options));
    for (iterator->Seek(prefix); iterator->Valid() && iterator->key().starts_with(prefix); iterator->Next()) {
        ++count;
    }

    if (!iterator->status().ok()) {
        error = std::string("Failed to count NuRaft materialized state prefix: ") +
                iterator->status().ToString();
        return false;
    }

    error.clear();
    return true;
}

}  // namespace

NuRaftFileBackedStateMachineSink::NuRaftFileBackedStateMachineSink(NuRaftStateLayout layout)
    : store_(std::move(layout)) {}

bool NuRaftFileBackedStateMachineSink::apply_all(const std::vector<NuRaftAppliedRequest>& requests,
                                                 std::string& error) {
    return store_.append_all(requests, error);
}

bool NuRaftFileBackedStateMachineSink::read_all(std::vector<NuRaftAppliedRequest>& requests,
                                                std::string& error) const {
    return store_.read_all(requests, error);
}

NuRaftKvStateMachineSink::NuRaftKvStateMachineSink(NuRaftStateLayout layout)
    : layout_(std::move(layout)) {}

NuRaftKvStateMachineSink::~NuRaftKvStateMachineSink() = default;

bool NuRaftKvStateMachineSink::initialize_db(std::string& error) const {
    if (db_ != nullptr) {
        error.clear();
        return true;
    }

    std::lock_guard<std::mutex> lock(materialized_db_registry_mutex());
    if (db_ != nullptr) {
        error.clear();
        return true;
    }

    try {
        std::filesystem::create_directories(layout_.materialized_state_dir);
    } catch (const std::exception& e) {
        error = std::string("Failed to create NuRaft materialized state directory: ") + e.what();
        return false;
    }

    auto& registry = materialized_db_registry();
    if (const auto existing = registry[layout_.materialized_state_dir].lock()) {
        db_ = existing;
        error.clear();
        return true;
    }

    rocksdb::Options options;
    options.create_if_missing = true;
    options.OptimizeForPointLookup(64);
    rocksdb::BlockBasedTableOptions table_options;
    table_options.cache_index_and_filter_blocks = true;
    table_options.pin_l0_filter_and_index_blocks_in_cache = true;
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
    rocksdb::DB* db = nullptr;
    const rocksdb::Status status = rocksdb::DB::Open(options, layout_.materialized_state_dir, &db);
    if (!status.ok()) {
        error = std::string("Failed to open NuRaft materialized state DB: ") + status.ToString();
        return false;
    }

    db_ = std::shared_ptr<rocksdb::DB>(db, [](rocksdb::DB* ptr) { delete ptr; });
    registry[layout_.materialized_state_dir] = db_;
    error.clear();
    return true;
}

bool NuRaftKvStateMachineSink::apply_all(const std::vector<NuRaftAppliedRequest>& requests,
                                         std::string& error) {
    if (!initialize_db(error)) {
        return false;
    }

    rocksdb::WriteBatch batch;
    std::map<std::string, ImportReplaySession> import_sessions;
    for (const auto& request : requests) {
        batch.Put(applied_key(request.index), request.encode_binary());
        if (!apply_materialized_mutation(db_.get(), batch, request, import_sessions, error)) {
            return false;
        }
    }

    const rocksdb::Status status = db_->Write(rocksdb::WriteOptions(), &batch);
    if (!status.ok()) {
        error = std::string("Failed to write NuRaft materialized state batch: ") + status.ToString();
        return false;
    }

    error.clear();
    return true;
}

bool NuRaftKvStateMachineSink::read_last_applied_index(uint64_t& last_applied_index,
                                                       std::string& error) const {
    last_applied_index = 0;
    if (!initialize_db(error)) {
        return false;
    }

    std::string upper_bound = kAppliedPrefix;
    if (!upper_bound.empty()) {
        upper_bound.back() = static_cast<char>(upper_bound.back() + 1);
    }

    std::unique_ptr<rocksdb::Iterator> iterator(db_->NewIterator(rocksdb::ReadOptions()));
    iterator->SeekForPrev(upper_bound);
    if (!iterator->status().ok()) {
        error = std::string("Failed to seek NuRaft applied-request prefix: ") + iterator->status().ToString();
        return false;
    }

    if (!iterator->Valid()) {
        error.clear();
        return true;
    }

    const std::string key = iterator->key().ToString();
    if (key.rfind(kAppliedPrefix, 0) != 0) {
        error.clear();
        return true;
    }

    try {
        last_applied_index = std::stoull(key.substr(std::strlen(kAppliedPrefix)));
    } catch (const std::exception& e) {
        error = std::string("Failed to parse NuRaft applied-request key '") + key + "': " + e.what();
        return false;
    }

    error.clear();
    return true;
}

bool NuRaftKvStateMachineSink::read_all(std::vector<NuRaftAppliedRequest>& requests,
                                        std::string& error) const {
    requests.clear();
    if (!initialize_db(error)) {
        return false;
    }

    std::vector<std::pair<std::string, std::string>> entries;
    if (!read_prefix_entries(db_.get(), kAppliedPrefix, entries, error)) {
        return false;
    }

    for (const auto& entry : entries) {
        NuRaftAppliedRequest request;
        if (!NuRaftAppliedRequest::decode(entry.second, request, error)) {
            return false;
        }
        requests.push_back(std::move(request));
    }

    error.clear();
    return true;
}

bool NuRaftKvStateMachineSink::read_all_after(uint64_t after_index,
                                               std::vector<NuRaftAppliedRequest>& requests,
                                               std::string& error) const {
    requests.clear();
    if (!initialize_db(error)) {
        return false;
    }

    // Seek directly past after_index so already-applied entries are never read from disk.
    const std::string start_key = applied_key(after_index + 1);
    rocksdb::ReadOptions read_options;
    std::unique_ptr<rocksdb::Iterator> iterator(db_->NewIterator(read_options));
    for (iterator->Seek(start_key);
         iterator->Valid() && iterator->key().starts_with(kAppliedPrefix);
         iterator->Next()) {
        NuRaftAppliedRequest request;
        if (!NuRaftAppliedRequest::decode(iterator->value().ToString(), request, error)) {
            return false;
        }
        requests.push_back(std::move(request));
    }

    if (!iterator->status().ok()) {
        error = std::string("Failed to scan NuRaft materialized state after index ") +
                std::to_string(after_index) + ": " + iterator->status().ToString();
        return false;
    }

    error.clear();
    return true;
}

bool NuRaftKvStateMachineSink::create_checkpoint(const std::string& checkpoint_path,
                                                 std::string& error) const {
    if (!initialize_db(error)) {
        return false;
    }

    std::error_code ec;
    std::filesystem::remove_all(checkpoint_path, ec);
    ec.clear();
    std::filesystem::create_directories(std::filesystem::path(checkpoint_path).parent_path(), ec);
    if (ec) {
        error = std::string("Failed to prepare NuRaft checkpoint parent dir: ") + ec.message();
        return false;
    }

    rocksdb::Checkpoint* checkpoint = nullptr;
    rocksdb::Status status = rocksdb::Checkpoint::Create(db_.get(), &checkpoint);
    std::unique_ptr<rocksdb::Checkpoint> checkpoint_guard(checkpoint);
    if (!status.ok()) {
        error = std::string("Failed to create NuRaft materialized-state checkpoint handle: ") + status.ToString();
        return false;
    }

    status = checkpoint_guard->CreateCheckpoint(checkpoint_path);
    if (!status.ok()) {
        error = std::string("Failed to create NuRaft materialized-state checkpoint: ") + status.ToString();
        return false;
    }

    error.clear();
    return true;
}

bool NuRaftKvStateMachineSink::read_materialized_value(const std::string& key,
                                                       std::string& value,
                                                       bool& found,
                                                       std::string& error) const {
    if (!initialize_db(error)) {
        return false;
    }

    return read_db_value(db_.get(), key, value, found, error);
}

bool NuRaftKvStateMachineSink::read_materialized_prefix(
    const std::string& prefix,
    std::vector<std::pair<std::string, std::string>>& entries,
    std::string& error) const {
    if (!initialize_db(error)) {
        return false;
    }

    return read_prefix_entries(db_.get(), prefix, entries, error);
}

bool NuRaftKvStateMachineSink::count_materialized_prefix(const std::string& prefix,
                                                         size_t& count,
                                                         std::string& error) const {
    if (!initialize_db(error)) {
        return false;
    }

    return count_prefix_entries(db_.get(), prefix, count, error);
}

bool NuRaftKvStateMachineSink::read_materialized_entries(
    std::vector<std::pair<std::string, std::string>>& entries,
    std::string& error) const {
    if (!initialize_db(error)) {
        return false;
    }

    return read_prefix_entries(db_.get(), kStatePrefix, entries, error);
}
