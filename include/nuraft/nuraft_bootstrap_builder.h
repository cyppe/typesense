#pragma once

#include <cstdint>
#include <string>

#include "nuraft_metadata_store.h"

class NuRaftBootstrapBuilder {
public:
    static bool build(const std::string& local_host,
                      uint32_t peer_port,
                      uint32_t api_port,
                      const std::string& nodes_config,
                      bool api_uses_ssl,
                      NuRaftBootstrapConfig& config,
                      std::string& error);
};
