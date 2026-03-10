#include "nuraft/nuraft_segment_log_store.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nuraft/nuraft_file_store.h"

namespace {

constexpr size_t kRecordHeaderSize = sizeof(uint64_t) + sizeof(uint64_t);

enum class LogScanFailureKind {
    kNone,
    kTruncatedTail,
    kInvalid,
};

template <typename T>
void append_le(std::string& out, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
    }
}

template <typename T>
bool read_le(std::string_view bytes, size_t offset, T& value) {
    if (offset + sizeof(T) > bytes.size()) {
        return false;
    }

    using UnsignedT = typename std::make_unsigned<T>::type;
    UnsignedT result = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        result |= static_cast<UnsignedT>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8);
    }
    value = static_cast<T>(result);
    return true;
}

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

bool scan_log_bytes(std::string_view bytes,
                    std::vector<NuRaftLogEntry>* entries,
                    uint64_t& next_index,
                    size_t& valid_prefix,
                    LogScanFailureKind& failure_kind,
                    std::string& error) {
    size_t offset = 0;
    uint64_t expected_index = 1;
    valid_prefix = 0;
    failure_kind = LogScanFailureKind::kNone;
    while (offset < bytes.size()) {
        uint64_t index = 0;
        uint64_t payload_size = 0;
        if (!read_le<uint64_t>(bytes, offset, index) ||
            !read_le<uint64_t>(bytes, offset + sizeof(uint64_t), payload_size)) {
            failure_kind = LogScanFailureKind::kTruncatedTail;
            error = "NuRaft segment log is truncated before a record header is complete";
            return false;
        }
        offset += kRecordHeaderSize;

        if (payload_size > bytes.size() - offset) {
            failure_kind = LogScanFailureKind::kTruncatedTail;
            error = "NuRaft segment log contains a truncated record payload";
            return false;
        }

        if (index != expected_index) {
            failure_kind = LogScanFailureKind::kInvalid;
            error = "NuRaft segment log contains a non-contiguous index";
            return false;
        }

        NuRaftRequestEnvelope envelope;
        if (!NuRaftRequestEnvelope::deserialize(bytes.substr(offset, payload_size), envelope, error)) {
            failure_kind = LogScanFailureKind::kInvalid;
            error = "NuRaft segment log contains an invalid request envelope: " + error;
            return false;
        }

        if (entries != nullptr) {
            entries->push_back({index, envelope});
        }

        offset += static_cast<size_t>(payload_size);
        valid_prefix = offset;
        expected_index = index + 1;
    }

    next_index = expected_index;
    error.clear();
    return true;
}

void close_fd_if_open(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

}  // namespace

bool NuRaftLogEntry::operator==(const NuRaftLogEntry& other) const {
    return index == other.index &&
           envelope.version() == other.envelope.version() &&
           envelope.payload_encoding() == other.envelope.payload_encoding() &&
           envelope.flags() == other.envelope.flags() &&
           envelope.request_json() == other.envelope.request_json();
}

NuRaftSegmentLogStore::NuRaftSegmentLogStore(NuRaftStateLayout layout)
    : layout_(std::move(layout)) {}

NuRaftSegmentLogStore::~NuRaftSegmentLogStore() {
    close_fd_if_open(append_fd_);
}

const NuRaftStateLayout& NuRaftSegmentLogStore::layout() const {
    return layout_;
}

uint64_t NuRaftSegmentLogStore::next_index() const {
    return next_index_;
}

bool NuRaftSegmentLogStore::initialize(std::string& error) {
    close_fd_if_open(append_fd_);
    if (!NuRaftFileStore::ensure_layout(layout_, error)) {
        return false;
    }

    if (!std::filesystem::exists(layout_.active_log_segment_file)) {
        next_index_ = 1;
        error.clear();
        return true;
    }

    std::string bytes;
    if (!NuRaftFileStore::read_file(layout_.active_log_segment_file, bytes, error)) {
        return false;
    }

    size_t valid_prefix = 0;
    LogScanFailureKind failure_kind = LogScanFailureKind::kNone;
    return scan_log_bytes(bytes, nullptr, next_index_, valid_prefix, failure_kind, error);
}

