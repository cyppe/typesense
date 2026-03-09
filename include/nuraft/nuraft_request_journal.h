#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nuraft_segment_log_store.h"

class NuRaftRequestJournal {
public:
    explicit NuRaftRequestJournal(NuRaftStateLayout layout);

    const NuRaftStateLayout& layout() const;

    bool initialize(std::string& error);
    bool recover_truncated_tail(std::string& error);
    bool append_request_json(const std::string& request_json, uint64_t& index, std::string& error);
    bool replay(std::vector<NuRaftLogEntry>& entries, std::string& error) const;

private:
    NuRaftSegmentLogStore log_store_;
};
