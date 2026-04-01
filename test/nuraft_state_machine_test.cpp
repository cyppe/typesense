#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include <unistd.h>

#include <libnuraft/nuraft.hxx>

#include "nuraft/nuraft_applied_request_store.h"
#include "nuraft/nuraft_file_store.h"
#include "nuraft/nuraft_snapshot_coordinator.h"
#include "nuraft/nuraft_state_initializer.h"
#include "nuraft/nuraft_state_machine_sink.h"
#include "nuraft/typesense_state_machine.h"
#include "string_utils.h"

namespace {

constexpr uint64_t kLargeSnapshotSentinelBytes = 24ULL * 1024ULL * 1024ULL;

uint64_t make_route_hash(const std::string& method, const std::string& path) {
    const std::string method_path = method + path;
    const uint64_t hash = StringUtils::hash_wy(method_path.c_str(), method_path.size());
    return (hash > 100) ? hash : (hash + 100);
}

void write_sparse_file(const std::filesystem::path& path, uint64_t size) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open()) << path;
    if (size > 0) {
        output.seekp(static_cast<std::streamoff>(size - 1));
        output.put('x');
    }
}

struct SnapshotObject {
    nuraft::ulong obj_id = 0;
    bool is_first = false;
    bool is_last = false;
    std::string relative_path;
    nuraft::ptr<nuraft::buffer> data;
};

std::vector<SnapshotObject> read_snapshot_objects(TypesenseStateMachine& source,
                                                  nuraft::snapshot& snapshot) {
    std::vector<SnapshotObject> objects;
    void* user_ctx = nullptr;
    nuraft::ulong obj_id = 0;
    bool is_first = true;

    while (true) {
        nuraft::ptr<nuraft::buffer> data;
        bool is_last = false;
        const int read_rc = source.read_logical_snp_obj(snapshot, user_ctx, obj_id, data, is_last);
        if (read_rc != 0) {
            ADD_FAILURE() << "read_logical_snp_obj failed for obj_id=" << obj_id
                          << " with rc=" << read_rc;
            break;
        }

        SnapshotObject object;
        object.obj_id = obj_id;
        object.is_first = is_first;
        object.is_last = is_last;
        object.data = data;

        if (obj_id > 0) {
            data->pos(0);
            size_t name_len = 0;
            const char* name_ptr = reinterpret_cast<const char*>(data->get_bytes(name_len));
            object.relative_path.assign(name_ptr, name_len);
            data->pos(0);
        }

        objects.push_back(object);
        if (is_last) {
            break;
        }

        ++obj_id;
        is_first = false;
    }

    source.free_user_snp_ctx(user_ctx);
    return objects;
}

void deliver_snapshot_objects(TypesenseStateMachine& target,
                              nuraft::snapshot& snapshot,
                              const std::vector<SnapshotObject>& objects,
                              const std::function<bool(const SnapshotObject&)>& should_deliver) {
    std::vector<const SnapshotObject*> delivered;
    delivered.reserve(objects.size());
    for (const auto& object : objects) {
        if (should_deliver(object)) {
            delivered.push_back(&object);
        }
    }

    ASSERT_FALSE(delivered.empty());
    ASSERT_EQ(0u, delivered.front()->obj_id);

    for (size_t i = 0; i < delivered.size(); ++i) {
        const SnapshotObject& object = *delivered[i];
        nuraft::ulong obj_id = object.obj_id;
        const bool is_first = (i == 0);
        const bool is_last = (i + 1 == delivered.size());
        target.save_logical_snp_obj(snapshot, obj_id, *object.data, is_first, is_last);
    }
}

class TypesenseStateMachineTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     (std::string("typesense_state_machine_test-") + std::to_string(getpid()))).string();
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

    void create_snapshot(TypesenseStateMachine& state_machine, uint64_t last_log_index) {
        auto config = nuraft::cs_new<nuraft::cluster_config>();
        nuraft::snapshot snapshot(last_log_index, 1, config);
        bool completed = false;
        bool success = false;
        nuraft::async_result<bool>::handler_type handler =
            [&](bool result, nuraft::ptr<std::exception>& /*except*/) {
                completed = true;
                success = result;
            };
        state_machine.create_snapshot(snapshot, handler);
        ASSERT_TRUE(completed);
        ASSERT_TRUE(success);
    }

    void create_snapshot(TypesenseStateMachine& state_machine,
                         uint64_t last_log_index,
                         uint64_t last_log_term) {
        auto config = nuraft::cs_new<nuraft::cluster_config>();
        nuraft::snapshot snapshot(last_log_index, last_log_term, config);
        bool completed = false;
        bool success = false;
        nuraft::async_result<bool>::handler_type handler =
            [&](bool result, nuraft::ptr<std::exception>& /*except*/) {
                completed = true;
                success = result;
            };
        state_machine.create_snapshot(snapshot, handler);
        ASSERT_TRUE(completed);
        ASSERT_TRUE(success);
    }

    std::string temp_dir_;
};

