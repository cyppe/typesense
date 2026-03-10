#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

#include "nuraft/nuraft_applied_request_store.h"
#include "nuraft/nuraft_metadata_store.h"
#include "nuraft/nuraft_snapshot_coordinator.h"
#include "nuraft/nuraft_state_initializer.h"
#include "nuraft/nuraft_state_machine_sink.h"
#include "string_utils.h"

namespace {

uint64_t make_route_hash(const std::string& method, const std::string& path) {
    const std::string method_path = method + path;
    const uint64_t hash = StringUtils::hash_wy(method_path.c_str(), method_path.size());
    return (hash > 100) ? hash : (hash + 100);
}

}  // namespace

class NuRaftSnapshotCoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_snapshot_coordinator_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    void initialize_node(const std::string& data_dir) {
        NuRaftPrototypeOptions options;
        options.data_dir = data_dir;
        options.local_host = "127.0.0.1";
        options.peer_port = 7107;
        options.api_port = 8108;

        NuRaftIdentity identity;
        NuRaftBootstrapConfig bootstrap_config;
        std::string error;
        ASSERT_TRUE(NuRaftStateInitializer::initialize(options, identity, bootstrap_config, error)) << error;
    }

    std::string temp_dir_;
};

TEST_F(NuRaftSnapshotCoordinatorTest, CreatesAndInstallsSnapshotExport) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");
    const std::string source_dir = (std::filesystem::path(temp_dir_) / "source").string();
    const std::string restored_dir = (std::filesystem::path(temp_dir_) / "restored").string();
    const std::string export_dir = (std::filesystem::path(temp_dir_) / "exported-snapshot").string();
    initialize_node(source_dir);

    const NuRaftStateLayout source_layout = NuRaftStateLayout::from_data_dir(source_dir);
    std::string error;

    // Populate the KV sink with applied requests directly.
    auto source_sink = std::make_unique<NuRaftKvStateMachineSink>(source_layout);

    NuRaftAppliedRequest req1;
    req1.index = 1;
    req1.route_hash = collection_create_hash;
    req1.body = R"({"name":"books"})";
    NuRaftAppliedRequest req2;
    req2.index = 2;
    req2.route_hash = document_write_hash;
    req2.params = {{"collection", "books"}, {"id", "doc-1"}};
    req2.body = R"({"id":"doc-1","title":"Dune"})";

    ASSERT_TRUE(source_sink->apply_all({req1, req2}, error)) << error;

    NuRaftSnapshotCoordinator coordinator(source_layout);
    NuRaftSnapshotDescriptor descriptor;
    ASSERT_TRUE(coordinator.create_snapshot(export_dir, source_sink.get(), descriptor, error)) << error;
    EXPECT_EQ(descriptor.last_log_index, 2u);
    EXPECT_EQ(descriptor.last_applied_index, 2u);

    NuRaftSnapshotDescriptor persisted_descriptor;
    ASSERT_TRUE(coordinator.read_last_snapshot(persisted_descriptor, error)) << error;
    EXPECT_EQ(persisted_descriptor, descriptor);
    EXPECT_TRUE(std::filesystem::is_directory(std::filesystem::path(source_layout.snapshot_dir) / descriptor.snapshot_id));
    EXPECT_TRUE(std::filesystem::is_directory(
        std::filesystem::path(export_dir) / "state" / NuRaftStateLayout::kPrototypeRootName / "meta"));
    EXPECT_TRUE(std::filesystem::is_directory(
        std::filesystem::path(export_dir) / "state" / NuRaftStateLayout::kPrototypeRootName /
            "snapshot" / descriptor.snapshot_id / "materialized_state"));

    NuRaftPrototypeOptions restored_options;
    restored_options.data_dir = restored_dir;
    restored_options.local_host = "127.0.0.1";
    restored_options.peer_port = 7207;
    restored_options.api_port = 8208;
    NuRaftIdentity restored_identity_before;
    NuRaftBootstrapConfig restored_bootstrap_before;
    ASSERT_TRUE(NuRaftStateInitializer::initialize(restored_options,
                                                   restored_identity_before,
                                                   restored_bootstrap_before,
                                                   error)) << error;

    NuRaftSnapshotCoordinator restore_coordinator(NuRaftStateLayout::from_data_dir(restored_dir));
    NuRaftSnapshotDescriptor restored_descriptor;
    ASSERT_TRUE(restore_coordinator.install_snapshot(export_dir, restored_descriptor, error)) << error;
    EXPECT_EQ(restored_descriptor, descriptor);
    EXPECT_TRUE(std::filesystem::is_directory(
        std::filesystem::path(NuRaftStateLayout::from_data_dir(restored_dir).snapshot_dir) / descriptor.snapshot_id));

    NuRaftMetadataStore restored_metadata(NuRaftStateLayout::from_data_dir(restored_dir));
    NuRaftIdentity restored_identity_after;
    ASSERT_TRUE(restored_metadata.read_identity(restored_identity_after, error)) << error;
    EXPECT_EQ(restored_identity_after, restored_identity_before);

    NuRaftKvStateMachineSink restored_sink(NuRaftStateLayout::from_data_dir(restored_dir));
    std::vector<std::pair<std::string, std::string>> entries;
    ASSERT_TRUE(restored_sink.read_materialized_entries(entries, error)) << error;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].first, "state/collections/books");
    EXPECT_EQ(entries[1].first, "state/documents/books/doc-1");
}

TEST_F(NuRaftSnapshotCoordinatorTest, InstallClearsStaleMaterializedStateWhenSourceSnapshotHasNone) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const std::string source_dir = (std::filesystem::path(temp_dir_) / "source-no-kv").string();
    const std::string restored_dir = (std::filesystem::path(temp_dir_) / "restored-with-kv").string();
    const std::string export_dir = (std::filesystem::path(temp_dir_) / "exported-no-kv").string();
    initialize_node(source_dir);
    initialize_node(restored_dir);

    const NuRaftStateLayout source_layout = NuRaftStateLayout::from_data_dir(source_dir);
    std::string error;

    NuRaftSnapshotCoordinator source_coordinator(source_layout);
    NuRaftSnapshotDescriptor source_descriptor;
    ASSERT_TRUE(source_coordinator.create_snapshot(export_dir, nullptr, source_descriptor, error)) << error;

    // Create stale materialized state in the restored node.
    const NuRaftStateLayout restored_layout = NuRaftStateLayout::from_data_dir(restored_dir);
    {
        NuRaftKvStateMachineSink sink(restored_layout);
        NuRaftAppliedRequest stale_req;
        stale_req.index = 1;
        stale_req.route_hash = collection_create_hash;
        stale_req.body = R"({"name":"stale"})";
        ASSERT_TRUE(sink.apply_all({stale_req}, error)) << error;
    }

    ASSERT_TRUE(std::filesystem::is_directory(restored_layout.materialized_state_dir));

    NuRaftSnapshotCoordinator restored_coordinator(restored_layout);
    NuRaftSnapshotDescriptor restored_descriptor;
    ASSERT_TRUE(restored_coordinator.install_snapshot(export_dir, restored_descriptor, error)) << error;
    EXPECT_EQ(restored_descriptor, source_descriptor);
    EXPECT_FALSE(std::filesystem::exists(restored_layout.materialized_state_dir));
}
