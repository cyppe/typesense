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
    // Self is matched by host+peer_port: "127.0.0.2" + 7109
    ASSERT_TRUE(NuRaftBootstrapBuilder::build("127.0.0.2",
                                              7109,
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

TEST(NuRaftBootstrapBuilderTest, SelectsSelfBySamePortDifferentHosts) {
    // Standard cluster deployment: all nodes share api_port 8108 and peer_port 8107,
    // distinguished by IP address.
    NuRaftBootstrapConfig config;
    std::string error;
    ASSERT_TRUE(NuRaftBootstrapBuilder::build("172.28.10.12",
                                              8107,
                                              8108,
                                              "172.28.10.11:8107:8108,172.28.10.12:8107:8108,172.28.10.13:8107:8108",
                                              false,
                                              config,
                                              error)) << error;

    EXPECT_EQ(config.self, (NuRaftPeerAddress{"172.28.10.12", 8107, 8108}));
    ASSERT_EQ(config.peers.size(), 2u);
    EXPECT_EQ(config.peers[0], (NuRaftPeerAddress{"172.28.10.11", 8107, 8108}));
    EXPECT_EQ(config.peers[1], (NuRaftPeerAddress{"172.28.10.13", 8107, 8108}));

    // Each node must get a unique server_id despite sharing the same ports.
    EXPECT_NE(config.self.server_id(), config.peers[0].server_id());
    EXPECT_NE(config.self.server_id(), config.peers[1].server_id());
    EXPECT_NE(config.peers[0].server_id(), config.peers[1].server_id());
}

TEST(NuRaftBootstrapBuilderTest, RejectsNodesConfigWithoutLocalPeeringEndpoint) {
    NuRaftBootstrapConfig config;
    std::string error;
    // local_host=10.0.0.99 peer_port=9999 doesn't match any node entry.
    ASSERT_FALSE(NuRaftBootstrapBuilder::build("10.0.0.99",
                                               9999,
                                               8108,
                                               "127.0.0.1:7107:8108,127.0.0.2:7109:8110",
                                               false,
                                               config,
                                               error));
    EXPECT_NE(error.find("does not include local peering endpoint"), std::string::npos) << error;
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
