#include "nuraft/nuraft_snapshot_coordinator.h"

#include <filesystem>
#include <utility>

#include "json.hpp"
#include "nuraft/nuraft_applied_request_store.h"
#include "nuraft/nuraft_file_store.h"
#include "nuraft/nuraft_request_journal.h"
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

    if (!std::filesystem::is_directory(root / "meta") || !std::filesystem::is_directory(root / "log")) {
        error = "NuRaft snapshot path does not contain installable prototype state";
        return false;
    }

    error.clear();
    return true;
}

bool build_descriptor(const NuRaftStateLayout& layout,
                      NuRaftSnapshotDescriptor& descriptor,
                      std::string& error) {
    NuRaftRequestJournal journal(layout);
    if (!journal.initialize(error)) {
        return false;
    }

    std::vector<NuRaftLogEntry> entries;
    if (!journal.replay(entries, error)) {
        return false;
    }

    NuRaftMetadataStore metadata_store(layout);
    if (!metadata_store.initialize(error)) {
        return false;
    }

    NuRaftReplayProgress progress;
    if (!std::filesystem::exists(layout.replay_progress_file)) {
        if (!metadata_store.write_replay_progress(progress, error)) {
            return false;
        }
    } else if (!metadata_store.read_replay_progress(progress, error)) {
        return false;
    }

    descriptor = NuRaftSnapshotDescriptor();
    descriptor.last_log_index = entries.empty() ? 0 : entries.back().index;
    descriptor.last_applied_index = progress.last_applied_index;
    descriptor.snapshot_id = "snapshot-" + zero_padded_index(descriptor.last_applied_index) + "-" +
                             zero_padded_index(descriptor.last_log_index);
    error.clear();
    return true;
}

}  // namespace

bool NuRaftSnapshotDescriptor::operator==(const NuRaftSnapshotDescriptor& other) const {
    return format_version == other.format_version &&
           snapshot_id == other.snapshot_id &&
           last_log_index == other.last_log_index &&
           last_applied_index == other.last_applied_index;
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

    if (!build_descriptor(layout_, descriptor, error)) {
        return false;
    }

    if (!NuRaftFileStore::write_file_atomically(layout_.snapshot_descriptor_file,
                                                encode_descriptor(descriptor).dump(),
                                                error)) {
        return false;
    }

    const std::filesystem::path local_root = std::filesystem::path(layout_.snapshot_dir) / descriptor.snapshot_id;
    if (!replace_tree(layout_.meta_dir, local_root / "meta", error) ||
        !replace_tree(layout_.log_dir, local_root / "log", error)) {
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
        if (!replace_tree(local_root / "meta", export_root / "meta", error) ||
            !replace_tree(local_root / "log", export_root / "log", error)) {
            return false;
        }

        if (std::filesystem::exists(local_root / "materialized_state") &&
            !replace_tree(local_root / "materialized_state", export_root / "materialized_state", error)) {
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

    if (!replace_tree(root / "meta", layout_.meta_dir, error) ||
        !replace_tree(root / "log", layout_.log_dir, error)) {
        return false;
    }

    if (had_identity && !target_metadata.write_identity(preserved_identity, error)) {
        return false;
    }
    if (had_bootstrap_config && !target_metadata.write_bootstrap_config(preserved_bootstrap_config, error)) {
        return false;
    }

    if (std::filesystem::exists(root / "materialized_state") &&
        !replace_tree(root / "materialized_state", layout_.materialized_state_dir, error)) {
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
