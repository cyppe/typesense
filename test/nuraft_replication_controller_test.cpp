#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "nuraft/nuraft_metadata_store.h"
#include "nuraft/nuraft_replication_controller.h"

namespace {

std::vector<char*> make_argv(std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    return argv;
}

}  // namespace

class NuRaftReplicationControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_replication_controller_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string temp_dir_;
};

TEST_F(NuRaftReplicationControllerTest, RunsStartupPreflightFromCliArgs) {
    NuRaftReplicationController controller;
    std::vector<std::string> args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--node-host=127.0.0.1",
        "--peering-port=7107",
        "--api-port=8108",
    };
    std::vector<char*> argv = make_argv(args);
    std::ostringstream out;
    std::ostringstream err;

    ASSERT_EQ(controller.run(static_cast<int>(args.size()), argv.data(), out, err), 0);
    EXPECT_TRUE(err.str().empty());
    EXPECT_NE(out.str().find("server_id=8108"), std::string::npos);
    EXPECT_NE(out.str().find("leader_url=http://127.0.0.1:8108/"), std::string::npos);

    NuRaftMetadataStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string read_error;
    NuRaftIdentity identity;
    ASSERT_TRUE(store.read_identity(identity, read_error)) << read_error;
    EXPECT_EQ(identity.server_id, 8108);
    EXPECT_EQ(identity.peer_endpoint, "127.0.0.1:7107");
}

TEST_F(NuRaftReplicationControllerTest, PersistsMultiNodeBootstrapSelection) {
    NuRaftReplicationController controller;
    std::vector<std::string> args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--node-host=127.0.0.99",
        "--api-port=8110",
        "--nodes=127.0.0.1:7107:8108,127.0.0.2:7109:8110,[2001:db8::1]:7111:8112",
        "--api-uses-ssl",
    };
    std::vector<char*> argv = make_argv(args);
    std::ostringstream out;
    std::ostringstream err;

    ASSERT_EQ(controller.run(static_cast<int>(args.size()), argv.data(), out, err), 0);
    EXPECT_TRUE(err.str().empty());
    EXPECT_NE(out.str().find("leader_url=https://127.0.0.2:8110/"), std::string::npos);

    NuRaftMetadataStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string read_error;
    NuRaftBootstrapConfig bootstrap_config;
    ASSERT_TRUE(store.read_bootstrap_config(bootstrap_config, read_error)) << read_error;
    EXPECT_EQ(bootstrap_config.self, (NuRaftPeerAddress{"127.0.0.2", 7109, 8110}));
    ASSERT_EQ(bootstrap_config.peers.size(), 3u);
}

TEST_F(NuRaftReplicationControllerTest, ReportsMissingRequiredDataDir) {
    NuRaftReplicationController controller;
    std::vector<std::string> args = {
        "./typesense-server-nuraft-prototype",
    };
    std::vector<char*> argv = make_argv(args);
    std::ostringstream out;
    std::ostringstream err;

    ASSERT_EQ(controller.run(static_cast<int>(args.size()), argv.data(), out, err), 1);
    EXPECT_TRUE(out.str().empty());
    EXPECT_NE(err.str().find("need option: --data-dir"), std::string::npos);
}
