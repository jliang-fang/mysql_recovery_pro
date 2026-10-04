# Unit test configuration for dbrecovery

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test_tmp)
set(DBREC_TEST_TMPDIR_VALUE "${CMAKE_CURRENT_BINARY_DIR}/test_tmp")

# SchemaInferrer v2.0 pipeline tests (Phase 4 - Slice A)
add_executable(test_schema_inferrer innodb/schema_inferrer_test.cpp)
target_link_libraries(test_schema_inferrer PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_schema_inferrer)

# Page fragment reassembler tests
add_executable(test_page_fragment_reassembler test_page_fragment_reassembler.cpp)
target_link_libraries(test_page_fragment_reassembler PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_page_fragment_reassembler)

# V2 fragment reassembler tests
add_executable(test_fragment_reassembler_v2 test_fragment_reassembler_v2.cpp)
target_link_libraries(test_fragment_reassembler_v2 PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_fragment_reassembler_v2)

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

# Header repair tests
add_executable(test_header_recovery test_header_recovery.cpp)
target_link_libraries(test_header_recovery PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_header_recovery)

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

# Version-aware recovery engine tests (Phase 2 - version evidence safety)
add_executable(test_version_aware_recovery test_version_aware_recovery.cpp)
target_link_libraries(test_version_aware_recovery PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_version_aware_recovery)

# AutoDetector page-zero boundary tests
add_executable(test_auto_detector_page0 auto_detector_page0_test.cpp)
target_link_libraries(test_auto_detector_page0 PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_auto_detector_page0)

# Binary stream read boundary tests
add_executable(test_binary_stream_reader binary_stream_reader_test.cpp)
target_link_libraries(test_binary_stream_reader PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_binary_stream_reader)

# Multi-page version evidence aggregation tests (Phase 2 P0)
add_executable(test_version_evidence test_version_evidence.cpp)
target_link_libraries(test_version_evidence PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_version_evidence)

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

# Instant DDL tests (MySQL 8.0.23+ instant ADD COLUMN support)
add_executable(test_instant_ddl unit/test_instant_ddl.cpp)
target_link_libraries(test_instant_ddl PRIVATE dbrecovery_innodb GTest::GTest GTest::Main nlohmann_json::nlohmann_json)
gtest_discover_tests(test_instant_ddl)

# IBD file recovery tests
add_executable(test_ibd_recovery unit/test_ibd_recovery.cpp)
target_link_libraries(test_ibd_recovery PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_ibd_recovery)

# TDE keyring tests
add_executable(test_tde_keyring unit/test_tde_keyring.cpp)
target_link_libraries(test_tde_keyring PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_tde_keyring PROPERTIES ENVIRONMENT "DBREC_TEST_TMPDIR=${DBREC_TEST_TMPDIR_VALUE}")

# Comprehensive recovery tests
add_executable(test_comprehensive_recovery unit/test_comprehensive_recovery.cpp)
target_link_libraries(test_comprehensive_recovery PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_comprehensive_recovery)

# Data validation tests
add_executable(test_data_validation unit/test_data_validation.cpp)
target_link_libraries(test_data_validation PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_data_validation)

# BLOB chain traversal tests
add_executable(test_blob_chain_traversal unit/test_blob_chain_traversal.cpp)
target_link_libraries(test_blob_chain_traversal PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_blob_chain_traversal)

# ext4 block bitmap tests
add_executable(test_ext4_block_bitmap unit/test_ext4_block_bitmap.cpp)
target_link_libraries(test_ext4_block_bitmap PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_ext4_block_bitmap)

# File signature detection tests
add_executable(test_file_signature unit/test_file_signature.cpp)
target_link_libraries(test_file_signature PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_file_signature)

# XFS extent validation tests
add_executable(test_xfs_extent unit/test_xfs_extent.cpp)
target_link_libraries(test_xfs_extent PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_xfs_extent)

# RollPtrDecoder tests (P0 - Track 1)
add_executable(test_roll_ptr_decoder innodb/roll_ptr_decoder_test.cpp)
target_link_libraries(test_roll_ptr_decoder PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_roll_ptr_decoder)

# UndoPageIndex tests (P0 - Track 1)
add_executable(test_undo_page_index innodb/undo_page_index_test.cpp)
target_link_libraries(test_undo_page_index PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_undo_page_index)

# TrxSysTimestampIndex tests (P0 - Track 1)
add_executable(test_trx_sys_timestamp_index innodb/trx_sys_timestamp_index_test.cpp)
target_link_libraries(test_trx_sys_timestamp_index PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_trx_sys_timestamp_index)

