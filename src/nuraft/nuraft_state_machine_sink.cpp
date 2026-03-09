#include "nuraft/nuraft_state_machine_sink.h"

NuRaftFileBackedStateMachineSink::NuRaftFileBackedStateMachineSink(NuRaftStateLayout layout)
    : store_(std::move(layout)) {}

bool NuRaftFileBackedStateMachineSink::apply_all(const std::vector<NuRaftAppliedRequest>& requests,
                                                 std::string& error) {
    return store_.append_all(requests, error);
}

bool NuRaftFileBackedStateMachineSink::read_all(std::vector<NuRaftAppliedRequest>& requests,
                                                std::string& error) const {
    return store_.read_all(requests, error);
}
