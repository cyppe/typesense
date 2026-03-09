#include "nuraft/nuraft_static_cluster.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "nuraft/nuraft_applied_request_store.h"
#include "nuraft/nuraft_prototype_state_machine.h"
#include "nuraft/nuraft_request_journal.h"
#include "string_utils.h"

namespace {

bool parse_server_id(const std::string& value, int32_t& server_id, std::string& error) {
    if (value.empty()) {
        error = "NuRaft static cluster entry is missing a server id";
        return false;
    }

    try {
        const long parsed = std::stol(value);
        if (parsed <= 0 || parsed > std::numeric_limits<int32_t>::max()) {
            error = "NuRaft static cluster server id is out of range";
            return false;
        }
        server_id = static_cast<int32_t>(parsed);
        error.clear();
        return true;
    } catch (const std::exception&) {
        error = "NuRaft static cluster server id is invalid";
        return false;
    }
}

bool validate_data_dirs(const NuRaftBootstrapConfig& bootstrap_config,
                        const std::map<int32_t, std::string>& data_dirs,
                        int32_t leader_server_id,
                        std::string& error) {
    if (data_dirs.empty()) {
        error = "NuRaft static cluster requires at least one data-dir mapping";
        return false;
    }

    for (const auto& peer : bootstrap_config.peers) {
        const auto it = data_dirs.find(peer.server_id());
        if (it == data_dirs.end()) {
            error = "NuRaft static cluster data-dir map is missing peer server id " +
                    std::to_string(peer.server_id());
            return false;
        }
        if (it->second.empty()) {
            error = "NuRaft static cluster data-dir map contains an empty path";
            return false;
        }
    }

    if (data_dirs.find(leader_server_id) == data_dirs.end()) {
        error = "NuRaft static cluster leader is missing from the data-dir map";
        return false;
    }

    error.clear();
    return true;
}

bool read_log_entries(const std::string& data_dir,
                      std::vector<NuRaftLogEntry>& entries,
                      std::string& error) {
    NuRaftRequestJournal journal(NuRaftStateLayout::from_data_dir(data_dir));
    if (!journal.initialize(error)) {
        return false;
    }
    return journal.replay(entries, error);
}

bool append_missing_entries(const std::vector<NuRaftLogEntry>& leader_entries,
                            const std::string& follower_data_dir,
                            std::string& error) {
    std::vector<NuRaftLogEntry> follower_entries;
    if (!read_log_entries(follower_data_dir, follower_entries, error)) {
        return false;
    }

    if (follower_entries.size() > leader_entries.size()) {
        error = "NuRaft static cluster follower log is ahead of the leader";
        return false;
    }

    for (size_t i = 0; i < follower_entries.size(); ++i) {
        if (!(follower_entries[i] == leader_entries[i])) {
            error = "NuRaft static cluster follower log diverges from leader history";
            return false;
        }
    }

    NuRaftRequestJournal follower_journal(NuRaftStateLayout::from_data_dir(follower_data_dir));
    if (!follower_journal.initialize(error)) {
        return false;
    }

    for (size_t i = follower_entries.size(); i < leader_entries.size(); ++i) {
        uint64_t appended_index = 0;
        if (!follower_journal.append_request_json(leader_entries[i].envelope.request_json(), appended_index, error)) {
            return false;
        }
        if (appended_index != leader_entries[i].index) {
            error = "NuRaft static cluster follower append assigned an unexpected index";
            return false;
        }
    }

    error.clear();
    return true;
}

bool make_sink(const NuRaftStateLayout& layout,
               const std::string& sink_kind,
               std::unique_ptr<NuRaftStateMachineSink>& sink,
               std::string& error) {
    if (sink_kind == "file") {
        sink = std::make_unique<NuRaftFileBackedStateMachineSink>(layout);
        error.clear();
        return true;
    }
    if (sink_kind == "kv") {
        sink = std::make_unique<NuRaftKvStateMachineSink>(layout);
        error.clear();
        return true;
    }

    error = "Unsupported NuRaft static cluster state-machine sink: " + sink_kind;
    return false;
}

bool apply_pending_entries(const std::string& data_dir,
                           const std::string& sink_kind,
                           std::string& error) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(data_dir);
    std::unique_ptr<NuRaftStateMachineSink> sink;
    if (!make_sink(layout, sink_kind, sink, error)) {
        return false;
    }

