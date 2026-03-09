#pragma once

#include <cstdint>
#include <string>

enum class NuRaftRouteKind {
    kUnknown = 0,
    kDocumentImport,
    kDocumentWrite,
    kDocumentDelete,
    kCollectionCreate,
    kCollectionDrop,
};

class NuRaftRouteClassifier {
public:
    static NuRaftRouteKind classify(uint64_t route_hash);
    static const char* kind_name(NuRaftRouteKind kind);
};
