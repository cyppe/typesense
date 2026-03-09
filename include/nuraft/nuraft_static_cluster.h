#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nuraft_metadata_store.h"

struct NuRaftStaticClusterNodeStatus {
    int32_t server_id = 0;
    std::string data_dir;
    bool is_leader = false;
    uint64_t last_log_index = 0;
    uint64_t last_applied_index = 0;
    uint64_t applied_request_count = 0;

    bool operator==(const NuRaftStaticClusterNodeStatus& other) const;
};

class NuRaftStaticCluster {
public:
    static bool parse_data_dir_map(const std::string& encoded,
                                   std::map<int32_t, std::string>& data_dirs,
                                   std::string& error);
    static bool discover_leader(const NuRaftBootstrapConfig& bootstrap_config,
                                int32_t preferred_leader_server_id,
                                NuRaftPeerAddress& leader,
                                std::string& error);
    static bool append_request(const NuRaftBootstrapConfig& bootstrap_config,
                               const std::map<int32_t, std::string>& data_dirs,
                               int32_t preferred_leader_server_id,
                               int32_t self_server_id,
                               const std::string& request_json,
                               uint64_t& index,
                               bool& forwarded_to_leader,
                               int32_t& target_server_id,
                               std::string& error);
    static bool collect_status(const NuRaftBootstrapConfig& bootstrap_config,
                               const std::map<int32_t, std::string>& data_dirs,
                               int32_t preferred_leader_server_id,
                               std::vector<NuRaftStaticClusterNodeStatus>& statuses,
                               std::string& error);
    static bool replicate_and_apply(const NuRaftBootstrapConfig& bootstrap_config,
                                    const std::map<int32_t, std::string>& data_dirs,
                                    int32_t preferred_leader_server_id,
                                    const std::string& state_machine_sink,
                                    std::vector<NuRaftStaticClusterNodeStatus>& statuses,
                                    std::string& error);
};
