#pragma once

#include <string>
#include <vector>

#include "nuraft_replay_coordinator.h"

class NuRaftPrototypeStateMachine {
public:
    explicit NuRaftPrototypeStateMachine(NuRaftStateLayout layout);

    bool initialize(std::string& error);
    bool apply_pending(std::vector<NuRaftLogEntry>& applied_entries, std::string& error);
    bool read_applied_requests(std::vector<std::string>& request_json_lines, std::string& error) const;

private:
    NuRaftStateLayout layout_;
    NuRaftReplayCoordinator replay_coordinator_;
};
