#pragma once

#include <memory>
#include <string>
#include <vector>

#include "nuraft_replay_coordinator.h"
#include "nuraft_state_machine_sink.h"

class NuRaftPrototypeStateMachine {
public:
    explicit NuRaftPrototypeStateMachine(NuRaftStateLayout layout);
    NuRaftPrototypeStateMachine(NuRaftStateLayout layout, std::unique_ptr<NuRaftStateMachineSink> sink);

    bool initialize(std::string& error);
    bool apply_pending(std::vector<NuRaftLogEntry>& applied_entries, std::string& error);
    bool read_applied_requests(std::vector<NuRaftAppliedRequest>& requests, std::string& error) const;

private:
    NuRaftStateLayout layout_;
    std::unique_ptr<NuRaftStateMachineSink> sink_;
    NuRaftReplayCoordinator replay_coordinator_;
};
