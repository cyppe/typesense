#include "nuraft/nuraft_replication_controller.h"

int main(int argc, char** argv) {
    NuRaftReplicationController controller;
    return controller.run(argc, argv);
}
