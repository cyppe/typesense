#pragma once

#include <iosfwd>

#include "nuraft_state_initializer.h"

class NuRaftReplicationController {
public:
    static constexpr const char* kPrototypeStateRoot = NuRaftStateLayout::kPrototypeRootName;

    int run(int argc, char** argv) const;
    int run(int argc, char** argv, std::ostream& out, std::ostream& err) const;
    int run(const NuRaftPrototypeOptions& options, std::ostream& out, std::ostream& err) const;
};
