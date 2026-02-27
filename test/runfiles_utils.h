#pragma once

#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

inline bool path_ends_with(const std::string& value, const std::string& suffix) {
    if (suffix.size() > value.size()) {
        return false;
    }

    return value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline void append_unique_path(std::vector<std::filesystem::path>& paths, const std::filesystem::path& candidate) {
    if (candidate.empty()) {
        return;
    }

    const auto normalized = candidate.lexically_normal();
    for (const auto& path : paths) {
        if (path == normalized) {
            return;
        }
    }

    paths.push_back(normalized);
}

inline void append_unique_suffix(std::vector<std::string>& suffixes, const std::string& candidate) {
    if (candidate.empty()) {
        return;
    }

    for (const auto& suffix : suffixes) {
        if (suffix == candidate) {
            return;
        }
    }

    suffixes.push_back(candidate);
}

inline std::string resolve_in_external_repo_aliases(const std::filesystem::path& root, const std::string& suffix) {
    const auto separator_pos = suffix.find('/');
    if (separator_pos == std::string::npos || separator_pos == 0 || separator_pos == suffix.size() - 1) {
        return {};
    }

    const std::string repo_name = suffix.substr(0, separator_pos);
    const std::string relative_path = suffix.substr(separator_pos + 1);

    std::error_code ec;

    const auto direct_repo_candidate = root / repo_name / relative_path;
    if (std::filesystem::exists(direct_repo_candidate, ec)) {
        return direct_repo_candidate.string();
    }
    ec.clear();

    const auto external_root = root / "external";
    if (!std::filesystem::exists(external_root, ec)) {
        return {};
    }
    ec.clear();

    for (std::filesystem::directory_iterator it(external_root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end;
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const auto& entry = *it;
        const auto entry_name = entry.path().filename().string();
        if (entry_name != repo_name && !path_ends_with(entry_name, "+" + repo_name)) {
            continue;
        }

        const auto aliased_candidate = entry.path() / relative_path;
        if (std::filesystem::exists(aliased_candidate, ec)) {
            return aliased_candidate.string();
        }
        ec.clear();
    }

    return {};
}

inline std::string resolve_by_recursive_suffix_scan(const std::filesystem::path& root, const std::vector<std::string>& suffixes) {
    if (suffixes.empty()) {
        return {};
    }

    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        return {};
    }
    ec.clear();

    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end;
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const auto& entry = *it;
        if (!entry.is_regular_file(ec) && !entry.is_symlink(ec)) {
            ec.clear();
            continue;
        }
        ec.clear();

        const auto entry_path = entry.path().generic_string();
        for (const auto& suffix : suffixes) {
            if (path_ends_with(entry_path, suffix)) {
                return entry.path().string();
            }
        }
    }

    return {};
}

inline std::string resolve_test_path(const std::initializer_list<std::string>& candidates) {
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    std::vector<std::string> suffixes;
    for (const auto& candidate : candidates) {
        const auto normalized = std::filesystem::path(candidate).generic_string();
        append_unique_suffix(suffixes, normalized);

        const auto external_pos = normalized.find("/external/");
        if (external_pos != std::string::npos) {
            append_unique_suffix(suffixes, normalized.substr(external_pos + std::string("/external/").size()));
            continue;
        }

        if (normalized.rfind("external/", 0) == 0) {
            append_unique_suffix(suffixes, normalized.substr(std::string("external/").size()));
        }
    }

    std::vector<std::filesystem::path> search_roots;
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir != nullptr) {
        const std::filesystem::path runfiles_root(test_srcdir);
        append_unique_path(search_roots, runfiles_root);

        for (const auto& candidate : candidates) {
            const auto candidate_path = runfiles_root / candidate;
            if (std::filesystem::exists(candidate_path)) {
                return candidate_path.string();
            }
        }

        const char* test_workspace = std::getenv("TEST_WORKSPACE");
        if (test_workspace != nullptr) {
            const std::filesystem::path workspace_root = runfiles_root / test_workspace;
            append_unique_path(search_roots, workspace_root);

            for (const auto& candidate : candidates) {
                const auto candidate_path = workspace_root / candidate;
                if (std::filesystem::exists(candidate_path)) {
                    return candidate_path.string();
                }
            }
        }
    }

    append_unique_path(search_roots, std::filesystem::current_path());

    for (const auto& root : search_roots) {
        for (const auto& suffix : suffixes) {
            const auto path = resolve_in_external_repo_aliases(root, suffix);
            if (!path.empty()) {
                return path;
            }
        }
    }

    for (const auto& root : search_roots) {
        const auto path = resolve_by_recursive_suffix_scan(root, suffixes);
        if (!path.empty()) {
            return path;
        }
    }

    return candidates.begin()[0];
}
