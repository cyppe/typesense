#include "nuraft/nuraft_metadata_store.h"

#include "json.hpp"

bool NuRaftIdentity::operator==(const NuRaftIdentity& other) const {
    return format_version == other.format_version &&
           server_id == other.server_id &&
           peer_endpoint == other.peer_endpoint &&
           api_port == other.api_port;
}

NuRaftMetadataStore::NuRaftMetadataStore(NuRaftStateLayout layout)
    : layout_(std::move(layout)) {}

const NuRaftStateLayout& NuRaftMetadataStore::layout() const {
    return layout_;
}

bool NuRaftMetadataStore::initialize(std::string& error) const {
    return NuRaftFileStore::ensure_layout(layout_, error);
}

bool NuRaftMetadataStore::write_identity(const NuRaftIdentity& identity, std::string& error) const {
    nlohmann::json encoded = {
        {"format_version", identity.format_version},
        {"server_id", identity.server_id},
        {"peer_endpoint", identity.peer_endpoint},
        {"api_port", identity.api_port},
    };
    return NuRaftFileStore::write_file_atomically(layout_.identity_file, encoded.dump(), error);
}

bool NuRaftMetadataStore::read_identity(NuRaftIdentity& identity, std::string& error) const {
    std::string encoded;
    if (!NuRaftFileStore::read_file(layout_.identity_file, encoded, error)) {
        return false;
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(encoded);
    } catch (const std::exception& e) {
        error = std::string("Failed to parse NuRaft identity metadata: ") + e.what();
        return false;
    }

    if (!parsed.contains("format_version") || !parsed["format_version"].is_number_unsigned() ||
        !parsed.contains("server_id") || !parsed["server_id"].is_number_integer() ||
        !parsed.contains("peer_endpoint") || !parsed["peer_endpoint"].is_string() ||
        !parsed.contains("api_port") || !parsed["api_port"].is_number_integer()) {
        error = "NuRaft identity metadata is missing required fields";
        return false;
    }

    identity.format_version = parsed["format_version"].get<uint32_t>();
    identity.server_id = parsed["server_id"].get<int32_t>();
    identity.peer_endpoint = parsed["peer_endpoint"].get<std::string>();
    identity.api_port = parsed["api_port"].get<int32_t>();
    error.clear();
    return true;
}
