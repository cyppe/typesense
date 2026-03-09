#include "nuraft/nuraft_state_machine_sink.h"

#include <filesystem>
#include <memory>
#include <utility>

#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include "json.hpp"

namespace {

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

std::string import_key(const std::string& collection, uint64_t index) {
    return std::string(kStatePrefix) + "imports/" + collection + "/" + zero_padded_index(index);
}

std::string import_prefix(const std::string& collection) {
    return std::string(kStatePrefix) + "imports/" + collection + "/";
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
                !delete_prefix(db, batch, delete_prefix_for_collection(collection), error)) {
                return false;
            }
            break;
        case NuRaftRouteKind::kDocumentWrite:
            if (!resolve_collection_name(request, collection, error) ||
                !resolve_document_id(request, document_id, error)) {
                return false;
            }
            batch.Put(document_key(collection, document_id), request.body);
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
            if (!resolve_collection_name(request, collection, error)) {
                return false;
            }
            batch.Put(import_key(collection, request.index), request.body);
            break;
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

    try {
        std::filesystem::create_directories(layout_.materialized_state_dir);
    } catch (const std::exception& e) {
        error = std::string("Failed to create NuRaft materialized state directory: ") + e.what();
        return false;
    }

    rocksdb::Options options;
    options.create_if_missing = true;
    rocksdb::DB* db = nullptr;
    const rocksdb::Status status = rocksdb::DB::Open(options, layout_.materialized_state_dir, &db);
    if (!status.ok()) {
        error = std::string("Failed to open NuRaft materialized state DB: ") + status.ToString();
        return false;
    }

    db_.reset(db);
    error.clear();
    return true;
}

bool NuRaftKvStateMachineSink::apply_all(const std::vector<NuRaftAppliedRequest>& requests,
                                         std::string& error) {
    if (!initialize_db(error)) {
        return false;
    }

    rocksdb::WriteBatch batch;
    for (const auto& request : requests) {
        batch.Put(applied_key(request.index), request.encode());
        if (!apply_materialized_mutation(db_.get(), batch, request, error)) {
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

bool NuRaftKvStateMachineSink::read_materialized_entries(
    std::vector<std::pair<std::string, std::string>>& entries,
    std::string& error) const {
    if (!initialize_db(error)) {
        return false;
    }

    return read_prefix_entries(db_.get(), kStatePrefix, entries, error);
}