TEST_F(TypesenseStateMachineTest, LogicalSnapshotTransferChunksLargeDbFilesAndPreservesDbPayload) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const std::string source_dir = (std::filesystem::path(temp_dir_) / "source").string();
    const std::string target_dir = (std::filesystem::path(temp_dir_) / "target").string();
    initialize_node(source_dir);
    initialize_node(target_dir);

    const NuRaftStateLayout source_layout = NuRaftStateLayout::from_data_dir(source_dir);
    const NuRaftStateLayout target_layout = NuRaftStateLayout::from_data_dir(target_dir);
    std::string error;

    auto source_sink = std::make_unique<NuRaftKvStateMachineSink>(source_layout);
    NuRaftAppliedRequest create_collection;
    create_collection.index = 1;
    create_collection.route_hash = collection_create_hash;
    create_collection.route_kind = NuRaftRouteKind::kCollectionCreate;
    create_collection.body = R"({"name":"books"})";
    ASSERT_TRUE(source_sink->apply_all({create_collection}, error)) << error;

    std::filesystem::create_directories(std::filesystem::path(source_dir) / "db" / "archive");
    const auto large_source_file = std::filesystem::path(source_dir) / "db" / "archive" / "logical-transfer-large.bin";
    write_sparse_file(large_source_file, kLargeSnapshotSentinelBytes);

    TypesenseStateMachine source_state_machine(source_layout, source_sink.get());
    create_snapshot(source_state_machine, 1);

    NuRaftSnapshotCoordinator source_coordinator(source_layout);
    NuRaftSnapshotDescriptor source_descriptor;
    ASSERT_TRUE(source_coordinator.read_last_snapshot(source_descriptor, error)) << error;

    const auto leader_snapshot_file = std::filesystem::path(source_layout.snapshot_dir) /
                                      source_descriptor.snapshot_id / "db" / "archive" / "logical-transfer-large.bin";
    ASSERT_TRUE(std::filesystem::exists(leader_snapshot_file));
    ASSERT_EQ(kLargeSnapshotSentinelBytes, std::filesystem::file_size(leader_snapshot_file));

    auto snapshot = nuraft::cs_new<nuraft::snapshot>(1, 1, nuraft::cs_new<nuraft::cluster_config>());
    auto objects = read_snapshot_objects(source_state_machine, *snapshot);

    size_t large_file_chunks = 0;
    for (const auto& object : objects) {
        if (object.relative_path == "db/archive/logical-transfer-large.bin") {
            ++large_file_chunks;
        }
    }
    EXPECT_GT(large_file_chunks, 1u)
        << "large db checkpoint files should be split across multiple logical snapshot objects";

    NuRaftKvStateMachineSink target_sink(target_layout);
    TypesenseStateMachine target_state_machine(target_layout, &target_sink);
    deliver_snapshot_objects(target_state_machine, *snapshot, objects, [](const SnapshotObject&) {
        return true;
    });

    ASSERT_TRUE(target_state_machine.apply_snapshot(*snapshot));

    NuRaftSnapshotCoordinator target_coordinator(target_layout);
    NuRaftSnapshotDescriptor target_descriptor;
    ASSERT_TRUE(target_coordinator.read_last_snapshot(target_descriptor, error)) << error;
    EXPECT_EQ(target_descriptor, source_descriptor);

    const auto target_snapshot_file = std::filesystem::path(target_layout.snapshot_dir) /
                                      target_descriptor.snapshot_id / "db" / "archive" / "logical-transfer-large.bin";
    ASSERT_TRUE(std::filesystem::exists(target_snapshot_file));
    EXPECT_EQ(kLargeSnapshotSentinelBytes, std::filesystem::file_size(target_snapshot_file));

    EXPECT_EQ(1u, target_descriptor.last_applied_index);
}

TEST_F(TypesenseStateMachineTest, SnapshotDescriptorPersistsLastLogTermAcrossRestart) {
    const std::string source_dir = (std::filesystem::path(temp_dir_) / "source-term").string();
    initialize_node(source_dir);

    const NuRaftStateLayout source_layout = NuRaftStateLayout::from_data_dir(source_dir);
    std::string error;

    auto source_sink = std::make_unique<NuRaftKvStateMachineSink>(source_layout);
    TypesenseStateMachine source_state_machine(source_layout, source_sink.get());
    create_snapshot(source_state_machine, 7, 9);

    NuRaftSnapshotCoordinator coordinator(source_layout);
    NuRaftSnapshotDescriptor descriptor;
    ASSERT_TRUE(coordinator.read_last_snapshot(descriptor, error)) << error;
    EXPECT_EQ(9u, descriptor.last_log_term);

    TypesenseStateMachine restarted_state_machine(source_layout, source_sink.get());
    auto restarted_snapshot = restarted_state_machine.last_snapshot();
    ASSERT_TRUE(restarted_snapshot != nullptr);
    EXPECT_EQ(7u, restarted_snapshot->get_last_log_idx());
    EXPECT_EQ(9u, restarted_snapshot->get_last_log_term());
}

