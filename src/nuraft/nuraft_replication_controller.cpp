#include "nuraft/nuraft_replication_controller.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nuraft/nuraft_snapshot_coordinator.h"
#include "nuraft/nuraft_static_cluster.h"

namespace {

std::string prototype_usage(const char* program_name) {
    const std::string binary_name = (program_name == nullptr || std::string(program_name).empty()) ?
                                    "typesense-server-nuraft-prototype" :
                                    std::string(program_name);
    return "usage: " + binary_name + " --data-dir <dir> [options]\n"
           "options:\n"
           "  --node-host <host>        Advertised host for the prototype node (default: 127.0.0.1)\n"
           "  --api-port <port>         API identity port (default: 8108)\n"
           "  --peering-port <port>     Peer identity port (default: 8107)\n"
           "  --nodes <list>            Comma-separated host:peer_port:api_port list\n"
           "  --append-request-json <json>  Append one request envelope after startup preflight\n"
            "  --state-machine-sink <kind>   Apply sink: file (default) or kv\n"
           "  --cluster-data-dirs <list>    Static server_id=data_dir map for multi-node prototype\n"
           "  --cluster-leader-api-port <port>  Preferred static leader server_id/api_port\n"
           "  --create-snapshot <path>      Export the current prototype snapshot to a target path\n"
           "  --install-snapshot <path>     Install a previously exported prototype snapshot\n"
            "  --replay-log              Print the persisted request journal after startup preflight\n"
            "  --apply-pending           Apply pending replay entries into the prototype state-machine sink\n"
            "  --auto-apply-pending      Apply pending replay entries during startup after optional append\n"
           "  --replicate-cluster       Copy leader log to mapped followers and apply pending on all nodes\n"
           "  --dump-cluster-status     Print per-node log/apply progress for the mapped static cluster\n"
            "  --dump-materialized-state Print materialized KV state after startup/apply (kv sink only)\n"
           "  --dump-snapshot-descriptor Print the last local snapshot descriptor\n"
            "  --recover-truncated-tail  Explicitly trim truncated EOF log garbage before continuing\n"
            "  --api-uses-ssl            Use HTTPS when deriving leader URLs\n"
            "  --help                    Print this message\n";
}

void print_cluster_status(const std::vector<NuRaftStaticClusterNodeStatus>& statuses, std::ostream& out) {
    out << "cluster_nodes=" << statuses.size() << "\n";
    for (const auto& status : statuses) {
        out << "cluster node_server_id=" << status.server_id
            << " role=" << (status.is_leader ? "leader" : "follower")
            << " last_log_index=" << status.last_log_index
            << " last_applied_index=" << status.last_applied_index
            << " applied_request_count=" << status.applied_request_count
            << " data_dir=" << status.data_dir << "\n";
    }
}

void print_snapshot_descriptor(const NuRaftSnapshotDescriptor& descriptor, std::ostream& out) {
    out << "snapshot_id=" << descriptor.snapshot_id
        << " snapshot_last_log_index=" << descriptor.last_log_index
        << " snapshot_last_applied_index=" << descriptor.last_applied_index << "\n";
}

bool initialize_request_journal(NuRaftRequestJournal& request_journal,
                                bool recover_truncated_tail,
                                std::ostream& err,
                                std::string& error) {
    if (request_journal.initialize(error)) {
        return true;
    }

    if (!recover_truncated_tail) {
        return false;
    }

    const std::string original_error = error;
    if (!request_journal.recover_truncated_tail(error)) {
        err << "Failed to recover truncated NuRaft request journal tail after initialization error '"
            << original_error << "': " << error << "\n";
        return false;
    }

    if (!request_journal.initialize(error)) {
        err << "NuRaft request journal still fails after truncated-tail recovery: " << error << "\n";
        return false;
    }

    err << "Recovered truncated NuRaft request journal tail after initialization error: "
        << original_error << "\n";
    return true;
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

bool parse_options(int argc,
                   char** argv,
                   NuRaftPrototypeRunOptions& run_options,
                   bool& help_requested,
                   std::string& error,
                   std::string& usage) {
    run_options = NuRaftPrototypeRunOptions();
    run_options.startup_options.local_host = "127.0.0.1";
    run_options.startup_options.peer_port = 8107;
    run_options.startup_options.api_port = 8108;
    help_requested = false;
    usage = prototype_usage(argc > 0 ? argv[0] : nullptr);

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help") {
            help_requested = true;
            continue;
        }

        if (argument == "--api-uses-ssl") {
            run_options.startup_options.api_uses_ssl = true;
            continue;
        }

        if (argument == "--replay-log") {
            run_options.replay_log = true;
            continue;
        }

        if (argument == "--apply-pending") {
            run_options.apply_pending = true;
            continue;
        }

        if (argument == "--auto-apply-pending") {
            run_options.auto_apply_pending = true;
            continue;
        }

        if (argument == "--recover-truncated-tail") {
            run_options.recover_truncated_tail = true;
            continue;
        }

        if (argument == "--dump-materialized-state") {
            run_options.dump_materialized_state = true;
            continue;
        }

        if (argument == "--dump-snapshot-descriptor") {
            run_options.dump_snapshot_descriptor = true;
            continue;
        }

        if (argument == "--replicate-cluster") {
            run_options.replicate_cluster = true;
            continue;
        }

        if (argument == "--dump-cluster-status") {
            run_options.dump_cluster_status = true;
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

        if (option_name == "data-dir") {
            run_options.startup_options.data_dir = option_value;
        } else if (option_name == "node-host") {
            run_options.startup_options.local_host = option_value;
        } else if (option_name == "nodes") {
            run_options.startup_options.nodes_config = option_value;
        } else if (option_name == "append-request-json") {
            run_options.append_request_json = option_value;
        } else if (option_name == "state-machine-sink") {
            run_options.state_machine_sink = option_value;
        } else if (option_name == "cluster-data-dirs") {
            run_options.cluster_data_dirs = option_value;
        } else if (option_name == "cluster-leader-api-port") {
            if (!parse_uint32(option_value, run_options.cluster_leader_api_port, error)) {
                error = "invalid value for --cluster-leader-api-port: " + error;
                return false;
            }
        } else if (option_name == "create-snapshot") {
            run_options.create_snapshot_path = option_value;
        } else if (option_name == "install-snapshot") {
            run_options.install_snapshot_path = option_value;
        } else if (option_name == "api-port") {
            if (!parse_uint32(option_value, run_options.startup_options.api_port, error)) {
                error = "invalid value for --api-port: " + error;
                return false;
            }
        } else if (option_name == "peering-port") {
            if (!parse_uint32(option_value, run_options.startup_options.peer_port, error)) {
                error = "invalid value for --peering-port: " + error;
                return false;
            }
        } else {
            error = "undefined option: --" + option_name;
            return false;
        }
    }

    if (!help_requested && run_options.startup_options.data_dir.empty()) {
        error = "need option: --data-dir";
        return false;
    }

    error.clear();
    return true;
}

}  // namespace

