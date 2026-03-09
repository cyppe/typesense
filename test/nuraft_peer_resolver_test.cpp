#include <gtest/gtest.h>

#include <string>

#include "nuraft/nuraft_peer_resolver.h"

TEST(NuRaftPeerResolverTest, ParsesIpv4Address) {
    NuRaftPeerAddress address;
    std::string error;
    ASSERT_TRUE(NuRaftPeerResolver::parse_address("127.0.0.1:8107:8108", address, error)) << error;
    EXPECT_EQ(address.host, "127.0.0.1");
    EXPECT_EQ(address.peer_port, 8107u);
    EXPECT_EQ(address.api_port, 8108u);
    EXPECT_EQ(address.server_id(), 8108);
    EXPECT_EQ(address.peer_endpoint(), "127.0.0.1:8107");
    EXPECT_EQ(address.leader_url(false), "http://127.0.0.1:8108/");
}

TEST(NuRaftPeerResolverTest, ParsesBracketedIpv6Address) {
    NuRaftPeerAddress address;
    std::string error;
    ASSERT_TRUE(NuRaftPeerResolver::parse_address("[2001:db8::1]:7107:7108", address, error)) << error;
    EXPECT_EQ(address.host, "[2001:db8::1]");
    EXPECT_EQ(address.peer_endpoint(), "[2001:db8::1]:7107");
    EXPECT_EQ(address.leader_url(true), "https://[2001:db8::1]:7108/");
}

TEST(NuRaftPeerResolverTest, RejectsMissingApiPort) {
    NuRaftPeerAddress address;
    std::string error;
    ASSERT_FALSE(NuRaftPeerResolver::parse_address("127.0.0.1:8107", address, error));
    EXPECT_EQ(error, "NuRaft peer address is missing the peer port separator");
}

TEST(NuRaftPeerResolverTest, RejectsUnterminatedIpv6Host) {
    NuRaftPeerAddress address;
    std::string error;
    ASSERT_FALSE(NuRaftPeerResolver::parse_address("[2001:db8::1:8107:8108", address, error));
    EXPECT_EQ(error, "NuRaft peer address has an unterminated IPv6 host");
}

TEST(NuRaftPeerResolverTest, RejectsOutOfRangePorts) {
    NuRaftPeerAddress address;
    std::string error;
    ASSERT_FALSE(NuRaftPeerResolver::parse_address("127.0.0.1:70000:8108", address, error));
    EXPECT_EQ(error, "NuRaft peer address port is out of range");
}
