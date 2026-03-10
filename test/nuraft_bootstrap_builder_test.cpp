#include <gtest/gtest.h>

#include <string>

#include "nuraft/nuraft_bootstrap_builder.h"

TEST(NuRaftBootstrapBuilderTest, BuildsSingleNodeConfigWhenNodesAreEmpty) {
    NuRaftBootstrapConfig config;
    std::string error;
    ASSERT_TRUE(NuRaftBootstrapBuilder::build("127.0.0.1", 7107, 8108, "", false, config, error)) << error;

    EXPECT_EQ(config.group_id, "default_group");
    EXPECT_EQ(config.self, (NuRaftPeerAddress{"127.0.0.1", 7107, 8108}));
    EXPECT_TRUE(config.peers.empty());
    EXPECT_FALSE(config.api_uses_ssl);
}

TEST(NuRaftBootstrapBuilderTest, SelectsSelfFromExistingNodesConfig) {
    NuRaftBootstrapConfig config;
    std::string error;
    ASSERT_TRUE(NuRaftBootstrapBuilder::build("127.0.0.99",
                                              7999,
                                              8110,
                                              "127.0.0.1:7107:8108,127.0.0.2:7109:8110,[2001:db8::1]:7111:8112",
                                              true,
                                              config,
                                              error)) << error;

    EXPECT_EQ(config.self, (NuRaftPeerAddress{"127.0.0.2", 7109, 8110}));
    ASSERT_EQ(config.peers.size(), 2u);
    EXPECT_EQ(config.peers[0], (NuRaftPeerAddress{"127.0.0.1", 7107, 8108}));
    EXPECT_EQ(config.peers[1], (NuRaftPeerAddress{"[2001:db8::1]", 7111, 8112}));
    EXPECT_TRUE(config.api_uses_ssl);
}

TEST(NuRaftBootstrapBuilderTest, RejectsNodesConfigWithoutLocalApiPort) {
    NuRaftBootstrapConfig config;
    std::string error;
    ASSERT_FALSE(NuRaftBootstrapBuilder::build("127.0.0.1",
                                               7107,
                                               8999,
                                               "127.0.0.1:7107:8108,127.0.0.2:7109:8110",
                                               false,
                                               config,
                                               error));
    EXPECT_EQ(error, "NuRaft bootstrap config does not include the local api_port");
}

TEST(NuRaftBootstrapBuilderTest, RejectsMalformedNodeEntry) {
    NuRaftBootstrapConfig config;
    std::string error;
    ASSERT_FALSE(NuRaftBootstrapBuilder::build("127.0.0.1",
                                               7107,
                                               8108,
                                               "127.0.0.1:7107",
                                               false,
                                               config,
                                               error));
    EXPECT_EQ(error, "Failed to parse NuRaft bootstrap node '127.0.0.1:7107': NuRaft peer address is missing the peer port separator");
}
