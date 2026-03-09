#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"
#include "nuraft/nuraft_metadata_store.h"
#include "nuraft/nuraft_prototype_state_machine.h"
#include "nuraft/nuraft_recovery_coordinator.h"
#include "nuraft/nuraft_request_journal.h"
#include "nuraft/nuraft_state_initializer.h"
#include "nuraft/nuraft_state_machine_sink.h"
#include "nuraft/nuraft_static_cluster.h"
#include "string_utils.h"

namespace {

enum class BenchmarkMode {
    kAll,
    kAppendApply,
    kSnapshotRecovery,
};

struct BenchmarkOptions {
    BenchmarkMode mode = BenchmarkMode::kAll;
    std::string data_dir;
    uint32_t docs = 1000;
    uint32_t post_snapshot_docs = 100;
    bool keep_data = false;
};

uint64_t make_route_hash(const std::string& method, const std::string& path) {
    const std::string method_path = method + path;
    const uint64_t hash = StringUtils::hash_wy(method_path.c_str(), method_path.size());
    return (hash > 100) ? hash : (hash + 100);
}

std::string benchmark_usage(const char* program_name) {
    const std::string binary_name = (program_name == nullptr || std::string(program_name).empty()) ?
                                    "nuraft-prototype-benchmark" :
                                    std::string(program_name);
    return "usage: " + binary_name + " [options]\n"
           "options:\n"
           "  --mode <all|append-apply|snapshot-recovery>  Benchmark scenario set (default: all)\n"
           "  --data-dir <dir>         Working directory root for benchmark state\n"
           "  --docs <count>           Number of document writes before snapshot (default: 1000)\n"
           "  --post-snapshot-docs <count>  Number of writes after snapshot in recovery benchmark (default: 100)\n"
           "  --keep-data              Keep benchmark state directories after completion\n"
           "  --help                   Print this message\n";
}

bool parse_uint32(const std::string& value, uint32_t& parsed, std::string& error) {
    try {
        const unsigned long long numeric = std::stoull(value);
        if (numeric > std::numeric_limits<uint32_t>::max()) {
            error = "integer value is out of range";
            return false;
        }
        parsed = static_cast<uint32_t>(numeric);
        return true;
    } catch (const std::exception&) {
        error = "integer value is invalid";
        return false;
    }
}

bool parse_mode(const std::string& value, BenchmarkMode& mode, std::string& error) {
    if (value == "all") {
        mode = BenchmarkMode::kAll;
    } else if (value == "append-apply") {
        mode = BenchmarkMode::kAppendApply;
    } else if (value == "snapshot-recovery") {
        mode = BenchmarkMode::kSnapshotRecovery;
    } else {
        error = "unsupported mode: " + value;
        return false;
    }
    error.clear();
    return true;
}

bool parse_options(int argc,
                   char** argv,
                   BenchmarkOptions& options,
                   bool& help_requested,
                   std::string& usage,
                   std::string& error) {
    options = BenchmarkOptions();
    help_requested = false;
    usage = benchmark_usage(argc > 0 ? argv[0] : nullptr);

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help") {
            help_requested = true;
            continue;
        }

        if (argument == "--keep-data") {
            options.keep_data = true;
            continue;
        }

        if (argument.rfind("--", 0) != 0) {
            error = "unexpected positional argument: " + argument;
            return false;
        }

        std::string option_name;
        std::string option_value;
        const size_t equals_pos = argument.find('=');
        if (equals_pos != std::string::npos) {
            option_name = argument.substr(2, equals_pos - 2);
            option_value = argument.substr(equals_pos + 1);
        } else {
            option_name = argument.substr(2);
            if (i + 1 >= argc) {
                error = "option needs value: --" + option_name;
                return false;
            }
            option_value = argv[++i];
        }

