#include "nuraft/nuraft_request_journal.h"

NuRaftRequestJournal::NuRaftRequestJournal(NuRaftStateLayout layout)
    : log_store_(std::move(layout)) {}

const NuRaftStateLayout& NuRaftRequestJournal::layout() const {
    return log_store_.layout();
}

bool NuRaftRequestJournal::initialize(std::string& error) {
    return log_store_.initialize(error);
}

bool NuRaftRequestJournal::append_request_json(const std::string& request_json,
                                              uint64_t& index,
                                              std::string& error) {
    if (request_json.empty()) {
        error = "NuRaft request journal cannot append an empty request payload";
        return false;
    }

    return log_store_.append(NuRaftRequestEnvelope(request_json), index, error);
}

bool NuRaftRequestJournal::replay(std::vector<NuRaftLogEntry>& entries, std::string& error) const {
    return log_store_.read_all(entries, error);
}
