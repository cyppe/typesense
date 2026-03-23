#include "build_info.h"
#include "build_info_generated.h"

namespace {

#ifndef TYPESENSE_VERSION
#define TYPESENSE_VERSION "unknown"
#endif

constexpr typesense_build_info_t kTypesenseBuildInfo = {
    TYPESENSE_VERSION,
    TYPESENSE_BUILD_GIT_SHA,
    TYPESENSE_BUILD_GIT_SHORT_SHA,
    TYPESENSE_BUILD_GIT_REF,
    TYPESENSE_BUILD_GIT_EXACT_TAG,
    TYPESENSE_BUILD_GIT_TREE_STATUS,
};

}  // namespace

const typesense_build_info_t& get_typesense_build_info() {
    return kTypesenseBuildInfo;
}
