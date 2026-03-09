#include "nuraft/nuraft_recovery_coordinator.h"

#include <filesystem>
#include <memory>
#include <utility>

#include "nuraft/nuraft_prototype_state_machine.h"
#include "nuraft/nuraft_static_cluster.h"
#include "nuraft/nuraft_state_machine_sink.h"

namespace {

bool apply_pending_on_leader(const std::string& data_dir, std::string& error) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(data_dir);
    auto sink = std::make_unique<NuRaftKvStateMachineSink>(layout);
    NuRaftPrototypeStateMachine state_machine(layout, std::move(sink));
    if (!state_machine.initialize(error)) {
        return false;
    }

    std::vector<NuRaftLogEntry> applied_entries;
    return state_machine.apply_pending(applied_entries, error);
}

bool is_peer_healthy(int32_t server_id, const std::map<int32_t, bool>& peer_health) {
    const auto it = peer_health.find(server_id);
    return it == peer_health.end() ? true : it->second;
}

}  // namespace

bool NuRaftRecoveryCoordinator::run_timed_snapshot(const NuRaftBootstrapConfig& bootstrap_config,
                                                   const std::map<int32_t, std::string>& data_dirs,
                                                   int32_t preferred_leader_server_id,
                                                   const std::map<int32_t, bool>& peer_health,
                                                   NuRaftTimedSnapshotPolicy policy,
                                                   int64_t current_time_seconds,
                                                   int64_t snapshot_interval_seconds,
                                                   int64_t& last_snapshot_time_seconds,
                                                   NuRaftTimedSnapshotResult& result,
                                                   std::string& error) {
    result = NuRaftTimedSnapshotResult();

    NuRaftPeerAddress leader;
    if (!NuRaftStaticCluster::discover_leader(bootstrap_config, preferred_leader_server_id, leader, error)) {
        return false;
    }

    result.leader_server_id = leader.server_id();
    if (current_time_seconds - last_snapshot_time_seconds < snapshot_interval_seconds) {
        error.clear();
        return true;
    }
    result.due = true;

    if (data_dirs.find(leader.server_id()) == data_dirs.end()) {
        error = "NuRaft timed snapshot leader is missing from the data-dir map";
        return false;
    }

    if (!is_peer_healthy(leader.server_id(), peer_health)) {
        error.clear();
        return true;
    }

    for (const auto& peer : bootstrap_config.peers) {
        if (peer.server_id() == leader.server_id()) {
            continue;
        }
        if (!is_peer_healthy(peer.server_id(), peer_health)) {
            result.unhealthy_peer_ids.push_back(peer.server_id());
        }
    }

    if (policy == NuRaftTimedSnapshotPolicy::kRequireHealthyPeers && !result.unhealthy_peer_ids.empty()) {
        result.blocked_by_unhealthy_peer = true;
        error.clear();
        return true;
    }

    const std::string& leader_data_dir = data_dirs.at(leader.server_id());
    if (!apply_pending_on_leader(leader_data_dir, error)) {
        return false;
    }

    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(leader_data_dir);
    auto sink = std::make_unique<NuRaftKvStateMachineSink>(layout);
    NuRaftKvStateMachineSink* sink_ptr = sink.get();
    NuRaftPrototypeStateMachine state_machine(layout, std::move(sink));
    if (!state_machine.initialize(error)) {
        return false;
    }

    NuRaftSnapshotCoordinator coordinator(layout);
    if (!coordinator.create_snapshot("", sink_ptr, result.descriptor, error)) {
        return false;
    }

    last_snapshot_time_seconds = current_time_seconds;
    result.created_snapshot = true;
    error.clear();
    return true;
}

bool NuRaftRecoveryCoordinator::install_latest_snapshot(const std::string& source_data_dir,
                                                        const std::string& target_data_dir,
                                                        NuRaftSnapshotDescriptor& descriptor,
                                                        std::string& error) {
    const NuRaftStateLayout source_layout = NuRaftStateLayout::from_data_dir(source_data_dir);
    NuRaftSnapshotCoordinator source_coordinator(source_layout);
    if (!source_coordinator.read_last_snapshot(descriptor, error)) {
        return false;
    }

    const std::filesystem::path snapshot_path =
        std::filesystem::path(source_layout.snapshot_dir) / descriptor.snapshot_id;
    if (!std::filesystem::is_directory(snapshot_path)) {
        error = "NuRaft recovery coordinator could not find the latest leader snapshot payload";
        return false;
    }

    NuRaftSnapshotCoordinator target_coordinator(NuRaftStateLayout::from_data_dir(target_data_dir));
    return target_coordinator.install_snapshot(snapshot_path.string(), descriptor, error);
}
