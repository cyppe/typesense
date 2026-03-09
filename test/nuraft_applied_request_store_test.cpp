#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

#include "nuraft/nuraft_applied_request_store.h"

class NuRaftAppliedRequestStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_applied_request_store_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string temp_dir_;
};

TEST_F(NuRaftAppliedRequestStoreTest, DecodesLogEntryIntoStructuredRequest) {
    NuRaftAppliedRequest applied_request;
    std::string error;
    NuRaftLogEntry entry;
    entry.index = 7;
    entry.envelope = NuRaftRequestEnvelope(
        "{\"route_hash\":1234,\"params\":{\"collection\":\"books\"},\"first_chunk_aggregate\":true,"
        "\"last_chunk_aggregate\":false,\"body\":\"{\\\"id\\\":\\\"1\\\"}\",\"metadata\":\"meta\","
        "\"start_ts\":55,\"log_index\":7,\"is_binary_body\":false}");

    ASSERT_TRUE(NuRaftAppliedRequest::from_log_entry(entry, applied_request, error)) << error;
    EXPECT_EQ(applied_request.index, 7u);
    EXPECT_EQ(applied_request.route_hash, 1234u);
    EXPECT_EQ(applied_request.route_kind, NuRaftRouteKind::kUnknown);
    EXPECT_EQ(applied_request.params.at("collection"), "books");
    EXPECT_EQ(applied_request.metadata, "meta");
    EXPECT_EQ(applied_request.body, "{\"id\":\"1\"}");
    EXPECT_EQ(applied_request.start_ts, 55u);
    EXPECT_EQ(applied_request.log_index, 7);
}

TEST_F(NuRaftAppliedRequestStoreTest, AppendsAndReadsStructuredRequests) {
    NuRaftAppliedRequestStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;

    NuRaftAppliedRequest expected;
    expected.index = 1;
    expected.route_hash = 1234;
    expected.route_kind = NuRaftRouteKind::kUnknown;
    expected.params = {{"collection", "books"}};
    expected.body = "{\"id\":\"1\"}";
    expected.metadata = "meta";
    expected.start_ts = 11;

    ASSERT_TRUE(store.append_all({expected}, error)) << error;

    std::vector<NuRaftAppliedRequest> actual;
    ASSERT_TRUE(store.read_all(actual, error)) << error;
    ASSERT_EQ(actual.size(), 1u);
    EXPECT_EQ(actual[0], expected);
}
