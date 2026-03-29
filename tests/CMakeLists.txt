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

# parse_records tests (P1-1)
add_executable(test_parse_records innodb/parse_records_test.cpp)
target_link_libraries(test_parse_records PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_parse_records)

# Fuzz tests for SDI parsing
add_executable(test_fuzz_sdi test_fuzz_sdi.cpp)
target_link_libraries(test_fuzz_sdi PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
target_compile_definitions(test_fuzz_sdi PRIVATE FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION=1)
gtest_discover_tests(test_fuzz_sdi)

# Redo log replay tests
add_executable(test_redo_log_replay innodb/redo_log_replay_test.cpp)
target_link_libraries(test_redo_log_replay PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_redo_log_replay)

# Integration tests (requires generated .ibd files)
add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
if(TARGET test_integration)
    add_test(NAME test_integration COMMAND test_integration)
    set_property(TEST test_integration APPEND PROPERTY ENVIRONMENT "TEST_DATA_DIR=${CMAKE_SOURCE_DIR}/tests/data")
endif()

# Undo log tests (P2-1)
add_executable(test_undo_log innodb/undo_log_test.cpp)
target_link_libraries(test_undo_log PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_undo_log)

# Undo log rollback tests
add_executable(test_undo_log_rollback test_undo_log_rollback.cpp)
target_link_libraries(test_undo_log_rollback PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_undo_log_rollback)

# MySQL 5.6/5.7 compatibility tests (P1-4)
add_executable(test_mysql56_57_compatibility innodb/mysql56_57_compatibility_test.cpp)
target_link_libraries(test_mysql56_57_compatibility PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_mysql56_57_compatibility)