        if (option_name == "mode") {
            if (!parse_mode(option_value, options.mode, error)) {
                return false;
            }
        } else if (option_name == "data-dir") {
            options.data_dir = option_value;
        } else if (option_name == "docs") {
            if (!parse_uint32(option_value, options.docs, error)) {
                error = "invalid value for --docs: " + error;
                return false;
            }
        } else if (option_name == "post-snapshot-docs") {
            if (!parse_uint32(option_value, options.post_snapshot_docs, error)) {
                error = "invalid value for --post-snapshot-docs: " + error;
                return false;
            }
        } else {
            error = "undefined option: --" + option_name;
            return false;
        }
    }

    error.clear();
    return true;
}

std::string make_default_root() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return (std::filesystem::temp_directory_path() /
            ("nuraft_prototype_benchmark-" + std::to_string(millis))).string();
}

bool initialize_node(const std::string& data_dir,
                     uint32_t peer_port,
                     uint32_t api_port,
                     const std::string& nodes_config,
                     std::string& error) {
    NuRaftPrototypeOptions options;
    options.data_dir = data_dir;
    options.local_host = "127.0.0.1";
    options.peer_port = peer_port;
    options.api_port = api_port;
    options.nodes_config = nodes_config;

    NuRaftIdentity identity;
    NuRaftBootstrapConfig bootstrap_config;
    return NuRaftStateInitializer::initialize(options, identity, bootstrap_config, error);
}

std::string collection_create_request(uint64_t route_hash) {
    nlohmann::json request = {
        {"route_hash", route_hash},
        {"params", nlohmann::json::object()},
        {"body", "{\"name\":\"books\"}"},
    };
    return request.dump();
}

std::string document_write_request(uint64_t route_hash, uint32_t doc_id) {
    const std::string doc_id_str = "doc-" + std::to_string(doc_id);
    nlohmann::json request = {
        {"route_hash", route_hash},
        {"params", {{"collection", "books"}, {"id", doc_id_str}}},
        {"body", std::string("{\"id\":\"") + doc_id_str + "\",\"title\":\"Book " + std::to_string(doc_id) + "\"}"},
    };
    return request.dump();
}

template <typename Fn>
bool measure_ms(Fn&& fn, double& elapsed_ms, std::string& error) {
    const auto start = std::chrono::steady_clock::now();
    if (!fn(error)) {
        return false;
    }
    const auto end = std::chrono::steady_clock::now();
    elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    error.clear();
    return true;
}

bool append_requests(const std::string& data_dir,
                     uint32_t docs,
                     uint64_t collection_create_hash,
                     uint64_t document_write_hash,
                     uint64_t& last_index,
                     std::string& error) {
    NuRaftRequestJournal journal(NuRaftStateLayout::from_data_dir(data_dir));
    if (!journal.initialize(error)) {
        return false;
    }

    if (!journal.append_request_json(collection_create_request(collection_create_hash), last_index, error)) {
        return false;
    }

    for (uint32_t doc_id = 1; doc_id <= docs; ++doc_id) {
        if (!journal.append_request_json(document_write_request(document_write_hash, doc_id), last_index, error)) {
            return false;
        }
    }

    error.clear();
    return true;
}

bool apply_pending_kv(const std::string& data_dir,
                      uint64_t& applied_count,
                      std::string& error) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(data_dir);
    auto sink = std::make_unique<NuRaftKvStateMachineSink>(layout);
    NuRaftPrototypeStateMachine state_machine(layout, std::move(sink));
    if (!state_machine.initialize(error)) {
        return false;
    }

    std::vector<NuRaftLogEntry> applied_entries;
    if (!state_machine.apply_pending(applied_entries, error)) {
        return false;
    }

    applied_count = applied_entries.size();
    error.clear();
    return true;
}

bool read_materialized_count(const std::string& data_dir,
                             uint64_t& count,
                             std::string& error) {
    NuRaftKvStateMachineSink sink(NuRaftStateLayout::from_data_dir(data_dir));
    std::vector<std::pair<std::string, std::string>> entries;
    if (!sink.read_materialized_entries(entries, error)) {
        return false;
    }
    count = entries.size();
    error.clear();
    return true;
}

