#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nuraft_request_envelope.h"
#include "nuraft_state_layout.h"

struct NuRaftLogEntry {
    uint64_t index = 0;
    NuRaftRequestEnvelope envelope;

    bool operator==(const NuRaftLogEntry& other) const;
};

class NuRaftSegmentLogStore {
public:
    explicit NuRaftSegmentLogStore(NuRaftStateLayout layout);
    ~NuRaftSegmentLogStore();

    const NuRaftStateLayout& layout() const;
    uint64_t next_index() const;

    bool initialize(std::string& error);
    bool recover_truncated_tail(std::string& error);
    bool append(const NuRaftRequestEnvelope& envelope, uint64_t& index, std::string& error);
    bool read_all(std::vector<NuRaftLogEntry>& entries, std::string& error) const;

private:
    bool ensure_append_fd(bool& log_already_exists, std::string& error);

    NuRaftStateLayout layout_;
    uint64_t next_index_ = 1;
    int append_fd_ = -1;
};
