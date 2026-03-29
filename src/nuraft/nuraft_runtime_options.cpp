#include "nuraft/nuraft_runtime_options.h"

#include <cstdlib>
#include <limits>
#include <string>

#include "INIReader.h"
#include "file_utils.h"
#include "typesense_server_utils.h"

namespace {

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

bool parse_bool(const std::string& value, bool& parsed, std::string& error) {
    if (value == "true" || value == "1" || value == "yes" || value == "TRUE" || value == "True") {
        parsed = true;
        error.clear();
        return true;
    }

    if (value == "false" || value == "0" || value == "no" || value == "FALSE" || value == "False") {
        parsed = false;
        error.clear();
        return true;
    }

    error = "expected true/false";
    return false;
}

std::string get_env(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    return value == nullptr ? std::string() : std::string(value);
}

bool is_unspecified_bind_address(const std::string& host) {
    return host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]";
}

std::string derive_node_host(const Config& config) {
    if (!config.get_peering_address().empty() && !is_unspecified_bind_address(config.get_peering_address())) {
        return config.get_peering_address();
    }

    if (!is_unspecified_bind_address(config.get_api_address())) {
        return config.get_api_address();
    }

    return "127.0.0.1";
}

bool resolve_nodes_config(const std::string& configured_nodes, std::string& resolved_nodes, std::string& error) {
    if (configured_nodes.empty()) {
        resolved_nodes.clear();
        error.clear();
        return true;
    }

    // Preserve the old prototype convenience of accepting an inline node list, while
    // still supporting the upstream file-based contract for --nodes.
    if (!file_exists(configured_nodes) &&
        (configured_nodes.find(',') != std::string::npos || configured_nodes.find(':') != std::string::npos)) {
        resolved_nodes = configured_nodes;
        error.clear();
        return true;
    }

    const Option<std::string> nodes_op = Config::fetch_nodes_config(configured_nodes);
    if (!nodes_op.ok()) {
        error = nodes_op.error();
        return false;
    }

    // Strip trailing whitespace/newlines from file content.
    std::string content = nodes_op.get();
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r' ||
                                content.back() == ' ' || content.back() == '\t')) {
        content.pop_back();
    }
    resolved_nodes = std::move(content);
    error.clear();
    return true;
}

bool apply_runtime_env_overrides(NuRaftHttpServerOptions& runtime_options, std::string& error) {
    std::string value = get_env("TYPESENSE_NODE_HOST");
    if (!value.empty()) {
        runtime_options.startup_options.local_host = value;
    }

    value = get_env("TYPESENSE_API_USES_SSL");
    if (!value.empty() && !parse_bool(value, runtime_options.startup_options.api_uses_ssl, error)) {
        error = "invalid value for TYPESENSE_API_USES_SSL: " + error;
        return false;
    }

    value = get_env("TYPESENSE_RAFT_HEART_BEAT_INTERVAL_MS");
    if (!value.empty() && !parse_uint32(value, runtime_options.raft_params.heart_beat_interval_ms, error)) {
        error = "invalid value for TYPESENSE_RAFT_HEART_BEAT_INTERVAL_MS: " + error;
        return false;
    }

    value = get_env("TYPESENSE_RAFT_ELECTION_TIMEOUT_LOWER_MS");
    if (!value.empty() && !parse_uint32(value, runtime_options.raft_params.election_timeout_lower_bound_ms, error)) {
        error = "invalid value for TYPESENSE_RAFT_ELECTION_TIMEOUT_LOWER_MS: " + error;
        return false;
    }

    value = get_env("TYPESENSE_RAFT_ELECTION_TIMEOUT_UPPER_MS");
    if (!value.empty() && !parse_uint32(value, runtime_options.raft_params.election_timeout_upper_bound_ms, error)) {
        error = "invalid value for TYPESENSE_RAFT_ELECTION_TIMEOUT_UPPER_MS: " + error;
        return false;
    }

    value = get_env("TYPESENSE_RAFT_RESERVED_LOG_ITEMS");
    if (!value.empty() && !parse_uint32(value, runtime_options.raft_params.reserved_log_items, error)) {
        error = "invalid value for TYPESENSE_RAFT_RESERVED_LOG_ITEMS: " + error;
        return false;
    }

    value = get_env("TYPESENSE_RAFT_CLIENT_REQ_TIMEOUT_MS");
    if (!value.empty() && !parse_uint32(value, runtime_options.raft_params.client_req_timeout_ms, error)) {
        error = "invalid value for TYPESENSE_RAFT_CLIENT_REQ_TIMEOUT_MS: " + error;
        return false;
    }

    value = get_env("TYPESENSE_RAFT_AUTO_FORWARDING");
    if (!value.empty() && !parse_bool(value, runtime_options.raft_params.auto_forwarding, error)) {
        error = "invalid value for TYPESENSE_RAFT_AUTO_FORWARDING: " + error;
        return false;
    }

    value = get_env("TYPESENSE_RAFT_AUTO_FORWARDING_REQ_TIMEOUT_MS");
    if (!value.empty() && !parse_uint32(value, runtime_options.raft_params.auto_forwarding_req_timeout_ms, error)) {
        error = "invalid value for TYPESENSE_RAFT_AUTO_FORWARDING_REQ_TIMEOUT_MS: " + error;
        return false;
    }

    value = get_env("TYPESENSE_RAFT_SNAPSHOT_DISTANCE");
    if (!value.empty() && !parse_uint32(value, runtime_options.raft_params.snapshot_distance, error)) {
        error = "invalid value for TYPESENSE_RAFT_SNAPSHOT_DISTANCE: " + error;
        return false;
    }

    value = get_env("TYPESENSE_RAFT_LEADERSHIP_EXPIRY_MS");
    if (!value.empty() && !parse_uint32(value, runtime_options.raft_params.leadership_expiry_ms, error)) {
        error = "invalid value for TYPESENSE_RAFT_LEADERSHIP_EXPIRY_MS: " + error;
        return false;
    }

    value = get_env("TYPESENSE_RAFT_ASIO_THREAD_POOL_SIZE");
    if (!value.empty() && !parse_uint32(value, runtime_options.raft_params.asio_thread_pool_size, error)) {
        error = "invalid value for TYPESENSE_RAFT_ASIO_THREAD_POOL_SIZE: " + error;
        return false;
    }

    error.clear();
    return true;
}

