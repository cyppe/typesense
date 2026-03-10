#include "nuraft/nuraft_metadata_store.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "json.hpp"

bool NuRaftIdentity::operator==(const NuRaftIdentity& other) const {
    return format_version == other.format_version &&
           server_id == other.server_id &&
           peer_endpoint == other.peer_endpoint &&
           api_port == other.api_port;
}

bool NuRaftBootstrapConfig::operator==(const NuRaftBootstrapConfig& other) const {
    return format_version == other.format_version &&
           group_id == other.group_id &&
           self == other.self &&
           peers == other.peers &&
           api_uses_ssl == other.api_uses_ssl;
}

bool NuRaftReplayProgress::operator==(const NuRaftReplayProgress& other) const {
    return format_version == other.format_version &&
           last_applied_index == other.last_applied_index;
}

namespace {

void close_fd_if_open(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

nlohmann::json encode_peer(const NuRaftPeerAddress& peer) {
    return {
        {"host", peer.host},
        {"peer_port", peer.peer_port},
        {"api_port", peer.api_port},
    };
}

bool decode_peer(const nlohmann::json& encoded, NuRaftPeerAddress& peer, std::string& error) {
    if (!encoded.is_object() ||
        !encoded.contains("host") || !encoded["host"].is_string() ||
        !encoded.contains("peer_port") || !encoded["peer_port"].is_number_unsigned() ||
        !encoded.contains("api_port") || !encoded["api_port"].is_number_unsigned()) {
        error = "NuRaft bootstrap metadata is missing required peer fields";
        return false;
    }

    peer.host = encoded["host"].get<std::string>();
    peer.peer_port = encoded["peer_port"].get<uint32_t>();
    peer.api_port = encoded["api_port"].get<uint32_t>();
    return true;
}

bool validate_bootstrap_config(const NuRaftBootstrapConfig& config, std::string& error) {
    if (config.group_id.empty()) {
        error = "NuRaft bootstrap config must include a group id";
        return false;
    }

    if (config.self.host.empty() || config.self.peer_port == 0 || config.self.api_port == 0) {
        error = "NuRaft bootstrap config must include a complete self address";
        return false;
    }

    if (config.peers.empty()) {
        error = "NuRaft bootstrap config must include at least one peer";
        return false;
    }

    std::set<int32_t> seen_server_ids;
    bool found_self = false;
    for (const auto& peer : config.peers) {
        if (peer.host.empty() || peer.peer_port == 0 || peer.api_port == 0) {
            error = "NuRaft bootstrap config contains an incomplete peer address";
            return false;
        }

        const int32_t server_id = peer.server_id();
        if (!seen_server_ids.insert(server_id).second) {
            error = "NuRaft bootstrap config contains duplicate server ids";
            return false;
        }

        if (peer == config.self) {
            found_self = true;
        }
    }

    if (!found_self) {
        error = "NuRaft bootstrap config peer list must include self";
        return false;
    }

    error.clear();
    return true;
}

constexpr uint32_t kReplayProgressMagic = 0x52505247;  // RPRG
constexpr size_t kReplayProgressSlotSize =
    sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t);
constexpr size_t kReplayProgressFileSize = kReplayProgressSlotSize * 2;

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

uint32_t replay_progress_checksum(std::string_view bytes) {
    uint32_t checksum = 2166136261u;
    for (const char ch : bytes) {
        checksum ^= static_cast<uint8_t>(ch);
        checksum *= 16777619u;
    }
    return checksum;
}

std::string encode_replay_progress_slot(uint64_t sequence, const NuRaftReplayProgress& progress) {
    std::string slot;
    slot.reserve(kReplayProgressSlotSize);
    append_le<uint32_t>(slot, kReplayProgressMagic);
    append_le<uint32_t>(slot, progress.format_version);
    append_le<uint64_t>(slot, sequence);
    append_le<uint64_t>(slot, progress.last_applied_index);
    append_le<uint32_t>(slot, replay_progress_checksum(slot));
    return slot;
}

bool decode_replay_progress_slot(std::string_view bytes,
                                 uint64_t& sequence,
                                 NuRaftReplayProgress& progress) {
    if (bytes.size() != kReplayProgressSlotSize) {
        return false;
    }

    uint32_t magic = 0;
    uint32_t format_version = 0;
    uint64_t last_applied_index = 0;
    uint32_t checksum = 0;
    if (!read_le<uint32_t>(bytes, 0, magic) ||
        !read_le<uint32_t>(bytes, sizeof(uint32_t), format_version) ||
        !read_le<uint64_t>(bytes, sizeof(uint32_t) * 2, sequence) ||
        !read_le<uint64_t>(bytes, sizeof(uint32_t) * 2 + sizeof(uint64_t), last_applied_index) ||
        !read_le<uint32_t>(bytes, kReplayProgressSlotSize - sizeof(uint32_t), checksum)) {
        return false;
    }

    if (magic != kReplayProgressMagic) {
        return false;
    }

    const std::string_view checksum_bytes = bytes.substr(0, kReplayProgressSlotSize - sizeof(uint32_t));
    if (replay_progress_checksum(checksum_bytes) != checksum) {
        return false;
    }

    progress.format_version = format_version;
    progress.last_applied_index = last_applied_index;
    return true;
}

bool read_binary_replay_progress(const std::string& encoded,
                                 NuRaftReplayProgress& progress,
                                 uint64_t& sequence,
                                 std::string& error) {
    if (encoded.size() < kReplayProgressSlotSize) {
        error = "NuRaft replay progress metadata is truncated";
        return false;
    }

    bool found = false;
    uint64_t best_sequence = 0;
    NuRaftReplayProgress best_progress;
    for (size_t slot = 0; slot + kReplayProgressSlotSize <= encoded.size() && slot < kReplayProgressFileSize;
         slot += kReplayProgressSlotSize) {
        uint64_t slot_sequence = 0;
        NuRaftReplayProgress slot_progress;
        if (!decode_replay_progress_slot(std::string_view(encoded).substr(slot, kReplayProgressSlotSize),
                                         slot_sequence,
                                         slot_progress)) {
            continue;
        }

        if (!found || slot_sequence >= best_sequence) {
            best_sequence = slot_sequence;
            best_progress = slot_progress;
            found = true;
        }
    }

    if (!found) {
        error = "NuRaft replay progress metadata does not contain a valid slot";
        return false;
    }

    progress = best_progress;
    sequence = best_sequence;
    error.clear();
    return true;
}

}  // namespace

