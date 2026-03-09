#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "json.hpp"
#include "nuraft/nuraft_metadata_store.h"
#include "nuraft/nuraft_replication_controller.h"
#include "nuraft/nuraft_state_machine_sink.h"
#include "string_utils.h"

namespace {

std::vector<char*> make_argv(std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    return argv;
}

uint64_t make_route_hash(const std::string& method, const std::string& path) {
    const std::string method_path = method + path;
    const uint64_t hash = StringUtils::hash_wy(method_path.c_str(), method_path.size());
    return (hash > 100) ? hash : (hash + 100);
}

std::string append_request_arg(const nlohmann::json& request) {
    return "--append-request-json=" + request.dump();
}

}  // namespace

class NuRaftReplicationControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("nuraft_replication_controller_test-") + std::to_string(getpid()))).string();
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::string temp_dir_;
};

TEST_F(NuRaftReplicationControllerTest, RunsStartupPreflightFromCliArgs) {
    NuRaftReplicationController controller;
    std::vector<std::string> args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--node-host=127.0.0.1",
        "--peering-port=7107",
        "--api-port=8108",
    };
    std::vector<char*> argv = make_argv(args);
    std::ostringstream out;
    std::ostringstream err;

    ASSERT_EQ(controller.run(static_cast<int>(args.size()), argv.data(), out, err), 0);
    EXPECT_TRUE(err.str().empty());
    EXPECT_NE(out.str().find("server_id=8108"), std::string::npos);
    EXPECT_NE(out.str().find("leader_url=http://127.0.0.1:8108/"), std::string::npos);

    NuRaftMetadataStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string read_error;
    NuRaftIdentity identity;
    ASSERT_TRUE(store.read_identity(identity, read_error)) << read_error;
    EXPECT_EQ(identity.server_id, 8108);
    EXPECT_EQ(identity.peer_endpoint, "127.0.0.1:7107");
}

TEST_F(NuRaftReplicationControllerTest, PersistsMultiNodeBootstrapSelection) {
    NuRaftReplicationController controller;
    std::vector<std::string> args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--node-host=127.0.0.99",
        "--api-port=8110",
        "--nodes=127.0.0.1:7107:8108,127.0.0.2:7109:8110,[2001:db8::1]:7111:8112",
        "--api-uses-ssl",
    };
    std::vector<char*> argv = make_argv(args);
    std::ostringstream out;
    std::ostringstream err;

    ASSERT_EQ(controller.run(static_cast<int>(args.size()), argv.data(), out, err), 0);
    EXPECT_TRUE(err.str().empty());
    EXPECT_NE(out.str().find("leader_url=https://127.0.0.2:8110/"), std::string::npos);

    NuRaftMetadataStore store(NuRaftStateLayout::from_data_dir(temp_dir_));
    std::string read_error;
    NuRaftBootstrapConfig bootstrap_config;
    ASSERT_TRUE(store.read_bootstrap_config(bootstrap_config, read_error)) << read_error;
    EXPECT_EQ(bootstrap_config.self, (NuRaftPeerAddress{"127.0.0.2", 7109, 8110}));
    ASSERT_EQ(bootstrap_config.peers.size(), 3u);
}

TEST_F(NuRaftReplicationControllerTest, RejectsMultiNodeSelfAddressRewriteThroughCli) {
    NuRaftReplicationController controller;
    std::vector<std::string> init_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--node-host=127.0.0.1",
        "--api-port=8108",
        "--nodes=127.0.0.1:7107:8108,127.0.0.2:7109:8109",
    };
    std::vector<char*> init_argv = make_argv(init_args);
    std::ostringstream init_out;
    std::ostringstream init_err;
    ASSERT_EQ(controller.run(static_cast<int>(init_args.size()), init_argv.data(), init_out, init_err), 0);
    EXPECT_TRUE(init_err.str().empty());

    std::vector<std::string> refresh_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--node-host=10.0.0.5",
        "--api-port=8108",
        "--nodes=10.0.0.5:7207:8108,127.0.0.2:7109:8109",
    };
    std::vector<char*> refresh_argv = make_argv(refresh_args);
    std::ostringstream refresh_out;
    std::ostringstream refresh_err;
    ASSERT_EQ(controller.run(static_cast<int>(refresh_args.size()), refresh_argv.data(), refresh_out, refresh_err), 1);
    EXPECT_TRUE(refresh_out.str().empty());
    EXPECT_NE(refresh_err.str().find("refuses to rewrite the persisted self peer address for a multi-node bootstrap"),
              std::string::npos);
}

