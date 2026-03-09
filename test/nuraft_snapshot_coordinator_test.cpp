#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

#include "nuraft/nuraft_metadata_store.h"
#include "nuraft/nuraft_prototype_state_machine.h"
#include "nuraft/nuraft_request_journal.h"
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

TEST_F(NuRaftSnapshotCoordinatorTest, CreatesAndInstallsPrototypeSnapshotExport) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");
    const std::string source_dir = (std::filesystem::path(temp_dir_) / "source").string();
    const std::string restored_dir = (std::filesystem::path(temp_dir_) / "restored").string();
    const std::string export_dir = (std::filesystem::path(temp_dir_) / "exported-snapshot").string();
    initialize_node(source_dir);

    const NuRaftStateLayout source_layout = NuRaftStateLayout::from_data_dir(source_dir);
    std::string error;
    NuRaftRequestJournal journal(source_layout);
    ASSERT_TRUE(journal.initialize(error)) << error;

    uint64_t index = 0;
    ASSERT_TRUE(journal.append_request_json(
        std::string("{\"route_hash\":") + std::to_string(collection_create_hash) +
            ",\"params\":{},\"body\":\"{\\\"name\\\":\\\"books\\\"}\"}",
        index,
        error)) << error;
    ASSERT_TRUE(journal.append_request_json(
        std::string("{\"route_hash\":") + std::to_string(document_write_hash) +
            ",\"params\":{\"collection\":\"books\",\"id\":\"doc-1\"},"
            "\"body\":\"{\\\"id\\\":\\\"doc-1\\\",\\\"title\\\":\\\"Dune\\\"}\"}",
        index,
        error)) << error;

    auto source_sink = std::make_unique<NuRaftKvStateMachineSink>(source_layout);
    NuRaftKvStateMachineSink* source_sink_ptr = source_sink.get();
    NuRaftPrototypeStateMachine state_machine(source_layout, std::move(source_sink));
    ASSERT_TRUE(state_machine.initialize(error)) << error;

    std::vector<NuRaftLogEntry> applied_entries;
    ASSERT_TRUE(state_machine.apply_pending(applied_entries, error)) << error;
    ASSERT_EQ(applied_entries.size(), 2u);

    NuRaftSnapshotCoordinator coordinator(source_layout);
    NuRaftSnapshotDescriptor descriptor;
    ASSERT_TRUE(coordinator.create_snapshot(export_dir, source_sink_ptr, descriptor, error)) << error;
    EXPECT_EQ(descriptor.last_log_index, 2u);
    EXPECT_EQ(descriptor.last_applied_index, 2u);

    NuRaftSnapshotDescriptor persisted_descriptor;
    ASSERT_TRUE(coordinator.read_last_snapshot(persisted_descriptor, error)) << error;
    EXPECT_EQ(persisted_descriptor, descriptor);
    EXPECT_TRUE(std::filesystem::is_directory(std::filesystem::path(source_layout.snapshot_dir) / descriptor.snapshot_id));
    EXPECT_TRUE(std::filesystem::is_directory(
        std::filesystem::path(export_dir) / "state" / NuRaftStateLayout::kPrototypeRootName / "materialized_state"));

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

    NuRaftMetadataStore restored_metadata(NuRaftStateLayout::from_data_dir(restored_dir));
    NuRaftIdentity restored_identity_after;
    ASSERT_TRUE(restored_metadata.read_identity(restored_identity_after, error)) << error;
    EXPECT_EQ(restored_identity_after, restored_identity_before);

    NuRaftReplayProgress restored_progress;
    ASSERT_TRUE(restored_metadata.read_replay_progress(restored_progress, error)) << error;
    EXPECT_EQ(restored_progress.last_applied_index, 2u);

    NuRaftRequestJournal restored_journal(NuRaftStateLayout::from_data_dir(restored_dir));
    ASSERT_TRUE(restored_journal.initialize(error)) << error;
    std::vector<NuRaftLogEntry> restored_entries;
    ASSERT_TRUE(restored_journal.replay(restored_entries, error)) << error;
    ASSERT_EQ(restored_entries.size(), 2u);

    NuRaftKvStateMachineSink restored_sink(NuRaftStateLayout::from_data_dir(restored_dir));
    std::vector<std::pair<std::string, std::string>> entries;
    ASSERT_TRUE(restored_sink.read_materialized_entries(entries, error)) << error;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].first, "state/collections/books");
    EXPECT_EQ(entries[1].first, "state/documents/books/doc-1");
}