NuRaftMetadataStore::NuRaftMetadataStore(NuRaftStateLayout layout)
    : layout_(std::move(layout)) {}

NuRaftMetadataStore::~NuRaftMetadataStore() {
    close_fd_if_open(replay_progress_fd_);
}

const NuRaftStateLayout& NuRaftMetadataStore::layout() const {
    return layout_;
}

bool NuRaftMetadataStore::initialize(std::string& error) const {
    return NuRaftFileStore::ensure_layout(layout_, error);
}

bool NuRaftMetadataStore::write_identity(const NuRaftIdentity& identity, std::string& error) const {
    nlohmann::json encoded = {
        {"format_version", identity.format_version},
        {"server_id", identity.server_id},
        {"peer_endpoint", identity.peer_endpoint},
        {"api_port", identity.api_port},
    };
    return NuRaftFileStore::write_file_atomically(layout_.identity_file, encoded.dump(), error);
}

bool NuRaftMetadataStore::read_identity(NuRaftIdentity& identity, std::string& error) const {
    std::string encoded;
    if (!NuRaftFileStore::read_file(layout_.identity_file, encoded, error)) {
        return false;
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(encoded);
    } catch (const std::exception& e) {
        error = std::string("Failed to parse NuRaft identity metadata: ") + e.what();
        return false;
    }

    if (!parsed.contains("format_version") || !parsed["format_version"].is_number_unsigned() ||
        !parsed.contains("server_id") || !parsed["server_id"].is_number_integer() ||
        !parsed.contains("peer_endpoint") || !parsed["peer_endpoint"].is_string() ||
        !parsed.contains("api_port") || !parsed["api_port"].is_number_integer()) {
        error = "NuRaft identity metadata is missing required fields";
        return false;
    }

    identity.format_version = parsed["format_version"].get<uint32_t>();
    identity.server_id = parsed["server_id"].get<int32_t>();
    identity.peer_endpoint = parsed["peer_endpoint"].get<std::string>();
    identity.api_port = parsed["api_port"].get<int32_t>();
    error.clear();
    return true;
}

