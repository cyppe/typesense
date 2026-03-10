#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <libnuraft/nuraft.hxx>

#include "nuraft_metadata_store.h"
#include "nuraft_state_layout.h"
#include "typesense_log_store.h"

// NuRaft state manager for Typesense.
//
// Persists cluster configuration and server state to files:
//   <layout.cluster_config_file>  → serialized nuraft::cluster_config
//   <layout.server_state_file>    → serialized nuraft::srv_state
//
// Loads log store from the raft_log/ directory within the NuRaft state layout.
class TypesenseStateManager : public nuraft::state_mgr {
public:
    TypesenseStateManager(const NuRaftStateLayout& layout,
                          const NuRaftIdentity& identity,
                          const NuRaftBootstrapConfig& bootstrap_config);
    ~TypesenseStateManager() override;

    nuraft::ptr<nuraft::cluster_config> load_config() override;
    void save_config(const nuraft::cluster_config& config) override;
    void save_state(const nuraft::srv_state& state) override;
    nuraft::ptr<nuraft::srv_state> read_state() override;
    nuraft::ptr<nuraft::log_store> load_log_store() override;
    nuraft::int32 server_id() override;
    void system_exit(const int exit_code) override;

    TypesenseLogStore* get_log_store_raw() const;

private:
    nuraft::ptr<nuraft::cluster_config> make_initial_config() const;
    bool read_file(const std::string& path, std::string& data) const;
    bool write_file(const std::string& path, const void* data, size_t size) const;

    NuRaftStateLayout layout_;
    NuRaftIdentity identity_;
    NuRaftBootstrapConfig bootstrap_config_;
    nuraft::ptr<TypesenseLogStore> log_store_;
};
