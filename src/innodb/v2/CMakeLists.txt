add_library(dbrecovery_v2 STATIC
    page_parser.cpp
    record_parser.cpp
    redo_parser.cpp
    undo_parser.cpp
)
target_include_directories(dbrecovery_v2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_link_libraries(dbrecovery_v2 dbrecovery_core)

if(EXISTS "${CMAKE_SOURCE_DIR}/test_data/sample.ibd")
    add_executable(test_page_v2 ../test/test_page_parser_v2.cpp)
    target_link_libraries(test_page_v2 dbrecovery_v2)
endif()
