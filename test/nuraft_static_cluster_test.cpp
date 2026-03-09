#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>
#include <unistd.h>

#include "nuraft/nuraft_metadata_store.h"
#include "nuraft/nuraft_prototype_state_machine.h"
#include "nuraft/nuraft_request_journal.h"
#include "nuraft/nuraft_snapshot_coordinator.h"
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

    if (follower_entries.size() > leader_entries.size()) {
        error = "Test follower log is ahead of the leader";
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
        if (appended_index != leader_entries[i].index) {
            error = "Test follower append assigned an unexpected index";
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

class NuRaftStaticClusterTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_static_cluster_test-") + std::to_string(getpid()))).string();
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
                         const std::string& host,
                         uint32_t peer_port,
                         uint32_t api_port,
                         const std::string& nodes_config) {
        NuRaftPrototypeOptions options;
        options.data_dir = data_dir;
        options.local_host = host;
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

TEST_F(NuRaftStaticClusterTest, ReplicatesLeaderLogAndAppliesFollowers) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");
    const std::string nodes_config = "127.0.0.1:7107:8108,127.0.0.1:7109:8109,127.0.0.1:7111:8110";
    const std::string node1 = node_dir("node1");
    const std::string node2 = node_dir("node2");
    const std::string node3 = node_dir("node3");

    initialize_node(node1, "127.0.0.1", 7107, 8108, nodes_config);
    initialize_node(node2, "127.0.0.1", 7109, 8109, nodes_config);
    initialize_node(node3, "127.0.0.1", 7111, 8110, nodes_config);

    NuRaftMetadataStore metadata_store(NuRaftStateLayout::from_data_dir(node2));
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
    EXPECT_EQ(appended_index, 1u);
    EXPECT_TRUE(forwarded_to_leader);
    EXPECT_EQ(target_server_id, 8108);

    ASSERT_TRUE(NuRaftStaticCluster::append_request(bootstrap_config,
                                                    data_dirs,
                                                    8108,
                                                    8110,
                                                    "{\"route_hash\":" + std::to_string(document_write_hash) +
                                                        ",\"params\":{\"collection\":\"books\",\"id\":\"doc-1\"},\"body\":\"{\\\"id\\\":\\\"doc-1\\\",\\\"title\\\":\\\"Dune\\\"}\"}",
                                                    appended_index,
                                                    forwarded_to_leader,
                                                    target_server_id,
                                                    error)) << error;
    EXPECT_EQ(appended_index, 2u);
    EXPECT_TRUE(forwarded_to_leader);
    EXPECT_EQ(target_server_id, 8108);

    std::vector<NuRaftStaticClusterNodeStatus> statuses;
    ASSERT_TRUE(NuRaftStaticCluster::replicate_and_apply(bootstrap_config,
                                                         data_dirs,
                                                         8108,
                                                         "kv",
                                                         statuses,
                                                         error)) << error;
    ASSERT_EQ(statuses.size(), 3u);
    EXPECT_EQ(statuses[0].server_id, 8108);
    EXPECT_TRUE(statuses[0].is_leader);
    EXPECT_EQ(statuses[0].last_log_index, 2u);
    EXPECT_EQ(statuses[0].last_applied_index, 2u);
    EXPECT_EQ(statuses[1].last_log_index, 2u);
    EXPECT_EQ(statuses[1].last_applied_index, 2u);
    EXPECT_EQ(statuses[2].last_log_index, 2u);
    EXPECT_EQ(statuses[2].last_applied_index, 2u);

    NuRaftKvStateMachineSink follower_sink(NuRaftStateLayout::from_data_dir(node3));
    std::vector<std::pair<std::string, std::string>> entries;
    ASSERT_TRUE(follower_sink.read_materialized_entries(entries, error)) << error;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].first, "state/collections/books");
    EXPECT_EQ(entries[1].first, "state/documents/books/doc-1");
}

