/**
 * @file test_instant_ddl.cpp
 * @brief Unit tests for instant ADD COLUMN feature (MySQL 8.0.23+)
 *
 * Tests the version_added/version_dropped column filtering logic
 * that enables recovery from tables with instant DDL modifications.
 */

#include <iostream>
#include <cassert>
#include <cstring>
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/engine/schema_template.hpp"
#include <nlohmann/json.hpp>

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

int test_column_version_fields() {
    std::cout << "\n=== Test: Column version fields ===\n";

    // Test default values
    catalog::ColumnInfo col;
    TEST_ASSERT(col.version_added == 0, "Default version_added should be 0");
    TEST_ASSERT(col.version_dropped == 66, "Default version_dropped should be 66 (not dropped)");

    // Test setting version fields
    col.version_added = 1;
    col.version_dropped = 5;
    TEST_ASSERT(col.version_added == 1, "version_added should be settable");
    TEST_ASSERT(col.version_dropped == 5, "version_dropped should be settable");

    return 0;
}

int test_table_schema_row_version() {
    std::cout << "\n=== Test: TableSchema row version support ===\n";

    catalog::TableSchema schema;
    schema.table_name = "test_table";
    schema.max_row_version = 3;
    schema.mysql_version_id = 80029;

    TEST_ASSERT(schema.max_row_version == 3, "max_row_version should be settable");
    TEST_ASSERT(schema.mysql_version_id == 80029, "mysql_version_id should be settable");

    return 0;
}

int test_get_active_columns() {
    std::cout << "\n=== Test: get_active_columns filtering ===\n";

    catalog::TableSchema schema;
    schema.table_name = "versioned_table";

    // Column 1: exists from version 0, never dropped
    catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = 4; // LONG (INT)
    col1.version_added = 0;
    col1.version_dropped = 66;
    col1.physical_pos = 0;
    col1.is_nullable = false;
    schema.columns[1] = col1;

    // Column 2: added at version 1, never dropped
    catalog::ColumnInfo col2;
    col2.name = "name";
    col2.mysql_type_id = 16; // VARCHAR
    col2.version_added = 1;
    col2.version_dropped = 66;
    col2.physical_pos = 1;
    col2.is_nullable = true;
    schema.columns[2] = col2;

    // Column 3: added at version 2, dropped at version 4
    catalog::ColumnInfo col3;
    col3.name = "temp_col";
    col3.mysql_type_id = 4;
    col3.version_added = 2;
    col3.version_dropped = 4;
    col3.physical_pos = 2;
    col3.is_nullable = true;
    schema.columns[3] = col3;

    // Column 4: added at version 3, never dropped
    catalog::ColumnInfo col4;
    col4.name = "status";
    col4.mysql_type_id = 2; // TINY
    col4.version_added = 3;
    col4.version_dropped = 66;
    col4.physical_pos = 3;
    col4.is_nullable = false;
    schema.columns[4] = col4;

    // Test version 0: only col1
    auto cols_v0 = schema.get_active_columns(0);
    TEST_ASSERT(cols_v0.size() == 1, "Version 0 should have 1 column (id)");
    TEST_ASSERT(cols_v0[0].name == "id", "Version 0 should have 'id' column");

    // Test version 1: col1 + col2
    auto cols_v1 = schema.get_active_columns(1);
    TEST_ASSERT(cols_v1.size() == 2, "Version 1 should have 2 columns");
    TEST_ASSERT(cols_v1[0].name == "id", "First column should be 'id'");
    TEST_ASSERT(cols_v1[1].name == "name", "Second column should be 'name'");

    // Test version 2: col1 + col2 + col3
    auto cols_v2 = schema.get_active_columns(2);
    TEST_ASSERT(cols_v2.size() == 3, "Version 2 should have 3 columns");

    // Test version 3: col1 + col2 + col3 + col4
    auto cols_v3 = schema.get_active_columns(3);
    TEST_ASSERT(cols_v3.size() == 4, "Version 3 should have 4 columns");

    // Test version 4: col1 + col2 + col4 (col3 dropped)
    auto cols_v4 = schema.get_active_columns(4);
    TEST_ASSERT(cols_v4.size() == 3, "Version 4 should have 3 columns (temp_col dropped)");

    // Test version 5: same as v4
    auto cols_v5 = schema.get_active_columns(5);
    TEST_ASSERT(cols_v5.size() == 3, "Version 5 should have 3 columns");

    return 0;
}

int test_get_nullable_count_for_version() {
    std::cout << "\n=== Test: get_nullable_count_for_version ===\n";

    catalog::TableSchema schema;
    schema.table_name = "nullable_test";

    // col1: not nullable, version 0
    catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = 4;
    col1.version_added = 0;
    col1.version_dropped = 66;
    col1.is_nullable = false;
    col1.physical_pos = 0;
    schema.columns[1] = col1;

    // col2: nullable, version 0
    catalog::ColumnInfo col2;
    col2.name = "data";
    col2.mysql_type_id = 16;
    col2.version_added = 0;
    col2.version_dropped = 66;
    col2.is_nullable = true;
    col2.physical_pos = 1;
    schema.columns[2] = col2;

    // col3: nullable, added at version 1
    catalog::ColumnInfo col3;
    col3.name = "extra";
    col3.mysql_type_id = 16;
    col3.version_added = 1;
    col3.version_dropped = 66;
    col3.is_nullable = true;
    col3.physical_pos = 2;
    schema.columns[3] = col3;

    TEST_ASSERT(schema.get_nullable_count_for_version(0) == 1, "Version 0 should have 1 nullable column");
    TEST_ASSERT(schema.get_nullable_count_for_version(1) == 2, "Version 1 should have 2 nullable columns");

    return 0;
}

