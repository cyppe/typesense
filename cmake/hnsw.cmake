# Download hnsw (header-only)

set(HNSW_COMMIT 687d981753f8bafcd16421cbd2a166d0b62bc520)
set(HNSW_NAME hnswlib-${HNSW_COMMIT})
set(HNSW_TAR_PATH ${DEP_ROOT_DIR}/${HNSW_NAME}.tar.gz)
set(HNSW_URL https://github.com/nmslib/hnswlib/archive/${HNSW_COMMIT}.tar.gz)

if(NOT EXISTS ${HNSW_TAR_PATH})
    message(STATUS "Downloading ${HNSW_URL}")
    file(DOWNLOAD ${HNSW_URL} ${HNSW_TAR_PATH})
endif()

if(NOT EXISTS ${DEP_ROOT_DIR}/${HNSW_NAME})
    message(STATUS "Extracting ${HNSW_NAME}...")
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf ${HNSW_TAR_PATH} WORKING_DIRECTORY ${DEP_ROOT_DIR}/)
endif()
