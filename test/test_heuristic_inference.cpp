/**
 * @file test_heuristic_inference.cpp
 * @brief Test Heuristic metadata inference module
 */

#include <iostream>
#include <fstream>
#include <cstring>
#include <cassert>
#include <vector>
#include "dbrecovery/tools/heuristic_inference.hpp"

// 测试工具函数
static std::vector<uint8_t> create_test_page(uint32_t page_no, mysql_recovery::PageType type) {
    std::vector<uint8_t> page(16384, 0);

    // FIL_HEADER (offset 0-37)
    // FIL_PAGE_PREV (0-3)
    std::memcpy(page.data() + 0, "\xFF\xFF\xFF\xFF", 4);
    // FIL_PAGE_NEXT (4-7)
    std::memcpy(page.data() + 4, "\xFF\xFF\xFF\xFF", 4);
    // FIL_PAGE_LSN (8-15)
    std::memcpy(page.data() + 8, "\x01\x00\x00\x00\x00\x00\x00\x00", 8);
    // FIL_PAGE_TYPE (16-17)
    uint16_t type_val = static_cast<uint16_t>(type);
    std::memcpy(page.data() + 16, &type_val, 2);
    // FIL_PAGE_SPACE_ID (18-21)
    std::memcpy(page.data() + 18, "\x01\x00\x00\x00", 4);
    // FIL_PAGE_SPACE_OFFSET (22-23)
    std::memcpy(page.data() + 22, "\x00\x00", 2);
    // FIL_PAGE_FILE_FLUSH_LSN (24-31)
    std::memcpy(page.data() + 24, "\x00\x00\x00\x00\x00\x00\x00\x00", 8);

    // PAGE_HEADER (offset 38-53)
    // PAGE_N_DIR_SLOTS (38-39)
    std::memcpy(page.data() + 38, "\x02\x00", 2);
    // PAGE_HEAP_TOP (40-41)
    std::memcpy(page.data() + 40, "\x36\x00", 2);  // 54
    // PAGE_N_HEAP (42-43)
    std::memcpy(page.data() + 42, "\x02\x00", 2);
    // PAGE_FREE (44-45)
    std::memcpy(page.data() + 44, "\x00\x00", 2);
    // PAGE_GARBAGE_END (46-47)
    std::memcpy(page.data() + 46, "\x00\x00", 2);
    // PAGE_LAST_INSERT (48-49)
    std::memcpy(page.data() + 48, "\x00\x00", 2);
    // PAGE_DIRECTION (50-51)
    std::memcpy(page.data() + 50, "\x00\x00", 2);
    // PAGE_N_DIRECT_PAGES (52-53)
    std::memcpy(page.data() + 52, "\x00\x00", 2);

    // BTR_HEADER (offset 38-49)
    // n_levels (38-41)
    std::memcpy(page.data() + 38, "\x01\x00\x00\x00", 4);
    // index_id (42-49)
    std::memcpy(page.data() + 42, "\xF0\xDE\xBC\x9A\x78\x56\x34\x12", 8);

    // Add some record data at offset 96
    // Record 1: INFIMUM (system record)
    std::memcpy(page.data() + 96, "\x00\x00\x00\x00\x00", 5);
    std::memcpy(page.data() + 96 + 5, "infimum", 7);

    // Record 2: SUPREMUM (system record)
    std::memcpy(page.data() + 112, "\x00\x00\x00\x00\x00", 5);
    std::memcpy(page.data() + 112 + 5, "supremum", 8);

    // Record 3: User record
    std::memcpy(page.data() + 128, "\x00\x00\x00\x00\x00", 5);
    std::memcpy(page.data() + 128 + 5, "test_data", 9);

    return page;
}

// P5-01: Column name inference
bool test_column_name_inference() {
    std::cout << "[TEST] P5-01: Column name inference...\n";

    dbrecovery::tools::HeuristicInference inference;

    // Test single letter names
    assert(inference.infer_column_name(0) == "A");
    assert(inference.infer_column_name(1) == "B");
    assert(inference.infer_column_name(25) == "Z");

    // Test two letter names
    assert(inference.infer_column_name(26) == "AA");
    assert(inference.infer_column_name(27) == "AB");
    assert(inference.infer_column_name(51) == "AZ");
    assert(inference.infer_column_name(52) == "BA");

    // Test numeric fallback
    assert(inference.infer_column_name(100) == "COL_100");

    std::cout << "    [PASS] Column name inference\n";
    return true;
}

