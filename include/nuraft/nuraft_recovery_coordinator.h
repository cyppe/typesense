#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nuraft_metadata_store.h"
#include "nuraft_snapshot_coordinator.h"

enum class NuRaftTimedSnapshotPolicy {
    kRequireHealthyPeers,
    kLeaderOnly,
};

struct NuRaftTimedSnapshotResult {
    bool due = false;
    bool created_snapshot = false;
    bool blocked_by_unhealthy_peer = false;
    int32_t leader_server_id = 0;
    std::vector<int32_t> unhealthy_peer_ids;
    NuRaftSnapshotDescriptor descriptor;
};

class NuRaftRecoveryCoordinator {
public:
    static bool run_timed_snapshot(const NuRaftBootstrapConfig& bootstrap_config,
                                   const std::map<int32_t, std::string>& data_dirs,
                                   int32_t preferred_leader_server_id,
                                   const std::map<int32_t, bool>& peer_health,
                                   NuRaftTimedSnapshotPolicy policy,
                                   int64_t current_time_seconds,
                                   int64_t snapshot_interval_seconds,
                                   int64_t& last_snapshot_time_seconds,
                                   NuRaftTimedSnapshotResult& result,
                                   std::string& error);

    static bool install_latest_snapshot(const std::string& source_data_dir,
                                        const std::string& target_data_dir,
                                        NuRaftSnapshotDescriptor& descriptor,
                                        std::string& error);
};
