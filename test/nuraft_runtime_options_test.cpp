#include <gtest/gtest.h>

#include <cmdline.h>

#include <string>
#include <vector>

#include "nuraft/nuraft_runtime_options.h"
#include "runfiles_utils.h"

namespace {

std::vector<char*> get_argv(std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    return argv;
}

class ConfigImpl : public Config {
public:
    ConfigImpl() : Config() {
    }
};

void clear_runtime_env() {
    unsetenv("TYPESENSE_DATA_DIR");
    unsetenv("TYPESENSE_API_KEY");
    unsetenv("TYPESENSE_ENABLE_CORS");
    unsetenv("TYPESENSE_MAX_GROUP_LIMIT");
    unsetenv("TYPESENSE_HEALTHY_READ_LAG");
    unsetenv("TYPESENSE_CACHE_NUM_ENTRIES");
    unsetenv("TYPESENSE_NODE_HOST");
    unsetenv("TYPESENSE_API_USES_SSL");
    unsetenv("TYPESENSE_RAFT_HEART_BEAT_INTERVAL_MS");
    unsetenv("TYPESENSE_RAFT_AUTO_FORWARDING");
    unsetenv("TYPESENSE_RAFT_SNAPSHOT_DISTANCE");
}

}  // namespace

TEST(NuRaftRuntimeOptionsTest, HelpIncludesSharedAndNuRaftOptions) {
    clear_runtime_env();

    std::vector<std::string> args = {
        "./typesense-server",
        "--help",
    };
    std::vector<char*> argv = get_argv(args);

    cmdline::parser options;
    ConfigImpl config;
    NuRaftHttpServerOptions runtime_options;
    bool help_requested = false;
    std::string usage;
    std::string error;

    ASSERT_TRUE(load_nuraft_runtime_options(
        options, static_cast<int>(argv.size() - 1), argv.data(), config, runtime_options, help_requested, usage, error));
    ASSERT_TRUE(help_requested);
    ASSERT_TRUE(error.empty());

    ASSERT_NE(usage.find("--enable-cors"), std::string::npos);
    ASSERT_NE(usage.find("--max-group-limit"), std::string::npos);
    ASSERT_NE(usage.find("--healthy-read-lag"), std::string::npos);
    ASSERT_NE(usage.find("--cache-num-entries"), std::string::npos);
    ASSERT_NE(usage.find("--node-host"), std::string::npos);
    ASSERT_NE(usage.find("--api-uses-ssl"), std::string::npos);
    ASSERT_NE(usage.find("--raft-heart-beat-interval-ms"), std::string::npos);
}

TEST(NuRaftRuntimeOptionsTest, LoadsSharedConfigAndNuRaftCliOverrides) {
    clear_runtime_env();

    std::vector<std::string> args = {
        "./typesense-server",
        "--data-dir=/tmp/runtime-data",
        "--api-key=abcd",
        "--enable-cors=false",
        "--max-group-limit=5000",
        "--healthy-read-lag=2000",
        "--cache-num-entries=10000",
        "--api-address=0.0.0.0",
        "--api-port=9108",
        "--peering-port=9107",
        "--nodes=10.0.0.1:9107:9108,10.0.0.2:9107:9108",
        "--node-host=10.0.0.1",
        "--api-uses-ssl",
        "--raft-heart-beat-interval-ms=150",
        "--raft-auto-forwarding=false",
    };
    std::vector<char*> argv = get_argv(args);

    cmdline::parser options;
    ConfigImpl config;
    NuRaftHttpServerOptions runtime_options;
    bool help_requested = false;
    std::string usage;
    std::string error;

    ASSERT_TRUE(load_nuraft_runtime_options(
        options, static_cast<int>(argv.size() - 1), argv.data(), config, runtime_options, help_requested, usage, error))
        << error;
    ASSERT_FALSE(help_requested);

    ASSERT_FALSE(config.get_enable_cors());
    ASSERT_EQ(uint32_t{5000}, config.get_max_group_limit());
    ASSERT_EQ(size_t{2000}, config.get_healthy_read_lag());
    ASSERT_EQ(size_t{10000}, config.get_cache_num_entries());
    ASSERT_EQ(std::string("0.0.0.0"), config.get_api_address());
    ASSERT_EQ(9108, config.get_api_port());

    ASSERT_EQ(std::string("/tmp/runtime-data"), runtime_options.startup_options.data_dir);
    ASSERT_EQ(std::string("10.0.0.1"), runtime_options.startup_options.local_host);
    ASSERT_EQ(uint32_t{9107}, runtime_options.startup_options.peer_port);
    ASSERT_EQ(uint32_t{9108}, runtime_options.startup_options.api_port);
    ASSERT_EQ(std::string("10.0.0.1:9107:9108,10.0.0.2:9107:9108"), runtime_options.startup_options.nodes_config);
    ASSERT_TRUE(runtime_options.startup_options.api_uses_ssl);
    ASSERT_EQ(uint32_t{150}, runtime_options.raft_params.heart_beat_interval_ms);
    ASSERT_FALSE(runtime_options.raft_params.auto_forwarding);
}

