#include "nuraft/nuraft_replication_controller.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

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
           "  --replay-log              Print the persisted request journal after startup preflight\n"
           "  --api-uses-ssl            Use HTTPS when deriving leader URLs\n"
           "  --help                    Print this message\n";
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
    NuRaftRequestJournal request_journal(layout);
    if (!request_journal.initialize(error)) {
        err << "Failed to initialize NuRaft request journal: " << error << "\n";
        return 1;
    }

    out << "NuRaft prototype startup preflight initialized under '" << layout.root_dir << "'.\n"
        << "server_id=" << identity.server_id << " peer_endpoint=" << identity.peer_endpoint
        << " leader_url=" << bootstrap_config.self.leader_url(bootstrap_config.api_uses_ssl)
        << " peers=" << bootstrap_config.peers.size() << "\n";

    if (!options.append_request_json.empty()) {
        uint64_t appended_index = 0;
        if (!request_journal.append_request_json(options.append_request_json, appended_index, error)) {
            err << "Failed to append prototype request: " << error << "\n";
            return 1;
        }

        out << "appended_index=" << appended_index
            << " request_bytes=" << options.append_request_json.size() << "\n";
    }

    if (options.replay_log) {
        std::vector<NuRaftLogEntry> entries;
        if (!request_journal.replay(entries, error)) {
            err << "Failed to replay prototype request journal: " << error << "\n";
            return 1;
        }

        out << "replay_count=" << entries.size() << "\n";
        for (const auto& entry : entries) {
            out << "replay index=" << entry.index
                << " request_json=" << entry.envelope.request_json() << "\n";
        }
    }

    return 0;
}
