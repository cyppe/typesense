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

TEST_F(NuRaftStateMachineSinkTest, MaterializesCollectionAndDocumentStateThroughKvSink) {
    NuRaftKvStateMachineSink sink(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string error;

    NuRaftAppliedRequest create_collection;
    create_collection.index = 1;
    create_collection.route_hash = 101;
    create_collection.route_kind = NuRaftRouteKind::kCollectionCreate;
    create_collection.body = "{\"name\":\"books\"}";

    NuRaftAppliedRequest write_document;
    write_document.index = 2;
    write_document.route_hash = 102;
    write_document.route_kind = NuRaftRouteKind::kDocumentWrite;
    write_document.params = {{"collection", "books"}, {"id", "doc-1"}};
    write_document.body = "{\"id\":\"doc-1\",\"title\":\"Dune\"}";

    ASSERT_TRUE(sink.apply_all({create_collection, write_document}, error)) << error;

    std::vector<NuRaftAppliedRequest> applied_requests;
    ASSERT_TRUE(sink.read_all(applied_requests, error)) << error;
    ASSERT_EQ(applied_requests.size(), 2u);

    std::vector<std::pair<std::string, std::string>> entries;
    ASSERT_TRUE(sink.read_materialized_entries(entries, error)) << error;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].first, "state/collections/books");
    EXPECT_EQ(entries[0].second, "{\"name\":\"books\"}");
    EXPECT_EQ(entries[1].first, "state/documents/books/doc-1");
    EXPECT_EQ(entries[1].second, "{\"id\":\"doc-1\",\"title\":\"Dune\"}");
}

TEST_F(NuRaftStateMachineSinkTest, PersistsDeletesAcrossKvSinkRestart) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    std::string error;

    {
        NuRaftKvStateMachineSink first_sink(layout);

        NuRaftAppliedRequest create_collection;
        create_collection.index = 1;
        create_collection.route_hash = 101;
        create_collection.route_kind = NuRaftRouteKind::kCollectionCreate;
        create_collection.body = "{\"name\":\"books\"}";

        NuRaftAppliedRequest write_document;
        write_document.index = 2;
        write_document.route_hash = 102;
        write_document.route_kind = NuRaftRouteKind::kDocumentWrite;
        write_document.params = {{"collection", "books"}, {"id", "doc-1"}};
        write_document.body = "{\"id\":\"doc-1\",\"title\":\"Dune\"}";

        NuRaftAppliedRequest delete_document;
        delete_document.index = 3;
        delete_document.route_hash = 103;
        delete_document.route_kind = NuRaftRouteKind::kDocumentDelete;
        delete_document.params = {{"collection", "books"}, {"id", "doc-1"}};
        delete_document.body = "";

        ASSERT_TRUE(first_sink.apply_all({create_collection, write_document, delete_document}, error)) << error;
    }

    NuRaftKvStateMachineSink restarted_sink(layout);
    std::vector<NuRaftAppliedRequest> applied_requests;
    ASSERT_TRUE(restarted_sink.read_all(applied_requests, error)) << error;
    ASSERT_EQ(applied_requests.size(), 3u);

    std::vector<std::pair<std::string, std::string>> entries;
    ASSERT_TRUE(restarted_sink.read_materialized_entries(entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].first, "state/collections/books");
}
