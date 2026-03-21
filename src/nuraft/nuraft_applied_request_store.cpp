#include "nuraft/nuraft_applied_request_store.h"

#include <filesystem>

#include "json.hpp"
#include "nuraft/nuraft_file_store.h"
#include "string_utils.h"

bool NuRaftLogEntry::operator==(const NuRaftLogEntry& other) const {
    return index == other.index &&
           envelope.version() == other.envelope.version() &&
           envelope.payload_encoding() == other.envelope.payload_encoding() &&
           envelope.flags() == other.envelope.flags() &&
           envelope.request_json() == other.envelope.request_json();
}

namespace {

nlohmann::json encode_request(const NuRaftAppliedRequest& request);
bool decode_request(const nlohmann::json& encoded,
                    NuRaftAppliedRequest& request,
                    std::string& error);
std::string encode_binary_request(const NuRaftAppliedRequest& request);
bool decode_binary_request(std::string_view encoded,
                           NuRaftAppliedRequest& request,
                           std::string& error);

constexpr uint32_t kBinaryAppliedRequestMagic = 0x54534152;  // TSAR
constexpr uint16_t kBinaryAppliedRequestVersion = 1;

template <typename T>
void append_le(std::string& out, T value) {
    using UnsignedT = typename std::make_unsigned<T>::type;
    const UnsignedT converted = static_cast<UnsignedT>(value);
    for (size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<char>((converted >> (i * 8)) & 0xff));
    }
}

template <typename T>
bool read_le(std::string_view bytes, size_t& offset, T& value) {
    if (offset + sizeof(T) > bytes.size()) {
        return false;
    }

    using UnsignedT = typename std::make_unsigned<T>::type;
    UnsignedT converted = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        converted |= static_cast<UnsignedT>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8);
    }

    value = static_cast<T>(converted);
    offset += sizeof(T);
    return true;
}

void append_string(std::string& out, std::string_view value) {
    append_le<uint64_t>(out, value.size());
    out.append(value.data(), value.size());
}

bool read_string(std::string_view bytes, size_t& offset, std::string& value) {
    uint64_t size = 0;
    if (!read_le<uint64_t>(bytes, offset, size) || offset + size > bytes.size()) {
        return false;
    }

    value.assign(bytes.data() + offset, size);
    offset += size;
    return true;
}

void append_bool(std::string& out, bool value) {
    out.push_back(value ? '\1' : '\0');
}

bool read_bool(std::string_view bytes, size_t& offset, bool& value) {
    if (offset >= bytes.size()) {
        return false;
    }

    value = bytes[offset] != '\0';
    ++offset;
    return true;
}

}  // namespace

bool NuRaftAppliedRequest::operator==(const NuRaftAppliedRequest& other) const {
    return index == other.index &&
           route_hash == other.route_hash &&
           route_kind == other.route_kind &&
           params == other.params &&
           metadata == other.metadata &&
           body == other.body &&
           first_chunk_aggregate == other.first_chunk_aggregate &&
           last_chunk_aggregate == other.last_chunk_aggregate &&
           start_ts == other.start_ts &&
           log_index == other.log_index &&
           is_binary_body == other.is_binary_body;
}

std::string NuRaftAppliedRequest::encode() const {
    return encode_request(*this).dump();
}

std::string NuRaftAppliedRequest::encode_binary() const {
    return encode_binary_request(*this);
}