bool replicate_missing_entries(const std::string& leader_data_dir,
                               const std::string& follower_data_dir,
                               uint64_t& replicated_entries,
                               std::string& error) {
    NuRaftRequestJournal leader_journal(NuRaftStateLayout::from_data_dir(leader_data_dir));
    if (!leader_journal.initialize(error)) {
        return false;
    }

    std::vector<NuRaftLogEntry> leader_entries;
    if (!leader_journal.replay(leader_entries, error)) {
        return false;
    }

    NuRaftRequestJournal follower_journal(NuRaftStateLayout::from_data_dir(follower_data_dir));
    if (!follower_journal.initialize(error)) {
        return false;
    }

    std::vector<NuRaftLogEntry> follower_entries;
    if (!follower_journal.replay(follower_entries, error)) {
        return false;
    }

    if (follower_entries.size() > leader_entries.size()) {
        error = "follower log is ahead of leader history";
        return false;
    }

    for (size_t i = 0; i < follower_entries.size(); ++i) {
        if (!(follower_entries[i] == leader_entries[i])) {
            error = "follower log diverges from leader history";
            return false;
        }
    }

    replicated_entries = 0;
    for (size_t i = follower_entries.size(); i < leader_entries.size(); ++i) {
        uint64_t appended_index = 0;
        if (!follower_journal.append_request_json(leader_entries[i].envelope.request_json(), appended_index, error)) {
            return false;
        }
        ++replicated_entries;
    }

    error.clear();
    return true;
}

bool append_cluster_requests(const NuRaftBootstrapConfig& bootstrap_config,
                             const std::map<int32_t, std::string>& data_dirs,
                             uint32_t start_doc_id,
                             uint32_t docs,
                             uint64_t document_write_hash,
                             uint64_t& last_index,
                             std::string& error) {
    bool forwarded_to_leader = false;
    int32_t target_server_id = 0;
    for (uint32_t offset = 0; offset < docs; ++offset) {
        const uint32_t absolute_doc_id = start_doc_id + offset;
        if (!NuRaftStaticCluster::append_request(bootstrap_config,
                                                 data_dirs,
                                                 8108,
                                                 8109,
                                                 document_write_request(document_write_hash, absolute_doc_id),
                                                 last_index,
                                                 forwarded_to_leader,
                                                 target_server_id,
                                                 error)) {
            return false;
        }
    }

    error.clear();
    return true;
}

nlohmann::json run_append_apply_benchmark(const BenchmarkOptions& options, const std::string& root_dir, std::string& error) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");
    const std::string data_dir = (std::filesystem::path(root_dir) / "append-apply").string();

    if (!initialize_node(data_dir, 7107, 8108, "", error)) {
        return {};
    }

    double append_ms = 0.0;
    uint64_t last_index = 0;
    if (!measure_ms([&](std::string& run_error) {
            return append_requests(data_dir, options.docs, collection_create_hash, document_write_hash, last_index, run_error);
        }, append_ms, error)) {
        return {};
    }

    double apply_ms = 0.0;
    uint64_t applied_count = 0;
    if (!measure_ms([&](std::string& run_error) {
            return apply_pending_kv(data_dir, applied_count, run_error);
        }, apply_ms, error)) {
        return {};
    }

    uint64_t materialized_count = 0;
    if (!read_materialized_count(data_dir, materialized_count, error)) {
        return {};
    }

    return {
        {"mode", "append-apply"},
        {"data_dir", data_dir},
        {"docs", options.docs},
        {"appended_entries", last_index},
        {"append_ms", append_ms},
        {"append_entries_per_sec", append_ms > 0.0 ? (1000.0 * static_cast<double>(last_index) / append_ms) : 0.0},
        {"apply_ms", apply_ms},
        {"applied_entries", applied_count},
        {"apply_entries_per_sec", apply_ms > 0.0 ? (1000.0 * static_cast<double>(applied_count) / apply_ms) : 0.0},
        {"materialized_entries", materialized_count},
    };
}

