#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>
#include <unistd.h>

#include "json.hpp"
#include "nuraft/nuraft_state_machine_sink.h"

namespace {

std::map<std::string, std::string> to_entry_map(
    const std::vector<std::pair<std::string, std::string>>& entries) {
    std::map<std::string, std::string> result;
    for (const auto& entry : entries) {
        result[entry.first] = entry.second;
    }
    return result;
}

}  // namespace

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

TEST_F(NuRaftStateMachineSinkTest, MaterializesChunkedImportReplayAcrossKvSinkRestart) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    const std::string request_id = "00000000000000000042";
    std::string error;

    NuRaftAppliedRequest create_collection;
    create_collection.index = 1;
    create_collection.route_hash = 101;
    create_collection.route_kind = NuRaftRouteKind::kCollectionCreate;
    create_collection.body = "{\"name\":\"books\"}";

    NuRaftAppliedRequest import_chunk_one;
    import_chunk_one.index = 2;
    import_chunk_one.route_hash = 201;
    import_chunk_one.route_kind = NuRaftRouteKind::kDocumentImport;
    import_chunk_one.params = {{"collection", "books"}};
    import_chunk_one.body = "{\"id\":\"doc-1\",\"title\":\"Dune\"}\n{\"id\":\"doc";
    import_chunk_one.first_chunk_aggregate = true;
    import_chunk_one.last_chunk_aggregate = false;
    import_chunk_one.start_ts = 42;

    {
        NuRaftKvStateMachineSink first_sink(layout);
        ASSERT_TRUE(first_sink.apply_all({create_collection, import_chunk_one}, error)) << error;

        std::vector<std::pair<std::string, std::string>> first_entries;
        ASSERT_TRUE(first_sink.read_materialized_entries(first_entries, error)) << error;
        const auto first_map = to_entry_map(first_entries);
        ASSERT_EQ(first_map.at("state/documents/books/doc-1"), "{\"id\":\"doc-1\",\"title\":\"Dune\"}");
        ASSERT_EQ(first_map.at("state/import_buffers/books/" + request_id), "{\"id\":\"doc");

        const auto first_summary = nlohmann::json::parse(first_map.at("state/imports/books/" + request_id));
        EXPECT_EQ(first_summary["chunk_count"], 1);
        EXPECT_EQ(first_summary["document_count"], 1);
        EXPECT_EQ(first_summary["pending_body_bytes"], 10);
        EXPECT_EQ(first_summary["complete"], false);
    }

    NuRaftAppliedRequest import_chunk_two;
    import_chunk_two.index = 3;
    import_chunk_two.route_hash = 201;
    import_chunk_two.route_kind = NuRaftRouteKind::kDocumentImport;
    import_chunk_two.params = {{"collection", "books"}};
    import_chunk_two.body = "-2\",\"title\":\"Foundation\"}\n";
    import_chunk_two.first_chunk_aggregate = false;
    import_chunk_two.last_chunk_aggregate = true;
    import_chunk_two.start_ts = 42;

    NuRaftKvStateMachineSink restarted_sink(layout);
    ASSERT_TRUE(restarted_sink.apply_all({import_chunk_two}, error)) << error;

    std::vector<std::pair<std::string, std::string>> entries;
    ASSERT_TRUE(restarted_sink.read_materialized_entries(entries, error)) << error;
    const auto entry_map = to_entry_map(entries);
    ASSERT_EQ(entry_map.at("state/collections/books"), "{\"name\":\"books\"}");
    ASSERT_EQ(entry_map.at("state/documents/books/doc-1"), "{\"id\":\"doc-1\",\"title\":\"Dune\"}");
    ASSERT_EQ(entry_map.at("state/documents/books/doc-2"), "{\"id\":\"doc-2\",\"title\":\"Foundation\"}");
    EXPECT_EQ(entry_map.count("state/import_buffers/books/" + request_id), 0u);

    const auto summary = nlohmann::json::parse(entry_map.at("state/imports/books/" + request_id));
    EXPECT_EQ(summary["chunk_count"], 2);
    EXPECT_EQ(summary["document_count"], 2);
    EXPECT_EQ(summary["pending_body_bytes"], 0);
    EXPECT_EQ(summary["complete"], true);
    EXPECT_EQ(summary["last_index"], 3);
}