bool NuRaftMetadataStore::write_bootstrap_config(const NuRaftBootstrapConfig& config, std::string& error) const {
    if (!validate_bootstrap_config(config, error)) {
        return false;
    }

    nlohmann::json peers = nlohmann::json::array();
    for (const auto& peer : config.peers) {
        peers.push_back(encode_peer(peer));
    }

    nlohmann::json encoded = {
        {"format_version", config.format_version},
        {"group_id", config.group_id},
        {"self", encode_peer(config.self)},
        {"peers", peers},
        {"api_uses_ssl", config.api_uses_ssl},
    };
    return NuRaftFileStore::write_file_atomically(layout_.bootstrap_config_file, encoded.dump(), error);
}

bool NuRaftMetadataStore::read_bootstrap_config(NuRaftBootstrapConfig& config, std::string& error) const {
    std::string encoded;
    if (!NuRaftFileStore::read_file(layout_.bootstrap_config_file, encoded, error)) {
        return false;
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(encoded);
    } catch (const std::exception& e) {
        error = std::string("Failed to parse NuRaft bootstrap metadata: ") + e.what();
        return false;
    }

    if (!parsed.contains("format_version") || !parsed["format_version"].is_number_unsigned() ||
        !parsed.contains("group_id") || !parsed["group_id"].is_string() ||
        !parsed.contains("self") ||
        !parsed.contains("peers") || !parsed["peers"].is_array() ||
        !parsed.contains("api_uses_ssl") || !parsed["api_uses_ssl"].is_boolean()) {
        error = "NuRaft bootstrap metadata is missing required fields";
        return false;
    }

    NuRaftBootstrapConfig loaded;
    loaded.format_version = parsed["format_version"].get<uint32_t>();
    loaded.group_id = parsed["group_id"].get<std::string>();
    loaded.api_uses_ssl = parsed["api_uses_ssl"].get<bool>();

    if (!decode_peer(parsed["self"], loaded.self, error)) {
        return false;
    }

    for (const auto& peer_json : parsed["peers"]) {
        NuRaftPeerAddress peer;
        if (!decode_peer(peer_json, peer, error)) {
            return false;
        }
        loaded.peers.push_back(peer);
    }

    if (!validate_bootstrap_config(loaded, error)) {
        return false;
    }

    config = std::move(loaded);
    error.clear();
    return true;
}

