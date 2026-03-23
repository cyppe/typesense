#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <collection_manager.h>
#include "collection.h"
#include "temp_dir_utils.h"
#include "logger.h"

class CollectionOperationsTest : public ::testing::Test {
protected:
    Store *store;
    CollectionManager & collectionManager = CollectionManager::get_instance();
    std::atomic<bool> quit = false;
    std::string state_dir_path;

    std::vector<std::string> query_fields;
    std::vector<sort_by> sort_fields;

    void setupCollection() {
        state_dir_path = typesense_test::make_test_temp_dir("collection_operations");
        TS_LOG(INFO) << "Truncating and creating: " << state_dir_path;
        typesense_test::reset_test_temp_dir(state_dir_path);

        store = new Store(state_dir_path);
        collectionManager.init(store, 1.0, "auth_key", quit);
        collectionManager.load(8, 1000);
    }

    virtual void SetUp() {
        setupCollection();
    }

    virtual void TearDown() {
        collectionManager.dispose();
        delete store;
        typesense_test::cleanup_test_temp_dir(state_dir_path);
    }
};

TEST_F(CollectionOperationsTest, IncrementInt32Value) {
    nlohmann::json schema = R"({
        "name": "coll1",
        "fields": [
            {"name": "title", "type": "string"},
            {"name": "points", "type": "int32"},
            {"name": "points64", "type": "int64"}
        ]
    })"_json;

    Collection *coll = collectionManager.create_collection(schema).get();

    nlohmann::json doc;
    doc["id"] = "0";
    doc["title"] = "Sherlock Holmes";
    doc["points"] = 100;
    doc["points64"] = 0;
    ASSERT_TRUE(coll->add(doc.dump()).ok());

    // increment by 1
    doc.erase("points");
    doc["id"] = "0";
    doc["$operations"] = R"({"increment": {"points": 1}})"_json;
    ASSERT_TRUE(coll->add(doc.dump(), UPDATE).ok());

    auto res = coll->search("*", {"title"}, "points:101", {}, {}, {0}, 3, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(size_t{1}, res["hits"].size());

    ASSERT_EQ(size_t{4}, res["hits"][0]["document"].size());
    ASSERT_EQ("0", res["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ("Sherlock Holmes", res["hits"][0]["document"]["title"].get<std::string>());
    ASSERT_EQ(size_t{101}, res["hits"][0]["document"]["points"].get<size_t>());

    // increment by 10
    doc["id"] = "0";
    doc["$operations"] = R"({"increment": {"points": 10}})"_json;
    ASSERT_TRUE(coll->add(doc.dump(), UPDATE).ok());

    res = coll->search("*", {"title"}, "points:111", {}, {}, {0}, 3, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(size_t{1}, res["hits"].size());
    ASSERT_EQ(size_t{4}, res["hits"][0]["document"].size());
    ASSERT_EQ("0", res["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ("Sherlock Holmes", res["hits"][0]["document"]["title"].get<std::string>());
    ASSERT_EQ(size_t{111}, res["hits"][0]["document"]["points"].get<size_t>());

    // increment points64 by 5
    doc["id"] = "0";
    doc["$operations"] = R"({"increment": {"points64": 5}})"_json;
    ASSERT_TRUE(coll->add(doc.dump(), UPDATE).ok());

    res = coll->search("*", {"title"}, "points:111", {}, {}, {0}, 3, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(size_t{1}, res["hits"].size());
    ASSERT_EQ(size_t{4}, res["hits"][0]["document"].size());
    ASSERT_EQ("0", res["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ(size_t{5}, res["hits"][0]["document"]["points64"].get<size_t>());

    // decrement by 10 using negative number
    doc["id"] = "0";
    doc["$operations"] = R"({"increment": {"points": -10}})"_json;
    ASSERT_TRUE(coll->add(doc.dump(), UPDATE).ok());

    res = coll->search("*", {"title"}, "points:101", {}, {}, {0}, 3, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(size_t{1}, res["hits"].size());
    ASSERT_EQ(size_t{4}, res["hits"][0]["document"].size());
    ASSERT_EQ("0", res["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ("Sherlock Holmes", res["hits"][0]["document"]["title"].get<std::string>());
    ASSERT_EQ(size_t{101}, res["hits"][0]["document"]["points"].get<size_t>());

    // bad field - should not increment but title field should be updated
    doc["id"] = "0";
    doc["title"] = "The Sherlock Holmes";
    doc["$operations"] = R"({"increment": {"pointsx": -10}})"_json;
    ASSERT_TRUE(coll->add(doc.dump(), UPDATE).ok());
    res = coll->search("*", {"title"}, "", {}, {}, {0}, 3, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(size_t{1}, res["hits"].size());
    ASSERT_EQ(size_t{4}, res["hits"][0]["document"].size());
    ASSERT_EQ("0", res["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ("The Sherlock Holmes", res["hits"][0]["document"]["title"].get<std::string>());
    ASSERT_EQ(size_t{101}, res["hits"][0]["document"]["points"].get<size_t>());
}

TEST_F(CollectionOperationsTest, IncrementInt32ValueCreationViaOptionalField) {
    nlohmann::json schema = R"({
        "name": "coll1",
        "fields": [
            {"name": "title", "type": "string"},
            {"name": "points", "type": "int32", "optional": true}
        ]
    })"_json;

    Collection *coll = collectionManager.create_collection(schema).get();

    nlohmann::json doc;
    doc["id"] = "0";
    doc["title"] = "Sherlock Holmes";
    doc["$operations"] = R"({"increment": {"points": 1}})"_json;
    ASSERT_TRUE(coll->add(doc.dump(), EMPLACE).ok());

    auto res = coll->search("*", {"title"}, "points:1", {}, {}, {0}, 3, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(size_t{1}, res["hits"].size());
    ASSERT_EQ(size_t{3}, res["hits"][0]["document"].size());
    ASSERT_EQ("0", res["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ("Sherlock Holmes", res["hits"][0]["document"]["title"].get<std::string>());
    ASSERT_EQ(size_t{1}, res["hits"][0]["document"]["points"].get<size_t>());

    // try same with CREATE action
    doc.clear();
    doc["id"] = "1";
    doc["title"] = "Harry Potter";
    doc["$operations"] = R"({"increment": {"points": 10}})"_json;
    ASSERT_TRUE(coll->add(doc.dump(), CREATE).ok());

    res = coll->search("*", {"title"}, "points:10", {}, {}, {0}, 3, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(size_t{1}, res["hits"].size());
    ASSERT_EQ(size_t{3}, res["hits"][0]["document"].size());
    ASSERT_EQ("1", res["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ("Harry Potter", res["hits"][0]["document"]["title"].get<std::string>());
    ASSERT_EQ(size_t{10}, res["hits"][0]["document"]["points"].get<size_t>());
}

TEST_F(CollectionOperationsTest, ConcurrentIncrementUpdatesOnSameDocumentAreSerialized) {
    nlohmann::json schema = R"({
        "name": "coll1",
        "fields": [
            {"name": "title", "type": "string"},
            {"name": "points", "type": "int32"}
        ]
    })"_json;

    Collection* coll = collectionManager.create_collection(schema).get();

    nlohmann::json doc = {
        {"id", "counter"},
        {"title", "Concurrent counter"},
        {"points", 0}
    };
    ASSERT_TRUE(coll->add(doc.dump()).ok());

    for(size_t iteration = 0; iteration < 10; ++iteration) {
        doc["points"] = 0;
        ASSERT_TRUE(coll->add(doc.dump(), UPSERT).ok());

        bool increment_one_ok = false;
        bool increment_two_ok = false;

        std::thread t1([&]() {
            nlohmann::json increment_one = {
                {"id", "counter"},
                {"$operations", {{"increment", {{"points", 1}}}}}
            };
            increment_one_ok = coll->add(increment_one.dump(), UPDATE).ok();
        });

        std::thread t2([&]() {
            nlohmann::json increment_two = {
                {"id", "counter"},
                {"$operations", {{"increment", {{"points", 2}}}}}
            };
            increment_two_ok = coll->add(increment_two.dump(), UPDATE).ok();
        });

        t1.join();
        t2.join();

        ASSERT_TRUE(increment_one_ok);
        ASSERT_TRUE(increment_two_ok);

        auto stored_doc_op = coll->get("counter");
        ASSERT_TRUE(stored_doc_op.ok());
        ASSERT_EQ(3, stored_doc_op.get()["points"].get<int32_t>()) << "iteration=" << iteration;
    }
}

TEST_F(CollectionOperationsTest, ConcurrentPartialUpdatesOnSameDocumentPreserveDisjointFields) {
    nlohmann::json schema = R"({
        "name": "coll1",
        "fields": [
            {"name": "title", "type": "string"},
            {"name": "category", "type": "string", "optional": true},
            {"name": "points", "type": "int32"}
        ]
    })"_json;

    Collection* coll = collectionManager.create_collection(schema).get();

    nlohmann::json doc = {
        {"id", "shared"},
        {"title", "Original"},
        {"category", "base"},
        {"points", 0}
    };
    ASSERT_TRUE(coll->add(doc.dump()).ok());

    for(size_t iteration = 0; iteration < 10; ++iteration) {
        doc["title"] = "Original";
        doc["category"] = "base";
        doc["points"] = 0;
        ASSERT_TRUE(coll->add(doc.dump(), UPSERT).ok());

        bool update_title_ok = false;
        bool update_points_ok = false;

        std::thread t1([&]() {
            nlohmann::json update_title = {
                {"id", "shared"},
                {"title", "Updated title"}
            };
            update_title_ok = coll->add(update_title.dump(), UPDATE).ok();
        });

        std::thread t2([&]() {
            nlohmann::json update_points = {
                {"id", "shared"},
                {"$operations", {{"increment", {{"points", 3}}}}},
                {"category", "merged"}
            };
            update_points_ok = coll->add(update_points.dump(), UPDATE).ok();
        });

        t1.join();
        t2.join();

        ASSERT_TRUE(update_title_ok);
        ASSERT_TRUE(update_points_ok);

        auto stored_doc_op = coll->get("shared");
        ASSERT_TRUE(stored_doc_op.ok());
        const auto& stored_doc = stored_doc_op.get();
        ASSERT_EQ("Updated title", stored_doc["title"].get<std::string>()) << "iteration=" << iteration;
        ASSERT_EQ("merged", stored_doc["category"].get<std::string>()) << "iteration=" << iteration;
        ASSERT_EQ(3, stored_doc["points"].get<int32_t>()) << "iteration=" << iteration;
    }
}

TEST_F(CollectionOperationsTest, ConcurrentCreatesOnSameDocumentIdDoNotBothSucceed) {
    nlohmann::json schema = R"({
        "name": "coll1",
        "fields": [
            {"name": "title", "type": "string"}
        ]
    })"_json;

    Collection* coll = collectionManager.create_collection(schema).get();

    for(size_t iteration = 0; iteration < 10; ++iteration) {
        const std::string doc_id = "duplicate-" + std::to_string(iteration);

        bool create_one_ok = false;
        bool create_two_ok = false;
        std::string create_one_error;
        std::string create_two_error;

        std::thread t1([&]() {
            nlohmann::json create_one = {
                {"id", doc_id},
                {"title", "Title A"}
            };
            auto add_op = coll->add(create_one.dump(), CREATE);
            create_one_ok = add_op.ok();
            if(!add_op.ok()) {
                create_one_error = add_op.error();
            }
        });

        std::thread t2([&]() {
            nlohmann::json create_two = {
                {"id", doc_id},
                {"title", "Title B"}
            };
            auto add_op = coll->add(create_two.dump(), CREATE);
            create_two_ok = add_op.ok();
            if(!add_op.ok()) {
                create_two_error = add_op.error();
            }
        });

        t1.join();
        t2.join();

        ASSERT_NE(create_one_ok, create_two_ok) << "iteration=" << iteration;
        ASSERT_TRUE(create_one_error.empty() || create_one_error.find("already exists") != std::string::npos);
        ASSERT_TRUE(create_two_error.empty() || create_two_error.find("already exists") != std::string::npos);

        auto stored_doc_op = coll->get(doc_id);
        ASSERT_TRUE(stored_doc_op.ok()) << "iteration=" << iteration;
        const auto& stored_doc = stored_doc_op.get();
        const std::string stored_title = stored_doc["title"].get<std::string>();
        ASSERT_TRUE(stored_title == "Title A" || stored_title == "Title B") << "iteration=" << iteration;
    }
}

TEST_F(CollectionOperationsTest, ConcurrentExplicitIdUpdatesOnSameDocumentAreSerialized) {
    nlohmann::json schema = R"({
        "name": "coll1",
        "fields": [
            {"name": "title", "type": "string"},
            {"name": "points", "type": "int32"}
        ]
    })"_json;

    Collection* coll = collectionManager.create_collection(schema).get();

    nlohmann::json doc = {
        {"id", "counter"},
        {"title", "Concurrent counter"},
        {"points", 0}
    };
    ASSERT_TRUE(coll->add(doc.dump()).ok());

    for(size_t iteration = 0; iteration < 10; ++iteration) {
        doc["points"] = 0;
        ASSERT_TRUE(coll->add(doc.dump(), UPSERT).ok());

        bool increment_one_ok = false;
        bool increment_two_ok = false;

        std::thread t1([&]() {
            nlohmann::json increment_one = {
                {"$operations", {{"increment", {{"points", 1}}}}}
            };
            increment_one_ok = coll->add(increment_one.dump(), UPDATE, "counter").ok();
        });

        std::thread t2([&]() {
            nlohmann::json increment_two = {
                {"$operations", {{"increment", {{"points", 2}}}}}
            };
            increment_two_ok = coll->add(increment_two.dump(), UPDATE, "counter").ok();
        });

        t1.join();
        t2.join();

        ASSERT_TRUE(increment_one_ok);
        ASSERT_TRUE(increment_two_ok);

        auto stored_doc_op = coll->get("counter");
        ASSERT_TRUE(stored_doc_op.ok());
        ASSERT_EQ(3, stored_doc_op.get()["points"].get<int32_t>()) << "iteration=" << iteration;
    }
}
