#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "nuraft/nuraft_prototype_state_machine.h"
#include "nuraft/nuraft_recovery_coordinator.h"
#include "nuraft/nuraft_request_journal.h"
#include "nuraft/nuraft_state_initializer.h"
#include "nuraft/nuraft_state_machine_sink.h"
#include "nuraft/nuraft_static_cluster.h"
#include "string_utils.h"

namespace {

uint64_t make_route_hash(const std::string& method, const std::string& path) {
    const std::string method_path = method + path;
    const uint64_t hash = StringUtils::hash_wy(method_path.c_str(), method_path.size());
    return (hash > 100) ? hash : (hash + 100);
}

bool replicate_missing_entries_for_test(const std::string& leader_data_dir,
                                        const std::string& follower_data_dir,
                                        std::string& error) {
    NuRaftRequestJournal leader_journal(NuRaftStateLayout::from_data_dir(leader_data_dir));
    if (!leader_journal.initialize(error)) {
        return false;
    }

    std::vector<NuRaftLogEntry> leader_entries;
    if (!leader_journal.replay(leader_entries, error)) {
        return false;
    }

    NuRaftRequestJournal follower_journal(NuRaftStateLayout::from_data_dir(follower_data_dir));
    if (!follower_journal.initialize(error)) {
        return false;
    }

    std::vector<NuRaftLogEntry> follower_entries;
    if (!follower_journal.replay(follower_entries, error)) {
        return false;
    }

    for (size_t i = 0; i < follower_entries.size(); ++i) {
        if (!(follower_entries[i] == leader_entries[i])) {
            error = "Test follower log diverges from leader history";
            return false;
        }
    }

    for (size_t i = follower_entries.size(); i < leader_entries.size(); ++i) {
        uint64_t appended_index = 0;
        if (!follower_journal.append_request_json(leader_entries[i].envelope.request_json(), appended_index, error)) {
            return false;
        }
    }

    error.clear();
    return true;
}

bool apply_pending_kv_for_test(const std::string& data_dir, std::string& error) {
    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(data_dir);
    auto sink = std::make_unique<NuRaftKvStateMachineSink>(layout);
    NuRaftPrototypeStateMachine state_machine(layout, std::move(sink));
    if (!state_machine.initialize(error)) {
        return false;
    }

    std::vector<NuRaftLogEntry> applied_entries;
    return state_machine.apply_pending(applied_entries, error);
}

}  // namespace

class NuRaftRecoveryCoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_recovery_coordinator_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string node_dir(const std::string& name) const {
        return (std::filesystem::path(temp_dir_) / name).string();
    }

    void initialize_node(const std::string& data_dir,
                         uint32_t peer_port,
                         uint32_t api_port,
                         const std::string& nodes_config) {
        NuRaftPrototypeOptions options;
        options.data_dir = data_dir;
        options.local_host = "127.0.0.1";
        options.peer_port = peer_port;
        options.api_port = api_port;
        options.nodes_config = nodes_config;

        NuRaftIdentity identity;
        NuRaftBootstrapConfig bootstrap_config;
        std::string error;
        ASSERT_TRUE(NuRaftStateInitializer::initialize(options, identity, bootstrap_config, error)) << error;
    }

    std::string temp_dir_;
};