TEST(NuRaftRuntimeOptionsTest, LoadsRuntimeOverridesFromEnvAndConfigFile) {
    clear_runtime_env();
    putenv((char*)"TYPESENSE_NODE_HOST=env-node");
    putenv((char*)"TYPESENSE_API_USES_SSL=true");
    putenv((char*)"TYPESENSE_RAFT_HEART_BEAT_INTERVAL_MS=175");
    putenv((char*)"TYPESENSE_RAFT_AUTO_FORWARDING=false");

    std::vector<std::string> args = {
        "./typesense-server",
        std::string("--config=") + resolve_test_path({"test/nuraft_runtime.ini"}),
    };
    std::vector<char*> argv = get_argv(args);

    cmdline::parser options;
    ConfigImpl config;
    NuRaftHttpServerOptions runtime_options;
    bool help_requested = false;
    std::string usage;
    std::string error;

    ASSERT_TRUE(load_nuraft_runtime_options(
        options, static_cast<int>(argv.size() - 1), argv.data(), config, runtime_options, help_requested, usage, error))
        << error;
    ASSERT_FALSE(help_requested);

    ASSERT_EQ(std::string("/tmp/nuraft-runtime"), config.get_data_dir());
    ASSERT_TRUE(config.get_enable_cors());
    ASSERT_EQ(uint32_t{2500}, config.get_max_group_limit());
    ASSERT_EQ(size_t{1500}, config.get_healthy_read_lag());
    ASSERT_EQ(size_t{2048}, config.get_cache_num_entries());

    // Config file should override env values for the NuRaft-only fields, matching shared config precedence.
    ASSERT_EQ(std::string("file-node"), runtime_options.startup_options.local_host);
    ASSERT_FALSE(runtime_options.startup_options.api_uses_ssl);
    ASSERT_EQ(uint32_t{250}, runtime_options.raft_params.heart_beat_interval_ms);
    ASSERT_TRUE(runtime_options.raft_params.auto_forwarding);
    ASSERT_EQ(std::string("127.0.0.1:7107:7108,127.0.0.2:7107:7108"), runtime_options.startup_options.nodes_config);

    clear_runtime_env();
}

TEST(NuRaftRuntimeOptionsTest, DefaultsSnapshotDistanceToProductionValue) {
    clear_runtime_env();

    std::vector<std::string> args = {
        "./typesense-server",
        "--data-dir=/tmp/runtime-data",
        "--api-key=abcd",
    };
    std::vector<char*> argv = get_argv(args);

    cmdline::parser options;
    ConfigImpl config;
    NuRaftHttpServerOptions runtime_options;
    bool help_requested = false;
    std::string usage;
    std::string error;

    ASSERT_TRUE(load_nuraft_runtime_options(
        options, static_cast<int>(argv.size() - 1), argv.data(), config, runtime_options, help_requested, usage, error))
        << error;
    ASSERT_FALSE(help_requested);
    ASSERT_EQ(uint32_t{100000}, runtime_options.raft_params.snapshot_distance);
}