TEST_F(NuRaftStaticClusterTest, CollectsLaggingFollowerStatusBeforeReplication) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const std::string nodes_config = "127.0.0.1:7107:8108,127.0.0.1:7109:8109,127.0.0.1:7111:8110";
    const std::string node1 = node_dir("lag-node1");
    const std::string node2 = node_dir("lag-node2");
    const std::string node3 = node_dir("lag-node3");

    initialize_node(node1, "127.0.0.1", 7107, 8108, nodes_config);
    initialize_node(node2, "127.0.0.1", 7109, 8109, nodes_config);
    initialize_node(node3, "127.0.0.1", 7111, 8110, nodes_config);

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
    EXPECT_EQ(appended_index, 1u);
    EXPECT_TRUE(forwarded_to_leader);

    std::vector<NuRaftStaticClusterNodeStatus> statuses;
    ASSERT_TRUE(NuRaftStaticCluster::collect_status(bootstrap_config, data_dirs, 8108, statuses, error)) << error;
    ASSERT_EQ(statuses.size(), 3u);
    EXPECT_EQ(statuses[0].server_id, 8108);
    EXPECT_TRUE(statuses[0].is_leader);
    EXPECT_EQ(statuses[0].last_log_index, 1u);
    EXPECT_EQ(statuses[0].last_applied_index, 0u);
    EXPECT_EQ(statuses[1].last_log_index, 0u);
    EXPECT_EQ(statuses[1].last_applied_index, 0u);
    EXPECT_EQ(statuses[2].last_log_index, 0u);
    EXPECT_EQ(statuses[2].last_applied_index, 0u);
}

TEST_F(NuRaftStaticClusterTest, SupportsLeaderSwitchAfterCatchUp) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");
    const std::string nodes_config = "127.0.0.1:7107:8108,127.0.0.1:7109:8109,127.0.0.1:7111:8110";
    const std::string node1 = node_dir("switch-node1");
    const std::string node2 = node_dir("switch-node2");
    const std::string node3 = node_dir("switch-node3");

    initialize_node(node1, "127.0.0.1", 7107, 8108, nodes_config);
    initialize_node(node2, "127.0.0.1", 7109, 8109, nodes_config);
    initialize_node(node3, "127.0.0.1", 7111, 8110, nodes_config);

    NuRaftMetadataStore metadata_store(NuRaftStateLayout::from_data_dir(node2));
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
    EXPECT_EQ(target_server_id, 8108);

    std::vector<NuRaftStaticClusterNodeStatus> statuses;
    ASSERT_TRUE(NuRaftStaticCluster::replicate_and_apply(bootstrap_config,
                                                         data_dirs,
                                                         8108,
                                                         "kv",
                                                         statuses,
                                                         error)) << error;
    ASSERT_EQ(statuses.size(), 3u);

    ASSERT_TRUE(NuRaftStaticCluster::append_request(bootstrap_config,
                                                    data_dirs,
                                                    8109,
                                                    8110,
                                                    "{\"route_hash\":" + std::to_string(document_write_hash) +
                                                        ",\"params\":{\"collection\":\"books\",\"id\":\"doc-1\"},\"body\":\"{\\\"id\\\":\\\"doc-1\\\",\\\"title\\\":\\\"Dune\\\"}\"}",
                                                    appended_index,
                                                    forwarded_to_leader,
                                                    target_server_id,
                                                    error)) << error;
    EXPECT_EQ(appended_index, 2u);
    EXPECT_TRUE(forwarded_to_leader);
    EXPECT_EQ(target_server_id, 8109);

    ASSERT_TRUE(NuRaftStaticCluster::replicate_and_apply(bootstrap_config,
                                                         data_dirs,
                                                         8109,
                                                         "kv",
                                                         statuses,
                                                         error)) << error;
    ASSERT_EQ(statuses.size(), 3u);
    EXPECT_FALSE(statuses[0].is_leader);
    EXPECT_EQ(statuses[0].last_log_index, 2u);
    EXPECT_TRUE(statuses[1].is_leader);
    EXPECT_EQ(statuses[1].last_log_index, 2u);
    EXPECT_EQ(statuses[1].last_applied_index, 2u);
    EXPECT_FALSE(statuses[2].is_leader);
    EXPECT_EQ(statuses[2].last_log_index, 2u);
    EXPECT_EQ(statuses[2].last_applied_index, 2u);
}

