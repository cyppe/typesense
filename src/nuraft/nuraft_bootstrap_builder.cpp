#include "nuraft/nuraft_bootstrap_builder.h"

#include <utility>
#include <vector>

#include "string_utils.h"

namespace {

bool build_single_node_config(const std::string& local_host,
                              uint32_t peer_port,
                              uint32_t api_port,
                              bool api_uses_ssl,
                              NuRaftBootstrapConfig& config,
                              std::string& error) {
    if (local_host.empty() || peer_port == 0 || api_port == 0) {
        error = "NuRaft bootstrap builder requires a complete local address";
        return false;
    }

    config = NuRaftBootstrapConfig();
    config.self = {local_host, peer_port, api_port};
    config.peers = {config.self};
    config.api_uses_ssl = api_uses_ssl;
    error.clear();
    return true;
}

}  // namespace

bool NuRaftBootstrapBuilder::build(const std::string& local_host,
                                   uint32_t peer_port,
                                   uint32_t api_port,
                                   const std::string& nodes_config,
                                   bool api_uses_ssl,
                                   NuRaftBootstrapConfig& config,
                                   std::string& error) {
    if (nodes_config.empty()) {
        return build_single_node_config(local_host, peer_port, api_port, api_uses_ssl, config, error);
    }

    std::vector<std::string> node_strings;
    StringUtils::split(nodes_config, node_strings, ",");
    if (node_strings.empty()) {
        error = "NuRaft bootstrap config is empty after parsing nodes";
        return false;
    }

    NuRaftBootstrapConfig built;
    built.api_uses_ssl = api_uses_ssl;

    bool found_self = false;
    for (const auto& node_string : node_strings) {
        NuRaftPeerAddress peer;
        if (!NuRaftPeerResolver::parse_address(node_string, peer, error)) {
            error = std::string("Failed to parse NuRaft bootstrap node '") + node_string + "': " + error;
            return false;
        }

        built.peers.push_back(peer);
        if (peer.api_port == api_port) {
            built.self = peer;
            found_self = true;
        }
    }

    if (!found_self) {
        error = "NuRaft bootstrap config does not include the local api_port";
        return false;
    }

    config = std::move(built);
    error.clear();
    return true;
}