bool NuRaftAppliedRequest::from_log_entry(const NuRaftLogEntry& entry,
                                          NuRaftAppliedRequest& applied_request,
                                          std::string& error) {
    if (entry.envelope.payload_encoding() == NuRaftRequestEnvelope::kAppliedRequestBinaryEncoding) {
        if (!decode_binary_request(entry.envelope.payload(), applied_request, error)) {
            return false;
        }
        applied_request.index = entry.index;
        applied_request.route_kind = NuRaftRouteClassifier::classify(applied_request.route_hash);
        error.clear();
        return true;
    }

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
        applied_request.route_kind = NuRaftRouteClassifier::classify(applied_request.route_hash);
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

bool NuRaftAppliedRequest::decode(const std::string& encoded,
                                  NuRaftAppliedRequest& applied_request,
                                  std::string& error) {
    if (decode_binary(encoded, applied_request, error)) {
        return true;
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(encoded);
    } catch (const std::exception& e) {
        error = std::string("Failed to parse NuRaft applied request record: ") + e.what();
        return false;
    }

    return decode_request(parsed, applied_request, error);
}

bool NuRaftAppliedRequest::decode_binary(std::string_view encoded,
                                         NuRaftAppliedRequest& applied_request,
                                         std::string& error) {
    return decode_binary_request(encoded, applied_request, error);
}

namespace {

nlohmann::json encode_request(const NuRaftAppliedRequest& request) {
    return {
        {"index", request.index},
        {"route_hash", request.route_hash},
        {"route_kind", NuRaftRouteClassifier::kind_name(request.route_kind)},
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

std::string encode_binary_request(const NuRaftAppliedRequest& request) {
    std::string encoded;
    encoded.reserve(sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint64_t) * 5 + sizeof(int64_t) +
                    request.metadata.size() + request.body.size() + request.params.size() * 24);

    append_le<uint32_t>(encoded, kBinaryAppliedRequestMagic);
    append_le<uint16_t>(encoded, kBinaryAppliedRequestVersion);
    append_le<uint64_t>(encoded, request.index);
    append_le<uint64_t>(encoded, request.route_hash);
    append_le<uint32_t>(encoded, request.params.size());
    for (const auto& [key, value] : request.params) {
        append_string(encoded, key);
        append_string(encoded, value);
    }
    append_string(encoded, request.metadata);
    append_string(encoded, request.body);
    append_bool(encoded, request.first_chunk_aggregate);
    append_bool(encoded, request.last_chunk_aggregate);
    append_le<uint64_t>(encoded, request.start_ts);
    append_le<int64_t>(encoded, request.log_index);
    append_bool(encoded, request.is_binary_body);
    return encoded;
}

bool decode_binary_request(std::string_view encoded,
                           NuRaftAppliedRequest& request,
                           std::string& error) {
    size_t offset = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    if (!read_le<uint32_t>(encoded, offset, magic) ||
        !read_le<uint16_t>(encoded, offset, version)) {
        error = "NuRaft applied request binary payload is truncated.";
        return false;
    }

    if (magic != kBinaryAppliedRequestMagic) {
        error = "NuRaft applied request binary payload has an invalid magic.";
        return false;
    }

    if (version != kBinaryAppliedRequestVersion) {
        error = "NuRaft applied request binary payload version is unsupported.";
        return false;
    }

    request = {};
    uint32_t params_count = 0;
    if (!read_le<uint64_t>(encoded, offset, request.index) ||
        !read_le<uint64_t>(encoded, offset, request.route_hash) ||
        !read_le<uint32_t>(encoded, offset, params_count)) {
        error = "NuRaft applied request binary payload is missing fixed fields.";
        return false;
    }

    for (uint32_t i = 0; i < params_count; ++i) {
        std::string key;
        std::string value;
        if (!read_string(encoded, offset, key) || !read_string(encoded, offset, value)) {
            error = "NuRaft applied request binary payload has a truncated params section.";
            return false;
        }
        request.params.emplace(std::move(key), std::move(value));
    }

    if (!read_string(encoded, offset, request.metadata) ||
        !read_string(encoded, offset, request.body) ||
        !read_bool(encoded, offset, request.first_chunk_aggregate) ||
        !read_bool(encoded, offset, request.last_chunk_aggregate) ||
        !read_le<uint64_t>(encoded, offset, request.start_ts) ||
        !read_le<int64_t>(encoded, offset, request.log_index) ||
        !read_bool(encoded, offset, request.is_binary_body)) {
        error = "NuRaft applied request binary payload is truncated in the tail section.";
        return false;
    }

    if (offset != encoded.size()) {
        error = "NuRaft applied request binary payload has unexpected trailing bytes.";
        return false;
    }

    request.route_kind = NuRaftRouteClassifier::classify(request.route_hash);
    error.clear();
    return true;
}

bool decode_request(const nlohmann::json& encoded,
                    NuRaftAppliedRequest& request,
                    std::string& error) {
    if (!encoded.is_object() ||
        !encoded.contains("index") || !encoded["index"].is_number_unsigned() ||
        !encoded.contains("route_hash") || !encoded["route_hash"].is_number_unsigned() ||
        !encoded.contains("route_kind") || !encoded["route_kind"].is_string() ||
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
    request.route_kind = NuRaftRouteClassifier::classify(request.route_hash);
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
        existing_encoded.append(request.encode());
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
        NuRaftAppliedRequest request;
        if (!NuRaftAppliedRequest::decode(line, request, error)) {
            return false;
        }
        requests.push_back(std::move(request));
    }

    error.clear();
    return true;
}
