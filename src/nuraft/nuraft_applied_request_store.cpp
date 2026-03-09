#include "nuraft/nuraft_applied_request_store.h"

#include <filesystem>

#include "json.hpp"
#include "nuraft/nuraft_file_store.h"
#include "string_utils.h"

bool NuRaftAppliedRequest::operator==(const NuRaftAppliedRequest& other) const {
    return index == other.index &&
           route_hash == other.route_hash &&
           params == other.params &&
           metadata == other.metadata &&
           body == other.body &&
           first_chunk_aggregate == other.first_chunk_aggregate &&
           last_chunk_aggregate == other.last_chunk_aggregate &&
           start_ts == other.start_ts &&
           log_index == other.log_index &&
           is_binary_body == other.is_binary_body;
}

bool NuRaftAppliedRequest::from_log_entry(const NuRaftLogEntry& entry,
                                          NuRaftAppliedRequest& applied_request,
                                          std::string& error) {
    try {
        const nlohmann::json request = nlohmann::json::parse(entry.envelope.request_json());
        if (!request.contains("route_hash") || !request["route_hash"].is_number_unsigned() ||
            !request.contains("params") || !request["params"].is_object() ||
            !request.contains("body") || !request["body"].is_string()) {
            error = "NuRaft request envelope payload is missing required request fields";
            return false;
        }

        applied_request.index = entry.index;
        applied_request.route_hash = request["route_hash"].get<uint64_t>();
        applied_request.params = request["params"].get<std::map<std::string, std::string>>();
        applied_request.metadata = request.contains("metadata") ? request["metadata"].get<std::string>() : "";
        applied_request.body = request["body"].get<std::string>();
        applied_request.first_chunk_aggregate = request.contains("first_chunk_aggregate") ?
                                                request["first_chunk_aggregate"].get<bool>() : true;
        applied_request.last_chunk_aggregate = request.contains("last_chunk_aggregate") ?
                                               request["last_chunk_aggregate"].get<bool>() : false;
        applied_request.start_ts = request.contains("start_ts") ? request["start_ts"].get<uint64_t>() : 0;
        applied_request.log_index = request.contains("log_index") ? request["log_index"].get<int64_t>() : 0;
        applied_request.is_binary_body = request.contains("is_binary_body") ?
                                         request["is_binary_body"].get<bool>() : false;
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = std::string("Failed to decode NuRaft applied request: ") + e.what();
        return false;
    }
}

namespace {

nlohmann::json encode_request(const NuRaftAppliedRequest& request) {
    return {
        {"index", request.index},
        {"route_hash", request.route_hash},
        {"params", request.params},
        {"metadata", request.metadata},
        {"body", request.body},
        {"first_chunk_aggregate", request.first_chunk_aggregate},
        {"last_chunk_aggregate", request.last_chunk_aggregate},
        {"start_ts", request.start_ts},
        {"log_index", request.log_index},
        {"is_binary_body", request.is_binary_body},
    };
}

bool decode_request(const nlohmann::json& encoded,
                    NuRaftAppliedRequest& request,
                    std::string& error) {
    if (!encoded.is_object() ||
        !encoded.contains("index") || !encoded["index"].is_number_unsigned() ||
        !encoded.contains("route_hash") || !encoded["route_hash"].is_number_unsigned() ||
        !encoded.contains("params") || !encoded["params"].is_object() ||
        !encoded.contains("metadata") || !encoded["metadata"].is_string() ||
        !encoded.contains("body") || !encoded["body"].is_string() ||
        !encoded.contains("first_chunk_aggregate") || !encoded["first_chunk_aggregate"].is_boolean() ||
        !encoded.contains("last_chunk_aggregate") || !encoded["last_chunk_aggregate"].is_boolean() ||
        !encoded.contains("start_ts") || !encoded["start_ts"].is_number_unsigned() ||
        !encoded.contains("log_index") || !encoded["log_index"].is_number_integer() ||
        !encoded.contains("is_binary_body") || !encoded["is_binary_body"].is_boolean()) {
        error = "NuRaft applied request record is missing required fields";
        return false;
    }

    request.index = encoded["index"].get<uint64_t>();
    request.route_hash = encoded["route_hash"].get<uint64_t>();
    request.params = encoded["params"].get<std::map<std::string, std::string>>();
    request.metadata = encoded["metadata"].get<std::string>();
    request.body = encoded["body"].get<std::string>();
    request.first_chunk_aggregate = encoded["first_chunk_aggregate"].get<bool>();
    request.last_chunk_aggregate = encoded["last_chunk_aggregate"].get<bool>();
    request.start_ts = encoded["start_ts"].get<uint64_t>();
    request.log_index = encoded["log_index"].get<int64_t>();
    request.is_binary_body = encoded["is_binary_body"].get<bool>();
    error.clear();
    return true;
}

}  // namespace

NuRaftAppliedRequestStore::NuRaftAppliedRequestStore(NuRaftStateLayout layout)
    : layout_(std::move(layout)) {}

bool NuRaftAppliedRequestStore::append_all(const std::vector<NuRaftAppliedRequest>& requests,
                                           std::string& error) const {
    std::string existing_encoded;
    if (std::filesystem::exists(layout_.applied_requests_file) &&
        !NuRaftFileStore::read_file(layout_.applied_requests_file, existing_encoded, error)) {
        return false;
    }

    for (const auto& request : requests) {
        existing_encoded.append(encode_request(request).dump());
        existing_encoded.push_back('\n');
    }

    return NuRaftFileStore::write_file_atomically(layout_.applied_requests_file, existing_encoded, error);
}

bool NuRaftAppliedRequestStore::read_all(std::vector<NuRaftAppliedRequest>& requests, std::string& error) const {
    requests.clear();
    if (!std::filesystem::exists(layout_.applied_requests_file)) {
        error.clear();
        return true;
    }

    std::string encoded;
    if (!NuRaftFileStore::read_file(layout_.applied_requests_file, encoded, error)) {
        return false;
    }

    std::vector<std::string> lines;
    StringUtils::split(encoded, lines, "\n");
    for (const auto& line : lines) {
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(line);
        } catch (const std::exception& e) {
            error = std::string("Failed to parse NuRaft applied request record: ") + e.what();
            return false;
        }

        NuRaftAppliedRequest request;
        if (!decode_request(parsed, request, error)) {
            return false;
        }
        requests.push_back(std::move(request));
    }

    error.clear();
    return true;
}
