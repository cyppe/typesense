#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <string>

#include <curl/curl.h>

#include "collection_manager.h"
#include "http_client.h"
#include "nuraft/nuraft_http_runtime.h"
#include "store.h"
#include "threadpool.h"
#include "typesense_server_utils.h"
#include "tsconfig.h"

namespace {

#define TS_STRINGIFY_IMPL(x) #x
#define TS_STRINGIFY(x) TS_STRINGIFY_IMPL(x)

std::string runtime_usage(const char* program_name) {
    const std::string binary_name = (program_name == nullptr || std::string(program_name).empty()) ?
                                    "typesense-server-nuraft-runtime" :
                                    std::string(program_name);
    return "usage: " + binary_name + " --data-dir <dir> [options]\n"
           "options:\n"
           "  --data-dir <dir>               NuRaft prototype state root\n"
           "  --node-host <host>             Advertised host for this node (default: 127.0.0.1)\n"
           "  --listen-address <host>        HTTP listen address (default: 127.0.0.1)\n"
           "  --listen-port <port>           HTTP listen port (default: --api-port)\n"
           "  --api-port <port>              API identity port / server_id (default: 8108)\n"
           "  --peering-port <port>          Peer identity port (default: 8107)\n"
           "  --nodes <list>                 Comma-separated host:peer_port:api_port list\n"
           "  --cluster-data-dirs <list>     Static server_id=data_dir map for multi-node runtime\n"
           "  --cluster-leader-api-port <port>  Preferred static leader server_id/api_port\n"
           "  --install-snapshot <path>      Install an exported prototype snapshot before serving\n"
           "  --api-key <value>              API key for HTTP auth (default: xyz)\n"
           "  --api-uses-ssl                 Use HTTPS when deriving leader URLs\n"
           "  --help                         Print this message\n";
}

bool parse_uint32(const std::string& value, uint32_t& parsed, std::string& error) {
    try {
        const unsigned long long numeric = std::stoull(value);
        if (numeric > std::numeric_limits<uint32_t>::max()) {
            error = "integer value is out of range";
            return false;
        }
        parsed = static_cast<uint32_t>(numeric);
        error.clear();
        return true;
    } catch (const std::exception&) {
        error = "integer value is invalid";
        return false;
    }
}

bool parse_options(int argc,
                   char** argv,
                   NuRaftHttpServerOptions& options,
                   bool& help_requested,
                   std::string& error,
                   std::string& usage) {
    options = NuRaftHttpServerOptions();
    options.startup_options.local_host = "127.0.0.1";
    options.startup_options.peer_port = 8107;
    options.startup_options.api_port = 8108;
    options.listen_address = "127.0.0.1";
    options.listen_port = 8108;
    options.api_key = "xyz";
    help_requested = false;
    usage = runtime_usage(argc > 0 ? argv[0] : nullptr);

    bool listen_port_explicit = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help") {
            help_requested = true;
            continue;
        }

        if (argument == "--api-uses-ssl") {
            options.startup_options.api_uses_ssl = true;
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
            options.startup_options.data_dir = option_value;
        } else if (option_name == "node-host") {
            options.startup_options.local_host = option_value;
        } else if (option_name == "listen-address") {
            options.listen_address = option_value;
        } else if (option_name == "listen-port") {
            if (!parse_uint32(option_value, options.listen_port, error)) {
                error = "invalid value for --listen-port: " + error;
                return false;
            }
            listen_port_explicit = true;
        } else if (option_name == "api-port") {
            if (!parse_uint32(option_value, options.startup_options.api_port, error)) {
                error = "invalid value for --api-port: " + error;
                return false;
            }
            if (!listen_port_explicit) {
                options.listen_port = options.startup_options.api_port;
            }
        } else if (option_name == "peering-port") {
            if (!parse_uint32(option_value, options.startup_options.peer_port, error)) {
                error = "invalid value for --peering-port: " + error;
                return false;
            }
        } else if (option_name == "nodes") {
            options.startup_options.nodes_config = option_value;
        } else if (option_name == "cluster-data-dirs") {
            options.cluster_data_dirs = option_value;
        } else if (option_name == "cluster-leader-api-port") {
            if (!parse_uint32(option_value, options.cluster_leader_api_port, error)) {
                error = "invalid value for --cluster-leader-api-port: " + error;
                return false;
            }
        } else if (option_name == "install-snapshot") {
            options.install_snapshot_path = option_value;
        } else if (option_name == "api-key") {
            options.api_key = option_value;
        } else {
            error = "undefined option: --" + option_name;
            return false;
        }
    }

    if (!help_requested && options.startup_options.data_dir.empty()) {
        error = "need option: --data-dir";
        return false;
    }

    error.clear();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    NuRaftHttpServerOptions options;
    bool help_requested = false;
    std::string error;
    std::string usage;
    if (!parse_options(argc, argv, options, help_requested, error, usage)) {
        std::cerr << error << "\n" << usage;
        return 1;
    }

    if (help_requested) {
        std::cout << usage;
        return 0;
    }

    Config& config = Config::get_instance();
    config.set_api_key(options.api_key);
    config.set_data_dir(options.startup_options.data_dir);
    config.set_listen_address(options.listen_address);
    config.set_listen_port(static_cast<int>(options.listen_port));

    curl_global_init(CURL_GLOBAL_SSL);
    HttpClient::get_instance().init(options.api_key);

    std::atomic<bool> quit_product_state(false);
    ThreadPool app_thread_pool(4);
    ThreadPool server_thread_pool(4);
    std::filesystem::create_directories(options.startup_options.data_dir);
    std::filesystem::create_directories(options.startup_options.data_dir + "/db");
    Store store(options.startup_options.data_dir + "/db", 24 * 60 * 60, 1024, true, 0);
    HttpServer http_server(
        TS_STRINGIFY(TYPESENSE_VERSION),
        options.listen_address,
        options.listen_port,
        "",
        "",
        8 * 60 * 60 * 1000,
        true,
        std::set<std::string>{},
        &server_thread_pool
    );
    server = &http_server;
    http_server.set_auth_handler(nuraft_http_runtime_auth);
    http_server.on(HttpServer::STREAM_RESPONSE_MESSAGE, HttpServer::on_stream_response_message);
    http_server.on(HttpServer::REQUEST_PROCEED_MESSAGE, HttpServer::on_request_proceed_message);
    http_server.on(HttpServer::DEFER_PROCESSING_MESSAGE, HttpServer::on_deferred_processing_message);
    register_nuraft_http_runtime_routes(&http_server);

    CollectionManager& collection_manager = CollectionManager::get_instance();
    collection_manager.init(&store,
                            &app_thread_pool,
                            config.get_max_memory_ratio(),
                            config.get_api_key(),
                            quit_product_state,
                            config.get_filter_by_max_ops());

    NuRaftHttpRuntimeService runtime_service(&http_server, options);
    if (!runtime_service.initialize(error)) {
        collection_manager.dispose();
        app_thread_pool.shutdown();
        std::cerr << error << "\n";
        curl_global_cleanup();
        return 1;
    }

    const int exit_code = http_server.run(&runtime_service);
    quit_product_state.store(true);
    server = nullptr;
    collection_manager.dispose();
    app_thread_pool.shutdown();
    server_thread_pool.shutdown();
    curl_global_cleanup();
    return exit_code;
}
