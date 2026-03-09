#pragma once

#include <iosfwd>
#include <string>

#include "nuraft_prototype_state_machine.h"
#include "nuraft_request_journal.h"
#include "nuraft_state_initializer.h"

struct NuRaftPrototypeRunOptions {
    NuRaftPrototypeOptions startup_options;
    std::string append_request_json;
    bool replay_log = false;
    bool apply_pending = false;
    bool recover_truncated_tail = false;
    bool auto_apply_pending = false;
};

class NuRaftReplicationController {
public:
    static constexpr const char* kPrototypeStateRoot = NuRaftStateLayout::kPrototypeRootName;

    int run(int argc, char** argv) const;
    int run(int argc, char** argv, std::ostream& out, std::ostream& err) const;
    int run(const NuRaftPrototypeOptions& options, std::ostream& out, std::ostream& err) const;
    int run(const NuRaftPrototypeRunOptions& options, std::ostream& out, std::ostream& err) const;
};
