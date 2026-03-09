#pragma once

#include <string>
#include <string_view>

#include "nuraft_state_layout.h"

class NuRaftFileStore {
public:
    static bool ensure_layout(const NuRaftStateLayout& layout, std::string& error);
    static bool write_file_atomically(const std::string& path, std::string_view data, std::string& error);
    static bool read_file(const std::string& path, std::string& data, std::string& error);
};