TEST_F(NuRaftReplicationControllerTest, ReportsMissingRequiredDataDir) {
    NuRaftReplicationController controller;
    std::vector<std::string> args = {
        "./typesense-server-nuraft-prototype",
    };
    std::vector<char*> argv = make_argv(args);
    std::ostringstream out;
    std::ostringstream err;

    ASSERT_EQ(controller.run(static_cast<int>(args.size()), argv.data(), out, err), 1);
    EXPECT_TRUE(out.str().empty());
    EXPECT_NE(err.str().find("need option: --data-dir"), std::string::npos);
}

TEST_F(NuRaftReplicationControllerTest, AppendsAndReplaysRequestJournalThroughCli) {
    NuRaftReplicationController controller;
    std::vector<std::string> append_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--append-request-json={\"route\":\"/collections\",\"method\":\"POST\"}",
    };
    std::vector<char*> append_argv = make_argv(append_args);
    std::ostringstream append_out;
    std::ostringstream append_err;
    ASSERT_EQ(controller.run(static_cast<int>(append_args.size()), append_argv.data(), append_out, append_err), 0);
    EXPECT_TRUE(append_err.str().empty());
    EXPECT_NE(append_out.str().find("appended_index=1"), std::string::npos);

    std::vector<std::string> replay_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--replay-log",
    };
    std::vector<char*> replay_argv = make_argv(replay_args);
    std::ostringstream replay_out;
    std::ostringstream replay_err;
    ASSERT_EQ(controller.run(static_cast<int>(replay_args.size()), replay_argv.data(), replay_out, replay_err), 0);
    EXPECT_TRUE(replay_err.str().empty());
    EXPECT_NE(replay_out.str().find("replay_count=1"), std::string::npos);
    EXPECT_NE(replay_out.str().find("route_hash=0"), std::string::npos);
    EXPECT_NE(replay_out.str().find("route_kind=unknown"), std::string::npos);
    EXPECT_NE(replay_out.str().find("body={\"route\":\"/collections\",\"method\":\"POST\"}"), std::string::npos);
}

TEST_F(NuRaftReplicationControllerTest, AppliesPendingEntriesThroughCli) {
    NuRaftReplicationController controller;
    std::vector<std::string> append_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--append-request-json={\"route\":\"/collections\",\"method\":\"POST\"}",
    };
    std::vector<char*> append_argv = make_argv(append_args);
    std::ostringstream append_out;
    std::ostringstream append_err;
    ASSERT_EQ(controller.run(static_cast<int>(append_args.size()), append_argv.data(), append_out, append_err), 0);
    EXPECT_TRUE(append_err.str().empty());

    std::vector<std::string> apply_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--apply-pending",
    };
    std::vector<char*> apply_argv = make_argv(apply_args);
    std::ostringstream apply_out;
    std::ostringstream apply_err;
    ASSERT_EQ(controller.run(static_cast<int>(apply_args.size()), apply_argv.data(), apply_out, apply_err), 0);
    EXPECT_TRUE(apply_err.str().empty());
    EXPECT_NE(apply_out.str().find("applied_count=1"), std::string::npos);
    EXPECT_NE(apply_out.str().find("route_hash="), std::string::npos);
    EXPECT_NE(apply_out.str().find("route_kind=unknown"), std::string::npos);
    EXPECT_NE(apply_out.str().find("body_bytes="), std::string::npos);

    std::ostringstream second_apply_out;
    std::ostringstream second_apply_err;
    ASSERT_EQ(controller.run(static_cast<int>(apply_args.size()), apply_argv.data(), second_apply_out, second_apply_err), 0);
    EXPECT_TRUE(second_apply_err.str().empty());
    EXPECT_NE(second_apply_out.str().find("applied_count=0"), std::string::npos);
}

