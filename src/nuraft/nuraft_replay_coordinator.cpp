#include "nuraft/nuraft_replay_coordinator.h"

#include <filesystem>

NuRaftReplayCoordinator::NuRaftReplayCoordinator(NuRaftStateLayout layout)
    : metadata_store_(layout), request_journal_(layout) {}

bool NuRaftReplayCoordinator::initialize(std::string& error) {
    if (!metadata_store_.initialize(error)) {
        return false;
    }

    if (!request_journal_.initialize(error)) {
        return false;
    }

    if (!std::filesystem::exists(metadata_store_.layout().replay_progress_file)) {
        NuRaftReplayProgress initial_progress;
        if (!metadata_store_.write_replay_progress(initial_progress, error)) {
            return false;
        }
    }

    error.clear();
    return true;
}

bool NuRaftReplayCoordinator::read_progress(NuRaftReplayProgress& progress, std::string& error) const {
    return metadata_store_.read_replay_progress(progress, error);
}

bool NuRaftReplayCoordinator::pending_entries(std::vector<NuRaftLogEntry>& entries, std::string& error) const {
    NuRaftReplayProgress progress;
    if (!metadata_store_.read_replay_progress(progress, error)) {
        return false;
    }

    std::vector<NuRaftLogEntry> all_entries;
    if (!request_journal_.replay(all_entries, error)) {
        return false;
    }

    entries.clear();
    for (const auto& entry : all_entries) {
        if (entry.index > progress.last_applied_index) {
            entries.push_back(entry);
        }
    }

    error.clear();
    return true;
}

bool NuRaftReplayCoordinator::mark_replayed_through(uint64_t index, std::string& error) const {
    NuRaftReplayProgress progress;
    if (!metadata_store_.read_replay_progress(progress, error)) {
        return false;
    }

    if (index < progress.last_applied_index) {
        error = "NuRaft replay progress cannot move backwards";
        return false;
    }

    std::vector<NuRaftLogEntry> all_entries;
    if (!request_journal_.replay(all_entries, error)) {
        return false;
    }

    const uint64_t last_log_index = all_entries.empty() ? 0 : all_entries.back().index;
    if (index > last_log_index) {
        error = "NuRaft replay progress cannot advance beyond the persisted log";
        return false;
    }

    progress.last_applied_index = index;
    return metadata_store_.write_replay_progress(progress, error);
}
