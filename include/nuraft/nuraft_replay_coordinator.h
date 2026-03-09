#pragma once

#include <string>
#include <vector>

#include "nuraft_metadata_store.h"
#include "nuraft_request_journal.h"

class NuRaftReplayCoordinator {
public:
    explicit NuRaftReplayCoordinator(NuRaftStateLayout layout);

    bool initialize(std::string& error);
    bool read_progress(NuRaftReplayProgress& progress, std::string& error) const;
    bool pending_entries(std::vector<NuRaftLogEntry>& entries, std::string& error) const;
    bool mark_replayed_through(uint64_t index, std::string& error) const;

private:
    NuRaftMetadataStore metadata_store_;
    NuRaftRequestJournal request_journal_;
};
