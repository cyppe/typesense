#include "nuraft/nuraft_state_layout.h"

#include <string>

namespace {

std::string join_path(const std::string& base, const std::string& child) {
    if (base.empty()) {
        return child;
    }
    if (base.back() == '/') {
        return base + child;
    }
    return base + "/" + child;
}

}  // namespace

NuRaftStateLayout NuRaftStateLayout::from_data_dir(const std::string& data_dir) {
    return from_state_dir(join_path(data_dir, "state"));
}

NuRaftStateLayout NuRaftStateLayout::from_state_dir(const std::string& state_dir) {
    NuRaftStateLayout layout;
    layout.root_dir = join_path(state_dir, kPrototypeRootName);
    layout.meta_dir = join_path(layout.root_dir, "meta");
    layout.log_dir = join_path(layout.root_dir, "log");
    layout.snapshot_dir = join_path(layout.root_dir, "snapshot");
    layout.identity_file = join_path(layout.meta_dir, "identity.json");
    layout.server_state_file = join_path(layout.meta_dir, "srv_state.bin");
    layout.cluster_config_file = join_path(layout.meta_dir, "cluster_config.bin");
    layout.snapshot_descriptor_file = join_path(layout.meta_dir, "last_snapshot.bin");
    return layout;
}
