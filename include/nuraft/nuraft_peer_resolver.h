#pragma once

#include <cstdint>
#include <string>

struct NuRaftPeerAddress {
    std::string host;
    uint32_t peer_port = 0;
    uint32_t api_port = 0;

    int32_t server_id() const;
    std::string peer_endpoint() const;
    std::string api_endpoint() const;
    std::string leader_url(bool api_uses_ssl) const;

    bool operator==(const NuRaftPeerAddress& other) const;
};

class NuRaftPeerResolver {
public:
    static bool parse_address(const std::string& node,
                              NuRaftPeerAddress& address,
                              std::string& error);
};
