#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nuraft_file_store.h"
#include "nuraft_peer_resolver.h"

struct NuRaftIdentity {
    static constexpr uint32_t kCurrentFormatVersion = 1;

    uint32_t format_version = kCurrentFormatVersion;
    int32_t server_id = 0;
    std::string peer_endpoint;
    int32_t api_port = 0;

    bool operator==(const NuRaftIdentity& other) const;
};

struct NuRaftBootstrapConfig {
    static constexpr uint32_t kCurrentFormatVersion = 1;

    uint32_t format_version = kCurrentFormatVersion;
    std::string group_id = "default_group";
    NuRaftPeerAddress self;
    std::vector<NuRaftPeerAddress> peers;
    bool api_uses_ssl = false;

    bool operator==(const NuRaftBootstrapConfig& other) const;
};

struct NuRaftReplayProgress {
    static constexpr uint32_t kCurrentFormatVersion = 1;

    uint32_t format_version = kCurrentFormatVersion;
    uint64_t last_applied_index = 0;

    bool operator==(const NuRaftReplayProgress& other) const;
};

class NuRaftMetadataStore {
public:
    explicit NuRaftMetadataStore(NuRaftStateLayout layout);

    const NuRaftStateLayout& layout() const;
    bool initialize(std::string& error) const;
    bool write_identity(const NuRaftIdentity& identity, std::string& error) const;
    bool read_identity(NuRaftIdentity& identity, std::string& error) const;
    bool write_bootstrap_config(const NuRaftBootstrapConfig& config, std::string& error) const;
    bool read_bootstrap_config(NuRaftBootstrapConfig& config, std::string& error) const;
    bool write_replay_progress(const NuRaftReplayProgress& progress, std::string& error) const;
    bool read_replay_progress(NuRaftReplayProgress& progress, std::string& error) const;

private:
    NuRaftStateLayout layout_;
};
