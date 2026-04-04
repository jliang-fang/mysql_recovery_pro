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

# Fuzz tests for BLOB pointer parsing
add_executable(test_blob_pointer_fuzz innodb/blob_pointer_fuzz_test.cpp)
target_link_libraries(test_blob_pointer_fuzz PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_blob_pointer_fuzz)

# Fuzz tests for Redo Log parsing
add_executable(test_redo_log_fuzz innodb/redo_log_fuzz_test.cpp)
target_link_libraries(test_redo_log_fuzz PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_redo_log_fuzz)

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

# Page checksum validation tests (P1-5)
add_executable(test_page_checksum innodb/page_checksum_test.cpp)
target_link_libraries(test_page_checksum PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_page_checksum)

# Page scanner tests (P0-2)
add_executable(test_page_scanner test_page_scanner.cpp)
target_link_libraries(test_page_scanner PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_page_scanner)

# Page chain rebuilder tests (P0-3)
add_executable(test_page_chain_rebuilder test_page_chain_rebuilder.cpp)
target_link_libraries(test_page_chain_rebuilder PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_page_chain_rebuilder)

# SDI extractor tests (P0-4)
add_executable(test_sdi_extractor test_sdi_extractor.cpp)
target_link_libraries(test_sdi_extractor PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_sdi_extractor)

# B+Tree node parser tests (P0-5)
add_executable(test_btree_node_parser test_btree_node_parser.cpp)
target_link_libraries(test_btree_node_parser PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_btree_node_parser)

# Record chain rebuilder tests (P0-6)
add_executable(test_record_chain_rebuilder test_record_chain_rebuilder.cpp)
target_link_libraries(test_record_chain_rebuilder PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_record_chain_rebuilder)

# Blob chain rebuilder tests (P0-7)
add_executable(test_blob_chain_rebuilder test_blob_chain_rebuilder.cpp)
target_link_libraries(test_blob_chain_rebuilder PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_blob_chain_rebuilder)

# Corruption scenarios tests (P3 - Real-world corruption scenarios)
add_executable(test_corruption_scenarios ../test/test_corruption_scenarios.cpp)
target_link_libraries(test_corruption_scenarios PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
target_compile_definitions(test_corruption_scenarios PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/test_data")
gtest_discover_tests(test_corruption_scenarios)

# io_uring scanner tests (P4 - High-performance I/O)
add_executable(test_io_uring_scanner ../test/test_io_uring_scanner.cpp)
target_link_libraries(test_io_uring_scanner PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
target_compile_definitions(test_io_uring_scanner PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/test_data")
gtest_discover_tests(test_io_uring_scanner)

# Heuristic inference tests (P5 - Metadata inference)
add_executable(test_heuristic_inference ../test/test_heuristic_inference.cpp)
target_link_libraries(test_heuristic_inference PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
target_compile_definitions(test_heuristic_inference PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/test_data")
gtest_discover_tests(test_heuristic_inference)

# Page parser V2 tests (P0 - Full version compatibility)
# Note: test_page_parser_v2.cpp removed as page_parser_v2.cpp was removed
# The functionality is now in page_types_v2.hpp and page_exporter.hpp

# Page exporter and visualizer tests (P1 - Export and visualization)
# Note: test_page_exporter_v2.cpp removed as it depends on removed classes

# Version detection tests (P1 - Multi-version compatibility)
add_executable(test_version_detection test_version_detection.cpp)
target_link_libraries(test_version_detection PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
target_compile_definitions(test_version_detection PRIVATE TEST_DATA_DIR_MACRO="${CMAKE_SOURCE_DIR}/test/test_data")
gtest_discover_tests(test_version_detection)

# B+Tree traversal tests (Phase 2 Week 3 - B+Tree Traversal + Chain Break Detection)
add_executable(test_btree_traversal unit/btree_traversal_test.cpp)
target_link_libraries(test_btree_traversal PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_btree_traversal)

# Heuristic scanner tests (Phase 2 Week 4 - Heuristic Scanning + Bad Block Inference)
add_executable(test_heuristic_scanner unit/heuristic_scanner_test.cpp)
target_link_libraries(test_heuristic_scanner PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_heuristic_scanner)

# Page parser V3 tests (Phase 3 - Full version compatibility + row format parsing)
add_executable(test_page_parser_v3 ../test/test_page_parser_v3.cpp)
target_link_libraries(test_page_parser_v3 PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_page_parser_v3)

# Extreme scenarios tests (Phase 4 - Extreme recovery scenarios validation)
add_executable(test_extreme_scenarios test_extreme_scenarios.cpp)
target_link_libraries(test_extreme_scenarios PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_extreme_scenarios)