TEST_F(TypesenseStateMachineTest, LogicalSnapshotTransferRejectsMissingExpectedDbPayload) {
    const std::string source_dir = (std::filesystem::path(temp_dir_) / "source-missing-db").string();
    const std::string target_dir = (std::filesystem::path(temp_dir_) / "target-missing-db").string();
    initialize_node(source_dir);
    initialize_node(target_dir);

    const NuRaftStateLayout source_layout = NuRaftStateLayout::from_data_dir(source_dir);
    const NuRaftStateLayout target_layout = NuRaftStateLayout::from_data_dir(target_dir);
    std::string error;

    auto source_sink = std::make_unique<NuRaftKvStateMachineSink>(source_layout);
    std::filesystem::create_directories(std::filesystem::path(source_dir) / "db");
    ASSERT_TRUE(NuRaftFileStore::write_file_atomically((std::filesystem::path(source_dir) / "db" / "sentinel.txt").string(),
                                                       "live-store",
                                                       error)) << error;

    TypesenseStateMachine source_state_machine(source_layout, source_sink.get());
    create_snapshot(source_state_machine, 1);

    auto snapshot = nuraft::cs_new<nuraft::snapshot>(1, 1, nuraft::cs_new<nuraft::cluster_config>());
    auto objects = read_snapshot_objects(source_state_machine, *snapshot);

    TypesenseStateMachine target_state_machine(target_layout, nullptr);
    deliver_snapshot_objects(target_state_machine, *snapshot, objects, [](const SnapshotObject& object) {
        return object.obj_id == 0 || object.relative_path.rfind("db/", 0) != 0;
    });

    EXPECT_FALSE(target_state_machine.apply_snapshot(*snapshot));

    NuRaftSnapshotCoordinator target_coordinator(target_layout);
    NuRaftSnapshotDescriptor descriptor;
    EXPECT_FALSE(target_coordinator.read_last_snapshot(descriptor, error));
}

TEST_F(TypesenseStateMachineTest, LogicalSnapshotTransferClearsStaleDbWhenSourceSnapshotHasNoCheckpoint) {
    const uint64_t collection_create_hash = make_route_hash("POST", "collections");
    const std::string source_dir = (std::filesystem::path(temp_dir_) / "source-no-db").string();
    const std::string target_dir = (std::filesystem::path(temp_dir_) / "target-stale-db").string();
    initialize_node(source_dir);
    initialize_node(target_dir);

    const NuRaftStateLayout source_layout = NuRaftStateLayout::from_data_dir(source_dir);
    const NuRaftStateLayout target_layout = NuRaftStateLayout::from_data_dir(target_dir);
    std::string error;

    auto source_sink = std::make_unique<NuRaftKvStateMachineSink>(source_layout);
    NuRaftAppliedRequest create_collection;
    create_collection.index = 1;
    create_collection.route_hash = collection_create_hash;
    create_collection.route_kind = NuRaftRouteKind::kCollectionCreate;
    create_collection.body = R"({"name":"books"})";
    ASSERT_TRUE(source_sink->apply_all({create_collection}, error)) << error;

    std::filesystem::create_directories(std::filesystem::path(target_dir) / "db");
    ASSERT_TRUE(NuRaftFileStore::write_file_atomically((std::filesystem::path(target_dir) / "db" / "stale.txt").string(),
                                                       "stale-live-store",
                                                       error)) << error;

    TypesenseStateMachine source_state_machine(source_layout, source_sink.get());
    create_snapshot(source_state_machine, 1);

    auto snapshot = nuraft::cs_new<nuraft::snapshot>(1, 1, nuraft::cs_new<nuraft::cluster_config>());
    auto objects = read_snapshot_objects(source_state_machine, *snapshot);

    NuRaftKvStateMachineSink target_sink(target_layout);
    TypesenseStateMachine target_state_machine(target_layout, &target_sink);
    deliver_snapshot_objects(target_state_machine, *snapshot, objects, [](const SnapshotObject&) {
        return true;
    });

    ASSERT_TRUE(target_state_machine.apply_snapshot(*snapshot));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(target_dir) / "db"))
        << "stale main store should be removed when the transferred snapshot has no db checkpoint";
}

}  // namespace
