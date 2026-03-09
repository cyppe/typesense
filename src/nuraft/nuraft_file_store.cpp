#include "nuraft/nuraft_file_store.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool fsync_directory(const std::filesystem::path& directory, std::string& error) {
    const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        error = std::string("Failed to open directory '") + directory.string() + "': " + std::strerror(errno);
        return false;
    }

    const int rc = fsync(fd);
    const int saved_errno = errno;
    close(fd);
    if (rc != 0) {
        error = std::string("Failed to fsync directory '") + directory.string() + "': " + std::strerror(saved_errno);
        return false;
    }

    return true;
}

std::string make_temp_path(const std::string& path) {
    return path + ".tmp." + std::to_string(getpid());
}

}  // namespace

bool NuRaftFileStore::ensure_layout(const NuRaftStateLayout& layout, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(layout.meta_dir, ec);
    if (ec) {
        error = "Failed to create meta dir '" + layout.meta_dir + "': " + ec.message();
        return false;
    }

    std::filesystem::create_directories(layout.log_dir, ec);
    if (ec) {
        error = "Failed to create log dir '" + layout.log_dir + "': " + ec.message();
        return false;
    }

    std::filesystem::create_directories(layout.snapshot_dir, ec);
    if (ec) {
        error = "Failed to create snapshot dir '" + layout.snapshot_dir + "': " + ec.message();
        return false;
    }

    error.clear();
    return true;
}

bool NuRaftFileStore::write_file_atomically(const std::string& path,
                                            std::string_view data,
                                            std::string& error) {
    const std::filesystem::path target(path);
    const std::filesystem::path parent = target.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        error = "Failed to create parent dir '" + parent.string() + "': " + ec.message();
        return false;
    }

    const std::string temp_path = make_temp_path(path);
    const int fd = open(temp_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        error = "Failed to open temp file '" + temp_path + "': " + std::strerror(errno);
        return false;
    }

    size_t written = 0;
    while (written < data.size()) {
        const ssize_t rc = write(fd, data.data() + written, data.size() - written);
        if (rc < 0) {
            const int saved_errno = errno;
            close(fd);
            std::filesystem::remove(temp_path, ec);
            error = "Failed to write temp file '" + temp_path + "': " + std::strerror(saved_errno);
            return false;
        }
        written += static_cast<size_t>(rc);
    }

    if (fsync(fd) != 0) {
        const int saved_errno = errno;
        close(fd);
        std::filesystem::remove(temp_path, ec);
        error = "Failed to fsync temp file '" + temp_path + "': " + std::strerror(saved_errno);
        return false;
    }

    if (close(fd) != 0) {
        const int saved_errno = errno;
        std::filesystem::remove(temp_path, ec);
        error = "Failed to close temp file '" + temp_path + "': " + std::strerror(saved_errno);
        return false;
    }

    std::filesystem::rename(temp_path, target, ec);
    if (ec) {
        std::filesystem::remove(temp_path, ec);
        error = "Failed to rename temp file into place for '" + path + "': " + ec.message();
        return false;
    }

    if (!fsync_directory(parent, error)) {
        return false;
    }

    error.clear();
    return true;
}

bool NuRaftFileStore::read_file(const std::string& path, std::string& data, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        error = "Failed to open file '" + path + "' for reading";
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "Failed while reading file '" + path + "'";
        return false;
    }

    data = buffer.str();
    error.clear();
    return true;
}