bool apply_runtime_config_file_overrides(const cmdline::parser& options,
                                         NuRaftHttpServerOptions& runtime_options,
                                         std::string& error) {
    if (!options.exist("config")) {
        error.clear();
        return true;
    }

    const std::string config_path = options.get<std::string>("config");
    if (config_path.empty()) {
        error.clear();
        return true;
    }

    INIReader reader(config_path);
    if (reader.ParseError() != 0) {
        error.clear();
        return true;
    }

    if (reader.Exists("server", "node-host")) {
        runtime_options.startup_options.local_host = reader.Get("server", "node-host", "");
    }

    if (reader.Exists("server", "api-uses-ssl")) {
        bool parsed = runtime_options.startup_options.api_uses_ssl;
        if (!parse_bool(reader.Get("server", "api-uses-ssl", "false"), parsed, error)) {
            error = "invalid value for api-uses-ssl in config file: " + error;
            return false;
        }
        runtime_options.startup_options.api_uses_ssl = parsed;
    }

    uint32_t parsed_uint = 0;
    if (reader.Exists("server", "raft-heart-beat-interval-ms")) {
        if (!parse_uint32(reader.Get("server", "raft-heart-beat-interval-ms", "100"), parsed_uint, error)) {
            error = "invalid value for raft-heart-beat-interval-ms in config file: " + error;
            return false;
        }
        runtime_options.raft_params.heart_beat_interval_ms = parsed_uint;
    }

    if (reader.Exists("server", "raft-election-timeout-lower-ms")) {
        if (!parse_uint32(reader.Get("server", "raft-election-timeout-lower-ms", "200"), parsed_uint, error)) {
            error = "invalid value for raft-election-timeout-lower-ms in config file: " + error;
            return false;
        }
        runtime_options.raft_params.election_timeout_lower_bound_ms = parsed_uint;
    }

    if (reader.Exists("server", "raft-election-timeout-upper-ms")) {
        if (!parse_uint32(reader.Get("server", "raft-election-timeout-upper-ms", "400"), parsed_uint, error)) {
            error = "invalid value for raft-election-timeout-upper-ms in config file: " + error;
            return false;
        }
        runtime_options.raft_params.election_timeout_upper_bound_ms = parsed_uint;
    }

    if (reader.Exists("server", "raft-reserved-log-items")) {
        if (!parse_uint32(reader.Get("server", "raft-reserved-log-items", "5000"), parsed_uint, error)) {
            error = "invalid value for raft-reserved-log-items in config file: " + error;
            return false;
        }
        runtime_options.raft_params.reserved_log_items = parsed_uint;
    }

    if (reader.Exists("server", "raft-client-req-timeout-ms")) {
        if (!parse_uint32(reader.Get("server", "raft-client-req-timeout-ms", "3000"), parsed_uint, error)) {
            error = "invalid value for raft-client-req-timeout-ms in config file: " + error;
            return false;
        }
        runtime_options.raft_params.client_req_timeout_ms = parsed_uint;
    }

    if (reader.Exists("server", "raft-auto-forwarding")) {
        bool parsed = runtime_options.raft_params.auto_forwarding;
        if (!parse_bool(reader.Get("server", "raft-auto-forwarding", "true"), parsed, error)) {
            error = "invalid value for raft-auto-forwarding in config file: " + error;
            return false;
        }
        runtime_options.raft_params.auto_forwarding = parsed;
    }

    if (reader.Exists("server", "raft-auto-forwarding-req-timeout-ms")) {
        if (!parse_uint32(reader.Get("server", "raft-auto-forwarding-req-timeout-ms", "5000"), parsed_uint, error)) {
            error = "invalid value for raft-auto-forwarding-req-timeout-ms in config file: " + error;
            return false;
        }
        runtime_options.raft_params.auto_forwarding_req_timeout_ms = parsed_uint;
    }

    if (reader.Exists("server", "raft-snapshot-distance")) {
        if (!parse_uint32(reader.Get("server", "raft-snapshot-distance", "100000"), parsed_uint, error)) {
            error = "invalid value for raft-snapshot-distance in config file: " + error;
            return false;
        }
        runtime_options.raft_params.snapshot_distance = parsed_uint;
    }

    if (reader.Exists("server", "raft-leadership-expiry-ms")) {
        if (!parse_uint32(reader.Get("server", "raft-leadership-expiry-ms", "5000"), parsed_uint, error)) {
            error = "invalid value for raft-leadership-expiry-ms in config file: " + error;
            return false;
        }
        runtime_options.raft_params.leadership_expiry_ms = parsed_uint;
    }

    if (reader.Exists("server", "raft-asio-thread-pool-size")) {
        if (!parse_uint32(reader.Get("server", "raft-asio-thread-pool-size", "4"), parsed_uint, error)) {
            error = "invalid value for raft-asio-thread-pool-size in config file: " + error;
            return false;
        }
        runtime_options.raft_params.asio_thread_pool_size = parsed_uint;
    }

    error.clear();
    return true;
}