// P5-02: Column type inference
bool test_column_type_inference() {
    std::cout << "[TEST] P5-02: Column type inference...\n";

    dbrecovery::tools::HeuristicInference inference;

    // Test various type codes
    std::vector<uint8_t> col_meta = {1, 2, 4, 8, 10, 12, 15, 252, 251};

    assert(inference.infer_column_type(1, col_meta, 0) == "TINYINT");
    assert(inference.infer_column_type(2, col_meta, 1) == "SMALLINT");
    assert(inference.infer_column_type(4, col_meta, 2) == "INT");
    assert(inference.infer_column_type(8, col_meta, 3) == "BIGINT");
    assert(inference.infer_column_type(10, col_meta, 4) == "DATE");
    assert(inference.infer_column_type(12, col_meta, 5) == "DATETIME");
    assert(inference.infer_column_type(15, col_meta, 6) == "VARCHAR(255)");
    assert(inference.infer_column_type(252, col_meta, 7) == "TEXT");
    assert(inference.infer_column_type(251, col_meta, 8) == "JSON");

    // Test fallback types
    assert(inference.infer_column_type(0, col_meta, 0) == "INT");
    assert(inference.infer_column_type(5, col_meta, 0) == "DOUBLE");
    assert(inference.infer_column_type(240, col_meta, 0) == "BLOB");

    std::cout << "    [PASS] Column type inference\n";
    return true;
}

// P5-03: Row format inference
bool test_row_format_inference() {
    std::cout << "[TEST] P5-03: Row format inference...\n";

    // Create a 5.7 style page
    auto page57 = create_test_page(0, mysql_recovery::PageType::INDEX);
    // Clear header_version (5.7 has 0)
    std::memset(page57.data() + 54, 0, 2);

    auto format57 = dbrecovery::tools::HeuristicInference::infer_row_format(page57.data(), 16384);
    assert(format57 == mysql_recovery::RowFormat::COMPACT);

    // Create an 8.0 style page
    auto page80 = create_test_page(0, mysql_recovery::PageType::INDEX);
    // Set header_version (8.0 has > 0)
    uint16_t version = 1;
    std::memcpy(page80.data() + 54, &version, 2);

    auto format80 = dbrecovery::tools::HeuristicInference::infer_row_format(page80.data(), 16384);
    assert(format80 == mysql_recovery::RowFormat::DYNAMIC);

    std::cout << "    [PASS] Row format inference\n";
    return true;
}

// P5-04: MySQL version inference
bool test_version_inference() {
    std::cout << "[TEST] P5-04: MySQL version inference...\n";

    // Create a 5.7 style page
    auto page57 = create_test_page(0, mysql_recovery::PageType::INDEX);
    std::memset(page57.data() + 54, 0, 2);  // Clear header_version

    auto version57 = dbrecovery::tools::HeuristicInference::infer_version(page57.data(), 16384);
    assert(version57 == mysql_recovery::MySQLVersion::MYSQL_57);

    // Create an 8.0 style page
    auto page80 = create_test_page(0, mysql_recovery::PageType::INDEX);
    uint16_t version = 1;
    std::memcpy(page80.data() + 54, &version, 2);  // Set header_version

    auto version80 = dbrecovery::tools::HeuristicInference::infer_version(page80.data(), 16384);
    assert(version80 == mysql_recovery::MySQLVersion::MYSQL_80);

    std::cout << "    [PASS] MySQL version inference\n";
    return true;
}

// P5-05: Row count estimation
bool test_row_count_estimation() {
    std::cout << "[TEST] P5-05: Row count estimation...\n";

    uint64_t count1 = dbrecovery::tools::HeuristicInference::estimate_row_count(100, 100);
    assert(count1 > 0);

    uint64_t count2 = dbrecovery::tools::HeuristicInference::estimate_row_count(1000, 50);
    assert(count2 > count1);

    uint64_t count3 = dbrecovery::tools::HeuristicInference::estimate_row_count(0, 100);
    assert(count3 == 0);

    uint64_t count4 = dbrecovery::tools::HeuristicInference::estimate_row_count(100, 0);
    assert(count4 == 0);

    std::cout << "    Estimated rows (100 pages, 100 bytes/row): " << count1 << "\n";
    std::cout << "    Estimated rows (1000 pages, 50 bytes/row): " << count2 << "\n";
    std::cout << "    [PASS] Row count estimation\n";
    return true;
}

// P5-06: CREATE statement generation
bool test_create_statement_generation() {
    std::cout << "[TEST] P5-06: CREATE statement generation...\n";

    dbrecovery::tools::HeuristicInference inference;

    // Create a test schema
    dbrecovery::tools::HeuristicInference::TableSchema schema;
    schema.table_name = "test_table";
    schema.row_format = mysql_recovery::RowFormat::COMPACT;
    schema.version = mysql_recovery::MySQLVersion::MYSQL_57;

    // Add some columns
    dbrecovery::tools::HeuristicInference::ColumnInfo col1;
    col1.name = "id";
    col1.type = "BIGINT";
    col1.is_nullable = false;
    col1.is_primary_key = true;
    col1.ordinal_position = 0;
    schema.columns.push_back(col1);

    dbrecovery::tools::HeuristicInference::ColumnInfo col2;
    col2.name = "name";
    col2.type = "VARCHAR(255)";
    col2.is_nullable = true;
    col2.ordinal_position = 1;
    schema.columns.push_back(col2);

    dbrecovery::tools::HeuristicInference::ColumnInfo col3;
    col3.name = "created_at";
    col3.type = "DATETIME";
    col3.is_nullable = true;
    col3.ordinal_position = 2;
    schema.columns.push_back(col3);

    std::string ddl = dbrecovery::tools::HeuristicInference::generate_create_statement(schema);

    // Verify DDL contains expected elements
    assert(ddl.find("CREATE TABLE") != std::string::npos);
    assert(ddl.find("test_table") != std::string::npos);
    assert(ddl.find("`id`") != std::string::npos);
    assert(ddl.find("`name`") != std::string::npos);
    assert(ddl.find("`created_at`") != std::string::npos);
    assert(ddl.find("BIGINT") != std::string::npos);
    assert(ddl.find("VARCHAR") != std::string::npos);
    assert(ddl.find("DATETIME") != std::string::npos);
    assert(ddl.find("PRIMARY KEY") != std::string::npos);
    assert(ddl.find("ENGINE=InnoDB") != std::string::npos);

    std::cout << "    Generated DDL:\n" << ddl << "\n";
    std::cout << "    [PASS] CREATE statement generation\n";
    return true;
}

