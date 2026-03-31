#include "nuraft/nuraft_snapshot_coordinator.h"

#include <filesystem>
#include <utility>

#include "json.hpp"
#include "nuraft/nuraft_applied_request_store.h"
#include "nuraft/nuraft_file_store.h"
#include "nuraft/nuraft_state_machine_sink.h"

namespace {

std::string zero_padded_index(uint64_t index) {
    std::string value = std::to_string(index);
    if (value.size() < 20) {
        value.insert(0, 20 - value.size(), '0');
    }
    return value;
}

nlohmann::json encode_descriptor(const NuRaftSnapshotDescriptor& descriptor) {
    return {
        {"format_version", descriptor.format_version},
        {"snapshot_id", descriptor.snapshot_id},
        {"last_log_index", descriptor.last_log_index},
        {"last_applied_index", descriptor.last_applied_index},
        {"includes_main_db_checkpoint", descriptor.includes_main_db_checkpoint},
    };
}

bool decode_descriptor(const std::string& encoded,
                       NuRaftSnapshotDescriptor& descriptor,
                       std::string& error) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(encoded);
    } catch (const std::exception& e) {
        error = std::string("Failed to parse NuRaft snapshot descriptor: ") + e.what();
        return false;
    }

    if (!parsed.is_object() ||
        !parsed.contains("format_version") || !parsed["format_version"].is_number_unsigned() ||
        !parsed.contains("snapshot_id") || !parsed["snapshot_id"].is_string() ||
        !parsed.contains("last_log_index") || !parsed["last_log_index"].is_number_unsigned() ||
        !parsed.contains("last_applied_index") || !parsed["last_applied_index"].is_number_unsigned()) {
        error = "NuRaft snapshot descriptor is missing required fields";
        return false;
    }

    descriptor.format_version = parsed["format_version"].get<uint32_t>();
    descriptor.snapshot_id = parsed["snapshot_id"].get<std::string>();
    descriptor.last_log_index = parsed["last_log_index"].get<uint64_t>();
    descriptor.last_applied_index = parsed["last_applied_index"].get<uint64_t>();
    descriptor.includes_main_db_checkpoint =
        parsed.contains("includes_main_db_checkpoint") && parsed["includes_main_db_checkpoint"].is_boolean() ?
            parsed["includes_main_db_checkpoint"].get<bool>() : false;
    error.clear();
    return true;
}

bool copy_tree(const std::filesystem::path& source,
               const std::filesystem::path& destination,
               std::string& error) {
    std::error_code ec;
    if (!std::filesystem::exists(source)) {
        error = "NuRaft snapshot source path does not exist: " + source.string();
        return false;
    }

    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        error = "Failed to create parent dir for snapshot copy '" + destination.parent_path().string() + "': " +
                ec.message();
        return false;
    }

    std::filesystem::remove_all(destination, ec);
    ec.clear();
    std::filesystem::copy(source,
                          destination,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
        error = "Failed to copy NuRaft snapshot tree from '" + source.string() + "' to '" +
                destination.string() + "': " + ec.message();
        return false;
    }

    error.clear();
    return true;
}

bool remove_tree(const std::filesystem::path& target, std::string& error) {
    std::error_code ec;
    std::filesystem::remove_all(target, ec);
    if (ec) {
        error = "Failed to remove NuRaft snapshot tree '" + target.string() + "': " + ec.message();
        return false;
    }

    error.clear();
    return true;
}

bool replace_tree(const std::filesystem::path& source,
                  const std::filesystem::path& destination,
                  std::string& error) {
    if (!copy_tree(source, destination, error)) {
        return false;
    }
    error.clear();
    return true;
}

bool resolve_snapshot_root(const std::string& snapshot_path,
                           std::filesystem::path& root,
                           std::string& error) {
    const std::filesystem::path direct(snapshot_path);
    const std::filesystem::path exported = direct / "state" / NuRaftStateLayout::kPrototypeRootName;
    if (std::filesystem::is_directory(exported)) {
        root = exported;
    } else {
        root = direct;
    }

    if (!std::filesystem::is_directory(root / "meta")) {
        error = "NuRaft snapshot path does not contain installable prototype metadata";
        return false;
    }

    error.clear();
    return true;
}

std::filesystem::path data_dir_from_layout(const NuRaftStateLayout& layout);