TEST_F(NuRaftReplicationControllerTest, RecoversTruncatedTailAndAutoAppliesThroughCli) {
    NuRaftReplicationController controller;
    std::vector<std::string> append_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--append-request-json={\"route\":\"/collections\",\"method\":\"POST\"}",
    };
    std::vector<char*> append_argv = make_argv(append_args);
    std::ostringstream append_out;
    std::ostringstream append_err;
    ASSERT_EQ(controller.run(static_cast<int>(append_args.size()), append_argv.data(), append_out, append_err), 0);
    EXPECT_TRUE(append_err.str().empty());

    const NuRaftStateLayout layout = NuRaftStateLayout::from_data_dir(temp_dir_);
    {
        std::ofstream output(layout.active_log_segment_file, std::ios::binary | std::ios::app);
        ASSERT_TRUE(output.is_open());
        output.write("\x02\x00\x00", 3);
    }

    std::vector<std::string> recover_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--recover-truncated-tail",
        "--auto-apply-pending",
    };
    std::vector<char*> recover_argv = make_argv(recover_args);
    std::ostringstream recover_out;
    std::ostringstream recover_err;
    ASSERT_EQ(controller.run(static_cast<int>(recover_args.size()), recover_argv.data(), recover_out, recover_err), 0);
    EXPECT_NE(recover_err.str().find("Recovered truncated NuRaft request journal tail"), std::string::npos);
    EXPECT_NE(recover_out.str().find("applied_count=1"), std::string::npos);
}

TEST_F(NuRaftReplicationControllerTest, AppliesThroughKvSinkAndDumpsMaterializedState) {
    NuRaftReplicationController controller;
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");

    std::vector<std::string> append_collection_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        append_request_arg({
            {"route_hash", collection_create_hash},
            {"params", nlohmann::json::object()},
            {"body", "{\"name\":\"books\"}"},
        }),
    };
    std::vector<char*> append_collection_argv = make_argv(append_collection_args);
    std::ostringstream append_collection_out;
    std::ostringstream append_collection_err;
    ASSERT_EQ(controller.run(static_cast<int>(append_collection_args.size()),
                             append_collection_argv.data(),
                             append_collection_out,
                             append_collection_err), 0);
    EXPECT_TRUE(append_collection_err.str().empty());

    std::vector<std::string> append_document_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--append-request-json={\"route_hash\":" + std::to_string(document_write_hash) +
            ",\"params\":{\"collection\":\"books\",\"id\":\"doc-1\"},"
            "\"body\":\"{\\\"id\\\":\\\"doc-1\\\",\\\"title\\\":\\\"Dune\\\"}\"}",
    };
    std::vector<char*> append_document_argv = make_argv(append_document_args);
    std::ostringstream append_document_out;
    std::ostringstream append_document_err;
    ASSERT_EQ(controller.run(static_cast<int>(append_document_args.size()),
                             append_document_argv.data(),
                             append_document_out,
                             append_document_err), 0);
    EXPECT_TRUE(append_document_err.str().empty());

    std::vector<std::string> apply_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--state-machine-sink=kv",
        "--apply-pending",
        "--dump-materialized-state",
    };
    std::vector<char*> apply_argv = make_argv(apply_args);
    std::ostringstream apply_out;
    std::ostringstream apply_err;
    ASSERT_EQ(controller.run(static_cast<int>(apply_args.size()), apply_argv.data(), apply_out, apply_err), 0);
    EXPECT_TRUE(apply_err.str().empty());
    EXPECT_NE(apply_out.str().find("materialized_count=2"), std::string::npos);
    EXPECT_NE(apply_out.str().find("materialized key=state/collections/books"), std::string::npos);
    EXPECT_NE(apply_out.str().find("materialized key=state/documents/books/doc-1"), std::string::npos);
}