nlohmann::json run_snapshot_recovery_benchmark(const BenchmarkOptions& options, const std::string& root_dir, std::string& error) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");
    const std::string node1 = (std::filesystem::path(root_dir) / "cluster-node1").string();
    const std::string node2 = (std::filesystem::path(root_dir) / "cluster-node2").string();
    const std::string node3 = (std::filesystem::path(root_dir) / "cluster-node3").string();
    const std::string nodes_config = "127.0.0.1:7107:8108,127.0.0.1:7109:8109,127.0.0.1:7111:8110";

    if (!initialize_node(node1, 7107, 8108, nodes_config, error) ||
        !initialize_node(node2, 7109, 8109, nodes_config, error) ||
        !initialize_node(node3, 7111, 8110, nodes_config, error)) {
        return {};
    }

    NuRaftMetadataStore metadata_store(NuRaftStateLayout::from_data_dir(node1));
    NuRaftBootstrapConfig bootstrap_config;
    if (!metadata_store.read_bootstrap_config(bootstrap_config, error)) {
        return {};
    }

    const std::map<int32_t, std::string> data_dirs = {
        {8108, node1},
        {8109, node2},
        {8110, node3},
    };

    uint64_t last_index = 0;
    bool forwarded_to_leader = false;
    int32_t target_server_id = 0;
    if (!NuRaftStaticCluster::append_request(bootstrap_config,
                                             data_dirs,
                                             8108,
                                             8109,
                                             collection_create_request(collection_create_hash),
                                             last_index,
                                             forwarded_to_leader,
                                             target_server_id,
                                             error)) {
        return {};
    }

    if (!append_cluster_requests(bootstrap_config,
                                 data_dirs,
                                 1,
                                 options.docs,
                                 document_write_hash,
                                 last_index,
                                 error)) {
        return {};
    }

    std::vector<NuRaftStaticClusterNodeStatus> statuses;
    if (!NuRaftStaticCluster::replicate_and_apply(bootstrap_config, data_dirs, 8108, "kv", statuses, error)) {
        return {};
    }

    std::map<int32_t, bool> peer_health = {
        {8108, true},
        {8109, true},
        {8110, false},
    };
    int64_t last_snapshot_time = 0;
    NuRaftTimedSnapshotResult snapshot_result;
    double snapshot_ms = 0.0;
    if (!measure_ms([&](std::string& run_error) {
            return NuRaftRecoveryCoordinator::run_timed_snapshot(bootstrap_config,
                                                                 data_dirs,
                                                                 8108,
                                                                 peer_health,
                                                                 NuRaftTimedSnapshotPolicy::kLeaderOnly,
                                                                 100,
                                                                 60,
                                                                 last_snapshot_time,
                                                                 snapshot_result,
                                                                 run_error);
        }, snapshot_ms, error)) {
        return {};
    }

    double post_snapshot_append_ms = 0.0;
    if (!measure_ms([&](std::string& run_error) {
            return append_cluster_requests(bootstrap_config,
                                           data_dirs,
                                           options.docs + 1,
                                           options.post_snapshot_docs,
                                           document_write_hash,
                                           last_index,
                                           run_error);
        }, post_snapshot_append_ms, error)) {
        return {};
    }

    NuRaftSnapshotDescriptor installed_descriptor;
    double install_ms = 0.0;
    if (!measure_ms([&](std::string& run_error) {
            return NuRaftRecoveryCoordinator::install_latest_snapshot(node1, node3, installed_descriptor, run_error);
        }, install_ms, error)) {
        return {};
    }

    uint64_t replayed_entries = 0;
    double replay_ms = 0.0;
    if (!measure_ms([&](std::string& run_error) {
            return replicate_missing_entries(node1, node3, replayed_entries, run_error);
        }, replay_ms, error)) {
        return {};
    }

    double delta_apply_ms = 0.0;
    uint64_t delta_applied_entries = 0;
    if (!measure_ms([&](std::string& run_error) {
            return apply_pending_kv(node3, delta_applied_entries, run_error);
        }, delta_apply_ms, error)) {
        return {};
    }

    uint64_t recovered_materialized_count = 0;
    if (!read_materialized_count(node3, recovered_materialized_count, error)) {
        return {};
    }

    const double recovery_total_ms = install_ms + replay_ms + delta_apply_ms;

    return {
        {"mode", "snapshot-recovery"},
        {"root_dir", root_dir},
        {"docs_before_snapshot", options.docs},
        {"docs_after_snapshot", options.post_snapshot_docs},
        {"snapshot_ms", snapshot_ms},
        {"snapshot_last_applied_index", snapshot_result.descriptor.last_applied_index},
        {"snapshot_last_log_index", snapshot_result.descriptor.last_log_index},
        {"snapshot_created", snapshot_result.created_snapshot},
        {"post_snapshot_append_ms", post_snapshot_append_ms},
        {"post_snapshot_append_entries_per_sec", post_snapshot_append_ms > 0.0 ? (1000.0 * static_cast<double>(options.post_snapshot_docs) / post_snapshot_append_ms) : 0.0},
        {"install_ms", install_ms},
        {"installed_snapshot_last_applied_index", installed_descriptor.last_applied_index},
        {"replay_ms", replay_ms},
        {"replayed_entries_after_install", replayed_entries},
        {"replayed_entries_per_sec", replay_ms > 0.0 ? (1000.0 * static_cast<double>(replayed_entries) / replay_ms) : 0.0},
        {"delta_apply_ms", delta_apply_ms},
        {"delta_applied_entries", delta_applied_entries},
        {"delta_apply_entries_per_sec", delta_apply_ms > 0.0 ? (1000.0 * static_cast<double>(delta_applied_entries) / delta_apply_ms) : 0.0},
        {"recovered_materialized_entries", recovered_materialized_count},
        {"recovery_total_ms", recovery_total_ms},
    };
}

}  // namespace

