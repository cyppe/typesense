#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nuraft_route_classifier.h"
#include "nuraft_segment_log_store.h"

struct NuRaftAppliedRequest {
    uint64_t index = 0;
    uint64_t route_hash = 0;
    NuRaftRouteKind route_kind = NuRaftRouteKind::kUnknown;
    std::map<std::string, std::string> params;
    std::string metadata;
    std::string body;
    bool first_chunk_aggregate = true;
    bool last_chunk_aggregate = false;
    uint64_t start_ts = 0;
    int64_t log_index = 0;
    bool is_binary_body = false;

    bool operator==(const NuRaftAppliedRequest& other) const;
    std::string encode() const;

    static bool from_log_entry(const NuRaftLogEntry& entry,
                               NuRaftAppliedRequest& applied_request,
                               std::string& error);
    static bool decode(const std::string& encoded,
                       NuRaftAppliedRequest& applied_request,
                       std::string& error);
};

class NuRaftAppliedRequestStore {
public:
    explicit NuRaftAppliedRequestStore(NuRaftStateLayout layout);

    bool append_all(const std::vector<NuRaftAppliedRequest>& requests, std::string& error) const;
    bool read_all(std::vector<NuRaftAppliedRequest>& requests, std::string& error) const;

private:
    NuRaftStateLayout layout_;
};
