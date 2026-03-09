#include "nuraft/nuraft_prototype_state_machine.h"

#include <filesystem>

#include "nuraft/nuraft_file_store.h"
#include "string_utils.h"

NuRaftPrototypeStateMachine::NuRaftPrototypeStateMachine(NuRaftStateLayout layout)
    : layout_(layout), replay_coordinator_(layout) {}

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

    std::string existing_applied;
    if (std::filesystem::exists(layout_.applied_requests_file) &&
        !NuRaftFileStore::read_file(layout_.applied_requests_file, existing_applied, error)) {
        return false;
    }

    for (const auto& entry : applied_entries) {
        existing_applied.append(entry.envelope.request_json());
        existing_applied.push_back('\n');
    }

    if (!NuRaftFileStore::write_file_atomically(layout_.applied_requests_file, existing_applied, error)) {
        return false;
    }

    return replay_coordinator_.mark_replayed_through(applied_entries.back().index, error);
}

bool NuRaftPrototypeStateMachine::read_applied_requests(std::vector<std::string>& request_json_lines,
                                                        std::string& error) const {
    request_json_lines.clear();
    if (!std::filesystem::exists(layout_.applied_requests_file)) {
        error.clear();
        return true;
    }

    std::string encoded;
    if (!NuRaftFileStore::read_file(layout_.applied_requests_file, encoded, error)) {
        return false;
    }

    StringUtils::split(encoded, request_json_lines, "\n");
    error.clear();
    return true;
}