int main(int argc, char** argv) {
    BenchmarkOptions options;
    bool help_requested = false;
    std::string usage;
    std::string error;
    if (!parse_options(argc, argv, options, help_requested, usage, error)) {
        std::cerr << error << "\n" << usage;
        return 1;
    }

    if (help_requested) {
        std::cout << usage;
        return 0;
    }

    const std::string root_dir = options.data_dir.empty() ? make_default_root() : options.data_dir;
    std::filesystem::remove_all(root_dir);
    std::filesystem::create_directories(root_dir);

    nlohmann::json result = {
        {"root_dir", root_dir},
        {"docs", options.docs},
        {"post_snapshot_docs", options.post_snapshot_docs},
    };

    bool ok = true;
    if (options.mode == BenchmarkMode::kAll || options.mode == BenchmarkMode::kAppendApply) {
        const nlohmann::json append_apply = run_append_apply_benchmark(options, root_dir, error);
        if (append_apply.is_null() || append_apply.empty()) {
            ok = false;
        } else {
            result["append_apply"] = append_apply;
        }
    }

    if (ok && (options.mode == BenchmarkMode::kAll || options.mode == BenchmarkMode::kSnapshotRecovery)) {
        const nlohmann::json snapshot_recovery = run_snapshot_recovery_benchmark(options, root_dir, error);
        if (snapshot_recovery.is_null() || snapshot_recovery.empty()) {
            ok = false;
        } else {
            result["snapshot_recovery"] = snapshot_recovery;
        }
    }

    if (!options.keep_data) {
        std::filesystem::remove_all(root_dir);
        result["root_dir"] = "(removed)";
    }

    if (!ok) {
        std::cerr << error << "\n";
        return 1;
    }

    std::cout << result.dump(2) << "\n";
    return 0;
}
