#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

#include "nuraft/nuraft_replay_coordinator.h"

class NuRaftReplayCoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_replay_coordinator_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string temp_dir_;
};

TEST_F(NuRaftReplayCoordinatorTest, FiltersPendingEntriesAfterProgressAdvance) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    std::string error;

    NuRaftRequestJournal journal(layout);
    ASSERT_TRUE(journal.initialize(error)) << error;
    uint64_t index = 0;
    ASSERT_TRUE(journal.append_request_json("{\"id\":1}", index, error)) << error;
    ASSERT_TRUE(journal.append_request_json("{\"id\":2}", index, error)) << error;

    NuRaftReplayCoordinator coordinator(layout);
    ASSERT_TRUE(coordinator.initialize(error)) << error;

    std::vector<NuRaftLogEntry> pending_entries;
    ASSERT_TRUE(coordinator.pending_entries(pending_entries, error)) << error;
    ASSERT_EQ(pending_entries.size(), 2u);

    ASSERT_TRUE(coordinator.mark_replayed_through(1, error)) << error;
    ASSERT_TRUE(coordinator.pending_entries(pending_entries, error)) << error;
    ASSERT_EQ(pending_entries.size(), 1u);
    EXPECT_EQ(pending_entries[0].index, 2u);
}

TEST_F(NuRaftReplayCoordinatorTest, RejectsProgressBeyondPersistedLog) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    std::string error;

    NuRaftRequestJournal journal(layout);
    ASSERT_TRUE(journal.initialize(error)) << error;
    uint64_t index = 0;
    ASSERT_TRUE(journal.append_request_json("{\"id\":1}", index, error)) << error;

    NuRaftReplayCoordinator coordinator(layout);
    ASSERT_TRUE(coordinator.initialize(error)) << error;
    ASSERT_FALSE(coordinator.mark_replayed_through(2, error));
    EXPECT_EQ(error, "NuRaft replay progress cannot advance beyond the persisted log");
}
