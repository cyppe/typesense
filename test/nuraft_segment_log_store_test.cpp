#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "nuraft/nuraft_file_store.h"
#include "nuraft/nuraft_segment_log_store.h"

class NuRaftSegmentLogStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_segment_log_store_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string temp_dir_;
};

TEST_F(NuRaftSegmentLogStoreTest, AppendsAndReplaysEntries) {
    NuRaftSegmentLogStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;
    ASSERT_TRUE(store.initialize(error)) << error;

    uint64_t first_index = 0;
    ASSERT_TRUE(store.append(NuRaftRequestEnvelope("{\"id\":1}"), first_index, error)) << error;
    EXPECT_EQ(first_index, 1u);

    uint64_t second_index = 0;
    ASSERT_TRUE(store.append(NuRaftRequestEnvelope("{\"id\":2}"), second_index, error)) << error;
    EXPECT_EQ(second_index, 2u);
    EXPECT_EQ(store.next_index(), 3u);

    std::vector<NuRaftLogEntry> entries;
    ASSERT_TRUE(store.read_all(entries, error)) << error;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0], (NuRaftLogEntry{1, NuRaftRequestEnvelope("{\"id\":1}")}));
    EXPECT_EQ(entries[1], (NuRaftLogEntry{2, NuRaftRequestEnvelope("{\"id\":2}")}));
}

TEST_F(NuRaftSegmentLogStoreTest, ReinitializesNextIndexFromPersistedLog) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    std::string error;

    NuRaftSegmentLogStore writer(layout);
    ASSERT_TRUE(writer.initialize(error)) << error;
    uint64_t index = 0;
    ASSERT_TRUE(writer.append(NuRaftRequestEnvelope("{\"phase\":1}"), index, error)) << error;
    EXPECT_EQ(index, 1u);

    NuRaftSegmentLogStore reopened(layout);
    ASSERT_TRUE(reopened.initialize(error)) << error;
    EXPECT_EQ(reopened.next_index(), 2u);
    ASSERT_TRUE(reopened.append(NuRaftRequestEnvelope("{\"phase\":2}"), index, error)) << error;
    EXPECT_EQ(index, 2u);

    std::vector<NuRaftLogEntry> entries;
    ASSERT_TRUE(reopened.read_all(entries, error)) << error;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[1].index, 2u);
    EXPECT_EQ(entries[1].envelope.request_json(), "{\"phase\":2}");
}

TEST_F(NuRaftSegmentLogStoreTest, RejectsTruncatedTailRecordOnInitialize) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    std::string error;
    ASSERT_TRUE(NuRaftFileStore::ensure_layout(layout, error)) << error;

    std::string truncated_record;
    truncated_record.push_back('\x01');
    truncated_record.push_back('\x00');
    ASSERT_TRUE(NuRaftFileStore::write_file_atomically(layout.active_log_segment_file, truncated_record, error)) << error;

    NuRaftSegmentLogStore store(layout);
    ASSERT_FALSE(store.initialize(error));
    EXPECT_EQ(error, "NuRaft segment log is truncated before a record header is complete");
}

TEST_F(NuRaftSegmentLogStoreTest, RecoversTruncatedTailRecord) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    std::string error;

    NuRaftSegmentLogStore writer(layout);
    ASSERT_TRUE(writer.initialize(error)) << error;
    uint64_t index = 0;
    ASSERT_TRUE(writer.append(NuRaftRequestEnvelope("{\"id\":1}"), index, error)) << error;

    {
        std::ofstream output(layout.active_log_segment_file, std::ios::binary | std::ios::app);
        ASSERT_TRUE(output.is_open());
        output.write("\x02\x00\x00", 3);
    }

    NuRaftSegmentLogStore recovered(layout);
    ASSERT_FALSE(recovered.initialize(error));
    EXPECT_EQ(error, "NuRaft segment log is truncated before a record header is complete");

    ASSERT_TRUE(recovered.recover_truncated_tail(error)) << error;
    EXPECT_EQ(recovered.next_index(), 2u);

    std::vector<NuRaftLogEntry> entries;
    ASSERT_TRUE(recovered.read_all(entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].index, 1u);
    EXPECT_EQ(entries[0].envelope.request_json(), "{\"id\":1}");
}