TEST_F(NuRaftRecoveryCoordinatorTest, TimedSnapshotCanProgressWithUnhealthyFollowerWhenPolicyAllowsIt) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");
    const std::string nodes_config = "127.0.0.1:7107:8108,127.0.0.1:7109:8109,127.0.0.1:7111:8110";
    const std::string node1 = node_dir("node1");
    const std::string node2 = node_dir("node2");
    const std::string node3 = node_dir("node3");

    initialize_node(node1, 7107, 8108, nodes_config);
    initialize_node(node2, 7109, 8109, nodes_config);
    initialize_node(node3, 7111, 8110, nodes_config);

    NuRaftMetadataStore metadata_store(NuRaftStateLayout::from_data_dir(node1));
    NuRaftBootstrapConfig bootstrap_config;
    std::string error;
    ASSERT_TRUE(metadata_store.read_bootstrap_config(bootstrap_config, error)) << error;

    const std::map<int32_t, std::string> data_dirs = {
        {8108, node1},
        {8109, node2},
        {8110, node3},
    };

    uint64_t appended_index = 0;
    bool forwarded_to_leader = false;
    int32_t target_server_id = 0;
    ASSERT_TRUE(NuRaftStaticCluster::append_request(bootstrap_config,
                                                    data_dirs,
                                                    8108,
                                                    8109,
                                                    "{\"route_hash\":" + std::to_string(collection_create_hash) +
                                                        ",\"params\":{},\"body\":\"{\\\"name\\\":\\\"books\\\"}\"}",
                                                    appended_index,
                                                    forwarded_to_leader,
                                                    target_server_id,
                                                    error)) << error;

    std::vector<NuRaftStaticClusterNodeStatus> statuses;
    ASSERT_TRUE(NuRaftStaticCluster::replicate_and_apply(bootstrap_config,
                                                         data_dirs,
                                                         8108,
                                                         "kv",
                                                         statuses,
                                                         error)) << error;

    std::map<int32_t, bool> all_healthy = {
        {8108, true},
        {8109, true},
        {8110, true},
    };
    int64_t last_snapshot_time = 0;
    NuRaftTimedSnapshotResult initial_snapshot;
    ASSERT_TRUE(NuRaftRecoveryCoordinator::run_timed_snapshot(bootstrap_config,
                                                              data_dirs,
                                                              8108,
                                                              all_healthy,
                                                              NuRaftTimedSnapshotPolicy::kLeaderOnly,
                                                              100,
                                                              60,
                                                              last_snapshot_time,
                                                              initial_snapshot,
                                                              error)) << error;
    ASSERT_TRUE(initial_snapshot.created_snapshot);
    EXPECT_EQ(initial_snapshot.descriptor.last_applied_index, 1u);

    ASSERT_TRUE(NuRaftStaticCluster::append_request(bootstrap_config,
                                                    data_dirs,
                                                    8108,
                                                    8109,
                                                    "{\"route_hash\":" + std::to_string(document_write_hash) +
                                                        ",\"params\":{\"collection\":\"books\",\"id\":\"doc-1\"},\"body\":\"{\\\"id\\\":\\\"doc-1\\\",\\\"title\\\":\\\"Dune\\\"}\"}",
                                                    appended_index,
                                                    forwarded_to_leader,
                                                    target_server_id,
                                                    error)) << error;
    ASSERT_TRUE(NuRaftStaticCluster::append_request(bootstrap_config,
                                                    data_dirs,
                                                    8108,
                                                    8109,
                                                    "{\"route_hash\":" + std::to_string(document_write_hash) +
                                                        ",\"params\":{\"collection\":\"books\",\"id\":\"doc-2\"},\"body\":\"{\\\"id\\\":\\\"doc-2\\\",\\\"title\\\":\\\"Foundation\\\"}\"}",
                                                    appended_index,
                                                    forwarded_to_leader,
                                                    target_server_id,
                                                    error)) << error;

    std::map<int32_t, bool> follower_three_down = {
        {8108, true},
        {8109, true},
        {8110, false},
    };
    int64_t blocked_last_snapshot_time = last_snapshot_time;
    NuRaftTimedSnapshotResult blocked_snapshot;
    ASSERT_TRUE(NuRaftRecoveryCoordinator::run_timed_snapshot(bootstrap_config,
                                                              data_dirs,
                                                              8108,
                                                              follower_three_down,
                                                              NuRaftTimedSnapshotPolicy::kRequireHealthyPeers,
                                                              200,
                                                              60,
                                                              blocked_last_snapshot_time,
                                                              blocked_snapshot,
                                                              error)) << error;
    EXPECT_TRUE(blocked_snapshot.due);
    EXPECT_FALSE(blocked_snapshot.created_snapshot);
    EXPECT_TRUE(blocked_snapshot.blocked_by_unhealthy_peer);
    ASSERT_EQ(blocked_snapshot.unhealthy_peer_ids.size(), 1u);
    EXPECT_EQ(blocked_snapshot.unhealthy_peer_ids[0], 8110);

    NuRaftSnapshotDescriptor stale_descriptor;
    ASSERT_TRUE(NuRaftRecoveryCoordinator::install_latest_snapshot(node1, node3, stale_descriptor, error)) << error;
    EXPECT_EQ(stale_descriptor.last_applied_index, 1u);

    {
        NuRaftKvStateMachineSink stale_sink(NuRaftStateLayout::from_data_dir(node3));
        std::vector<std::pair<std::string, std::string>> stale_entries;
        ASSERT_TRUE(stale_sink.read_materialized_entries(stale_entries, error)) << error;
        ASSERT_EQ(stale_entries.size(), 1u);
        EXPECT_EQ(stale_entries[0].first, "state/collections/books");
    }

    NuRaftTimedSnapshotResult fresh_snapshot;
    ASSERT_TRUE(NuRaftRecoveryCoordinator::run_timed_snapshot(bootstrap_config,
                                                              data_dirs,
                                                              8108,
                                                              follower_three_down,
                                                              NuRaftTimedSnapshotPolicy::kLeaderOnly,
                                                              200,
                                                              60,
                                                              last_snapshot_time,
                                                              fresh_snapshot,
                                                              error)) << error;
    EXPECT_TRUE(fresh_snapshot.created_snapshot);
    EXPECT_EQ(fresh_snapshot.descriptor.last_applied_index, 3u);
    EXPECT_FALSE(fresh_snapshot.blocked_by_unhealthy_peer);

    NuRaftSnapshotDescriptor installed_descriptor;
    ASSERT_TRUE(NuRaftRecoveryCoordinator::install_latest_snapshot(node1, node3, installed_descriptor, error)) << error;
    EXPECT_EQ(installed_descriptor.last_applied_index, 3u);

    NuRaftKvStateMachineSink follower_sink(NuRaftStateLayout::from_data_dir(node3));
    std::vector<std::pair<std::string, std::string>> follower_entries;
    ASSERT_TRUE(follower_sink.read_materialized_entries(follower_entries, error)) << error;
    ASSERT_EQ(follower_entries.size(), 3u);
    EXPECT_EQ(follower_entries[0].first, "state/collections/books");
    EXPECT_EQ(follower_entries[1].first, "state/documents/books/doc-1");
    EXPECT_EQ(follower_entries[2].first, "state/documents/books/doc-2");
}

