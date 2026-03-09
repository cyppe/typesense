#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "nuraft/nuraft_applied_request_store.h"
#include "nuraft/nuraft_request_journal.h"

class NuRaftRequestJournalTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_request_journal_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string temp_dir_;
};

TEST_F(NuRaftRequestJournalTest, AppendsAndReplaysRequestJson) {
    NuRaftRequestJournal journal(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;
    ASSERT_TRUE(journal.initialize(error)) << error;

    uint64_t index = 0;
    ASSERT_TRUE(journal.append_request_json("{\"route\":\"/collections\"}", index, error)) << error;
    EXPECT_EQ(index, 1u);

    std::vector<NuRaftLogEntry> entries;
    ASSERT_TRUE(journal.replay(entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].index, 1u);

    NuRaftAppliedRequest applied_request;
    ASSERT_TRUE(NuRaftAppliedRequest::from_log_entry(entries[0], applied_request, error)) << error;
    EXPECT_EQ(applied_request.body, "{\"route\":\"/collections\"}");
    EXPECT_EQ(applied_request.route_hash, 0u);
}

TEST_F(NuRaftRequestJournalTest, RejectsEmptyRequestJson) {
    NuRaftRequestJournal journal(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;
    ASSERT_TRUE(journal.initialize(error)) << error;

    uint64_t index = 0;
    ASSERT_FALSE(journal.append_request_json("", index, error));
    EXPECT_EQ(error, "NuRaft request journal cannot append an empty request payload");
}

TEST_F(NuRaftRequestJournalTest, RecoversTruncatedTail) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    NuRaftRequestJournal journal(layout);
    std::string error;
    ASSERT_TRUE(journal.initialize(error)) << error;

    uint64_t index = 0;
    ASSERT_TRUE(journal.append_request_json("{\"route\":\"/collections\"}", index, error)) << error;

    {
        std::ofstream output(layout.active_log_segment_file, std::ios::binary | std::ios::app);
        ASSERT_TRUE(output.is_open());
        output.write("\x02\x00\x00", 3);
    }

    NuRaftRequestJournal broken(layout);
    ASSERT_FALSE(broken.initialize(error));
    EXPECT_EQ(error, "NuRaft segment log is truncated before a record header is complete");
    ASSERT_TRUE(broken.recover_truncated_tail(error)) << error;
    ASSERT_TRUE(broken.initialize(error)) << error;

    std::vector<NuRaftLogEntry> entries;
    ASSERT_TRUE(broken.replay(entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
}
