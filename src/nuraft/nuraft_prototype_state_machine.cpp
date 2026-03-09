#include "nuraft/nuraft_prototype_state_machine.h"

NuRaftPrototypeStateMachine::NuRaftPrototypeStateMachine(NuRaftStateLayout layout)
    : NuRaftPrototypeStateMachine(layout, std::make_unique<NuRaftFileBackedStateMachineSink>(layout)) {}

NuRaftPrototypeStateMachine::NuRaftPrototypeStateMachine(NuRaftStateLayout layout,
                                                         std::unique_ptr<NuRaftStateMachineSink> sink)
    : layout_(std::move(layout)), sink_(std::move(sink)), replay_coordinator_(layout_) {}

bool NuRaftPrototypeStateMachine::initialize(std::string& error) {
    return replay_coordinator_.initialize(error);
}

bool NuRaftPrototypeStateMachine::apply_pending(std::vector<NuRaftLogEntry>& applied_entries,
                                                std::string& error) {
    if (!replay_coordinator_.pending_entries(applied_entries, error)) {
        return false;
    }

    if (applied_entries.empty()) {
        error.clear();
        return true;
    }

    std::vector<NuRaftAppliedRequest> applied_requests;
    for (const auto& entry : applied_entries) {
        NuRaftAppliedRequest applied_request;
        if (!NuRaftAppliedRequest::from_log_entry(entry, applied_request, error)) {
            return false;
        }
        applied_requests.push_back(std::move(applied_request));
    }

    if (!sink_->apply_all(applied_requests, error)) {
        return false;
    }

    return replay_coordinator_.mark_replayed_through(applied_entries.back().index, error);
}

bool NuRaftPrototypeStateMachine::read_applied_requests(std::vector<NuRaftAppliedRequest>& requests,
                                                        std::string& error) const {
    return sink_->read_all(requests, error);
}