bool build_descriptor(const NuRaftStateLayout& layout,
                      const NuRaftKvStateMachineSink* kv_sink,
                      NuRaftSnapshotDescriptor& descriptor,
                      std::string& error) {
    uint64_t last_applied_index = 0;
    if (kv_sink != nullptr) {
        if (!kv_sink->read_last_applied_index(last_applied_index, error)) {
            return false;
        }
    }

    descriptor = NuRaftSnapshotDescriptor();
    descriptor.last_log_index = last_applied_index;
    descriptor.last_applied_index = last_applied_index;
    descriptor.snapshot_id = "snapshot-" + zero_padded_index(descriptor.last_applied_index) + "-" +
                             zero_padded_index(descriptor.last_log_index);
    descriptor.includes_main_db_checkpoint = std::filesystem::is_directory(data_dir_from_layout(layout) / "db");
    error.clear();
    return true;
}

std::filesystem::path data_dir_from_layout(const NuRaftStateLayout& layout) {
    const auto prototype_root = std::filesystem::path(layout.root_dir);
    const auto state_dir = prototype_root.parent_path();
    return state_dir.parent_path();
}

}  // namespace

bool NuRaftSnapshotDescriptor::operator==(const NuRaftSnapshotDescriptor& other) const {
    return format_version == other.format_version &&
           snapshot_id == other.snapshot_id &&
           last_log_index == other.last_log_index &&
           last_applied_index == other.last_applied_index &&
           includes_main_db_checkpoint == other.includes_main_db_checkpoint;
}

NuRaftSnapshotCoordinator::NuRaftSnapshotCoordinator(NuRaftStateLayout layout)
    : layout_(std::move(layout)) {}

bool NuRaftSnapshotCoordinator::create_snapshot(const std::string& export_path,
                                                const NuRaftKvStateMachineSink* kv_sink,
                                                NuRaftSnapshotDescriptor& descriptor,
                                                std::string& error) const {
    if (!NuRaftFileStore::ensure_layout(layout_, error)) {
        return false;
    }

    if (!build_descriptor(layout_, kv_sink, descriptor, error)) {
        return false;
    }

    if (!NuRaftFileStore::write_file_atomically(layout_.snapshot_descriptor_file,
                                                encode_descriptor(descriptor).dump(),
                                                error)) {
        return false;
    }

    const std::filesystem::path local_root = std::filesystem::path(layout_.snapshot_dir) / descriptor.snapshot_id;
    const std::filesystem::path main_store_dir = data_dir_from_layout(layout_) / "db";
    if (!remove_tree(local_root, error)) {
        return false;
    }
    if (!replace_tree(layout_.meta_dir, local_root / "meta", error) ||
        !replace_tree(layout_.log_dir, local_root / "log", error)) {
        return false;
    }

    if (std::filesystem::exists(main_store_dir) &&
        !replace_tree(main_store_dir, local_root / "db", error)) {
        return false;
    }

    if (std::filesystem::exists(layout_.materialized_state_dir)) {
        if (kv_sink != nullptr) {
            if (!kv_sink->create_checkpoint((local_root / "materialized_state").string(), error)) {
                return false;
            }
        } else if (!replace_tree(layout_.materialized_state_dir, local_root / "materialized_state", error)) {
            return false;
        }
    }

    if (!export_path.empty()) {
        const std::filesystem::path export_root = std::filesystem::path(export_path) / "state" /
                                                  NuRaftStateLayout::kPrototypeRootName;
        if (!remove_tree(export_root, error)) {
            return false;
        }

        if (!replace_tree(layout_.meta_dir, export_root / "meta", error) ||
            !replace_tree(layout_.snapshot_dir, export_root / "snapshot", error)) {
            return false;
        }
    }

    error.clear();
    return true;
}

