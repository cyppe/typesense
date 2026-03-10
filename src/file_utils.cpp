#include "file_utils.h"

#include <filesystem>
#include <string>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

#include "logger.h"

namespace fs = std::filesystem;

bool directory_exists(const std::string& dir_path) {
    struct stat info;
    return stat(dir_path.c_str(), &info) == 0 && (info.st_mode & S_IFDIR);
}

bool create_directory(const std::string& dir_path) {
    std::error_code ec;
    return fs::create_directories(dir_path, ec) || (!ec && fs::exists(dir_path));
}

bool file_exists(const std::string& file_path) {
    struct stat info;
    return stat(file_path.c_str(), &info) == 0 && !(info.st_mode & S_IFDIR);
}

bool copy_dir(const std::string& from_path, const std::string& to_path) {
    struct stat from_stat;
    if (stat(from_path.c_str(), &from_stat) < 0 || !S_ISDIR(from_stat.st_mode)) {
        TS_LOG(WARNING) << "stat " << from_path << " failed";
        return false;
    }

    if (!create_directory(to_path)) {
        TS_LOG(WARNING) << "CreateDirectory " << to_path << " failed";
        return false;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(from_path, ec)) {
        if (ec) {
            TS_LOG(WARNING) << "directory iteration " << from_path << " failed: " << ec.message();
            return false;
        }
        if (!entry.is_regular_file()) {
            continue;
        }

        const fs::path src = entry.path();
        const fs::path dst = fs::path(to_path) / src.filename();
        if (link(src.c_str(), dst.c_str()) != 0) {
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                TS_LOG(WARNING) << "copy " << src.string() << " to " << dst.string() << " failed: " << ec.message();
                return false;
            }
        }
    }

    return true;
}

bool mv_dir(const std::string& from_path, const std::string& to_path) {
    struct stat from_stat;
    if (stat(from_path.c_str(), &from_stat) < 0 || !S_ISDIR(from_stat.st_mode)) {
        TS_LOG(WARNING) << "stat " << from_path << " failed";
        return false;
    }

    if (!create_directory(to_path)) {
        TS_LOG(WARNING) << "CreateDirectory " << to_path << " failed";
        return false;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(from_path, ec)) {
        if (ec) {
            TS_LOG(WARNING) << "directory iteration " << from_path << " failed: " << ec.message();
            return false;
        }

        const fs::path src = entry.path();
        const fs::path dst = fs::path(to_path) / src.filename();
        if (src == fs::path(to_path)) {
            continue;
        }

        fs::rename(src, dst, ec);
        if (ec) {
            TS_LOG(WARNING) << "move " << src.string() << " to " << dst.string() << " failed: " << ec.message();
            return false;
        }
    }

    return true;
}

bool rename_path(const std::string& from_path, const std::string& to_path) {
    std::error_code ec;
    fs::rename(from_path, to_path, ec);
    return !ec;
}

bool delete_path(const std::string& path, bool recursive) {
    std::error_code ec;
    if (recursive) {
        fs::remove_all(path, ec);
    } else {
        fs::remove(path, ec);
    }
    return !ec;
}

bool dir_enum_count(const std::string& path) {
    std::error_code ec;
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        static_cast<void>(entry);
        if (ec) {
            return false;
        }
        ++count;
    }

    return count;
}
