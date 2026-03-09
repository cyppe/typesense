#pragma once

#include <cstdint>
#include <string>

#include "nuraft_metadata_store.h"

struct NuRaftPrototypeOptions {
    std::string data_dir;
    std::string local_host;
    uint32_t peer_port = 0;
    uint32_t api_port = 0;
    std::string nodes_config;
    bool api_uses_ssl = false;
};

class NuRaftStateInitializer {
public:
    static bool initialize(const NuRaftPrototypeOptions& options,
                           NuRaftIdentity& identity,
                           NuRaftBootstrapConfig& bootstrap_config,
                           std::string& error);
};