TEST_F(NuRaftReplicationControllerTest, ReplaysChunkedImportThroughKvSinkAndDumpsDocuments) {
    NuRaftReplicationController controller;
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_import_hash = make_route_hash("POST", "collections/:collection/documents/import");

    std::vector<std::string> append_collection_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--append-request-json={\"route_hash\":" + std::to_string(collection_create_hash) +
            ",\"params\":{},\"body\":\"{\\\"name\\\":\\\"books\\\"}\"}",
    };
    std::vector<char*> append_collection_argv = make_argv(append_collection_args);
    std::ostringstream append_collection_out;
    std::ostringstream append_collection_err;
    ASSERT_EQ(controller.run(static_cast<int>(append_collection_args.size()),
                             append_collection_argv.data(),
                             append_collection_out,
                             append_collection_err), 0);
    EXPECT_TRUE(append_collection_err.str().empty());

    std::vector<std::string> append_import_chunk_one_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        append_request_arg({
            {"route_hash", document_import_hash},
            {"params", {{"collection", "books"}}},
            {"first_chunk_aggregate", true},
            {"last_chunk_aggregate", false},
            {"start_ts", 42},
            {"body", "{\"id\":\"doc-1\",\"title\":\"Dune\"}\n{\"id\":\"doc"},
        }),
    };
    std::vector<char*> append_import_chunk_one_argv = make_argv(append_import_chunk_one_args);
    std::ostringstream append_import_chunk_one_out;
    std::ostringstream append_import_chunk_one_err;
    ASSERT_EQ(controller.run(static_cast<int>(append_import_chunk_one_args.size()),
                             append_import_chunk_one_argv.data(),
                             append_import_chunk_one_out,
                             append_import_chunk_one_err), 0);
    EXPECT_TRUE(append_import_chunk_one_err.str().empty());

    std::vector<std::string> append_import_chunk_two_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        append_request_arg({
            {"route_hash", document_import_hash},
            {"params", {{"collection", "books"}}},
            {"first_chunk_aggregate", false},
            {"last_chunk_aggregate", true},
            {"start_ts", 42},
            {"body", "-2\",\"title\":\"Foundation\"}\n"},
        }),
    };
    std::vector<char*> append_import_chunk_two_argv = make_argv(append_import_chunk_two_args);
    std::ostringstream append_import_chunk_two_out;
    std::ostringstream append_import_chunk_two_err;
    ASSERT_EQ(controller.run(static_cast<int>(append_import_chunk_two_args.size()),
                             append_import_chunk_two_argv.data(),
                             append_import_chunk_two_out,
                             append_import_chunk_two_err), 0);
    EXPECT_TRUE(append_import_chunk_two_err.str().empty());

    std::vector<std::string> apply_args = {
        "./typesense-server-nuraft-prototype",
        "--data-dir=" + temp_dir_,
        "--state-machine-sink=kv",
        "--apply-pending",
        "--dump-materialized-state",
    };
    std::vector<char*> apply_argv = make_argv(apply_args);
    std::ostringstream apply_out;
    std::ostringstream apply_err;
    ASSERT_EQ(controller.run(static_cast<int>(apply_args.size()), apply_argv.data(), apply_out, apply_err), 0);
    EXPECT_TRUE(apply_err.str().empty());
    EXPECT_NE(apply_out.str().find("materialized key=state/documents/books/doc-1"), std::string::npos);
    EXPECT_NE(apply_out.str().find("materialized key=state/documents/books/doc-2"), std::string::npos);
    EXPECT_NE(apply_out.str().find("materialized key=state/imports/books/00000000000000000042"), std::string::npos);
    EXPECT_NE(apply_out.str().find("\"document_count\":2"), std::string::npos);
    EXPECT_EQ(apply_out.str().find("state/import_buffers/books/00000000000000000042"), std::string::npos);
}

