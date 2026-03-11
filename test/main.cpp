#include <gtest/gtest.h>
#include "embedder_manager.h"
#include "logger.h"

class TypesenseTestEnvironment : public testing::Environment {
public:
    virtual void SetUp() {

    }

    virtual void TearDown() {
        EmbedderManager::get_instance().delete_all_image_embedders();
        EmbedderManager::get_instance().delete_all_text_embedders();
    }
};

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new TypesenseTestEnvironment);
    int exitCode = RUN_ALL_TESTS();
    return exitCode;
}
