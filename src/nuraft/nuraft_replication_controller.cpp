#include "nuraft/nuraft_replication_controller.h"

#include <iostream>

int NuRaftReplicationController::run() const {
    std::cerr
        << "typesense-server-nuraft-prototype is an isolated compile-only shell. "
        << "NuRaft runtime wiring is not implemented yet; future state will live under '"
        << kPrototypeStateRoot << "'.\n";
    return 0;
}
