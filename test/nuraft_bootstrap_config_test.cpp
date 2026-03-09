#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

#include "nuraft/nuraft_metadata_store.h"

class NuRaftBootstrapConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_bootstrap_config_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string temp_dir_;
};

TEST_F(NuRaftBootstrapConfigTest, WritesAndReadsBootstrapConfig) {
    NuRaftMetadataStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;
    ASSERT_TRUE(store.initialize(error)) << error;

    NuRaftBootstrapConfig expected;
    expected.self = {"127.0.0.1", 7107, 8108};
    expected.peers = {
        expected.self,
        {"127.0.0.2", 7109, 8110},
    };
    expected.api_uses_ssl = true;

    ASSERT_TRUE(store.write_bootstrap_config(expected, error)) << error;

    NuRaftBootstrapConfig actual;
    ASSERT_TRUE(store.read_bootstrap_config(actual, error)) << error;
    EXPECT_EQ(actual, expected);
}

TEST_F(NuRaftBootstrapConfigTest, RejectsMissingSelfPeer) {
    NuRaftMetadataStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;
    ASSERT_TRUE(store.initialize(error)) << error;

    NuRaftBootstrapConfig config;
    config.self = {"127.0.0.1", 7107, 8108};
    config.peers = {
        {"127.0.0.2", 7109, 8110},
    };

    ASSERT_FALSE(store.write_bootstrap_config(config, error));
    EXPECT_EQ(error, "NuRaft bootstrap config peer list must include self");
}

TEST_F(NuRaftBootstrapConfigTest, RejectsMalformedBootstrapJson) {
    NuRaftMetadataStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;
    ASSERT_TRUE(store.initialize(error)) << error;
    ASSERT_TRUE(NuRaftFileStore::write_file_atomically(store.layout().bootstrap_config_file,
                                                       "{\"group_id\":\"default_group\"}",
                                                       error)) << error;

    NuRaftBootstrapConfig config;
    ASSERT_FALSE(store.read_bootstrap_config(config, error));
    EXPECT_EQ(error, "NuRaft bootstrap metadata is missing required fields");
}
