#pragma once

#include <cstdint>
#include <string>

#include "nuraft_file_store.h"

struct NuRaftIdentity {
    static constexpr uint32_t kCurrentFormatVersion = 1;

    uint32_t format_version = kCurrentFormatVersion;
    int32_t server_id = 0;
    std::string peer_endpoint;
    int32_t api_port = 0;

    bool operator==(const NuRaftIdentity& other) const;
};

class NuRaftMetadataStore {
public:
    explicit NuRaftMetadataStore(NuRaftStateLayout layout);

    const NuRaftStateLayout& layout() const;
    bool initialize(std::string& error) const;
    bool write_identity(const NuRaftIdentity& identity, std::string& error) const;
    bool read_identity(NuRaftIdentity& identity, std::string& error) const;

private:
    NuRaftStateLayout layout_;
};
