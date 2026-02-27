#include <gtest/gtest.h>
#include "personalization_model_manager.h"
#include "store.h"
#include <filesystem>
#include <fstream>
#include "collection_manager.h"
#include "runfiles_utils.h"
#include "temp_dir_utils.h"
#include "logger.h"

class PersonalizationModelManagerTest : public ::testing::Test {
protected:
    std::string temp_dir;
    std::string test_root_path;
    std::string state_dir_path;
    std::string model_dir_path;
    Store *store;
    CollectionManager& collectionManager = CollectionManager::get_instance();
    std::atomic<bool> quit = false;

    void SetUp() override {
        temp_dir = typesense_test::make_test_temp_dir("personalization_model_manager_tmp");
        typesense_test::reset_test_temp_dir(temp_dir);

        test_root_path = typesense_test::make_test_temp_dir("personalization_model_manager");
        model_dir_path = test_root_path + "/models";
        state_dir_path = test_root_path + "/state";

        typesense_test::reset_test_temp_dir(model_dir_path);
        typesense_test::reset_test_temp_dir(state_dir_path);
        EmbedderManager::set_model_dir(model_dir_path);

        // Create test collection
        Config::get_instance().set_data_dir(state_dir_path);

        TS_LOG(INFO) << "Truncating and creating: " << state_dir_path;
        nlohmann::json collection_schema = R"({
            "name": "companies",
            "fields": [
                {"name": "name", "type": "string"}
            ]
        })"_json;

        store = new Store(state_dir_path);
        collectionManager.init(store, 1.0, "auth_key", quit);
        collectionManager.create_collection(collection_schema);
        PersonalizationModelManager::init(store);
    }

    void TearDown() override {
        collectionManager.dispose();
        PersonalizationModelManager::dispose();
        delete store;
        typesense_test::cleanup_test_temp_dir(temp_dir);
        typesense_test::cleanup_test_temp_dir(test_root_path);
    }

    nlohmann::json create_valid_model(const std::string& id = "") {
        nlohmann::json model;
        model["id"] = id;
        model["name"] = "ts/tyrec-1";
        model["type"] = "recommendation";
        model["collection"] = "companies";
        return model;
    }

    std::string get_onnx_model_archive() {
        std::string archive_name = resolve_test_path({
            "test/resources/models.tar.gz",
            "_main/test/resources/models.tar.gz",
        });

        std::ifstream archive_file(archive_name, std::ios::binary);
        EXPECT_TRUE(archive_file.is_open()) << "Unable to open archive: " << archive_name;
        if (!archive_file.is_open()) {
            return {};
        }
        std::string archive_content((std::istreambuf_iterator<char>(archive_file)), std::istreambuf_iterator<char>());

        archive_file.close();
        return archive_content;
    }
};

TEST_F(PersonalizationModelManagerTest, AddModelSuccess) {
    nlohmann::json model = create_valid_model("test_id");
    std::string model_data = get_onnx_model_archive();
    auto result = PersonalizationModelManager::add_model(model, "test_id", true, model_data);
    ASSERT_TRUE(result.ok());
    ASSERT_FALSE(result.get().empty());
}

TEST_F(PersonalizationModelManagerTest, AddModelDuplicate) {
    nlohmann::json model = create_valid_model("test_id");
    std::string model_data = get_onnx_model_archive();
    auto result = PersonalizationModelManager::add_model(model, "test_id", true, model_data);
    ASSERT_TRUE(result.ok());
    auto result1 = PersonalizationModelManager::add_model(model, "test_id", true);
    ASSERT_FALSE(result1.ok());
    ASSERT_EQ(result1.code(), 409u);
    ASSERT_EQ(result1.error(), "Model id already exists");
}

TEST_F(PersonalizationModelManagerTest, GetModelSuccess) {
    nlohmann::json model = create_valid_model("test_id");
    std::string model_data = get_onnx_model_archive();
    auto result = PersonalizationModelManager::add_model(model, "test_id", true, model_data);
    ASSERT_TRUE(result.ok());
    auto get_result = PersonalizationModelManager::get_model("test_id");
    ASSERT_TRUE(get_result.ok());
    ASSERT_EQ(get_result.get()["id"], "test_id");
    ASSERT_EQ(get_result.get()["name"], "ts/tyrec-1");
    ASSERT_EQ(get_result.get()["type"], "recommendation");
    ASSERT_EQ(get_result.get()["num_dim"], 256);
}

