#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
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

TEST_F(NuRaftReplicationControllerTest, AppendsAndReplaysRequestJournalThroughCli) {
    NuRaftReplicationController controller;
    std::vector<std::string> append_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--append-request-json={\"route\":\"/collections\",\"method\":\"POST\"}",
    };
    std::vector<char*> append_argv = make_argv(append_args);
    std::ostringstream append_out;
    std::ostringstream append_err;
    ASSERT_EQ(controller.run(static_cast<int>(append_args.size()), append_argv.data(), append_out, append_err), 0);
    EXPECT_TRUE(append_err.str().empty());
    EXPECT_NE(append_out.str().find("appended_index=1"), std::string::npos);

    std::vector<std::string> replay_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--replay-log",
    };
    std::vector<char*> replay_argv = make_argv(replay_args);
    std::ostringstream replay_out;
    std::ostringstream replay_err;
    ASSERT_EQ(controller.run(static_cast<int>(replay_args.size()), replay_argv.data(), replay_out, replay_err), 0);
    EXPECT_TRUE(replay_err.str().empty());
    EXPECT_NE(replay_out.str().find("replay_count=1"), std::string::npos);
    EXPECT_NE(replay_out.str().find("route_hash=0"), std::string::npos);
    EXPECT_NE(replay_out.str().find("route_kind=unknown"), std::string::npos);
    EXPECT_NE(replay_out.str().find("body={\"route\":\"/collections\",\"method\":\"POST\"}"), std::string::npos);
}

TEST_F(NuRaftReplicationControllerTest, AppliesPendingEntriesThroughCli) {
    NuRaftReplicationController controller;
    std::vector<std::string> append_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--append-request-json={\"route\":\"/collections\",\"method\":\"POST\"}",
    };
    std::vector<char*> append_argv = make_argv(append_args);
    std::ostringstream append_out;
    std::ostringstream append_err;
    ASSERT_EQ(controller.run(static_cast<int>(append_args.size()), append_argv.data(), append_out, append_err), 0);
    EXPECT_TRUE(append_err.str().empty());

    std::vector<std::string> apply_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--apply-pending",
    };
    std::vector<char*> apply_argv = make_argv(apply_args);
    std::ostringstream apply_out;
    std::ostringstream apply_err;
    ASSERT_EQ(controller.run(static_cast<int>(apply_args.size()), apply_argv.data(), apply_out, apply_err), 0);
    EXPECT_TRUE(apply_err.str().empty());
    EXPECT_NE(apply_out.str().find("applied_count=1"), std::string::npos);
    EXPECT_NE(apply_out.str().find("route_hash="), std::string::npos);
    EXPECT_NE(apply_out.str().find("route_kind=unknown"), std::string::npos);
    EXPECT_NE(apply_out.str().find("body_bytes="), std::string::npos);

    std::ostringstream second_apply_out;
    std::ostringstream second_apply_err;
    ASSERT_EQ(controller.run(static_cast<int>(apply_args.size()), apply_argv.data(), second_apply_out, second_apply_err), 0);
    EXPECT_TRUE(second_apply_err.str().empty());
    EXPECT_NE(second_apply_out.str().find("applied_count=0"), std::string::npos);
}

TEST_F(NuRaftReplicationControllerTest, RecoversTruncatedTailAndAutoAppliesThroughCli) {
    NuRaftReplicationController controller;
    std::vector<std::string> append_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--append-request-json={\"route\":\"/collections\",\"method\":\"POST\"}",
    };
    std::vector<char*> append_argv = make_argv(append_args);
    std::ostringstream append_out;
    std::ostringstream append_err;
    ASSERT_EQ(controller.run(static_cast<int>(append_args.size()), append_argv.data(), append_out, append_err), 0);
    EXPECT_TRUE(append_err.str().empty());

    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    {
        std::ofstream output(layout.active_log_segment_file, std::ios::binary | std::ios::app);
        ASSERT_TRUE(output.is_open());
        output.write("\x02\x00\x00", 3);
    }

    std::vector<std::string> recover_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--recover-truncated-tail",
        "--auto-apply-pending",
    };
    std::vector<char*> recover_argv = make_argv(recover_args);
    std::ostringstream recover_out;
    std::ostringstream recover_err;
    ASSERT_EQ(controller.run(static_cast<int>(recover_args.size()), recover_argv.data(), recover_out, recover_err), 0);
    EXPECT_NE(recover_err.str().find("Recovered truncated NuRaft request journal tail"), std::string::npos);
    EXPECT_NE(recover_out.str().find("applied_count=1"), std::string::npos);
}