bool apply_runtime_cmdline_overrides(const cmdline::parser& options,
                                     NuRaftHttpServerOptions& runtime_options,
                                     std::string& error) {
    if (options.exist("node-host")) {
        runtime_options.startup_options.local_host = options.get<std::string>("node-host");
    }

    if (options.exist("api-uses-ssl")) {
        runtime_options.startup_options.api_uses_ssl = options.get<bool>("api-uses-ssl");
    }

    if (options.exist("raft-heart-beat-interval-ms")) {
        runtime_options.raft_params.heart_beat_interval_ms = options.get<uint32_t>("raft-heart-beat-interval-ms");
    }

    if (options.exist("raft-election-timeout-lower-ms")) {
        runtime_options.raft_params.election_timeout_lower_bound_ms =
            options.get<uint32_t>("raft-election-timeout-lower-ms");
    }

    if (options.exist("raft-election-timeout-upper-ms")) {
        runtime_options.raft_params.election_timeout_upper_bound_ms =
            options.get<uint32_t>("raft-election-timeout-upper-ms");
    }

    if (options.exist("raft-reserved-log-items")) {
        runtime_options.raft_params.reserved_log_items = options.get<uint32_t>("raft-reserved-log-items");
    }

    if (options.exist("raft-client-req-timeout-ms")) {
        runtime_options.raft_params.client_req_timeout_ms = options.get<uint32_t>("raft-client-req-timeout-ms");
    }

    if (options.exist("raft-auto-forwarding")) {
        runtime_options.raft_params.auto_forwarding = options.get<bool>("raft-auto-forwarding");
    }

    if (options.exist("raft-auto-forwarding-req-timeout-ms")) {
        runtime_options.raft_params.auto_forwarding_req_timeout_ms =
            options.get<uint32_t>("raft-auto-forwarding-req-timeout-ms");
    }

    if (options.exist("raft-snapshot-distance")) {
        runtime_options.raft_params.snapshot_distance = options.get<uint32_t>("raft-snapshot-distance");
    }

    if (options.exist("raft-leadership-expiry-ms")) {
        runtime_options.raft_params.leadership_expiry_ms = options.get<uint32_t>("raft-leadership-expiry-ms");
    }

    if (options.exist("raft-asio-thread-pool-size")) {
        runtime_options.raft_params.asio_thread_pool_size = options.get<uint32_t>("raft-asio-thread-pool-size");
    }

    error.clear();
    return true;
}

