#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "nuraft/nuraft_file_store.h"

class NuRaftFileStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_file_store_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string temp_dir_;
};

TEST_F(NuRaftFileStoreTest, LayoutUsesPrototypeRootUnderStateDir) {
    const auto layout = NuRaftStateLayout::from_data_dir(temp_dir_);

    EXPECT_EQ(layout.root_dir, temp_dir_ + "/state/nuraft-prototype");
    EXPECT_EQ(layout.meta_dir, layout.root_dir + "/meta");
    EXPECT_EQ(layout.log_dir, layout.root_dir + "/log");
    EXPECT_EQ(layout.snapshot_dir, layout.root_dir + "/snapshot");
    EXPECT_EQ(layout.identity_file, layout.meta_dir + "/identity.json");
}

TEST_F(NuRaftFileStoreTest, EnsureLayoutCreatesAllDirectories) {
    const auto layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    std::string error;

    ASSERT_TRUE(NuRaftFileStore::ensure_layout(layout, error)) << error;
    EXPECT_TRUE(std::filesystem::is_directory(layout.meta_dir));
    EXPECT_TRUE(std::filesystem::is_directory(layout.log_dir));
    EXPECT_TRUE(std::filesystem::is_directory(layout.snapshot_dir));
}

TEST_F(NuRaftFileStoreTest, AtomicWriteAndReadRoundTrip) {
    const auto layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    std::string error;
    ASSERT_TRUE(NuRaftFileStore::ensure_layout(layout, error)) << error;

    ASSERT_TRUE(NuRaftFileStore::write_file_atomically(layout.identity_file, "first", error)) << error;
    ASSERT_TRUE(NuRaftFileStore::write_file_atomically(layout.identity_file, "second", error)) << error;

    std::string data;
    ASSERT_TRUE(NuRaftFileStore::read_file(layout.identity_file, data, error)) << error;
    EXPECT_EQ(data, "second");
}