TEST_F(NuRaftReplicationControllerTest, SimulatesThreeNodeCatchUpThroughStaticClusterOptions) {
    NuRaftReplicationController controller;
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");
    const std::string nodes = "127.0.0.1:7107:8108,127.0.0.1:7109:8109,127.0.0.1:7111:8110";
    const std::string node1_dir = (std::filesystem::path(temp_dir_) / "node1").string();
    const std::string node2_dir = (std::filesystem::path(temp_dir_) / "node2").string();
    const std::string node3_dir = (std::filesystem::path(temp_dir_) / "node3").string();
    const std::string cluster_dirs = "8108=" + node1_dir + ",8109=" + node2_dir + ",8110=" + node3_dir;

    auto run_node = [&](const std::string& data_dir,
                        uint32_t peering_port,
                        uint32_t api_port,
                        const std::vector<std::string>& extra_args,
                        std::ostringstream& out,
                        std::ostringstream& err) {
        std::vector<std::string> args = {
            "./typesense-server-nuraft-prototype",
            "--data-dir=" + data_dir,
            "--node-host=127.0.0.1",
            "--peering-port=" + std::to_string(peering_port),
            "--api-port=" + std::to_string(api_port),
            "--nodes=" + nodes,
        };
        args.insert(args.end(), extra_args.begin(), extra_args.end());
        std::vector<char*> argv = make_argv(args);
        return controller.run(static_cast<int>(args.size()), argv.data(), out, err);
    };

    std::ostringstream init1_out;
    std::ostringstream init1_err;
    ASSERT_EQ(run_node(node1_dir, 7107, 8108, {}, init1_out, init1_err), 0);
    EXPECT_TRUE(init1_err.str().empty());

    std::ostringstream init2_out;
    std::ostringstream init2_err;
    ASSERT_EQ(run_node(node2_dir, 7109, 8109, {}, init2_out, init2_err), 0);
    EXPECT_TRUE(init2_err.str().empty());

    std::ostringstream init3_out;
    std::ostringstream init3_err;
    ASSERT_EQ(run_node(node3_dir, 7111, 8110, {}, init3_out, init3_err), 0);
    EXPECT_TRUE(init3_err.str().empty());

    std::ostringstream append_collection_out;
    std::ostringstream append_collection_err;
    ASSERT_EQ(run_node(node2_dir,
                       7109,
                       8109,
                       {
                           "--cluster-data-dirs=" + cluster_dirs,
                           "--cluster-leader-api-port=8108",
                           append_request_arg({
                               {"route_hash", collection_create_hash},
                               {"params", nlohmann::json::object()},
                               {"body", "{\"name\":\"books\"}"},
                           }),
                       },
                       append_collection_out,
                       append_collection_err),
              0);
    EXPECT_TRUE(append_collection_err.str().empty());
    EXPECT_NE(append_collection_out.str().find("leader_url=http://127.0.0.1:8108/"), std::string::npos);
    EXPECT_NE(append_collection_out.str().find("cluster_role=follower leader_server_id=8108"), std::string::npos);
    EXPECT_NE(append_collection_out.str().find("forwarded_to_leader=1"), std::string::npos);

    std::ostringstream append_document_out;
    std::ostringstream append_document_err;
    ASSERT_EQ(run_node(node3_dir,
                       7111,
                       8110,
                       {
                           "--cluster-data-dirs=" + cluster_dirs,
                           "--cluster-leader-api-port=8108",
                           append_request_arg({
                               {"route_hash", document_write_hash},
                               {"params", {{"collection", "books"}, {"id", "doc-1"}}},
                               {"body", "{\"id\":\"doc-1\",\"title\":\"Dune\"}"},
                           }),
                       },
                       append_document_out,
                       append_document_err),
              0);
    EXPECT_TRUE(append_document_err.str().empty());
    EXPECT_NE(append_document_out.str().find("forwarded_to_leader=1"), std::string::npos);

    std::ostringstream lag_status_out;
    std::ostringstream lag_status_err;
    ASSERT_EQ(run_node(node1_dir,
                       7107,
                       8108,
                       {
                           "--cluster-data-dirs=" + cluster_dirs,
                           "--cluster-leader-api-port=8108",
                           "--dump-cluster-status",
                       },
                       lag_status_out,
                       lag_status_err),
              0);
    EXPECT_TRUE(lag_status_err.str().empty());
    EXPECT_NE(lag_status_out.str().find("cluster node_server_id=8108 role=leader last_log_index=2 last_applied_index=0"),
              std::string::npos);
    EXPECT_NE(lag_status_out.str().find("cluster node_server_id=8109 role=follower last_log_index=0 last_applied_index=0"),
              std::string::npos);
    EXPECT_NE(lag_status_out.str().find("cluster node_server_id=8110 role=follower last_log_index=0 last_applied_index=0"),
              std::string::npos);

    std::ostringstream replicate_out;
    std::ostringstream replicate_err;
    ASSERT_EQ(run_node(node1_dir,
                       7107,
                       8108,
                       {
                           "--cluster-data-dirs=" + cluster_dirs,
                           "--cluster-leader-api-port=8108",
                           "--state-machine-sink=kv",
                           "--replicate-cluster",
                           "--dump-cluster-status",
                       },
                       replicate_out,
                       replicate_err),
              0);
    EXPECT_TRUE(replicate_err.str().empty());
    EXPECT_NE(replicate_out.str().find("cluster_replicated_nodes=3"), std::string::npos);
    EXPECT_NE(replicate_out.str().find("cluster node_server_id=8108 role=leader last_log_index=2 last_applied_index=2"), std::string::npos);
    EXPECT_NE(replicate_out.str().find("cluster node_server_id=8109 role=follower last_log_index=2 last_applied_index=2"), std::string::npos);
    EXPECT_NE(replicate_out.str().find("cluster node_server_id=8110 role=follower last_log_index=2 last_applied_index=2"), std::string::npos);

    NuRaftKvStateMachineSink follower_sink(NuRaftStateLayout::from_data_dir(node3_dir));
    std::string error;
    std::vector<std::pair<std::string, std::string>> entries;
    ASSERT_TRUE(follower_sink.read_materialized_entries(entries, error)) << error;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].first, "state/collections/books");
    EXPECT_EQ(entries[1].first, "state/documents/books/doc-1");
}

