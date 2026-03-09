#include "nuraft/nuraft_request_journal.h"

#include "json.hpp"

namespace {

std::string normalize_request_json(const std::string& request_json) {
    try {
        const nlohmann::json parsed = nlohmann::json::parse(request_json);
        if (parsed.is_object() &&
            parsed.contains("route_hash") &&
            parsed.contains("params") &&
            parsed.contains("body")) {
            return request_json;
        }
    } catch (const std::exception&) {
    }

    nlohmann::json wrapped = {
        {"route_hash", 0},
        {"params", nlohmann::json::object()},
        {"first_chunk_aggregate", true},
        {"last_chunk_aggregate", false},
        {"body", request_json},
        {"metadata", ""},
        {"start_ts", 0},
        {"log_index", 0},
        {"is_binary_body", false},
    };
    return wrapped.dump();
}

}  // namespace

NuRaftRequestJournal::NuRaftRequestJournal(NuRaftStateLayout layout)
    : log_store_(std::move(layout)) {}

const NuRaftStateLayout& NuRaftRequestJournal::layout() const {
    return log_store_.layout();
}

bool NuRaftRequestJournal::initialize(std::string& error) {
    return log_store_.initialize(error);
}

bool NuRaftRequestJournal::recover_truncated_tail(std::string& error) {
    return log_store_.recover_truncated_tail(error);
}

bool NuRaftRequestJournal::append_request_json(const std::string& request_json,
                                              uint64_t& index,
                                              std::string& error) {
    if (request_json.empty()) {
        error = "NuRaft request journal cannot append an empty request payload";
        return false;
    }

    return log_store_.append(NuRaftRequestEnvelope(normalize_request_json(request_json)), index, error);
}

bool NuRaftRequestJournal::replay(std::vector<NuRaftLogEntry>& entries, std::string& error) const {
    return log_store_.read_all(entries, error);
}