TEST_F(NuRaftStaticClusterTest, SnapshotInstalledFollowerCatchesUpToLaterWrites) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");
    const std::string nodes_config = "127.0.0.1:7107:8108,127.0.0.1:7109:8109,127.0.0.1:7111:8110";
    const std::string node1 = node_dir("snapshot-node1");
    const std::string node2 = node_dir("snapshot-node2");
    const std::string node3 = node_dir("snapshot-node3");
    const std::string export_dir = node_dir("snapshot-export");

    initialize_node(node1, "127.0.0.1", 7107, 8108, nodes_config);
    initialize_node(node2, "127.0.0.1", 7109, 8109, nodes_config);
    initialize_node(node3, "127.0.0.1", 7111, 8110, nodes_config);

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
    EXPECT_EQ(appended_index, 1u);

    {
        const NuRaftStateLayout leader_layout = NuRaftStateLayout::from_data_dir(node1);
        auto leader_sink = std::make_unique<NuRaftKvStateMachineSink>(leader_layout);
        NuRaftKvStateMachineSink* leader_sink_ptr = leader_sink.get();
        NuRaftPrototypeStateMachine leader_state_machine(leader_layout, std::move(leader_sink));
        ASSERT_TRUE(leader_state_machine.initialize(error)) << error;

        std::vector<NuRaftLogEntry> applied_entries;
        ASSERT_TRUE(leader_state_machine.apply_pending(applied_entries, error)) << error;
        ASSERT_EQ(applied_entries.size(), 1u);

        NuRaftSnapshotCoordinator snapshot_coordinator(leader_layout);
        NuRaftSnapshotDescriptor descriptor;
        ASSERT_TRUE(snapshot_coordinator.create_snapshot(export_dir, leader_sink_ptr, descriptor, error)) << error;
        EXPECT_EQ(descriptor.last_applied_index, 1u);
    }

    NuRaftSnapshotCoordinator follower_snapshot_coordinator(NuRaftStateLayout::from_data_dir(node3));
    NuRaftSnapshotDescriptor installed_descriptor;
    ASSERT_TRUE(follower_snapshot_coordinator.install_snapshot(export_dir, installed_descriptor, error)) << error;
    EXPECT_EQ(installed_descriptor.last_applied_index, 1u);

    ASSERT_TRUE(NuRaftStaticCluster::append_request(bootstrap_config,
                                                    data_dirs,
                                                    8108,
                                                    8110,
                                                    "{\"route_hash\":" + std::to_string(document_write_hash) +
                                                        ",\"params\":{\"collection\":\"books\",\"id\":\"doc-1\"},\"body\":\"{\\\"id\\\":\\\"doc-1\\\",\\\"title\\\":\\\"Dune\\\"}\"}",
                                                    appended_index,
                                                    forwarded_to_leader,
                                                    target_server_id,
                                                    error)) << error;
    EXPECT_EQ(appended_index, 2u);

    std::vector<NuRaftStaticClusterNodeStatus> statuses;
    ASSERT_TRUE(NuRaftStaticCluster::replicate_and_apply(bootstrap_config,
                                                         data_dirs,
                                                         8108,
                                                         "kv",
                                                         statuses,
                                                         error)) << error;
    ASSERT_EQ(statuses.size(), 3u);
    EXPECT_EQ(statuses[2].server_id, 8110);
    EXPECT_EQ(statuses[2].last_log_index, 2u);
    EXPECT_EQ(statuses[2].last_applied_index, 2u);

    NuRaftKvStateMachineSink follower_sink(NuRaftStateLayout::from_data_dir(node3));
    std::vector<std::pair<std::string, std::string>> entries;
    ASSERT_TRUE(follower_sink.read_materialized_entries(entries, error)) << error;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].first, "state/collections/books");
    EXPECT_EQ(entries[1].first, "state/documents/books/doc-1");
}

