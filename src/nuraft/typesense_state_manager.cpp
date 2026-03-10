#include "nuraft/typesense_state_manager.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <libnuraft/buffer.hxx>
#include <libnuraft/buffer_serializer.hxx>

namespace {

std::string raft_log_dir(const NuRaftStateLayout& layout) {
    // Store the NuRaft log in a dedicated RocksDB under the root directory.
    if (layout.root_dir.empty()) {
        return "raft_log";
    }
    if (layout.root_dir.back() == '/') {
        return layout.root_dir + "raft_log";
    }
    return layout.root_dir + "/raft_log";
}

}  // namespace

TypesenseStateManager::TypesenseStateManager(
    const NuRaftStateLayout& layout,
    const NuRaftIdentity& identity,
    const NuRaftBootstrapConfig& bootstrap_config)
    : layout_(layout),
      identity_(identity),
      bootstrap_config_(bootstrap_config) {
    std::filesystem::create_directories(layout_.meta_dir);
    std::string log_path = raft_log_dir(layout_);
    log_store_ = nuraft::cs_new<TypesenseLogStore>(log_path);
}

TypesenseStateManager::~TypesenseStateManager() = default;

nuraft::ptr<nuraft::cluster_config> TypesenseStateManager::load_config() {
    std::string data;
    if (read_file(layout_.cluster_config_file, data) && !data.empty()) {
        auto buf = nuraft::buffer::alloc(data.size());
        std::memcpy(buf->data(), data.data(), data.size());
        return nuraft::cluster_config::deserialize(*buf);
    }
    // First boot: create initial config from bootstrap.
    return make_initial_config();
}

void TypesenseStateManager::save_config(const nuraft::cluster_config& config) {
    auto buf = config.serialize();
    write_file(layout_.cluster_config_file,
               buf->data_begin(), buf->size());
}

void TypesenseStateManager::save_state(const nuraft::srv_state& state) {
    auto buf = state.serialize();
    write_file(layout_.server_state_file,
               buf->data_begin(), buf->size());
}

nuraft::ptr<nuraft::srv_state> TypesenseStateManager::read_state() {
    std::string data;
    if (read_file(layout_.server_state_file, data) && !data.empty()) {
        auto buf = nuraft::buffer::alloc(data.size());
        std::memcpy(buf->data(), data.data(), data.size());
        return nuraft::srv_state::deserialize(*buf);
    }
    return nullptr;  // First boot.
}

nuraft::ptr<nuraft::log_store> TypesenseStateManager::load_log_store() {
    return log_store_;
}

nuraft::int32 TypesenseStateManager::server_id() {
    return identity_.server_id;
}

void TypesenseStateManager::system_exit(const int exit_code) {
    // NuRaft calls this on abnormal termination. Log and exit.
    std::exit(exit_code);
}

TypesenseLogStore* TypesenseStateManager::get_log_store_raw() const {
    return log_store_.get();
}

nuraft::ptr<nuraft::cluster_config> TypesenseStateManager::make_initial_config() const {
    auto config = nuraft::cs_new<nuraft::cluster_config>();

    // Add self.
    auto self_srv = nuraft::cs_new<nuraft::srv_config>(
        identity_.server_id,
        0,  // dc_id
        identity_.peer_endpoint,
        "",    // aux
        false  // not learner
    );
    config->get_servers().push_back(self_srv);

    // Add peers from bootstrap config.
    for (const auto& peer : bootstrap_config_.peers) {
        int32_t peer_id = peer.server_id();
        if (peer_id == identity_.server_id) {
            continue;  // Skip self.
        }
        auto peer_srv = nuraft::cs_new<nuraft::srv_config>(
            peer_id,
            0,  // dc_id
            peer.peer_endpoint(),
            "",    // aux
            false  // not learner
        );
        config->get_servers().push_back(peer_srv);
    }

    return config;
}

bool TypesenseStateManager::read_file(const std::string& path, std::string& data) const {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }
    data.assign(std::istreambuf_iterator<char>(ifs),
                std::istreambuf_iterator<char>());
    return true;
}

bool TypesenseStateManager::write_file(const std::string& path,
                                       const void* data, size_t size) const {
    // Write to temp file, then rename for atomicity.
    std::string tmp_path = path + ".tmp";
    std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }
    ofs.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    ofs.close();
    if (ofs.fail()) {
        return false;
    }
    std::filesystem::rename(tmp_path, path);
    return true;
}