TEST_F(NuRaftRecoveryCoordinatorTest, FreshTimedSnapshotReducesFollowerReplayAfterRecovery) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");
    const std::string nodes_config = "127.0.0.1:7107:8108,127.0.0.1:7109:8109,127.0.0.1:7111:8110";
    const std::string node1 = node_dir("replay-node1");
    const std::string node2 = node_dir("replay-node2");
    const std::string node3 = node_dir("replay-node3");

    initialize_node(node1, 7107, 8108, nodes_config);
    initialize_node(node2, 7109, 8109, nodes_config);
    initialize_node(node3, 7111, 8110, nodes_config);

    NuRaftMetadataStore metadata_store(NuRaftStateLayout::from_data_dir(node1));
    NuRaftBootstrapConfig bootstrap_config;
    std::string error;
    ASSERT_TRUE(metadata_store.read_bootstrap_config(bootstrap_config, error)) << error;

    const std::map<int32_t, std::string> data_dirs = {
        {8108, node1},
        {8109, node2},
        {8110, node3},
    };

    uint64_t appended_index = 0;
    bool forwarded_to_leader = false;
    int32_t target_server_id = 0;
    ASSERT_TRUE(NuRaftStaticCluster::append_request(bootstrap_config,
                                                    data_dirs,
                                                    8108,
                                                    8109,
                                                    "{\"route_hash\":" + std::to_string(collection_create_hash) +
                                                        ",\"params\":{},\"body\":\"{\\\"name\\\":\\\"books\\\"}\"}",
                                                    appended_index,
                                                    forwarded_to_leader,
                                                    target_server_id,
                                                    error)) << error;
    std::vector<NuRaftStaticClusterNodeStatus> statuses;
    ASSERT_TRUE(NuRaftStaticCluster::replicate_and_apply(bootstrap_config,
                                                         data_dirs,
                                                         8108,
                                                         "kv",
                                                         statuses,
                                                         error)) << error;

    for (int doc_id = 1; doc_id <= 5; ++doc_id) {
        ASSERT_TRUE(NuRaftStaticCluster::append_request(
            bootstrap_config,
            data_dirs,
            8108,
            8109,
            "{\"route_hash\":" + std::to_string(document_write_hash) +
                ",\"params\":{\"collection\":\"books\",\"id\":\"doc-" + std::to_string(doc_id) +
                "\"},\"body\":\"{\\\"id\\\":\\\"doc-" + std::to_string(doc_id) +
                "\\\",\\\"title\\\":\\\"Book " + std::to_string(doc_id) + "\\\"}\"}",
            appended_index,
            forwarded_to_leader,
            target_server_id,
            error)) << error;
    }

    std::map<int32_t, bool> follower_three_down = {
        {8108, true},
        {8109, true},
        {8110, false},
    };
    int64_t last_snapshot_time = 0;
    NuRaftTimedSnapshotResult fresh_snapshot;
    ASSERT_TRUE(NuRaftRecoveryCoordinator::run_timed_snapshot(bootstrap_config,
                                                              data_dirs,
                                                              8108,
                                                              follower_three_down,
                                                              NuRaftTimedSnapshotPolicy::kLeaderOnly,
                                                              100,
                                                              60,
                                                              last_snapshot_time,
                                                              fresh_snapshot,
                                                              error)) << error;
    EXPECT_TRUE(fresh_snapshot.created_snapshot);
    EXPECT_EQ(fresh_snapshot.descriptor.last_applied_index, 6u);

    ASSERT_TRUE(NuRaftStaticCluster::append_request(bootstrap_config,
                                                    data_dirs,
                                                    8108,
                                                    8109,
                                                    "{\"route_hash\":" + std::to_string(document_write_hash) +
                                                        ",\"params\":{\"collection\":\"books\",\"id\":\"doc-6\"},\"body\":\"{\\\"id\\\":\\\"doc-6\\\",\\\"title\\\":\\\"Book 6\\\"}\"}",
                                                    appended_index,
                                                    forwarded_to_leader,
                                                    target_server_id,
                                                    error)) << error;

    NuRaftSnapshotDescriptor installed_descriptor;
    ASSERT_TRUE(NuRaftRecoveryCoordinator::install_latest_snapshot(node1, node3, installed_descriptor, error)) << error;
    EXPECT_EQ(installed_descriptor.last_applied_index, 6u);

    ASSERT_TRUE(replicate_missing_entries_for_test(node1, node3, error)) << error;

    NuRaftMetadataStore follower_metadata(NuRaftStateLayout::from_data_dir(node3));
    NuRaftReplayProgress progress_before_replay;
    ASSERT_TRUE(follower_metadata.read_replay_progress(progress_before_replay, error)) << error;
    EXPECT_EQ(progress_before_replay.last_applied_index, 6u);

    ASSERT_TRUE(apply_pending_kv_for_test(node3, error)) << error;

    NuRaftReplayProgress progress_after_replay;
    ASSERT_TRUE(follower_metadata.read_replay_progress(progress_after_replay, error)) << error;
    EXPECT_EQ(progress_after_replay.last_applied_index, 7u);

    NuRaftKvStateMachineSink follower_sink(NuRaftStateLayout::from_data_dir(node3));
    std::vector<std::pair<std::string, std::string>> entries;
    ASSERT_TRUE(follower_sink.read_materialized_entries(entries, error)) << error;
    ASSERT_EQ(entries.size(), 7u);
    EXPECT_EQ(entries.back().first, "state/documents/books/doc-6");
}
