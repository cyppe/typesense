#include "nuraft/nuraft_peer_resolver.h"

#include <string>

namespace {

bool parse_port(const std::string& value, uint32_t& port, std::string& error) {
    if (value.empty()) {
        error = "NuRaft peer address is missing a port";
        return false;
    }

    for (char ch : value) {
        if (ch < '0' || ch > '9') {
            error = "NuRaft peer address contains a non-numeric port";
            return false;
        }
    }

    const unsigned long parsed = std::stoul(value);
    if (parsed > 65535UL) {
        error = "NuRaft peer address port is out of range";
        return false;
    }

    port = static_cast<uint32_t>(parsed);
    return true;
}

}  // namespace

int32_t NuRaftPeerAddress::server_id() const {
    return static_cast<int32_t>(api_port);
}

std::string NuRaftPeerAddress::peer_endpoint() const {
    return host + ":" + std::to_string(peer_port);
}

std::string NuRaftPeerAddress::api_endpoint() const {
    return host + ":" + std::to_string(api_port);
}

std::string NuRaftPeerAddress::leader_url(bool api_uses_ssl) const {
    return std::string(api_uses_ssl ? "https://" : "http://") + api_endpoint() + "/";
}

bool NuRaftPeerAddress::operator==(const NuRaftPeerAddress& other) const {
    return host == other.host && peer_port == other.peer_port && api_port == other.api_port;
}

bool NuRaftPeerResolver::parse_address(const std::string& node,
                                      NuRaftPeerAddress& address,
                                      std::string& error) {
    if (node.empty()) {
        error = "NuRaft peer address is empty";
        return false;
    }

    std::string host;
    std::string peer_port_str;
    std::string api_port_str;

    if (node.front() == '[') {
        const size_t closing_bracket = node.find(']');
        if (closing_bracket == std::string::npos) {
            error = "NuRaft peer address has an unterminated IPv6 host";
            return false;
        }
        if (closing_bracket + 1 >= node.size() || node[closing_bracket + 1] != ':') {
            error = "NuRaft peer address is missing the peer port separator";
            return false;
        }

        host = node.substr(0, closing_bracket + 1);
        const size_t second_colon = node.find(':', closing_bracket + 2);
        if (second_colon == std::string::npos) {
            error = "NuRaft peer address is missing the API port separator";
            return false;
        }

        peer_port_str = node.substr(closing_bracket + 2, second_colon - (closing_bracket + 2));
        api_port_str = node.substr(second_colon + 1);
    } else {
        const size_t last_colon = node.rfind(':');
        if (last_colon == std::string::npos) {
            error = "NuRaft peer address is missing the API port separator";
            return false;
        }

        const size_t second_last_colon = node.rfind(':', last_colon - 1);
        if (second_last_colon == std::string::npos) {
            error = "NuRaft peer address is missing the peer port separator";
            return false;
        }

        host = node.substr(0, second_last_colon);
        peer_port_str = node.substr(second_last_colon + 1, last_colon - second_last_colon - 1);
        api_port_str = node.substr(last_colon + 1);
    }

    if (host.empty()) {
        error = "NuRaft peer address is missing a host";
        return false;
    }

    uint32_t peer_port = 0;
    if (!parse_port(peer_port_str, peer_port, error)) {
        return false;
    }

    uint32_t api_port = 0;
    if (!parse_port(api_port_str, api_port, error)) {
        return false;
    }

    address.host = host;
    address.peer_port = peer_port;
    address.api_port = api_port;
    error.clear();
    return true;
}