int NuRaftReplicationController::run(int argc, char** argv) const {
    return run(argc, argv, std::cout, std::cerr);
}

int NuRaftReplicationController::run(int argc, char** argv, std::ostream& out, std::ostream& err) const {
    NuRaftPrototypeRunOptions run_options;
    bool help_requested = false;
    std::string error;
    std::string usage;
    if (!parse_options(argc, argv, run_options, help_requested, error, usage)) {
        err << error << "\n" << usage;
        return 1;
    }

    if (help_requested) {
        out << usage;
        return 0;
    }

    return run(run_options, out, err);
}

int NuRaftReplicationController::run(const NuRaftPrototypeOptions& options,
                                     std::ostream& out,
                                     std::ostream& err) const {
    NuRaftPrototypeRunOptions run_options;
    run_options.startup_options = options;
    return run(run_options, out, err);
}

int NuRaftReplicationController::run(const NuRaftPrototypeRunOptions& options,
                                     std::ostream& out,
                                     std::ostream& err) const {
    NuRaftIdentity identity;
    NuRaftBootstrapConfig bootstrap_config;
    std::string error;
    if (!NuRaftStateInitializer::initialize(options.startup_options, identity, bootstrap_config, error)) {
        err << "Failed to initialize NuRaft prototype state: " << error << "\n";
        return 1;
    }

    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(options.startup_options.data_dir);
    NuRaftSnapshotCoordinator snapshot_coordinator(layout);
    if (!options.install_snapshot_path.empty()) {
        NuRaftSnapshotDescriptor installed_descriptor;
        if (!snapshot_coordinator.install_snapshot(options.install_snapshot_path, installed_descriptor, error)) {
            err << "Failed to install NuRaft snapshot: " << error << "\n";
            return 1;
        }

        NuRaftMetadataStore metadata_store(layout);
        if (!metadata_store.read_identity(identity, error) ||
            !metadata_store.read_bootstrap_config(bootstrap_config, error)) {
            err << "Failed to reload NuRaft metadata after snapshot install: " << error << "\n";
            return 1;
        }

        out << "installed_snapshot=" << installed_descriptor.snapshot_id
            << " installed_snapshot_last_log_index=" << installed_descriptor.last_log_index
            << " installed_snapshot_last_applied_index=" << installed_descriptor.last_applied_index << "\n";
    }

    NuRaftRequestJournal request_journal(layout);
    if (!initialize_request_journal(request_journal, options.recover_truncated_tail, err, error)) {
        err << "Failed to initialize NuRaft request journal: " << error << "\n";
        return 1;
    }

    std::unique_ptr<NuRaftStateMachineSink> sink;
    NuRaftKvStateMachineSink* kv_sink = nullptr;
    if (options.state_machine_sink == "file") {
        sink = std::make_unique<NuRaftFileBackedStateMachineSink>(layout);
    } else if (options.state_machine_sink == "kv") {
        auto kv_sink_instance = std::make_unique<NuRaftKvStateMachineSink>(layout);
        kv_sink = kv_sink_instance.get();
        sink = std::move(kv_sink_instance);
    } else {
        err << "Unsupported NuRaft state-machine sink: " << options.state_machine_sink << "\n";
        return 1;
    }

    NuRaftPrototypeStateMachine state_machine(layout, std::move(sink));
    if (!state_machine.initialize(error)) {
        err << "Failed to initialize NuRaft prototype state machine: " << error << "\n";
        return 1;
    }

    const bool cluster_enabled = !options.cluster_data_dirs.empty();
    std::map<int32_t, std::string> cluster_data_dirs;
    NuRaftPeerAddress discovered_leader = bootstrap_config.self;
    if (cluster_enabled) {
        if (!NuRaftStaticCluster::parse_data_dir_map(options.cluster_data_dirs, cluster_data_dirs, error)) {
            err << "Failed to parse NuRaft cluster data dirs: " << error << "\n";
            return 1;
        }

        if (!NuRaftStaticCluster::discover_leader(bootstrap_config,
                                                  static_cast<int32_t>(options.cluster_leader_api_port),
                                                  discovered_leader,
                                                  error)) {
            err << "Failed to discover NuRaft static cluster leader: " << error << "\n";
            return 1;
        }
    }

    out << "NuRaft prototype startup preflight initialized under '" << layout.root_dir << "'.\n"
        << "server_id=" << identity.server_id << " peer_endpoint=" << identity.peer_endpoint
        << " leader_url=" << discovered_leader.leader_url(bootstrap_config.api_uses_ssl)
        << " peers=" << bootstrap_config.peers.size();
    if (cluster_enabled) {
        out << " cluster_role=" << (identity.server_id == discovered_leader.server_id() ? "leader" : "follower")
            << " leader_server_id=" << discovered_leader.server_id();
    }
    out << "\n";

    if (!options.append_request_json.empty()) {
        uint64_t appended_index = 0;
        bool forwarded_to_leader = false;
        int32_t target_server_id = identity.server_id;
        const bool append_ok = cluster_enabled ?
            NuRaftStaticCluster::append_request(bootstrap_config,
                                               cluster_data_dirs,
                                               static_cast<int32_t>(options.cluster_leader_api_port),
                                               identity.server_id,
                                               options.append_request_json,
                                               appended_index,
                                               forwarded_to_leader,
                                               target_server_id,
                                               error) :
            request_journal.append_request_json(options.append_request_json, appended_index, error);
        if (!append_ok) {
            err << "Failed to append prototype request: " << error << "\n";
            return 1;
        }

        out << "appended_index=" << appended_index
            << " request_bytes=" << options.append_request_json.size();
        if (cluster_enabled) {
            out << " target_server_id=" << target_server_id
                << " forwarded_to_leader=" << (forwarded_to_leader ? 1 : 0);
        }
        out << "\n";
    }

    const bool should_apply_pending = options.apply_pending || options.auto_apply_pending;
    bool cluster_apply_performed = false;
    if (options.replicate_cluster) {
        if (!cluster_enabled) {
            err << "Cluster replication requires --cluster-data-dirs\n";
            return 1;
        }

        std::vector<NuRaftStaticClusterNodeStatus> statuses;
        if (!NuRaftStaticCluster::replicate_and_apply(bootstrap_config,
                                                      cluster_data_dirs,
                                                      static_cast<int32_t>(options.cluster_leader_api_port),
                                                      options.state_machine_sink,
                                                      statuses,
                                                      error)) {
            err << "Failed to replicate static NuRaft cluster: " << error << "\n";
            return 1;
        }

        cluster_apply_performed = true;
        out << "cluster_replicated_nodes=" << statuses.size() << "\n";
        if (options.dump_cluster_status) {
            print_cluster_status(statuses, out);
        }
    } else if (options.dump_cluster_status) {
        if (!cluster_enabled) {
            err << "Cluster status dump requires --cluster-data-dirs\n";
            return 1;
        }

        std::vector<NuRaftStaticClusterNodeStatus> statuses;
        if (!NuRaftStaticCluster::collect_status(bootstrap_config,
                                                 cluster_data_dirs,
                                                 static_cast<int32_t>(options.cluster_leader_api_port),
                                                 statuses,
                                                 error)) {
            err << "Failed to collect NuRaft cluster status: " << error << "\n";
            return 1;
        }

        print_cluster_status(statuses, out);
    }

    if (options.replay_log) {
        std::vector<NuRaftLogEntry> entries;
        if (!request_journal.replay(entries, error)) {
            err << "Failed to replay prototype request journal: " << error << "\n";
            return 1;
        }

        out << "replay_count=" << entries.size() << "\n";
        for (const auto& entry : entries) {
            NuRaftAppliedRequest applied_request;
            if (!NuRaftAppliedRequest::from_log_entry(entry, applied_request, error)) {
                err << "Failed to decode replayed prototype request: " << error << "\n";
                return 1;
            }

            out << "replay index=" << applied_request.index
                << " route_hash=" << applied_request.route_hash
                << " route_kind=" << NuRaftRouteClassifier::kind_name(applied_request.route_kind)
                << " body=" << applied_request.body << "\n";
        }
    }

    if (should_apply_pending && !cluster_apply_performed) {
        std::vector<NuRaftLogEntry> applied_entries;
        if (!state_machine.apply_pending(applied_entries, error)) {
            err << "Failed to apply pending prototype requests: " << error << "\n";
            return 1;
        }

        out << "applied_count=" << applied_entries.size() << "\n";
        std::vector<NuRaftAppliedRequest> applied_requests;
        if (!state_machine.read_applied_requests(applied_requests, error)) {
            err << "Failed to read applied prototype requests: " << error << "\n";
            return 1;
        }
        for (const auto& request : applied_requests) {
            out << "applied index=" << request.index
                << " route_hash=" << request.route_hash
                << " route_kind=" << NuRaftRouteClassifier::kind_name(request.route_kind)
                << " body_bytes=" << request.body.size() << "\n";
        }
    }

    if (options.dump_materialized_state) {
        if (kv_sink == nullptr) {
            err << "Materialized state dump requires --state-machine-sink=kv\n";
            return 1;
        }

        std::vector<std::pair<std::string, std::string>> materialized_entries;
        if (!kv_sink->read_materialized_entries(materialized_entries, error)) {
            err << "Failed to read NuRaft materialized state: " << error << "\n";
            return 1;
        }

        out << "materialized_count=" << materialized_entries.size() << "\n";
        for (const auto& entry : materialized_entries) {
            out << "materialized key=" << entry.first << " value=" << entry.second << "\n";
        }
    }

    if (!options.create_snapshot_path.empty()) {
        NuRaftSnapshotDescriptor descriptor;
        if (!snapshot_coordinator.create_snapshot(options.create_snapshot_path, kv_sink, descriptor, error)) {
            err << "Failed to create NuRaft snapshot: " << error << "\n";
            return 1;
        }
        print_snapshot_descriptor(descriptor, out);
    } else if (options.dump_snapshot_descriptor) {
        NuRaftSnapshotDescriptor descriptor;
        if (!snapshot_coordinator.read_last_snapshot(descriptor, error)) {
            err << "Failed to read NuRaft snapshot descriptor: " << error << "\n";
            return 1;
        }
        print_snapshot_descriptor(descriptor, out);
    }

    return 0;
}