# SecondaryIndexRescuer tests (P0 - Track 1)
add_executable(test_secondary_index_rescuer innodb/secondary_index_rescuer_test.cpp)
target_link_libraries(test_secondary_index_rescuer PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_secondary_index_rescuer)

# DamagedPageHandler tests (P0 - Track 1)
add_executable(test_damaged_page_handler innodb/damaged_page_handler_test.cpp)
target_link_libraries(test_damaged_page_handler PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_damaged_page_handler)

# PageTypeInferrer tests (P0 - Track 1)
add_executable(test_page_type_inferrer innodb/page_type_inferrer_test.cpp)
target_link_libraries(test_page_type_inferrer PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_page_type_inferrer)

# RawDiskCarver tests (P0 - Track 1)
add_executable(test_raw_disk_carver innodb/raw_disk_carver_test.cpp)
target_link_libraries(test_raw_disk_carver PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_raw_disk_carver)

# FlashbackGenerator tests (P0 - Track 1)
add_executable(test_flashback_generator innodb/flashback_generator_test.cpp)
target_link_libraries(test_flashback_generator PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_flashback_generator)

# DeepUndoAnalyzer tests (P0 - Track 1)
add_executable(test_deep_undo_analyzer innodb/deep_undo_analyzer_test.cpp)
target_link_libraries(test_deep_undo_analyzer PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_deep_undo_analyzer PROPERTIES ENVIRONMENT "DBREC_TEST_TMPDIR=${DBREC_TEST_TMPDIR_VALUE}")

# InteractiveSchemaBuilder tests (P1)
add_executable(test_interactive_schema unit/test_interactive_schema.cpp)
target_link_libraries(test_interactive_schema PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_interactive_schema)

# SecondaryIndexBTreeRepairer tests (P3)
add_executable(test_sec_btree_repair unit/test_sec_btree_repair.cpp)
target_link_libraries(test_sec_btree_repair PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_sec_btree_repair)

# HeuristicSchemaEngine tests (P5 - Schema inference v2.0)
add_executable(test_heuristic_schema_engine ../test/test_heuristic_schema_engine.cpp)
target_link_libraries(test_heuristic_schema_engine PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_heuristic_schema_engine)

# CrossPageSampler tests (Phase 4 - Slice C)
add_executable(test_cross_page_sampler tools/cross_page_sampler_test.cpp)
target_link_libraries(test_cross_page_sampler PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_cross_page_sampler)

# SchemaTemplateInference tests (Phase 4 - Slice C)
add_executable(test_schema_template_inference tools/schema_template_inference_test.cpp)
target_link_libraries(test_schema_template_inference PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_schema_template_inference)

# InteractiveBuilder tests (Phase 4 - Slice D)
add_executable(test_interactive_builder inference/interactive_builder_test.cpp)
target_link_libraries(test_interactive_builder PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_interactive_builder)

