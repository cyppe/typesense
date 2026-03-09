#pragma once

#include <cstdint>
#include <string>

#include "nuraft_metadata_store.h"

class NuRaftKvStateMachineSink;

struct NuRaftSnapshotDescriptor {
    static constexpr uint32_t kCurrentFormatVersion = 1;

    uint32_t format_version = kCurrentFormatVersion;
    std::string snapshot_id;
    uint64_t last_log_index = 0;
    uint64_t last_applied_index = 0;

    bool operator==(const NuRaftSnapshotDescriptor& other) const;
};

class NuRaftSnapshotCoordinator {
public:
    explicit NuRaftSnapshotCoordinator(NuRaftStateLayout layout);

    bool create_snapshot(const std::string& export_path,
                         const NuRaftKvStateMachineSink* kv_sink,
                         NuRaftSnapshotDescriptor& descriptor,
                         std::string& error) const;
    bool install_snapshot(const std::string& snapshot_path,
                          NuRaftSnapshotDescriptor& descriptor,
                          std::string& error) const;
    bool read_last_snapshot(NuRaftSnapshotDescriptor& descriptor, std::string& error) const;

private:
    NuRaftStateLayout layout_;
};