TEST_F(PersonalizationModelManagerTest, GetModelNotFound) {
    auto result = PersonalizationModelManager::get_model("nonexistent");
    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.code(), 404u);
    ASSERT_EQ(result.error(), "Model not found");
}

TEST_F(PersonalizationModelManagerTest, DeleteModelSuccess) {
    nlohmann::json model = create_valid_model("test_id");
    std::string model_data = get_onnx_model_archive();
    auto result = PersonalizationModelManager::add_model(model, "test_id", true, model_data);
    ASSERT_TRUE(result.ok());
    auto delete_result = PersonalizationModelManager::delete_model("test_id");
    ASSERT_TRUE(delete_result.ok());
    ASSERT_EQ(delete_result.get()["id"], "test_id");
    ASSERT_EQ(delete_result.get()["name"], "ts/tyrec-1");
    ASSERT_EQ(delete_result.get()["type"], "recommendation");
    ASSERT_EQ(delete_result.get()["num_dim"], 256);

    auto get_result = PersonalizationModelManager::get_model("test_id");
    ASSERT_FALSE(get_result.ok());
    ASSERT_EQ(get_result.code(), 404u);
    ASSERT_EQ(get_result.error(), "Model not found");
}

TEST_F(PersonalizationModelManagerTest, DeleteModelNotFound) {
    auto result = PersonalizationModelManager::delete_model("nonexistent");
    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.code(), 404u);
    ASSERT_EQ(result.error(), "Model not found");
}

TEST_F(PersonalizationModelManagerTest, GetAllModelsEmpty) {
    auto result = PersonalizationModelManager::get_all_models();
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.get().empty());
}

TEST_F(PersonalizationModelManagerTest, GetAllModelsWithData) {
    nlohmann::json model1 = create_valid_model("test_id1");
    nlohmann::json model2 = create_valid_model("test_id2");
    
    PersonalizationModelManager::add_model(model1, "test_id1", true, get_onnx_model_archive());
    PersonalizationModelManager::add_model(model2, "test_id2", true, get_onnx_model_archive());

    auto result = PersonalizationModelManager::get_all_models();
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.get().size(), size_t{2});
}

TEST_F(PersonalizationModelManagerTest, UpdateModelSuccess) {
    nlohmann::json model = create_valid_model("test_id");
    auto add_result = PersonalizationModelManager::add_model(model, "test_id", true, get_onnx_model_archive());
    ASSERT_TRUE(add_result.ok());

    nlohmann::json update;
    update["name"] = "ts/tyrec-1";
    auto update_result = PersonalizationModelManager::update_model("test_id", update, "");
    ASSERT_TRUE(update_result.ok());
    ASSERT_EQ(update_result.get()["name"], "ts/tyrec-1");
    ASSERT_EQ(update_result.get()["type"], "recommendation");
    ASSERT_EQ(update_result.get()["num_dim"], 256);
}

TEST_F(PersonalizationModelManagerTest, UpdateModelNotFound) {
    nlohmann::json update;
    update["name"] = "ts/tyrec-1";
    auto result = PersonalizationModelManager::update_model("nonexistent", update, "");
    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.code(), 404u);
    ASSERT_EQ(result.error(), "Model not found");
}

TEST_F(PersonalizationModelManagerTest, UpdateModelInvalidData) {
    nlohmann::json model = create_valid_model("test_id");
    std::string model_data = get_onnx_model_archive();
    auto result = PersonalizationModelManager::add_model(model, "test_id", true, model_data);
    ASSERT_TRUE(result.ok());
    nlohmann::json update;
    update["name"] = "invalid/name";
    auto update_result = PersonalizationModelManager::update_model("test_id", update, "");
    ASSERT_FALSE(update_result.ok());
    ASSERT_EQ(update_result.code(), 400u);
    ASSERT_EQ(update_result.error(), "Model namespace must be 'ts'.");
}
