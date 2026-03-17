# Unit test configuration for dbrecovery

add_library(dbrecovery_test_utils STATIC
    test_utils.cpp
)
target_include_directories(dbrecovery_test_utils PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dbrecovery_test_utils PUBLIC dbrecovery_innodb)

# Page header tests
add_executable(test_page_header test_page_header.cpp)
target_link_libraries(test_page_header PRIVATE dbrecovery_innodb dbrecovery_test_utils GTest::GTest GTest::Main)
gtest_discover_tests(test_page_header)

# B+Tree parser tests
add_executable(test_btree_parser test_btree_parser.cpp)
target_link_libraries(test_btree_parser PRIVATE dbrecovery_innodb dbrecovery_test_utils GTest::GTest GTest::Main)
gtest_discover_tests(test_btree_parser)

# Undo log parser tests
add_executable(test_undo_log test_undo_log.cpp)
target_link_libraries(test_undo_log PRIVATE dbrecovery_innodb dbrecovery_test_utils GTest::GTest GTest::Main)
gtest_discover_tests(test_undo_log)

# Blob pointer tests
add_executable(test_blob_pointer test_blob_pointer.cpp)
target_link_libraries(test_blob_pointer PRIVATE dbrecovery_innodb dbrecovery_test_utils GTest::GTest GTest::Main)
gtest_discover_tests(test_blob_pointer)

# Integration tests (requires generated .ibd files)
add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE dbrecovery_innodb dbrecovery_test_utils GTest::GTest GTest::Main)
set_property(TEST test_integration APPEND PROPERTY ENVIRONMENT "TEST_DATA_DIR=${CMAKE_SOURCE_DIR}/tests/data")
gtest_discover_tests(test_integration)