TEST_F(NuRaftReplicationControllerTest, ForwardsChunkedImportsAcrossLeaderSwitchThroughStaticClusterOptions) {
    NuRaftReplicationController controller;
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_import_hash = make_route_hash("POST", "collections/:collection/documents/import");
    const std::string nodes = "127.0.0.1:7107:8108,127.0.0.1:7109:8109,127.0.0.1:7111:8110";
    const std::string node1_dir = (std::filesystem::path(temp_dir_) / "import-node1").string();
    const std::string node2_dir = (std::filesystem::path(temp_dir_) / "import-node2").string();
    const std::string node3_dir = (std::filesystem::path(temp_dir_) / "import-node3").string();
    const std::string cluster_dirs = "8108=" + node1_dir + ",8109=" + node2_dir + ",8110=" + node3_dir;

    auto run_node = [&](const std::string& data_dir,
                        uint32_t peering_port,
                        uint32_t api_port,
                        const std::vector<std::string>& extra_args,
                        std::ostringstream& out,
                        std::ostringstream& err) {
        std::vector<std::string> args = {
            "./typesense-server-nuraft-prototype",
            "--data-dir=" + data_dir,
            "--node-host=127.0.0.1",
            "--peering-port=" + std::to_string(peering_port),
            "--api-port=" + std::to_string(api_port),
            "--nodes=" + nodes,
        };
        args.insert(args.end(), extra_args.begin(), extra_args.end());
        std::vector<char*> argv = make_argv(args);
        return controller.run(static_cast<int>(args.size()), argv.data(), out, err);
    };

    std::ostringstream init1_out;
    std::ostringstream init1_err;
    ASSERT_EQ(run_node(node1_dir, 7107, 8108, {}, init1_out, init1_err), 0);
    EXPECT_TRUE(init1_err.str().empty());

    std::ostringstream init2_out;
    std::ostringstream init2_err;
    ASSERT_EQ(run_node(node2_dir, 7109, 8109, {}, init2_out, init2_err), 0);
    EXPECT_TRUE(init2_err.str().empty());

    std::ostringstream init3_out;
    std::ostringstream init3_err;
    ASSERT_EQ(run_node(node3_dir, 7111, 8110, {}, init3_out, init3_err), 0);
    EXPECT_TRUE(init3_err.str().empty());

    std::ostringstream append_collection_out;
    std::ostringstream append_collection_err;
    ASSERT_EQ(run_node(node2_dir,
                       7109,
                       8109,
                       {
                           "--cluster-data-dirs=" + cluster_dirs,
                           "--cluster-leader-api-port=8108",
                           append_request_arg({
                               {"route_hash", collection_create_hash},
                               {"params", nlohmann::json::object()},
                               {"body", "{\"name\":\"books\"}"},
                           }),
                       },
                       append_collection_out,
                       append_collection_err),
              0);
    EXPECT_TRUE(append_collection_err.str().empty());

    std::ostringstream replicate_collection_out;
    std::ostringstream replicate_collection_err;
    ASSERT_EQ(run_node(node1_dir,
                       7107,
                       8108,
                       {
                           "--cluster-data-dirs=" + cluster_dirs,
                           "--cluster-leader-api-port=8108",
                           "--state-machine-sink=kv",
                           "--replicate-cluster",
                       },
                       replicate_collection_out,
                       replicate_collection_err),
              0);
    EXPECT_TRUE(replicate_collection_err.str().empty());

    std::ostringstream import_chunk_one_out;
    std::ostringstream import_chunk_one_err;
    ASSERT_EQ(run_node(node3_dir,
                       7111,
                       8110,
                       {
                           "--cluster-data-dirs=" + cluster_dirs,
                           "--cluster-leader-api-port=8108",
                           append_request_arg({
                               {"route_hash", document_import_hash},
                               {"params", {{"collection", "books"}}},
                               {"first_chunk_aggregate", true},
                               {"last_chunk_aggregate", false},
                               {"start_ts", 42},
                               {"body", "{\"id\":\"doc-1\",\"title\":\"Dune\"}\n{\"id\":\"doc"},
                           }),
                       },
                       import_chunk_one_out,
                       import_chunk_one_err),
              0);
    EXPECT_TRUE(import_chunk_one_err.str().empty());
    EXPECT_NE(import_chunk_one_out.str().find("forwarded_to_leader=1"), std::string::npos);

    std::ostringstream import_chunk_two_out;
    std::ostringstream import_chunk_two_err;
    ASSERT_EQ(run_node(node3_dir,
                       7111,
                       8110,
                       {
                           "--cluster-data-dirs=" + cluster_dirs,
                           "--cluster-leader-api-port=8108",
                           append_request_arg({
                               {"route_hash", document_import_hash},
                               {"params", {{"collection", "books"}}},
                               {"first_chunk_aggregate", false},
                               {"last_chunk_aggregate", true},
                               {"start_ts", 42},
                               {"body", "-2\",\"title\":\"Foundation\"}\n"},
                           }),
                       },
                       import_chunk_two_out,
                       import_chunk_two_err),
              0);
    EXPECT_TRUE(import_chunk_two_err.str().empty());
    EXPECT_NE(import_chunk_two_out.str().find("forwarded_to_leader=1"), std::string::npos);

    std::ostringstream replicate_import_one_out;
    std::ostringstream replicate_import_one_err;
    ASSERT_EQ(run_node(node1_dir,
                       7107,
                       8108,
                       {
                           "--cluster-data-dirs=" + cluster_dirs,
                           "--cluster-leader-api-port=8108",
                           "--state-machine-sink=kv",
                           "--replicate-cluster",
                       },
                       replicate_import_one_out,
                       replicate_import_one_err),
              0);
    EXPECT_TRUE(replicate_import_one_err.str().empty());

    std::ostringstream import_two_chunk_one_out;
    std::ostringstream import_two_chunk_one_err;
    ASSERT_EQ(run_node(node1_dir,
                       7107,
                       8108,
                       {
                           "--cluster-data-dirs=" + cluster_dirs,
                           "--cluster-leader-api-port=8109",
                           append_request_arg({
                               {"route_hash", document_import_hash},
                               {"params", {{"collection", "books"}}},
                               {"first_chunk_aggregate", true},
                               {"last_chunk_aggregate", false},
                               {"start_ts", 84},
                               {"body", "{\"id\":\"doc-3\",\"title\":\"Hyperion\"}\n{\"id\":\"doc"},
                           }),
                       },
                       import_two_chunk_one_out,
                       import_two_chunk_one_err),
              0);
    EXPECT_TRUE(import_two_chunk_one_err.str().empty());
    EXPECT_NE(import_two_chunk_one_out.str().find("target_server_id=8109"), std::string::npos);
    EXPECT_NE(import_two_chunk_one_out.str().find("forwarded_to_leader=1"), std::string::npos);

    std::ostringstream import_two_chunk_two_out;
    std::ostringstream import_two_chunk_two_err;
    ASSERT_EQ(run_node(node1_dir,
                       7107,
                       8108,
                       {
                           "--cluster-data-dirs=" + cluster_dirs,
                           "--cluster-leader-api-port=8109",
                           append_request_arg({
                               {"route_hash", document_import_hash},
                               {"params", {{"collection", "books"}}},
                               {"first_chunk_aggregate", false},
                               {"last_chunk_aggregate", true},
                               {"start_ts", 84},
                               {"body", "-4\",\"title\":\"Snow Crash\"}\n"},
                           }),
                       },
                       import_two_chunk_two_out,
                       import_two_chunk_two_err),
              0);
    EXPECT_TRUE(import_two_chunk_two_err.str().empty());
    EXPECT_NE(import_two_chunk_two_out.str().find("target_server_id=8109"), std::string::npos);

    std::ostringstream replicate_import_two_out;
    std::ostringstream replicate_import_two_err;
    ASSERT_EQ(run_node(node2_dir,
                       7109,
                       8109,
                       {
                           "--cluster-data-dirs=" + cluster_dirs,
                           "--cluster-leader-api-port=8109",
                           "--state-machine-sink=kv",
                           "--replicate-cluster",
                       },
                       replicate_import_two_out,
                       replicate_import_two_err),
              0);
    EXPECT_TRUE(replicate_import_two_err.str().empty());

    NuRaftKvStateMachineSink follower_sink(NuRaftStateLayout::from_data_dir(node3_dir));
    std::string error;
    std::vector<std::pair<std::string, std::string>> entries;
    ASSERT_TRUE(follower_sink.read_materialized_entries(entries, error)) << error;
    ASSERT_EQ(entries.size(), 7u);
    EXPECT_EQ(entries[0].first, "state/collections/books");
    EXPECT_EQ(entries[1].first, "state/documents/books/doc-1");
    EXPECT_EQ(entries[2].first, "state/documents/books/doc-2");
    EXPECT_EQ(entries[3].first, "state/documents/books/doc-3");
    EXPECT_EQ(entries[4].first, "state/documents/books/doc-4");
    EXPECT_EQ(entries[5].first, "state/imports/books/00000000000000000042");
    EXPECT_EQ(entries[6].first, "state/imports/books/00000000000000000084");
}

