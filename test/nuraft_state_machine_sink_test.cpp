#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

#include "nuraft/nuraft_state_machine_sink.h"

class NuRaftStateMachineSinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_state_machine_sink_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string temp_dir_;
};

TEST_F(NuRaftStateMachineSinkTest, PersistsAppliedRequestsThroughFileBackedSink) {
    NuRaftFileBackedStateMachineSink sink(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;

    NuRaftAppliedRequest expected;
    expected.index = 3;
    expected.route_hash = 123;
    expected.route_kind = NuRaftRouteKind::kUnknown;
    expected.body = "{\"id\":\"1\"}";

    ASSERT_TRUE(sink.apply_all({expected}, error)) << error;

    std::vector<NuRaftAppliedRequest> actual;
    ASSERT_TRUE(sink.read_all(actual, error)) << error;
    ASSERT_EQ(actual.size(), 1u);
    EXPECT_EQ(actual[0], expected);
}
