#include "nuraft/nuraft_state_initializer.h"

#include "nuraft/nuraft_bootstrap_builder.h"

bool NuRaftStateInitializer::initialize(const NuRaftPrototypeOptions& options,
                                        NuRaftIdentity& identity,
                                        NuRaftBootstrapConfig& bootstrap_config,
                                        std::string& error) {
    if (options.data_dir.empty()) {
        error = "NuRaft state initializer requires a data directory";
        return false;
    }

    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(options.data_dir);
    NuRaftMetadataStore store(layout);
    if (!store.initialize(error)) {
        return false;
    }

    NuRaftBootstrapConfig built_bootstrap_config;
    if (!NuRaftBootstrapBuilder::build(options.local_host,
                                       options.peer_port,
                                       options.api_port,
                                       options.nodes_config,
                                       options.api_uses_ssl,
                                       built_bootstrap_config,
                                       error)) {
        return false;
    }

    NuRaftIdentity built_identity;
    built_identity.server_id = built_bootstrap_config.self.server_id();
    built_identity.peer_endpoint = built_bootstrap_config.self.peer_endpoint();
    built_identity.api_port = static_cast<int32_t>(built_bootstrap_config.self.api_port);

    if (!store.write_identity(built_identity, error)) {
        return false;
    }

    if (!store.write_bootstrap_config(built_bootstrap_config, error)) {
        return false;
    }

    identity = built_identity;
    bootstrap_config = built_bootstrap_config;
    error.clear();
    return true;
}
