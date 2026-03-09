#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

#include "nuraft/nuraft_prototype_state_machine.h"

class NuRaftPrototypeStateMachineTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_prototype_state_machine_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string temp_dir_;
};

TEST_F(NuRaftPrototypeStateMachineTest, AppliesPendingEntriesOnlyOnce) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    std::string error;

    NuRaftRequestJournal journal(layout);
    ASSERT_TRUE(journal.initialize(error)) << error;
    uint64_t index = 0;
    ASSERT_TRUE(journal.append_request_json("{\"id\":1}", index, error)) << error;
    ASSERT_TRUE(journal.append_request_json("{\"id\":2}", index, error)) << error;

    NuRaftPrototypeStateMachine state_machine(layout);
    ASSERT_TRUE(state_machine.initialize(error)) << error;

    std::vector<NuRaftLogEntry> applied_entries;
    ASSERT_TRUE(state_machine.apply_pending(applied_entries, error)) << error;
    ASSERT_EQ(applied_entries.size(), 2u);

    std::vector<std::string> applied_requests;
    ASSERT_TRUE(state_machine.read_applied_requests(applied_requests, error)) << error;
    ASSERT_EQ(applied_requests.size(), 2u);
    EXPECT_EQ(applied_requests[0], "{\"id\":1}");
    EXPECT_EQ(applied_requests[1], "{\"id\":2}");

    ASSERT_TRUE(state_machine.apply_pending(applied_entries, error)) << error;
    EXPECT_TRUE(applied_entries.empty());
    ASSERT_TRUE(state_machine.read_applied_requests(applied_requests, error)) << error;
    ASSERT_EQ(applied_requests.size(), 2u);
}

TEST_F(NuRaftPrototypeStateMachineTest, AppliesOnlyNewEntriesAfterRestart) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    std::string error;

    NuRaftRequestJournal journal(layout);
    ASSERT_TRUE(journal.initialize(error)) << error;
    uint64_t index = 0;
    ASSERT_TRUE(journal.append_request_json("{\"id\":1}", index, error)) << error;

    NuRaftPrototypeStateMachine first_state_machine(layout);
    ASSERT_TRUE(first_state_machine.initialize(error)) << error;
    std::vector<NuRaftLogEntry> applied_entries;
    ASSERT_TRUE(first_state_machine.apply_pending(applied_entries, error)) << error;
    ASSERT_EQ(applied_entries.size(), 1u);

    ASSERT_TRUE(journal.append_request_json("{\"id\":2}", index, error)) << error;

    NuRaftPrototypeStateMachine restarted_state_machine(layout);
    ASSERT_TRUE(restarted_state_machine.initialize(error)) << error;
    ASSERT_TRUE(restarted_state_machine.apply_pending(applied_entries, error)) << error;
    ASSERT_EQ(applied_entries.size(), 1u);
    EXPECT_EQ(applied_entries[0].index, 2u);

    std::vector<std::string> applied_requests;
    ASSERT_TRUE(restarted_state_machine.read_applied_requests(applied_requests, error)) << error;
    ASSERT_EQ(applied_requests.size(), 2u);
    EXPECT_EQ(applied_requests[1], "{\"id\":2}");
}
