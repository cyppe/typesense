#pragma once

#include "nuraft_state_layout.h"

class NuRaftReplicationController {
public:
    static constexpr const char* kPrototypeStateRoot = NuRaftStateLayout::kPrototypeRootName;

    int run() const;
};