// P5-07: Simple type inferer
bool test_simple_type_inferer() {
    std::cout << "[TEST] P5-07: Simple type inferer...\n";

    using SimpleTypeInferer = dbrecovery::tools::SimpleTypeInferer;

    // Test integer detection
    assert(SimpleTypeInferer::is_integer("123") == true);
    assert(SimpleTypeInferer::is_integer("-456") == true);
    assert(SimpleTypeInferer::is_integer("0") == true);
    assert(SimpleTypeInferer::is_integer("12.3") == false);
    assert(SimpleTypeInferer::is_integer("abc") == false);

    // Test float detection
    assert(SimpleTypeInferer::is_float("12.3") == true);
    assert(SimpleTypeInferer::is_float("-45.67") == true);
    assert(SimpleTypeInferer::is_float("1.23e10") == true);
    assert(SimpleTypeInferer::is_float("123") == false);

    // Test date detection
    assert(SimpleTypeInferer::is_date("2024-01-15") == true);
    assert(SimpleTypeInferer::is_date("2024-01-15 10:30:45") == true);
    assert(SimpleTypeInferer::is_date("01-15-2024") == false);  // Wrong format

    // Test string detection
    assert(SimpleTypeInferer::is_string("hello") == true);
    assert(SimpleTypeInferer::is_string("123abc") == true);

    // Test type inference from values
    assert(SimpleTypeInferer::infer_type_from_value("123") == "BIGINT");
    assert(SimpleTypeInferer::infer_type_from_value("12.3") == "DOUBLE");
    assert(SimpleTypeInferer::infer_type_from_value("2024-01-15") == "DATETIME");
    assert(SimpleTypeInferer::infer_type_from_value("hello") == "VARCHAR");

    // Test type priorities
    assert(SimpleTypeInferer::get_type_priority("BIGINT") > SimpleTypeInferer::get_type_priority("VARCHAR"));
    assert(SimpleTypeInferer::get_type_priority("INT") > SimpleTypeInferer::get_type_priority("TEXT"));

    std::cout << "    [PASS] Simple type inferer\n";
    return true;
}

// P5-08: Schema inference with test page
bool test_schema_inference() {
    std::cout << "[TEST] P5-08: Schema inference with test page...\n";

    auto page = create_test_page(0, mysql_recovery::PageType::INDEX);

    dbrecovery::tools::HeuristicInference inference;
    inference.set_page_data(page.data(), page.size());

    auto schema = inference.infer_schema();

    assert(schema.has_value());
    assert(!schema->columns.empty());
    assert(schema->version != mysql_recovery::MySQLVersion::UNKNOWN);
    assert(!schema->create_ddl.empty());

    std::cout << "    Inferred version: " << static_cast<int>(schema->version) << "\n";
    std::cout << "    Inferred row format: " << static_cast<int>(schema->row_format) << "\n";
    std::cout << "    Number of columns: " << schema->columns.size() << "\n";
    std::cout << "    Estimated row count: " << schema->estimated_row_count << "\n";
    std::cout << "    [PASS] Schema inference\n";
    return true;
}

// ==================== 主函数 ====================

int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "MySQL Recovery Pro - Heuristic Inference Tests\n";
    std::cout << "========================================\n\n";

    int passed = 0;
    int failed = 0;

    if (test_column_name_inference()) passed++; else failed++;
    if (test_column_type_inference()) passed++; else failed++;
    if (test_row_format_inference()) passed++; else failed++;
    if (test_version_inference()) passed++; else failed++;
    if (test_row_count_estimation()) passed++; else failed++;
    if (test_create_statement_generation()) passed++; else failed++;
    if (test_simple_type_inferer()) passed++; else failed++;
    if (test_schema_inference()) passed++; else failed++;

    std::cout << "\n========================================\n";
    std::cout << "Test Results: " << passed << " passed, " << failed << " failed\n";
    std::cout << "========================================\n";

    return failed > 0 ? 1 : 0;
}
