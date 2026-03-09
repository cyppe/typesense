#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

#include "nuraft/nuraft_metadata_store.h"

class NuRaftMetadataStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_metadata_store_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string temp_dir_;
};

TEST_F(NuRaftMetadataStoreTest, WritesAndReadsIdentity) {
    NuRaftMetadataStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;
    ASSERT_TRUE(store.initialize(error)) << error;

    NuRaftIdentity expected;
    expected.server_id = 8108;
    expected.peer_endpoint = "127.0.0.1:8107";
    expected.api_port = 8108;

    ASSERT_TRUE(store.write_identity(expected, error)) << error;

    NuRaftIdentity actual;
    ASSERT_TRUE(store.read_identity(actual, error)) << error;
    EXPECT_EQ(actual, expected);
}

TEST_F(NuRaftMetadataStoreTest, RejectsMalformedIdentityJson) {
    NuRaftMetadataStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;
    ASSERT_TRUE(store.initialize(error)) << error;
    ASSERT_TRUE(NuRaftFileStore::write_file_atomically(store.layout().identity_file, "{\"server_id\":1}", error)) << error;

    NuRaftIdentity identity;
    ASSERT_FALSE(store.read_identity(identity, error));
    EXPECT_EQ(error, "NuRaft identity metadata is missing required fields");
}
