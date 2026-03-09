#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

#include "nuraft/nuraft_state_initializer.h"

class NuRaftStateInitializerTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_state_initializer_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string temp_dir_;
};

TEST_F(NuRaftStateInitializerTest, WritesIdentityAndBootstrapMetadata) {
    NuRaftPrototypeOptions options;
    options.data_dir = temp_dir_;
    options.local_host = "127.0.0.1";
    options.peer_port = 7107;
    options.api_port = 8108;

    NuRaftIdentity identity;
    NuRaftBootstrapConfig bootstrap_config;
    std::string error;
    ASSERT_TRUE(NuRaftStateInitializer::initialize(options, identity, bootstrap_config, error)) << error;

    EXPECT_EQ(identity.server_id, 8108);
    EXPECT_EQ(identity.peer_endpoint, "127.0.0.1:7107");
    ASSERT_EQ(bootstrap_config.peers.size(), 1u);
    EXPECT_EQ(bootstrap_config.self, bootstrap_config.peers[0]);

    NuRaftMetadataStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    NuRaftIdentity persisted_identity;
    ASSERT_TRUE(store.read_identity(persisted_identity, error)) << error;
    EXPECT_EQ(persisted_identity, identity);

    NuRaftBootstrapConfig persisted_bootstrap;
    ASSERT_TRUE(store.read_bootstrap_config(persisted_bootstrap, error)) << error;
    EXPECT_EQ(persisted_bootstrap, bootstrap_config);
}

TEST_F(NuRaftStateInitializerTest, UsesNodesConfigToSelectSelfIdentity) {
    NuRaftPrototypeOptions options;
    options.data_dir = temp_dir_;
    options.local_host = "127.0.0.99";
    options.peer_port = 7999;
    options.api_port = 8110;
    options.nodes_config = "127.0.0.1:7107:8108,127.0.0.2:7109:8110";
    options.api_uses_ssl = true;

    NuRaftIdentity identity;
    NuRaftBootstrapConfig bootstrap_config;
    std::string error;
    ASSERT_TRUE(NuRaftStateInitializer::initialize(options, identity, bootstrap_config, error)) << error;

    EXPECT_EQ(identity.server_id, 8110);
    EXPECT_EQ(identity.peer_endpoint, "127.0.0.2:7109");
    EXPECT_TRUE(bootstrap_config.api_uses_ssl);
    EXPECT_EQ(bootstrap_config.self, (NuRaftPeerAddress{"127.0.0.2", 7109, 8110}));
}

TEST_F(NuRaftStateInitializerTest, RefreshesSingleNodeSelfAddressAcrossRestart) {
    NuRaftPrototypeOptions options;
    options.data_dir = temp_dir_;
    options.local_host = "127.0.0.1";
    options.peer_port = 7107;
    options.api_port = 8108;

    NuRaftIdentity identity;
    NuRaftBootstrapConfig bootstrap_config;
    std::string error;
    ASSERT_TRUE(NuRaftStateInitializer::initialize(options, identity, bootstrap_config, error)) << error;

    options.local_host = "10.0.0.5";
    options.peer_port = 7207;
    ASSERT_TRUE(NuRaftStateInitializer::initialize(options, identity, bootstrap_config, error)) << error;
    EXPECT_EQ(identity.peer_endpoint, "10.0.0.5:7207");
    EXPECT_EQ(bootstrap_config.self, (NuRaftPeerAddress{"10.0.0.5", 7207, 8108}));
}

TEST_F(NuRaftStateInitializerTest, RefreshesMultiNodePeerListWithoutRewritingSelfIdentity) {
    NuRaftPrototypeOptions options;
    options.data_dir = temp_dir_;
    options.local_host = "127.0.0.1";
    options.peer_port = 7107;
    options.api_port = 8108;
    options.nodes_config = "127.0.0.1:7107:8108,127.0.0.2:7109:8109";

    NuRaftIdentity identity;
    NuRaftBootstrapConfig bootstrap_config;
    std::string error;
    ASSERT_TRUE(NuRaftStateInitializer::initialize(options, identity, bootstrap_config, error)) << error;

    options.nodes_config = "127.0.0.1:7107:8108,127.0.0.3:7111:8109";
    ASSERT_TRUE(NuRaftStateInitializer::initialize(options, identity, bootstrap_config, error)) << error;
    EXPECT_EQ(identity.peer_endpoint, "127.0.0.1:7107");
    ASSERT_EQ(bootstrap_config.peers.size(), 2u);
    EXPECT_EQ(bootstrap_config.peers[1], (NuRaftPeerAddress{"127.0.0.3", 7111, 8109}));
}

TEST_F(NuRaftStateInitializerTest, RejectsPersistedSelfAddressRewriteForMultiNodeBootstrap) {
    NuRaftPrototypeOptions options;
    options.data_dir = temp_dir_;
    options.local_host = "127.0.0.1";
    options.peer_port = 7107;
    options.api_port = 8108;
    options.nodes_config = "127.0.0.1:7107:8108,127.0.0.2:7109:8109";

    NuRaftIdentity identity;
    NuRaftBootstrapConfig bootstrap_config;
    std::string error;
    ASSERT_TRUE(NuRaftStateInitializer::initialize(options, identity, bootstrap_config, error)) << error;

    options.nodes_config = "10.0.0.5:7207:8108,127.0.0.2:7109:8109";
    ASSERT_FALSE(NuRaftStateInitializer::initialize(options, identity, bootstrap_config, error));
    EXPECT_EQ(error, "NuRaft state initializer refuses to rewrite the persisted self peer address for a multi-node bootstrap");
}

TEST_F(NuRaftStateInitializerTest, RejectsMissingDataDir) {
    NuRaftPrototypeOptions options;
    options.local_host = "127.0.0.1";
    options.peer_port = 7107;
    options.api_port = 8108;

    NuRaftIdentity identity;
    NuRaftBootstrapConfig bootstrap_config;
    std::string error;
    ASSERT_FALSE(NuRaftStateInitializer::initialize(options, identity, bootstrap_config, error));
    EXPECT_EQ(error, "NuRaft state initializer requires a data directory");
}