std::string runtime_help(const cmdline::parser& options) {
    return options.usage() +
           "\nShared server settings also work as environment variables using the TYPESENSE_ prefix,\n"
           "for example TYPESENSE_DATA_DIR and TYPESENSE_API_KEY.\n"
           "NuRaft-specific extras also support environment overrides:\n"
           "  TYPESENSE_NODE_HOST\n"
           "  TYPESENSE_API_USES_SSL\n"
           "  TYPESENSE_RAFT_*\n";
}

}  // namespace

void init_nuraft_runtime_cmdline_options(cmdline::parser& options, int argc, char** argv) {
    init_cmdline_options(options, argc, argv);

    options.add<std::string>(
        "node-host",
        '\0',
        "Advertised host for this NuRaft node (defaults to peering-address, then api-address if concrete, else 127.0.0.1).",
        false,
        "");
    options.add<bool>(
        "api-uses-ssl",
        '\0',
        "Use HTTPS when deriving NuRaft leader URLs. Defaults to true when both SSL certificate settings are present.",
        false,
        false);

    options.add<uint32_t>("raft-heart-beat-interval-ms", '\0', "NuRaft heartbeat interval in milliseconds.", false, 100);
    options.add<uint32_t>("raft-election-timeout-lower-ms", '\0', "NuRaft election timeout lower bound in milliseconds.", false, 200);
    options.add<uint32_t>("raft-election-timeout-upper-ms", '\0', "NuRaft election timeout upper bound in milliseconds.", false, 400);
    options.add<uint32_t>("raft-reserved-log-items", '\0', "NuRaft log entries kept after snapshot compaction.", false, 5000);
    options.add<uint32_t>("raft-client-req-timeout-ms", '\0', "NuRaft blocking client request timeout in milliseconds.", false, 3000);
    options.add<bool>("raft-auto-forwarding", '\0', "Allow followers to forward writes to the leader automatically.", false, true);
    options.add<uint32_t>("raft-auto-forwarding-req-timeout-ms", '\0', "Timeout for follower auto-forwarding requests in milliseconds.", false, 5000);
    options.add<uint32_t>("raft-snapshot-distance", '\0', "Number of commits between automatic NuRaft snapshots.", false, 100000);
    options.add<uint32_t>("raft-leadership-expiry-ms", '\0', "Step down without quorum acknowledgement after this many milliseconds (0 disables).", false, 5000);
    options.add<uint32_t>("raft-asio-thread-pool-size", '\0', "NuRaft ASIO transport thread count.", false, 4);

    options.add("help", '?', "Print this message.");
}

bool load_nuraft_runtime_options(cmdline::parser& options,
                                 int argc,
                                 char** argv,
                                 Config& config,
                                 NuRaftHttpServerOptions& runtime_options,
                                 bool& help_requested,
                                 std::string& usage,
                                 std::string& error) {
    init_nuraft_runtime_cmdline_options(options, argc, argv);
    if (!options.parse(argc, argv)) {
        usage = runtime_help(options);
        error = options.error_full();
        return false;
    }

    usage = runtime_help(options);
    help_requested = options.exist("help");
    if (help_requested) {
        error.clear();
        return true;
    }

    config.load_config_env();
    config.load_config_file(options);
    config.load_config_cmd_args(options);

    const Option<bool> validation = config.is_valid();
    if (!validation.ok()) {
        error = "Invalid configuration: " + validation.error();
        return false;
    }

    runtime_options = NuRaftHttpServerOptions();
    runtime_options.startup_options.data_dir = config.get_data_dir();
    runtime_options.startup_options.local_host = derive_node_host(config);
    runtime_options.startup_options.peer_port = static_cast<uint32_t>(config.get_peering_port());
    runtime_options.startup_options.api_port = static_cast<uint32_t>(config.get_api_port());
    runtime_options.startup_options.api_uses_ssl =
        !config.get_ssl_cert().empty() && !config.get_ssl_cert_key().empty();
    runtime_options.listen_address = config.get_api_address();
    runtime_options.listen_port = static_cast<uint32_t>(config.get_api_port());
    runtime_options.request_timeout_ms = config.get_request_timeout_ms();
    runtime_options.api_key = config.get_api_key();

    if (!resolve_nodes_config(config.get_nodes(), runtime_options.startup_options.nodes_config, error)) {
        return false;
    }

    if (!apply_runtime_env_overrides(runtime_options, error)) {
        return false;
    }

    if (!apply_runtime_config_file_overrides(options, runtime_options, error)) {
        return false;
    }

    if (!apply_runtime_cmdline_overrides(options, runtime_options, error)) {
        return false;
    }

    if (runtime_options.startup_options.local_host.empty()) {
        runtime_options.startup_options.local_host = derive_node_host(config);
    }

    error.clear();
    return true;
}
