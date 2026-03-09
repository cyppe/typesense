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
                   NuRaftPrototypeOptions& prototype_options,
                   bool& help_requested,
                   std::string& error,
                   std::string& usage) {
    prototype_options = NuRaftPrototypeOptions();
    prototype_options.local_host = "127.0.0.1";
    prototype_options.peer_port = 8107;
    prototype_options.api_port = 8108;
    help_requested = false;
    usage = prototype_usage(argc > 0 ? argv[0] : nullptr);

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help") {
            help_requested = true;
            continue;
        }

        if (argument == "--api-uses-ssl") {
            prototype_options.api_uses_ssl = true;
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
            prototype_options.data_dir = option_value;
        } else if (option_name == "node-host") {
            prototype_options.local_host = option_value;
        } else if (option_name == "nodes") {
            prototype_options.nodes_config = option_value;
        } else if (option_name == "api-port") {
            if (!parse_uint32(option_value, prototype_options.api_port, error)) {
                error = "invalid value for --api-port: " + error;
                return false;
            }
        } else if (option_name == "peering-port") {
            if (!parse_uint32(option_value, prototype_options.peer_port, error)) {
                error = "invalid value for --peering-port: " + error;
                return false;
            }
        } else {
            error = "undefined option: --" + option_name;
            return false;
        }
    }

    if (!help_requested && prototype_options.data_dir.empty()) {
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
    NuRaftPrototypeOptions prototype_options;
    bool help_requested = false;
    std::string error;
    std::string usage;
    if (!parse_options(argc, argv, prototype_options, help_requested, error, usage)) {
        err << error << "\n" << usage;
        return 1;
    }

    if (help_requested) {
        out << usage;
        return 0;
    }

    return run(prototype_options, out, err);
}

int NuRaftReplicationController::run(const NuRaftPrototypeOptions& options,
                                     std::ostream& out,
                                     std::ostream& err) const {
    NuRaftIdentity identity;
    NuRaftBootstrapConfig bootstrap_config;
    std::string error;
    if (!NuRaftStateInitializer::initialize(options, identity, bootstrap_config, error)) {
        err << "Failed to initialize NuRaft prototype state: " << error << "\n";
        return 1;
    }

    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(options.data_dir);
    out << "NuRaft prototype startup preflight initialized under '" << layout.root_dir << "'.\n"
        << "server_id=" << identity.server_id << " peer_endpoint=" << identity.peer_endpoint
        << " leader_url=" << bootstrap_config.self.leader_url(bootstrap_config.api_uses_ssl)
        << " peers=" << bootstrap_config.peers.size() << "\n";
    return 0;
}