TEST_F(NuRaftReplicationControllerTest, CreatesAndInstallsSnapshotThroughCli) {
    NuRaftReplicationController controller;
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const uint64_t document_write_hash = make_route_hash("POST", "collections/:collection/documents");
    const std::string source_dir = (std::filesystem::path(temp_dir_) / "snapshot-source").string();
    const std::string restored_dir = (std::filesystem::path(temp_dir_) / "snapshot-restored").string();
    const std::string export_dir = (std::filesystem::path(temp_dir_) / "snapshot-export").string();

    auto run_node = [&](const std::string& data_dir,
                        uint32_t peering_port,
                        uint32_t api_port,
                        const std::vector<std::string>& extra_args,
                        std::ostringstream& out,
                        std::ostringstream& err) {
        std::vector<std::string> args = {
            "./typesense-server-nuraft-prototype",
            "--data-dir=" + data_dir,
            "--node-host=127.0.0.1",
            "--peering-port=" + std::to_string(peering_port),
            "--api-port=" + std::to_string(api_port),
        };
        args.insert(args.end(), extra_args.begin(), extra_args.end());
        std::vector<char*> argv = make_argv(args);
        return controller.run(static_cast<int>(args.size()), argv.data(), out, err);
    };

    std::ostringstream init_source_out;
    std::ostringstream init_source_err;
    ASSERT_EQ(run_node(source_dir, 7107, 8108, {}, init_source_out, init_source_err), 0);
    EXPECT_TRUE(init_source_err.str().empty());

    std::ostringstream append_collection_out;
    std::ostringstream append_collection_err;
    ASSERT_EQ(run_node(source_dir,
                       7107,
                       8108,
                       {append_request_arg({
                           {"route_hash", collection_create_hash},
                           {"params", nlohmann::json::object()},
                           {"body", "{\"name\":\"books\"}"},
                       })},
                       append_collection_out,
                       append_collection_err),
              0);
    EXPECT_TRUE(append_collection_err.str().empty());

    std::ostringstream append_document_out;
    std::ostringstream append_document_err;
    ASSERT_EQ(run_node(source_dir,
                       7107,
                       8108,
                       {append_request_arg({
                           {"route_hash", document_write_hash},
                           {"params", {{"collection", "books"}, {"id", "doc-1"}}},
                           {"body", "{\"id\":\"doc-1\",\"title\":\"Dune\"}"},
                       })},
                       append_document_out,
                       append_document_err),
              0);
    EXPECT_TRUE(append_document_err.str().empty());

    std::ostringstream snapshot_out;
    std::ostringstream snapshot_err;
    ASSERT_EQ(run_node(source_dir,
                       7107,
                       8108,
                       {
                           "--state-machine-sink=kv",
                           "--apply-pending",
                           "--create-snapshot=" + export_dir,
                       },
                       snapshot_out,
                       snapshot_err),
              0);
    EXPECT_TRUE(snapshot_err.str().empty());
    EXPECT_NE(snapshot_out.str().find("snapshot_id=snapshot-00000000000000000002-00000000000000000002"), std::string::npos);

    std::ostringstream init_restored_out;
    std::ostringstream init_restored_err;
    ASSERT_EQ(run_node(restored_dir, 7207, 8208, {}, init_restored_out, init_restored_err), 0);
    EXPECT_TRUE(init_restored_err.str().empty());

    std::ostringstream restore_out;
    std::ostringstream restore_err;
    ASSERT_EQ(run_node(restored_dir,
                       7207,
                       8208,
                       {
                           "--state-machine-sink=kv",
                           "--install-snapshot=" + export_dir,
                           "--dump-materialized-state",
                           "--dump-snapshot-descriptor",
                       },
                       restore_out,
                       restore_err),
              0);
    EXPECT_TRUE(restore_err.str().empty());
    EXPECT_NE(restore_out.str().find("server_id=8208"), std::string::npos);
    EXPECT_NE(restore_out.str().find("installed_snapshot=snapshot-00000000000000000002-00000000000000000002"), std::string::npos);
    EXPECT_NE(restore_out.str().find("materialized key=state/collections/books"), std::string::npos);
    EXPECT_NE(restore_out.str().find("materialized key=state/documents/books/doc-1"), std::string::npos);
    EXPECT_NE(restore_out.str().find("snapshot_last_applied_index=2"), std::string::npos);
}
