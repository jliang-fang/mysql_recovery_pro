message(STATUS "=== Loading src/innodb/CMakeLists.txt ===")

# add_subdirectory(v2) # Temporarily disabled for stability

add_library(dbrecovery_core STATIC
    page_parser.cpp
    binary_reader.cpp
    sdi_manager.cpp
    blob_pointer.cpp
    page_defs.cpp
)
target_include_directories(dbrecovery_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
