# Unit test configuration for dbrecovery

# Page header tests
add_executable(test_page_header test_page_header.cpp)
target_link_libraries(test_page_header PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_page_header)

# BLOB recovery tests
add_executable(test_blob_recovery innodb/blob_recovery_test.cpp)
target_link_libraries(test_blob_recovery PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_blob_recovery)

# Redo log tests
add_executable(test_redo_log innodb/redo_log_test.cpp)
target_link_libraries(test_redo_log PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_redo_log)

# SDI manager tests
add_executable(test_sdi_manager innodb/sdi_manager_test.cpp)
target_link_libraries(test_sdi_manager PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_sdi_manager)

# Integration tests (requires generated .ibd files)
add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
if(TARGET test_integration)
    add_test(NAME test_integration COMMAND test_integration)
    set_property(TEST test_integration APPEND PROPERTY ENVIRONMENT "TEST_DATA_DIR=${CMAKE_SOURCE_DIR}/tests/data")
endif()
