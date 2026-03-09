#include <gtest/gtest.h>

#include <string>

#include "nuraft/nuraft_route_classifier.h"
#include "string_utils.h"

namespace {

uint64_t make_route_hash(const std::string& method, const std::string& path) {
    const std::string method_path = method + path;
    const uint64_t hash = StringUtils::hash_wy(method_path.c_str(), method_path.size());
    return (hash > 100) ? hash : (hash + 100);
}

}  // namespace

TEST(NuRaftRouteClassifierTest, ClassifiesKnownWriteRoutes) {
    EXPECT_EQ(NuRaftRouteClassifier::classify(make_route_hash("POST", "collections/:collection/documents/import")),
              NuRaftRouteKind::kDocumentImport);
    EXPECT_EQ(NuRaftRouteClassifier::classify(make_route_hash("POST", "collections/:collection/documents")),
              NuRaftRouteKind::kDocumentWrite);
    EXPECT_EQ(NuRaftRouteClassifier::classify(make_route_hash("DELETE", "collections/:collection/documents")),
              NuRaftRouteKind::kDocumentDelete);
    EXPECT_EQ(NuRaftRouteClassifier::classify(make_route_hash("POST", "collections")),
              NuRaftRouteKind::kCollectionCreate);
    EXPECT_EQ(NuRaftRouteClassifier::classify(make_route_hash("DELETE", "collections/:collection")),
              NuRaftRouteKind::kCollectionDrop);
}

TEST(NuRaftRouteClassifierTest, ReturnsUnknownForUnrecognizedRoute) {
    EXPECT_EQ(NuRaftRouteClassifier::classify(0), NuRaftRouteKind::kUnknown);
    EXPECT_STREQ(NuRaftRouteClassifier::kind_name(NuRaftRouteKind::kUnknown), "unknown");
}