TEST_F(NuRaftStaticClusterTest, RestartStyleReplayAppliesOnlyDeltaAfterLeaderSwitch) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");
    const std::string nodes_config = "127.0.0.1:7107:8108,127.0.0.1:7109:8109,127.0.0.1:7111:8110";
    const std::string node1 = node_dir("restart-node1");
    const std::string node2 = node_dir("restart-node2");
    const std::string node3 = node_dir("restart-node3");

    initialize_node(node1, "127.0.0.1", 7107, 8108, nodes_config);
    initialize_node(node2, "127.0.0.1", 7109, 8109, nodes_config);
    initialize_node(node3, "127.0.0.1", 7111, 8110, nodes_config);

    NuRaftMetadataStore metadata_store(NuRaftStateLayout::from_data_dir(node2));
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

    ASSERT_TRUE(NuRaftStaticCluster::append_request(bootstrap_config,
                                                    data_dirs,
                                                    8109,
                                                    8110,
                                                    "{\"route_hash\":" + std::to_string(document_write_hash) +
                                                        ",\"params\":{\"collection\":\"books\",\"id\":\"doc-1\"},\"body\":\"{\\\"id\\\":\\\"doc-1\\\",\\\"title\\\":\\\"Dune\\\"}\"}",
                                                    appended_index,
                                                    forwarded_to_leader,
                                                    target_server_id,
                                                    error)) << error;
    EXPECT_EQ(target_server_id, 8109);

    ASSERT_TRUE(replicate_missing_entries_for_test(node2, node3, error)) << error;

    NuRaftMetadataStore follower_metadata(NuRaftStateLayout::from_data_dir(node3));
    NuRaftReplayProgress progress_before_restart;
    ASSERT_TRUE(follower_metadata.read_replay_progress(progress_before_restart, error)) << error;
    EXPECT_EQ(progress_before_restart.last_applied_index, 1u);

    ASSERT_TRUE(apply_pending_kv_for_test(node3, error)) << error;

    NuRaftReplayProgress progress_after_restart;
    ASSERT_TRUE(follower_metadata.read_replay_progress(progress_after_restart, error)) << error;
    EXPECT_EQ(progress_after_restart.last_applied_index, 2u);

    NuRaftKvStateMachineSink follower_sink(NuRaftStateLayout::from_data_dir(node3));
    std::vector<std::pair<std::string, std::string>> entries;
    ASSERT_TRUE(follower_sink.read_materialized_entries(entries, error)) << error;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].first, "state/collections/books");
    EXPECT_EQ(entries[1].first, "state/documents/books/doc-1");
}

TEST_F(NuRaftStaticClusterTest, RejectsDivergedFollowerHistoryDuringCatchUp) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");
    const std::string nodes_config = "127.0.0.1:7107:8108,127.0.0.1:7109:8109,127.0.0.1:7111:8110";
    const std::string node1 = node_dir("diverge-node1");
    const std::string node2 = node_dir("diverge-node2");
    const std::string node3 = node_dir("diverge-node3");

    initialize_node(node1, "127.0.0.1", 7107, 8108, nodes_config);
    initialize_node(node2, "127.0.0.1", 7109, 8109, nodes_config);
    initialize_node(node3, "127.0.0.1", 7111, 8110, nodes_config);

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

    NuRaftRequestJournal diverged_follower_journal(NuRaftStateLayout::from_data_dir(node2));
    ASSERT_TRUE(diverged_follower_journal.initialize(error)) << error;
    ASSERT_TRUE(diverged_follower_journal.append_request_json(
        "{\"route_hash\":" + std::to_string(document_write_hash) +
            ",\"params\":{\"collection\":\"books\",\"id\":\"wrong\"},\"body\":\"{\\\"id\\\":\\\"wrong\\\",\\\"title\\\":\\\"Mismatch\\\"}\"}",
        appended_index,
        error)) << error;
    EXPECT_EQ(appended_index, 1u);

    std::vector<NuRaftStaticClusterNodeStatus> statuses;
    ASSERT_FALSE(NuRaftStaticCluster::replicate_and_apply(bootstrap_config,
                                                          data_dirs,
                                                          8108,
                                                          "kv",
                                                          statuses,
                                                          error));
    EXPECT_EQ(error, "NuRaft static cluster follower log diverges from leader history");
}
