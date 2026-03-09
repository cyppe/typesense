#include "nuraft/nuraft_route_classifier.h"

#include <string>
#include <vector>

#include "string_utils.h"

namespace {

uint64_t make_route_hash(const std::string& method, const std::string& path) {
    const std::string method_path = method + path;
    const uint64_t hash = StringUtils::hash_wy(method_path.c_str(), method_path.size());
    return (hash > 100) ? hash : (hash + 100);
}

}  // namespace

NuRaftRouteKind NuRaftRouteClassifier::classify(uint64_t route_hash) {
    static const uint64_t kDocumentImportHash = make_route_hash("POST", "collections/:collection/documents/import");
    static const uint64_t kDocumentCreateHash = make_route_hash("POST", "collections/:collection/documents");
    static const uint64_t kDocumentPatchHash = make_route_hash("PATCH", "collections/:collection/documents/:id");
    static const uint64_t kDocumentPatchManyHash = make_route_hash("PATCH", "collections/:collection/documents");
    static const uint64_t kDocumentDeleteHash = make_route_hash("DELETE", "collections/:collection/documents/:id");
    static const uint64_t kDocumentDeleteManyHash = make_route_hash("DELETE", "collections/:collection/documents");
    static const uint64_t kCollectionCreateHash = make_route_hash("POST", "collections");
    static const uint64_t kCollectionDropHash = make_route_hash("DELETE", "collections/:collection");

    if (route_hash == kDocumentImportHash) {
        return NuRaftRouteKind::kDocumentImport;
    }
    if (route_hash == kDocumentCreateHash || route_hash == kDocumentPatchHash || route_hash == kDocumentPatchManyHash) {
        return NuRaftRouteKind::kDocumentWrite;
    }
    if (route_hash == kDocumentDeleteHash || route_hash == kDocumentDeleteManyHash) {
        return NuRaftRouteKind::kDocumentDelete;
    }
    if (route_hash == kCollectionCreateHash) {
        return NuRaftRouteKind::kCollectionCreate;
    }
    if (route_hash == kCollectionDropHash) {
        return NuRaftRouteKind::kCollectionDrop;
    }

    return NuRaftRouteKind::kUnknown;
}

const char* NuRaftRouteClassifier::kind_name(NuRaftRouteKind kind) {
    switch (kind) {
        case NuRaftRouteKind::kDocumentImport:
            return "document_import";
        case NuRaftRouteKind::kDocumentWrite:
            return "document_write";
        case NuRaftRouteKind::kDocumentDelete:
            return "document_delete";
        case NuRaftRouteKind::kCollectionCreate:
            return "collection_create";
        case NuRaftRouteKind::kCollectionDrop:
            return "collection_drop";
        case NuRaftRouteKind::kUnknown:
        default:
            return "unknown";
    }
}