bool NuRaftSegmentLogStore::recover_truncated_tail(std::string& error) {
    close_fd_if_open(append_fd_);
    if (!std::filesystem::exists(layout_.active_log_segment_file)) {
        next_index_ = 1;
        error.clear();
        return true;
    }

    std::string bytes;
    if (!NuRaftFileStore::read_file(layout_.active_log_segment_file, bytes, error)) {
        return false;
    }

    uint64_t recovered_next_index = 1;
    size_t valid_prefix = 0;
    LogScanFailureKind failure_kind = LogScanFailureKind::kNone;
    if (scan_log_bytes(bytes, nullptr, recovered_next_index, valid_prefix, failure_kind, error)) {
        next_index_ = recovered_next_index;
        return true;
    }

    if (failure_kind != LogScanFailureKind::kTruncatedTail) {
        return false;
    }

    if (!NuRaftFileStore::write_file_atomically(layout_.active_log_segment_file,
                                                std::string_view(bytes.data(), valid_prefix),
                                                error)) {
        return false;
    }

    std::string recovered_bytes;
    if (!NuRaftFileStore::read_file(layout_.active_log_segment_file, recovered_bytes, error)) {
        return false;
    }

    valid_prefix = 0;
    failure_kind = LogScanFailureKind::kNone;
    if (!scan_log_bytes(recovered_bytes, nullptr, next_index_, valid_prefix, failure_kind, error)) {
        return false;
    }

    error.clear();
    return true;
}

bool NuRaftSegmentLogStore::ensure_append_fd(bool& log_already_exists, std::string& error) {
    log_already_exists = std::filesystem::exists(layout_.active_log_segment_file);
    if (append_fd_ >= 0) {
        error.clear();
        return true;
    }

    const std::filesystem::path log_path(layout_.active_log_segment_file);
    std::error_code ec;
    std::filesystem::create_directories(log_path.parent_path(), ec);
    if (ec) {
        error = "Failed to create log dir '" + log_path.parent_path().string() + "': " + ec.message();
        return false;
    }

    append_fd_ = open(log_path.c_str(), O_CREAT | O_APPEND | O_WRONLY, 0644);
    if (append_fd_ < 0) {
        error = "Failed to open segment log '" + log_path.string() + "': " + std::strerror(errno);
        return false;
    }

    error.clear();
    return true;
}

bool NuRaftSegmentLogStore::append(const NuRaftRequestEnvelope& envelope,
                                   uint64_t& index,
                                   std::string& error) {
    const std::string payload = envelope.serialize();
    std::string record;
    record.reserve(kRecordHeaderSize + payload.size());
    append_le<uint64_t>(record, next_index_);
    append_le<uint64_t>(record, payload.size());
    record.append(payload);

    const std::filesystem::path log_path(layout_.active_log_segment_file);
    bool log_already_exists = false;
    if (!ensure_append_fd(log_already_exists, error)) {
        return false;
    }

    size_t written = 0;
    while (written < record.size()) {
        const ssize_t rc = write(append_fd_, record.data() + written, record.size() - written);
        if (rc < 0) {
            error = "Failed to append segment log '" + log_path.string() + "': " + std::strerror(errno);
            return false;
        }
        written += static_cast<size_t>(rc);
    }

    if (fsync(append_fd_) != 0) {
        error = "Failed to fsync segment log '" + log_path.string() + "': " + std::strerror(errno);
        return false;
    }

    if (!log_already_exists && !fsync_directory(log_path.parent_path(), error)) {
        return false;
    }

    index = next_index_;
    ++next_index_;
    error.clear();
    return true;
}

bool NuRaftSegmentLogStore::read_all(std::vector<NuRaftLogEntry>& entries, std::string& error) const {
    entries.clear();
    if (!std::filesystem::exists(layout_.active_log_segment_file)) {
        error.clear();
        return true;
    }

    std::string bytes;
    if (!NuRaftFileStore::read_file(layout_.active_log_segment_file, bytes, error)) {
        return false;
    }

    uint64_t ignored_next_index = 1;
    size_t valid_prefix = 0;
    LogScanFailureKind failure_kind = LogScanFailureKind::kNone;
    return scan_log_bytes(bytes, &entries, ignored_next_index, valid_prefix, failure_kind, error);
}
