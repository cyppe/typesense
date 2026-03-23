#pragma once

#include <string_view>

struct typesense_build_info_t {
    std::string_view version;
    std::string_view git_sha;
    std::string_view git_short_sha;
    std::string_view git_ref;
    std::string_view git_exact_tag;
    std::string_view git_tree_status;
};

const typesense_build_info_t& get_typesense_build_info();
