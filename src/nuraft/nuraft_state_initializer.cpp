#include "nuraft/nuraft_state_initializer.h"

#include <filesystem>

#include "nuraft/nuraft_bootstrap_builder.h"

namespace {

NuRaftIdentity build_identity(const NuRaftBootstrapConfig& bootstrap_config) {
    NuRaftIdentity identity;
    identity.server_id = bootstrap_config.self.server_id();
    identity.peer_endpoint = bootstrap_config.self.peer_endpoint();
    identity.api_port = static_cast<int32_t>(bootstrap_config.self.api_port);
    return identity;
}

bool persist_metadata(const NuRaftMetadataStore& store,
                      const NuRaftIdentity& identity,
                      const NuRaftBootstrapConfig& bootstrap_config,
                      std::string& error) {
    if (!store.write_identity(identity, error)) {
        return false;
    }

    if (!store.write_bootstrap_config(bootstrap_config, error)) {
        return false;
    }

    error.clear();
    return true;
}

bool refresh_existing_metadata(const NuRaftMetadataStore& store,
                               const NuRaftIdentity& built_identity,
                               const NuRaftBootstrapConfig& built_bootstrap_config,
                               NuRaftIdentity& identity,
                               NuRaftBootstrapConfig& bootstrap_config,
                               std::string& error) {
    const bool has_identity = std::filesystem::exists(store.layout().identity_file);
    const bool has_bootstrap_config = std::filesystem::exists(store.layout().bootstrap_config_file);
    if (!has_identity || !has_bootstrap_config) {
        if (!persist_metadata(store, built_identity, built_bootstrap_config, error)) {
            return false;
        }

        identity = built_identity;
        bootstrap_config = built_bootstrap_config;
        return true;
    }

    NuRaftIdentity persisted_identity;
    NuRaftBootstrapConfig persisted_bootstrap_config;
    if (!store.read_identity(persisted_identity, error) ||
        !store.read_bootstrap_config(persisted_bootstrap_config, error)) {
        return false;
    }

    if (persisted_identity.server_id != built_identity.server_id ||
        persisted_identity.api_port != built_identity.api_port) {
        error = "NuRaft state initializer refuses to change the persisted server identity";
        return false;
    }

    const bool self_changed = !(persisted_bootstrap_config.self == built_bootstrap_config.self) ||
                              persisted_identity.peer_endpoint != built_identity.peer_endpoint;
    const bool allow_self_rewrite = self_changed &&
                                    persisted_bootstrap_config.peers.size() == 1 &&
                                    built_bootstrap_config.peers.size() == 1;
    if (self_changed && !allow_self_rewrite) {
        error = "NuRaft state initializer refuses to rewrite the persisted self peer address for a multi-node bootstrap";
        return false;
    }

    const NuRaftIdentity desired_identity = allow_self_rewrite ? built_identity : persisted_identity;
    const bool bootstrap_changed = !(persisted_bootstrap_config == built_bootstrap_config);
    const bool identity_changed = !(persisted_identity == desired_identity);
    if (bootstrap_changed || identity_changed) {
        if (!persist_metadata(store, desired_identity, built_bootstrap_config, error)) {
            return false;
        }
    }

    identity = desired_identity;
    bootstrap_config = built_bootstrap_config;
    error.clear();
    return true;
}

}  // namespace

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

    const NuRaftIdentity built_identity = build_identity(built_bootstrap_config);
    return refresh_existing_metadata(store, built_identity, built_bootstrap_config, identity, bootstrap_config, error);
}