bool NuRaftSnapshotCoordinator::install_snapshot(const std::string& snapshot_path,
                                                 NuRaftSnapshotDescriptor& descriptor,
                                                 std::string& error) const {
    std::filesystem::path root;
    if (!resolve_snapshot_root(snapshot_path, root, error)) {
        return false;
    }

    NuRaftMetadataStore target_metadata(layout_);
    bool had_identity = false;
    bool had_bootstrap_config = false;
    NuRaftIdentity preserved_identity;
    NuRaftBootstrapConfig preserved_bootstrap_config;
    if (std::filesystem::exists(layout_.identity_file)) {
        if (!target_metadata.read_identity(preserved_identity, error)) {
            return false;
        }
        had_identity = true;
    }
    if (std::filesystem::exists(layout_.bootstrap_config_file)) {
        if (!target_metadata.read_bootstrap_config(preserved_bootstrap_config, error)) {
            return false;
        }
        had_bootstrap_config = true;
    }

    const std::filesystem::path descriptor_path = root / "meta" / std::filesystem::path(layout_.snapshot_descriptor_file).filename();
    if (!std::filesystem::exists(descriptor_path)) {
        error = "NuRaft snapshot install source is missing the snapshot descriptor";
        return false;
    }

    std::string encoded_descriptor;
    if (!NuRaftFileStore::read_file(descriptor_path.string(), encoded_descriptor, error) ||
        !decode_descriptor(encoded_descriptor, descriptor, error)) {
        return false;
    }

    std::filesystem::path snapshot_state_root;
    const std::filesystem::path snapshot_archive_root = root / "snapshot";
    const std::filesystem::path main_store_dir = data_dir_from_layout(layout_) / "db";
    if (std::filesystem::is_directory(snapshot_archive_root)) {
        snapshot_state_root = snapshot_archive_root / descriptor.snapshot_id;
    } else {
        snapshot_state_root = root;
    }

    if (!std::filesystem::is_directory(snapshot_state_root / "meta")) {
        error = "NuRaft snapshot path does not contain the descriptor-selected snapshot payload";
        return false;
    }

    const bool expects_main_db_checkpoint =
        descriptor.format_version >= 2 ? descriptor.includes_main_db_checkpoint :
            std::filesystem::exists(snapshot_state_root / "db");

    // Logical snapshot transfer currently serializes only files, so empty
    // directories such as a fully compacted log/ tree may be absent on the
    // receiver. Recreate the empty directory instead of rejecting the
    // snapshot outright.
    {
        std::error_code ec;
        std::filesystem::create_directories(snapshot_state_root / "log", ec);
        if (ec) {
            error = "Failed to create NuRaft snapshot log dir '" +
                    (snapshot_state_root / "log").string() + "': " + ec.message();
            return false;
        }
    }

    if (!replace_tree(root / "meta", layout_.meta_dir, error) ||
        !replace_tree(snapshot_state_root / "log", layout_.log_dir, error)) {
        return false;
    }

    if (had_identity && !target_metadata.write_identity(preserved_identity, error)) {
        return false;
    }
    if (had_bootstrap_config && !target_metadata.write_bootstrap_config(preserved_bootstrap_config, error)) {
        return false;
    }

    // Remove source node's NuRaft cluster config and server state so the target
    // node performs a clean first-boot election with its own identity.
    {
        std::error_code ec;
        std::filesystem::remove(layout_.cluster_config_file, ec);
        std::filesystem::remove(layout_.server_state_file, ec);
    }

    if (std::filesystem::is_directory(snapshot_archive_root)) {
        if (!replace_tree(snapshot_archive_root, layout_.snapshot_dir, error)) {
            return false;
        }
    } else if (!replace_tree(snapshot_state_root,
                             std::filesystem::path(layout_.snapshot_dir) / descriptor.snapshot_id,
                             error)) {
        return false;
    }

    if (std::filesystem::exists(snapshot_state_root / "materialized_state")) {
        if (!replace_tree(snapshot_state_root / "materialized_state", layout_.materialized_state_dir, error)) {
            return false;
        }
    } else if (!remove_tree(layout_.materialized_state_dir, error)) {
        return false;
    }

    if (expects_main_db_checkpoint && !std::filesystem::is_directory(snapshot_state_root / "db")) {
        error = "NuRaft snapshot is missing the main Typesense db checkpoint at '" +
                (snapshot_state_root / "db").string() + "'";
        return false;
    }

    if (expects_main_db_checkpoint) {
        if (!replace_tree(snapshot_state_root / "db", main_store_dir, error)) {
            return false;
        }
    } else if (!remove_tree(main_store_dir, error)) {
        return false;
    }

    error.clear();
    return true;
}

bool NuRaftSnapshotCoordinator::read_last_snapshot(NuRaftSnapshotDescriptor& descriptor, std::string& error) const {
    std::string encoded;
    if (!NuRaftFileStore::read_file(layout_.snapshot_descriptor_file, encoded, error)) {
        return false;
    }

    return decode_descriptor(encoded, descriptor, error);
}
