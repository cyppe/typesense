#pragma once

class NuRaftReplicationController {
public:
    static constexpr const char* kPrototypeStateRoot = "nuraft-prototype";

    int run() const;
};
