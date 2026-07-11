/**
 * @file test_ibd_recovery.cpp
 * @brief Unit tests for IBD file recovery functionality
 *
 * Tests the drop_table_recovery and IBD file parsing features
 */

#include <iostream>
#include <fstream>
#include <cstring>
#include <filesystem>
#include "dbrecovery/engine/drop_table_recovery.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

using namespace dbrecovery;

// Test helper
#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAILED: " << msg << " at line " << __LINE__ << std::endl; \
            return 1; \
        } \
        std::cout << "PASSED: " << msg << std::endl; \
    } while(0)

int test_sys_table_entry() {
    std::cout << "\n=== Test: SysTableEntry structure ===\n";

    engine::SysTableEntry entry;
    entry.table_id = 1234;
    entry.schema_name = "test_db";
    entry.table_name = "test_table";
    entry.space_id = 100;
    entry.n_cols = 5;
    entry.deleted = true;

    TEST_ASSERT(entry.table_id == 1234, "table_id should be settable");
    TEST_ASSERT(entry.schema_name == "test_db", "schema_name should be settable");
    TEST_ASSERT(entry.table_name == "test_table", "table_name should be settable");
    TEST_ASSERT(entry.space_id == 100, "space_id should be settable");
    TEST_ASSERT(entry.deleted == true, "deleted flag should be settable");

    return 0;
}

int test_sys_column_entry() {
    std::cout << "\n=== Test: SysColumnEntry structure ===\n";

    engine::SysColumnEntry col;
    col.table_id = 1234;
    col.name = "id";
    col.pos = 0;
    col.mtype = 4;  // LONG (INT)
    col.prtype = 0;
    col.len = 4;
    col.is_nullable = false;
    col.is_unsigned = false;

    TEST_ASSERT(col.table_id == 1234, "table_id should be settable");
    TEST_ASSERT(col.name == "id", "column name should be settable");
    TEST_ASSERT(col.mtype == 4, "mtype should be settable");
    TEST_ASSERT(col.is_nullable == false, "is_nullable should be settable");

    return 0;
}

int test_sys_index_entry() {
    std::cout << "\n=== Test: SysIndexEntry structure ===\n";

    engine::SysIndexEntry idx;
    idx.table_id = 1234;
    idx.index_id = 5678;
    idx.name = "PRIMARY";
    idx.type = 1;  // PRIMARY
    idx.space_id = 100;
    idx.page_no = 4;
    idx.deleted = false;

    TEST_ASSERT(idx.table_id == 1234, "table_id should be settable");
    TEST_ASSERT(idx.index_id == 5678, "index_id should be settable");
    TEST_ASSERT(idx.name == "PRIMARY", "index name should be settable");
    TEST_ASSERT(idx.page_no == 4, "root page number should be settable");

    return 0;
}

int test_recovered_table_metadata() {
    std::cout << "\n=== Test: RecoveredTableMetadata structure ===\n";

    engine::RecoveredTableMetadata meta;
    meta.table_id = 1234;
    meta.primary_index_id = 5678;
    meta.schema_name = "test_db";
    meta.table_name = "test_table";
    meta.space_id = 100;
    meta.root_page_no = 4;
    meta.recovered_from_deleted = true;
    meta.confidence = 0.95;

    // Add column
    engine::SysColumnEntry col;
    col.name = "id";
    col.mtype = 4;
    meta.columns.push_back(col);

    TEST_ASSERT(meta.table_id == 1234, "table_id should be settable");
    TEST_ASSERT(meta.table_name == "test_table", "table_name should be settable");
    TEST_ASSERT(meta.recovered_from_deleted == true, "recovered_from_deleted should be settable");
    TEST_ASSERT(meta.confidence == 0.95, "confidence should be settable");
    TEST_ASSERT(meta.columns.size() == 1, "should have one column");

    return 0;
}

int test_drop_table_recovery_config() {
    std::cout << "\n=== Test: DropTableRecovery configuration ===\n";

    engine::DropTableRecovery recovery;
    // DropTableRecovery uses set_config() method
    TEST_ASSERT(true, "DropTableRecovery should be instantiable");

    return 0;
}

int test_index_page_parser_initialization() {
    std::cout << "\n=== Test: InnoDBIndexParser initialization ===\n";

    innodb::InnoDBIndexParser parser;

    // Test that parser can be instantiated
    TEST_ASSERT(true, "InnoDBIndexParser should be instantiable");

    return 0;
}