    NuRaftPrototypeStateMachine state_machine(layout, std::move(sink));
    if (!state_machine.initialize(error)) {
        return false;
    }

    std::vector<NuRaftLogEntry> applied_entries;
    return state_machine.apply_pending(applied_entries, error);
}

bool read_status(const std::string& data_dir,
                 int32_t server_id,
                 bool is_leader,
                 NuRaftStaticClusterNodeStatus& status,
                 std::string& error) {
    std::vector<NuRaftLogEntry> entries;
    if (!read_log_entries(data_dir, entries, error)) {
        return false;
    }

    NuRaftMetadataStore metadata_store(NuRaftStateLayout::from_data_dir(data_dir));
    if (!metadata_store.initialize(error)) {
        return false;
    }

    if (!std::filesystem::exists(metadata_store.layout().replay_progress_file)) {
        NuRaftReplayProgress initial_progress;
        if (!metadata_store.write_replay_progress(initial_progress, error)) {
            return false;
        }
    }

    NuRaftReplayProgress progress;
    if (!metadata_store.read_replay_progress(progress, error)) {
        return false;
    }

    std::vector<NuRaftAppliedRequest> applied_requests;
    NuRaftAppliedRequestStore applied_store(NuRaftStateLayout::from_data_dir(data_dir));
    if (!applied_store.read_all(applied_requests, error)) {
        return false;
    }

    status.server_id = server_id;
    status.data_dir = data_dir;
    status.is_leader = is_leader;
    status.last_log_index = entries.empty() ? 0 : entries.back().index;
    status.last_applied_index = progress.last_applied_index;
    status.applied_request_count = applied_requests.size();
    error.clear();
    return true;
}

}  // namespace

bool NuRaftStaticClusterNodeStatus::operator==(const NuRaftStaticClusterNodeStatus& other) const {
    return server_id == other.server_id &&
           data_dir == other.data_dir &&
           is_leader == other.is_leader &&
           last_log_index == other.last_log_index &&
           last_applied_index == other.last_applied_index &&
           applied_request_count == other.applied_request_count;
}

bool NuRaftStaticCluster::parse_data_dir_map(const std::string& encoded,
                                             std::map<int32_t, std::string>& data_dirs,
                                             std::string& error) {
    data_dirs.clear();
    if (encoded.empty()) {
        error = "NuRaft static cluster data-dir map is empty";
        return false;
    }

    std::vector<std::string> entries;
    StringUtils::split(encoded, entries, ",");
    for (const auto& entry : entries) {
        const size_t separator = entry.find('=');
        if (separator == std::string::npos) {
            error = "NuRaft static cluster entry must use server_id=data_dir syntax";
            return false;
        }

        int32_t server_id = 0;
        if (!parse_server_id(entry.substr(0, separator), server_id, error)) {
            return false;
        }

        const std::string data_dir = entry.substr(separator + 1);
        if (data_dir.empty()) {
            error = "NuRaft static cluster entry is missing a data directory";
            return false;
        }

        if (!data_dirs.emplace(server_id, data_dir).second) {
            error = "NuRaft static cluster data-dir map contains a duplicate server id";
            return false;
        }
    }

    error.clear();
    return true;
}