bool NuRaftMetadataStore::write_replay_progress(const NuRaftReplayProgress& progress, std::string& error) const {
    std::lock_guard<std::mutex> lock(replay_progress_mutex_);
    bool file_already_exists = false;
    if (!ensure_replay_progress_handle(file_already_exists, error)) {
        return false;
    }

    const uint64_t next_sequence = replay_progress_sequence_ + 1;
    const std::string slot = encode_replay_progress_slot(next_sequence, progress);
    const off_t offset = static_cast<off_t>(((next_sequence - 1) % 2) * kReplayProgressSlotSize);
    size_t written = 0;
    while (written < slot.size()) {
        const ssize_t rc =
            pwrite(replay_progress_fd_, slot.data() + written, slot.size() - written, offset + written);
        if (rc < 0) {
            error = "Failed to write replay progress file '" + layout_.replay_progress_file + "': " +
                    std::strerror(errno);
            return false;
        }
        written += static_cast<size_t>(rc);
    }

    if (fsync(replay_progress_fd_) != 0) {
        error = "Failed to fsync replay progress file '" + layout_.replay_progress_file + "': " +
                std::strerror(errno);
        return false;
    }

    replay_progress_sequence_ = next_sequence;
    replay_progress_initialized_ = true;

    if (!file_already_exists) {
        const std::filesystem::path parent = std::filesystem::path(layout_.replay_progress_file).parent_path();
        const int dir_fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
        if (dir_fd < 0) {
            error = "Failed to open replay progress dir '" + parent.string() + "': " + std::strerror(errno);
            return false;
        }
        if (fsync(dir_fd) != 0) {
            const int saved_errno = errno;
            close(dir_fd);
            error = "Failed to fsync replay progress dir '" + parent.string() + "': " + std::strerror(saved_errno);
            return false;
        }
        close(dir_fd);
    }

    error.clear();
    return true;
}

bool NuRaftMetadataStore::read_replay_progress(NuRaftReplayProgress& progress, std::string& error) const {
    std::string encoded;
    if (!NuRaftFileStore::read_file(layout_.replay_progress_file, encoded, error)) {
        return false;
    }

    if (!encoded.empty() && encoded.front() != '{') {
        uint64_t sequence = 0;
        return read_binary_replay_progress(encoded, progress, sequence, error);
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(encoded);
    } catch (const std::exception& e) {
        error = std::string("Failed to parse NuRaft replay progress metadata: ") + e.what();
        return false;
    }

    if (!parsed.contains("format_version") || !parsed["format_version"].is_number_unsigned() ||
        !parsed.contains("last_applied_index") || !parsed["last_applied_index"].is_number_unsigned()) {
        error = "NuRaft replay progress metadata is missing required fields";
        return false;
    }

    progress.format_version = parsed["format_version"].get<uint32_t>();
    progress.last_applied_index = parsed["last_applied_index"].get<uint64_t>();
    error.clear();
    return true;
}

bool NuRaftMetadataStore::ensure_replay_progress_handle(bool& file_already_exists, std::string& error) const {
    if (!NuRaftFileStore::ensure_layout(layout_, error)) {
        return false;
    }

    file_already_exists = std::filesystem::exists(layout_.replay_progress_file);
    if (replay_progress_fd_ >= 0 && replay_progress_initialized_) {
        error.clear();
        return true;
    }

    if (replay_progress_fd_ < 0) {
        replay_progress_fd_ = open(layout_.replay_progress_file.c_str(), O_CREAT | O_RDWR, 0644);
        if (replay_progress_fd_ < 0) {
            error = "Failed to open replay progress file '" + layout_.replay_progress_file + "': " +
                    std::strerror(errno);
            return false;
        }
    }

    uint64_t current_sequence = 0;
    if (file_already_exists) {
        std::string encoded;
        if (!NuRaftFileStore::read_file(layout_.replay_progress_file, encoded, error)) {
            return false;
        }

        if (!encoded.empty()) {
            NuRaftReplayProgress current_progress;
            if (encoded.front() == '{') {
                if (!read_replay_progress(current_progress, error)) {
                    return false;
                }
            } else if (!read_binary_replay_progress(encoded, current_progress, current_sequence, error)) {
                return false;
            }
        }
    }

    if (ftruncate(replay_progress_fd_, static_cast<off_t>(kReplayProgressFileSize)) != 0) {
        error = "Failed to resize replay progress file '" + layout_.replay_progress_file + "': " +
                std::strerror(errno);
        return false;
    }

    replay_progress_sequence_ = current_sequence;
    replay_progress_initialized_ = true;
    error.clear();
    return true;
}
