#include "nuraft/nuraft_metadata_store.h"

#include <set>
#include <utility>

#include "json.hpp"

bool NuRaftIdentity::operator==(const NuRaftIdentity& other) const {
    return format_version == other.format_version &&
           server_id == other.server_id &&
           peer_endpoint == other.peer_endpoint &&
           api_port == other.api_port;
}

bool NuRaftBootstrapConfig::operator==(const NuRaftBootstrapConfig& other) const {
    return format_version == other.format_version &&
           group_id == other.group_id &&
           self == other.self &&
           peers == other.peers &&
           api_uses_ssl == other.api_uses_ssl;
}

namespace {

nlohmann::json encode_peer(const NuRaftPeerAddress& peer) {
    return {
        {"host", peer.host},
        {"peer_port", peer.peer_port},
        {"api_port", peer.api_port},
    };
}

bool decode_peer(const nlohmann::json& encoded, NuRaftPeerAddress& peer, std::string& error) {
    if (!encoded.is_object() ||
        !encoded.contains("host") || !encoded["host"].is_string() ||
        !encoded.contains("peer_port") || !encoded["peer_port"].is_number_unsigned() ||
        !encoded.contains("api_port") || !encoded["api_port"].is_number_unsigned()) {
        error = "NuRaft bootstrap metadata is missing required peer fields";
        return false;
    }

    peer.host = encoded["host"].get<std::string>();
    peer.peer_port = encoded["peer_port"].get<uint32_t>();
    peer.api_port = encoded["api_port"].get<uint32_t>();
    return true;
}

bool validate_bootstrap_config(const NuRaftBootstrapConfig& config, std::string& error) {
    if (config.group_id.empty()) {
        error = "NuRaft bootstrap config must include a group id";
        return false;
    }

    if (config.self.host.empty() || config.self.peer_port == 0 || config.self.api_port == 0) {
        error = "NuRaft bootstrap config must include a complete self address";
        return false;
    }

    if (config.peers.empty()) {
        error = "NuRaft bootstrap config must include at least one peer";
        return false;
    }

    std::set<int32_t> seen_server_ids;
    bool found_self = false;
    for (const auto& peer : config.peers) {
        if (peer.host.empty() || peer.peer_port == 0 || peer.api_port == 0) {
            error = "NuRaft bootstrap config contains an incomplete peer address";
            return false;
        }

        const int32_t server_id = peer.server_id();
        if (!seen_server_ids.insert(server_id).second) {
            error = "NuRaft bootstrap config contains duplicate server ids";
            return false;
        }

        if (peer == config.self) {
            found_self = true;
        }
    }

    if (!found_self) {
        error = "NuRaft bootstrap config peer list must include self";
        return false;
    }

    error.clear();
    return true;
}

}  // namespace

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

bool NuRaftMetadataStore::write_bootstrap_config(const NuRaftBootstrapConfig& config, std::string& error) const {
    if (!validate_bootstrap_config(config, error)) {
        return false;
    }

    nlohmann::json peers = nlohmann::json::array();
    for (const auto& peer : config.peers) {
        peers.push_back(encode_peer(peer));
    }

    nlohmann::json encoded = {
        {"format_version", config.format_version},
        {"group_id", config.group_id},
        {"self", encode_peer(config.self)},
        {"peers", peers},
        {"api_uses_ssl", config.api_uses_ssl},
    };
    return NuRaftFileStore::write_file_atomically(layout_.bootstrap_config_file, encoded.dump(), error);
}

bool NuRaftMetadataStore::read_bootstrap_config(NuRaftBootstrapConfig& config, std::string& error) const {
    std::string encoded;
    if (!NuRaftFileStore::read_file(layout_.bootstrap_config_file, encoded, error)) {
        return false;
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(encoded);
    } catch (const std::exception& e) {
        error = std::string("Failed to parse NuRaft bootstrap metadata: ") + e.what();
        return false;
    }

    if (!parsed.contains("format_version") || !parsed["format_version"].is_number_unsigned() ||
        !parsed.contains("group_id") || !parsed["group_id"].is_string() ||
        !parsed.contains("self") ||
        !parsed.contains("peers") || !parsed["peers"].is_array() ||
        !parsed.contains("api_uses_ssl") || !parsed["api_uses_ssl"].is_boolean()) {
        error = "NuRaft bootstrap metadata is missing required fields";
        return false;
    }

    NuRaftBootstrapConfig loaded;
    loaded.format_version = parsed["format_version"].get<uint32_t>();
    loaded.group_id = parsed["group_id"].get<std::string>();
    loaded.api_uses_ssl = parsed["api_uses_ssl"].get<bool>();

    if (!decode_peer(parsed["self"], loaded.self, error)) {
        return false;
    }

    for (const auto& peer_json : parsed["peers"]) {
        NuRaftPeerAddress peer;
        if (!decode_peer(peer_json, peer, error)) {
            return false;
        }
        loaded.peers.push_back(peer);
    }

    if (!validate_bootstrap_config(loaded, error)) {
        return false;
    }

    config = std::move(loaded);
    error.clear();
    return true;
}