int test_schema_template_version_support() {
    std::cout << "\n=== Test: SchemaTemplate version support ===\n";

    engine::SchemaTemplate tpl;
    tpl.table_name = "test_tpl";
    tpl.max_row_version = 2;
    tpl.mysql_version_id = 80030;

    TEST_ASSERT(tpl.max_row_version == 2, "SchemaTemplate max_row_version");
    TEST_ASSERT(tpl.mysql_version_id == 80030, "SchemaTemplate mysql_version_id");

    // Add columns with versions
    engine::SchemaTemplate::Column col1;
    col1.name = "id";
    col1.mysql_type = 4;
    col1.version_added = 0;
    col1.version_dropped = 66;
    col1.physical_pos = 0;
    tpl.columns.push_back(col1);

    engine::SchemaTemplate::Column col2;
    col2.name = "added_col";
    col2.mysql_type = 16;
    col2.version_added = 1;
    col2.version_dropped = 66;
    col2.physical_pos = 1;
    tpl.columns.push_back(col2);

    // Test get_active_columns
    auto active_v0 = tpl.get_active_columns(0);
    TEST_ASSERT(active_v0.size() == 1, "Template version 0 should have 1 column");

    auto active_v1 = tpl.get_active_columns(1);
    TEST_ASSERT(active_v1.size() == 2, "Template version 1 should have 2 columns");

    return 0;
}

int test_json_parsing_with_version_fields() {
    std::cout << "\n=== Test: JSON parsing with version fields ===\n";

    // Test JSON with se_private_data containing version info
    std::string json_str = R"({
        "table_name": "test_instant",
        "table_id": 1234,
        "mysql_version_id": 80029,
        "columns": [
            {
                "name": "id",
                "type": 4,
                "is_nullable": false,
                "physical_pos": 0,
                "ordinal_position": 1
            },
            {
                "name": "added_later",
                "type": 16,
                "is_nullable": true,
                "physical_pos": 1,
                "ordinal_position": 2,
                "se_private_data": "version_added=2;physical_pos=1"
            }
        ]
    })";

    catalog::TableSchema schema;
    bool result = schema.from_json_string(json_str);

    TEST_ASSERT(result, "JSON parsing should succeed");
    TEST_ASSERT(schema.mysql_version_id == 80029, "mysql_version_id should be parsed");

    // Find the "added_later" column and check version_added
    bool found_added_later = false;
    for (const auto& [idx, col] : schema.columns) {
        if (col.name == "added_later") {
            found_added_later = true;
            TEST_ASSERT(col.version_added == 2, "version_added should be parsed from se_private_data");
            TEST_ASSERT(col.version_dropped == 66, "version_dropped should default to 66");
            break;
        }
    }
    TEST_ASSERT(found_added_later, "Column 'added_later' should be found");

    return 0;
}

int test_ibd2sql_compatible_version_logic() {
    std::cout << "\n=== Test: ibd2sql compatible version logic ===\n";

    // Reference: ibd2sql table.py init_pkmr()
    // Logic: version_added <= row_version < version_dropped

    catalog::ColumnInfo col;
    col.name = "test_col";

    // Test case 1: column exists in versions 0-2 (added=0, dropped=3)
    col.version_added = 0;
    col.version_dropped = 3;

    // Should exist in versions 0, 1, 2
    TEST_ASSERT(0 >= col.version_added && 0 < col.version_dropped, "Version 0: column should exist");
    TEST_ASSERT(1 >= col.version_added && 1 < col.version_dropped, "Version 1: column should exist");
    TEST_ASSERT(2 >= col.version_added && 2 < col.version_dropped, "Version 2: column should exist");
    // Should not exist in version 3+
    TEST_ASSERT(!(3 >= col.version_added && 3 < col.version_dropped), "Version 3: column should not exist");

    // Test case 2: column added later (added=2, dropped=66)
    col.version_added = 2;
    col.version_dropped = 66;  // 66 means "not dropped" per ibd2sql

    TEST_ASSERT(!(0 >= col.version_added && 0 < col.version_dropped), "Version 0: column should not exist yet");
    TEST_ASSERT(!(1 >= col.version_added && 1 < col.version_dropped), "Version 1: column should not exist yet");
    TEST_ASSERT((2 >= col.version_added && 2 < col.version_dropped), "Version 2: column should exist");
    TEST_ASSERT((10 >= col.version_added && 10 < col.version_dropped), "Version 10: column should still exist");

    return 0;
}

int main() {
    std::cout << "========================================\n";
    std::cout << "Instant ADD COLUMN Feature Tests\n";
    std::cout << "========================================\n";

    int failures = 0;

    failures += test_column_version_fields();
    failures += test_table_schema_row_version();
    failures += test_get_active_columns();
    failures += test_get_nullable_count_for_version();
    failures += test_schema_template_version_support();
    failures += test_json_parsing_with_version_fields();
    failures += test_ibd2sql_compatible_version_logic();

    std::cout << "\n========================================\n";
    if (failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
    } else {
        std::cout << "FAILED: " << failures << " test(s)\n";
    }
    std::cout << "========================================\n";

    return failures;
}