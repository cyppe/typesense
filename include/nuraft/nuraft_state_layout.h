#pragma once

#include <string>

struct NuRaftStateLayout {
    static constexpr const char* kPrototypeRootName = "nuraft-prototype";

    std::string root_dir;
    std::string meta_dir;
    std::string log_dir;
    std::string snapshot_dir;

    std::string identity_file;
    std::string bootstrap_config_file;
    std::string active_log_segment_file;
    std::string server_state_file;
    std::string cluster_config_file;
    std::string snapshot_descriptor_file;

    static NuRaftStateLayout from_data_dir(const std::string& data_dir);
    static NuRaftStateLayout from_state_dir(const std::string& state_dir);
};
