#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

#include <libnuraft/log_entry.hxx>

#include "nuraft/typesense_log_store.h"

class TypesenseLogStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("typesense_log_store_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string temp_dir_;
};

TEST_F(TypesenseLogStoreTest, CompactPastTailClampsNextSlotToStartIndex) {
    const std::string db_path = (std::filesystem::path(temp_dir_) / "log-store").string();
    TypesenseLogStore store(db_path);

    auto payload = nuraft::buffer::alloc(0);
    auto entry = nuraft::cs_new<nuraft::log_entry>(1, payload);
    EXPECT_EQ(1u, store.append(entry));
    EXPECT_EQ(2u, store.next_slot());

    ASSERT_TRUE(store.compact(42));
    EXPECT_EQ(43u, store.start_index());
    EXPECT_EQ(43u, store.next_slot());
    EXPECT_EQ(0u, store.last_entry()->get_term());

    store.close();

    TypesenseLogStore reopened(db_path);
    EXPECT_EQ(43u, reopened.start_index());
    EXPECT_EQ(43u, reopened.next_slot());
    EXPECT_EQ(0u, reopened.last_entry()->get_term());
}