bool NuRaftStaticCluster::discover_leader(const NuRaftBootstrapConfig& bootstrap_config,
                                          int32_t preferred_leader_server_id,
                                          NuRaftPeerAddress& leader,
                                          std::string& error) {
    if (bootstrap_config.peers.empty()) {
        error = "NuRaft static cluster bootstrap config has no peers";
        return false;
    }

    const int32_t leader_server_id = (preferred_leader_server_id != 0) ?
                                     preferred_leader_server_id :
                                     bootstrap_config.peers.front().server_id();
    for (const auto& peer : bootstrap_config.peers) {
        if (peer.server_id() == leader_server_id) {
            leader = peer;
            error.clear();
            return true;
        }
    }

    error = "NuRaft static cluster could not resolve the configured leader in bootstrap peers";
    return false;
}

bool NuRaftStaticCluster::append_request(const NuRaftBootstrapConfig& bootstrap_config,
                                         const std::map<int32_t, std::string>& data_dirs,
                                         int32_t preferred_leader_server_id,
                                         int32_t self_server_id,
                                         const std::string& request_json,
                                         uint64_t& index,
                                         bool& forwarded_to_leader,
                                         int32_t& target_server_id,
                                         std::string& error) {
    NuRaftPeerAddress leader;
    if (!discover_leader(bootstrap_config, preferred_leader_server_id, leader, error)) {
        return false;
    }

    if (!validate_data_dirs(bootstrap_config, data_dirs, leader.server_id(), error)) {
        return false;
    }

    target_server_id = leader.server_id();
    forwarded_to_leader = (self_server_id != leader.server_id());

    NuRaftRequestJournal journal(NuRaftStateLayout::from_data_dir(data_dirs.at(target_server_id)));
    if (!journal.initialize(error)) {
        return false;
    }

    return journal.append_request_json(request_json, index, error);
}

bool NuRaftStaticCluster::collect_status(const NuRaftBootstrapConfig& bootstrap_config,
                                         const std::map<int32_t, std::string>& data_dirs,
                                         int32_t preferred_leader_server_id,
                                         std::vector<NuRaftStaticClusterNodeStatus>& statuses,
                                         std::string& error) {
    NuRaftPeerAddress leader;
    if (!discover_leader(bootstrap_config, preferred_leader_server_id, leader, error)) {
        return false;
    }

    if (!validate_data_dirs(bootstrap_config, data_dirs, leader.server_id(), error)) {
        return false;
    }

    statuses.clear();
    for (const auto& peer : bootstrap_config.peers) {
        NuRaftStaticClusterNodeStatus status;
        if (!read_status(data_dirs.at(peer.server_id()),
                         peer.server_id(),
                         peer.server_id() == leader.server_id(),
                         status,
                         error)) {
            return false;
        }
        statuses.push_back(std::move(status));
    }

    std::sort(statuses.begin(), statuses.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.server_id < rhs.server_id;
    });
    error.clear();
    return true;
}

bool NuRaftStaticCluster::replicate_and_apply(const NuRaftBootstrapConfig& bootstrap_config,
                                              const std::map<int32_t, std::string>& data_dirs,
                                              int32_t preferred_leader_server_id,
                                              const std::string& state_machine_sink,
                                              std::vector<NuRaftStaticClusterNodeStatus>& statuses,
                                              std::string& error) {
    NuRaftPeerAddress leader;
    if (!discover_leader(bootstrap_config, preferred_leader_server_id, leader, error)) {
        return false;
    }

    if (!validate_data_dirs(bootstrap_config, data_dirs, leader.server_id(), error)) {
        return false;
    }

    std::vector<NuRaftLogEntry> leader_entries;
    if (!read_log_entries(data_dirs.at(leader.server_id()), leader_entries, error)) {
        return false;
    }

    for (const auto& peer : bootstrap_config.peers) {
        if (!append_missing_entries(leader_entries, data_dirs.at(peer.server_id()), error)) {
            return false;
        }
    }

    for (const auto& peer : bootstrap_config.peers) {
        if (!apply_pending_entries(data_dirs.at(peer.server_id()), state_machine_sink, error)) {
            return false;
        }
    }

    return collect_status(bootstrap_config, data_dirs, preferred_leader_server_id, statuses, error);
}
