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

TEST_F(NuRaftMetadataStoreTest, WritesAndReadsReplayProgress) {
    NuRaftMetadataStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;
    ASSERT_TRUE(store.initialize(error)) << error;

    NuRaftReplayProgress expected;
    expected.last_applied_index = 42;
    ASSERT_TRUE(store.write_replay_progress(expected, error)) << error;

    NuRaftReplayProgress actual;
    ASSERT_TRUE(store.read_replay_progress(actual, error)) << error;
    EXPECT_EQ(actual, expected);
}

TEST_F(NuRaftMetadataStoreTest, ReadsLegacyJsonReplayProgress) {
    NuRaftMetadataStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;
    ASSERT_TRUE(store.initialize(error)) << error;
    ASSERT_TRUE(NuRaftFileStore::write_file_atomically(store.layout().replay_progress_file,
                                                       R"({"format_version":1,"last_applied_index":7})",
                                                       error))
        << error;

    NuRaftReplayProgress actual;
    ASSERT_TRUE(store.read_replay_progress(actual, error)) << error;
    EXPECT_EQ(actual.last_applied_index, 7u);
}

TEST_F(NuRaftMetadataStoreTest, ReadsLatestReplayProgressAfterMultipleWrites) {
    NuRaftMetadataStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;
    ASSERT_TRUE(store.initialize(error)) << error;

    NuRaftReplayProgress first;
    first.last_applied_index = 11;
    ASSERT_TRUE(store.write_replay_progress(first, error)) << error;

    NuRaftReplayProgress second;
    second.last_applied_index = 29;
    ASSERT_TRUE(store.write_replay_progress(second, error)) << error;

    NuRaftReplayProgress third;
    third.last_applied_index = 47;
    ASSERT_TRUE(store.write_replay_progress(third, error)) << error;

    NuRaftReplayProgress actual;
    ASSERT_TRUE(store.read_replay_progress(actual, error)) << error;
    EXPECT_EQ(actual.last_applied_index, 47u);
}