# Inference benchmark tests (Phase 4 - Slice E)
add_executable(test_inference_benchmark inference/inference_benchmark_test.cpp)
target_link_libraries(test_inference_benchmark PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
target_compile_definitions(test_inference_benchmark PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
gtest_discover_tests(test_inference_benchmark)

# DDLChangeChainDetector tests (Phase 4 - Slice B)
add_executable(test_ddl_change_detector inference/ddl_change_detector_test.cpp)
target_link_libraries(test_ddl_change_detector PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_ddl_change_detector)

# ==================== P0 Regression Tests (ASan/UBSan) ====================
# P0 regression tests for all 7 recently applied fixes
add_executable(test_p0_regression p0_regression_test.cpp)
target_link_libraries(test_p0_regression PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_p0_regression)

# ==================== P1 Format Correctness Fuzz Tests ====================

# P1 Salvage Engine Fuzz Test
add_executable(test_p1_salvage_fuzz p1_salvage_fuzz_test.cpp)
target_link_libraries(test_p1_salvage_fuzz PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_p1_salvage_fuzz)

# P1 Physical Layout Analyzer Fuzz Test
add_executable(test_p1_layout_fuzz p1_layout_fuzz_test.cpp)
target_link_libraries(test_p1_layout_fuzz PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_p1_layout_fuzz)

# P1 Statistical Type Classifier Fuzz Test
add_executable(test_p1_type_classifier_fuzz p1_type_classifier_fuzz_test.cpp)
target_link_libraries(test_p1_type_classifier_fuzz PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_p1_type_classifier_fuzz)

# PartitionTableRecovery P1 state semantics
add_executable(test_partition_table_recovery partition_table_recovery_test.cpp)
target_link_libraries(test_partition_table_recovery PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_partition_table_recovery)

# SharedTablespaceRecovery Phase 5e streaming scan, manifest, checksum, zero-page ban
add_executable(test_shared_tablespace_recovery shared_tablespace_recovery_test.cpp)
target_link_libraries(test_shared_tablespace_recovery PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_shared_tablespace_recovery)

# Phase 5f: Redo log plan gate — inventory, patch-plan, pre-conditions, fail-closed
add_executable(test_redo_plan_gate redo_plan_gate_test.cpp)
target_link_libraries(test_redo_plan_gate PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_redo_plan_gate)

# Phase 4: Parallel export equivalence (serial vs parallel produce same output)
add_executable(test_parallel_export_equivalence parallel_export_equivalence_test.cpp)
target_link_libraries(test_parallel_export_equivalence PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_parallel_export_equivalence)

	# Compressed table recovery metadata tests
add_executable(test_compressed_table_recovery innodb/compressed_table_recovery_test.cpp)
target_link_libraries(test_compressed_table_recovery PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_compressed_table_recovery)

# Compressed table fixture tests (requires generated .ibd files)
add_executable(test_compressed_table_fixture compressed_table_fixture_test.cpp)
target_link_libraries(test_compressed_table_fixture PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
add_test(NAME test_compressed_table_fixture COMMAND test_compressed_table_fixture)

# Compressed table routing tests (fail-closed classification)
add_executable(test_compressed_table_routing innodb/compressed_table_routing_test.cpp)
target_link_libraries(test_compressed_table_routing PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_compressed_table_routing)

# Exporter canonical value and escaping tests
add_executable(test_export_optimization test_export_optimization.cpp)
target_link_libraries(test_export_optimization PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_export_optimization DISCOVERY_TIMEOUT 30)

# Phase 3f/g — combined pipeline, salvage accuracy, L3/L4 isolation, export rejection, idempotency
add_executable(test_combined_recovery test_combined_recovery.cpp)
target_link_libraries(test_combined_recovery PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_combined_recovery)

add_executable(test_salvage_accuracy test_salvage_accuracy.cpp)
target_link_libraries(test_salvage_accuracy PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_salvage_accuracy)

# FD adoption preserves capability-bound raw reads without pathname reopening.
add_executable(test_fd_adoption fd_adoption_test.cpp)
target_link_libraries(test_fd_adoption PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_fd_adoption PROPERTIES ENVIRONMENT "DBREC_TEST_TMPDIR=${DBREC_TEST_TMPDIR_VALUE}")

# Mount admission resolves descriptor identities, not mutable pathname aliases.
add_executable(test_mount_topology mount_topology_test.cpp)
target_link_libraries(test_mount_topology PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_mount_topology PROPERTIES ENVIRONMENT "DBREC_TEST_TMPDIR=${DBREC_TEST_TMPDIR_VALUE}")

add_executable(test_file_mapper_safety file_mapper_safety_test.cpp)
target_link_libraries(test_file_mapper_safety PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_file_mapper_safety PROPERTIES ENVIRONMENT "DBREC_TEST_TMPDIR=${DBREC_TEST_TMPDIR_VALUE}")

add_executable(test_l3_l4_isolation test_l3_l4_isolation.cpp)
target_link_libraries(test_l3_l4_isolation PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_l3_l4_isolation PROPERTIES ENVIRONMENT "DBREC_TEST_TMPDIR=${DBREC_TEST_TMPDIR_VALUE}")

add_executable(test_export_rejection test_export_rejection.cpp)
target_link_libraries(test_export_rejection PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_export_rejection PROPERTIES ENVIRONMENT "DBREC_TEST_TMPDIR=${DBREC_TEST_TMPDIR_VALUE}")

add_executable(test_drop_table_authorization drop_table_authorization_test.cpp)
target_link_libraries(test_drop_table_authorization PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_drop_table_authorization PROPERTIES ENVIRONMENT "DBREC_TEST_TMPDIR=${DBREC_TEST_TMPDIR_VALUE}")

# A page that straddles a 1 MiB scan chunk must still be recovered.
add_executable(test_signature_scraper_boundary signature_scraper_boundary_test.cpp)
target_link_libraries(test_signature_scraper_boundary PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_signature_scraper_boundary PROPERTIES ENVIRONMENT "DBREC_TEST_TMPDIR=${DBREC_TEST_TMPDIR_VALUE}")

add_executable(test_recovery_idempotency test_recovery_idempotency.cpp)
target_link_libraries(test_recovery_idempotency PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_recovery_idempotency)

# ==================== Real Fixture Gating (Phase C2) ====================
# Real-fixture tests validate against .ibd/undo files produced by real MySQL
# Podman containers under /app/mysql_recovery_test/fixtures. They are labelled
# REAL_MYSQL so operators can select them with `ctest -L REAL_MYSQL` or
# exclude them with `ctest -LE REAL_MYSQL`. The executable gate is fixture
# availability: each test calls GTEST_SKIP() when its fixture path is absent,
# so a missing fixture is reported as skipped, never a PASS and never a FAIL.
# DBREC_REAL_FIXTURES is documentation only; no test reads that variable.
#
#   ctest --test-dir build --output-on-failure -LE REAL_MYSQL         # exclude
#   ctest --test-dir build -L REAL_MYSQL                              # fixtures

# test_compressed_table_fixture loads real compressed .ibd files from
# /app/mysql_recovery_test/fixtures/compressed and GTEST_SKIP()s when the
# directory is absent (compressed_table_fixture_test.cpp SetUp()).
set_tests_properties(test_compressed_table_fixture PROPERTIES LABELS "REAL_MYSQL")

# real_fixture_test validates SDI version detection, page structure, and
# manifest integrity against real MySQL .ibd/undo files.
add_executable(test_real_fixture real_fixture_test.cpp)
target_link_libraries(test_real_fixture PRIVATE dbrecovery_innodb GTest::GTest GTest::Main nlohmann_json::nlohmann_json)
add_test(NAME test_real_fixture COMMAND test_real_fixture)
set_tests_properties(test_real_fixture PROPERTIES LABELS "REAL_MYSQL")

# real_recovery_pipeline_test validates partition, shared tablespace, secondary
# index, DDL change detector, and redo log recovery against real MySQL fixtures.
add_executable(test_real_recovery_pipeline real_recovery_pipeline_test.cpp)
target_link_libraries(test_real_recovery_pipeline PRIVATE dbrecovery_innodb GTest::GTest GTest::Main nlohmann_json::nlohmann_json)
add_test(NAME test_real_recovery_pipeline COMMAND test_real_recovery_pipeline)
set_tests_properties(test_real_recovery_pipeline PROPERTIES LABELS "REAL_MYSQL")

# real_fixture_multiversion_test validates SHA-256 integrity, version-family page
# markers and SDI expectations, ground-truth metadata parity, partition filenames,
# and undo-fixture presence across all five real fixture versions (5.6–9.0).
# It does not run end-to-end row, undo, or flashback recovery.
add_executable(test_real_fixture_multiversion real_fixture_multiversion_test.cpp)
target_link_libraries(test_real_fixture_multiversion PRIVATE dbrecovery_innodb GTest::GTest GTest::Main nlohmann_json::nlohmann_json)
add_test(NAME test_real_fixture_multiversion COMMAND test_real_fixture_multiversion)
set_tests_properties(test_real_fixture_multiversion PROPERTIES LABELS "REAL_MYSQL")

# Recovery report & UnifiedValidator tests (P1-3)
add_executable(test_recovery_report test_recovery_report.cpp)
target_link_libraries(test_recovery_report PRIVATE dbrecovery_innodb GTest::GTest GTest::Main nlohmann_json::nlohmann_json)
gtest_discover_tests(test_recovery_report PROPERTIES ENVIRONMENT "DBREC_TEST_TMPDIR=${DBREC_TEST_TMPDIR_VALUE}")

add_executable(test_recovery_cli_report test_recovery_cli_report.cpp)
target_link_libraries(test_recovery_cli_report PRIVATE GTest::GTest GTest::Main)
target_compile_definitions(test_recovery_cli_report PRIVATE DBRECOVERY_CLI_PATH="$<TARGET_FILE:mysql_recovery_pro>")
add_dependencies(test_recovery_cli_report mysql_recovery_pro)
gtest_discover_tests(test_recovery_cli_report PROPERTIES ENVIRONMENT "DBREC_TEST_TMPDIR=${DBREC_TEST_TMPDIR_VALUE}")



# Schema type conversion boundary tests
add_executable(test_schema_type_conversion catalog/schema_type_conversion_test.cpp)
target_link_libraries(test_schema_type_conversion PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_schema_type_conversion)

# Bounded binary JSON decoder tests
add_executable(test_index_page_parser_json innodb/index_page_parser_json_test.cpp)
target_link_libraries(test_index_page_parser_json PRIVATE dbrecovery_innodb GTest::GTest GTest::Main)
gtest_discover_tests(test_index_page_parser_json)
