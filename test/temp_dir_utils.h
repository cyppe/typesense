#pragma once

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace typesense_test {

inline std::string sanitize_path_component(const std::string& value) {
    std::string sanitized;
    sanitized.reserve(value.size());

    for (char ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        sanitized.push_back(std::isalnum(uch) ? ch : '_');
    }

    if (sanitized.empty()) {
        return "test";
    }

    return sanitized;
}

inline std::filesystem::path base_temp_path() {
    const char* configured = std::getenv("TYPESENSE_TEST_TMPDIR");
    if (configured != nullptr && configured[0] != '\0') {
        return configured;
    }

    const char* bazel_tmpdir = std::getenv("TEST_TMPDIR");
    if (bazel_tmpdir != nullptr && bazel_tmpdir[0] != '\0') {
        return std::filesystem::path(bazel_tmpdir) / "typesense_test";
    }

    return std::filesystem::path("/tmp/typesense_test");
}

inline std::string test_models_dir() {
    static const std::string value = []() {
        const char* configured = std::getenv("TYPESENSE_TEST_MODELS_DIR");
        if (configured != nullptr && configured[0] != '\0') {
            return std::string(configured);
        }

        const char* bazel_tmpdir = std::getenv("TEST_TMPDIR");
        if (bazel_tmpdir != nullptr && bazel_tmpdir[0] != '\0') {
            return (base_temp_path() / "models").string();
        }

        std::ostringstream process_dir;
        process_dir << "pid-" << getpid();
        return (base_temp_path() / "models" / process_dir.str()).string();
    }();

    return value;
}

inline std::string make_test_temp_dir(const std::string& test_name) {
    static std::atomic<uint64_t> sequence{0};

    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::ostringstream suffix;
    suffix << getpid() << "-" << now_ns << "-" << sequence.fetch_add(1, std::memory_order_relaxed);

    const auto path = base_temp_path() / sanitize_path_component(test_name) / suffix.str();
    return path.string();
}

inline void reset_test_temp_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
}

inline void cleanup_test_temp_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

} // namespace typesense_test