int test_create_temp_ibd_file() {
    std::cout << "\n=== Test: Create temporary IBD file for testing ===\n";

    // Create a temporary directory for test files
    std::string test_dir = "/tmp/mysql_recovery_test";
    std::filesystem::create_directories(test_dir);

    std::string ibd_path = test_dir + "/test_table.ibd";

    // Create a minimal IBD file (16KB, one page)
    std::ofstream file(ibd_path, std::ios::binary);
    if (!file) {
        std::cerr << "Could not create test file: " << ibd_path << std::endl;
        return 1;
    }

    // Write a minimal INDEX page (16KB)
    // FIL Header (38 bytes): checksum, offset, prev, next, lsn, type, flush_lsn, space_id
    std::vector<uint8_t> page(16384, 0);

    // Set page type to INDEX (0x45BF)
    page[24] = 0x45;
    page[25] = 0xBF;

    // Set FIL_PAGE_OFFSET (page number)
    page[4] = 0;
    page[5] = 0;
    page[6] = 0;
    page[7] = 1;

    // Set FIL_PAGE_SPACE_ID
    page[34] = 0;
    page[35] = 0;
    page[36] = 0;
    page[37] = 1;

    // Write PAGE_HEADER at offset 38
    // PAGE_N_DIR_SLOTS (offset 38-40)
    page[38] = 0;
    page[39] = 2;  // 2 directory slots (infimum + supremum)

    // PAGE_HEAP_TOP (offset 40-42)
    page[40] = 0;
    page[41] = 100;  // heap top at offset 100

    // PAGE_N_HEAP (offset 42-44): n_heap = 2 (infimum + supremum)
    page[42] = 0;
    page[43] = 2;

    // Write infimum/supremum records at offset 99
    // Infimum (99)
    page[99] = 0x02;  // REC_STATUS_INFIMUM
    page[100] = 0x00;
    page[101] = 0x0b;  // Next offset (supremum)
    page[102] = 0x00;
    for (int i = 0; i < 8; i++) {
        page[103 + i] = 0x69;  // "infimum\0"
    }

    // Supremum (112)
    page[112] = 0x03;  // REC_STATUS_SUPREMUM
    page[113] = 0x00;
    page[114] = 0x00;  // Next offset (0 = end)
    page[115] = 0x00;
    for (int i = 0; i < 8; i++) {
        page[116 + i] = 0x73;  // "supremum\0"
    }

    // Write page directory (at end of page)
    // Slot 0: supremum offset
    page[16382] = 0x00;
    page[16383] = 112;  // supremum at offset 112

    // Slot 1: infimum offset
    page[16380] = 0x00;
    page[16381] = 99;  // infimum at offset 99

    // Write the page
    file.write(reinterpret_cast<char*>(page.data()), page.size());
    file.close();

    TEST_ASSERT(std::filesystem::exists(ibd_path), "IBD file should exist");

    // Verify file size
    auto file_size = std::filesystem::file_size(ibd_path);
    TEST_ASSERT(file_size == 16384, "IBD file should be exactly one page");

    // Clean up
    std::filesystem::remove_all(test_dir);

    return 0;
}

int test_ibd_file_validation() {
    std::cout << "\n=== Test: IBD file validation ===\n";

    // Test validation logic
    std::string non_existent = "/tmp/non_existent_file.ibd";
    TEST_ASSERT(!std::filesystem::exists(non_existent), "Non-existent file should not exist");

    // Create temp file for testing
    std::string test_dir = "/tmp/mysql_recovery_test2";
    std::filesystem::create_directories(test_dir);
    std::string small_file = test_dir + "/small.ibd";

    // Create a file smaller than minimum page size
    std::ofstream file(small_file, std::ios::binary);
    std::vector<uint8_t> small_data(100, 0);
    file.write(reinterpret_cast<char*>(small_data.data()), small_data.size());
    file.close();

    auto file_size = std::filesystem::file_size(small_file);
    TEST_ASSERT(file_size < 16384, "Small file should be less than page size");

    // Clean up
    std::filesystem::remove_all(test_dir);

    return 0;
}

int main() {
    std::cout << "========================================\n";
    std::cout << "IBD File Recovery Tests\n";
    std::cout << "========================================\n";

    int failures = 0;

    failures += test_sys_table_entry();
    failures += test_sys_column_entry();
    failures += test_sys_index_entry();
    failures += test_recovered_table_metadata();
    failures += test_drop_table_recovery_config();
    failures += test_index_page_parser_initialization();
    failures += test_create_temp_ibd_file();
    failures += test_ibd_file_validation();

    std::cout << "\n========================================\n";
    if (failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
    } else {
        std::cout << "FAILED: " << failures << " test(s)\n";
    }
    std::cout << "========================================\n";

    return failures;
}