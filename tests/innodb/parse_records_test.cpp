// Test cases for enhanced parse_records functionality
// Tests for P1-1: Complete parse_records() implementation

#include <gtest/gtest.h>
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== parse_records() Test Cases ====================

// Test Case 1: parse_records with valid COMPACT format page
TEST(ParseRecordsTest, ValidCompactFormat) {
    // Create a minimal valid page with COMPACT format
    std::vector<uint8_t> page_data(16384, 0);

    // Set page type to INDEX (9969 = 0x26f1)
    // FIL_PAGE_TYPE is at offset 16 (relative to page start)
    write_le<uint16_t>(page_data.data(), 16, 9969);  // 0x26f1 at offset 16

    // Set page level to 0 (leaf)
    write_le<uint16_t>(page_data.data(), 42, 0);

    // Set flags with COMPACT format (bit 0-2 = 1)
    write_le<uint16_t>(page_data.data(), 48, 0x0001);

    // Set heap_top (enough space for records)
    write_le<uint16_t>(page_data.data(), 40, 100);

    // Set n_dir_slots (at least 2 for INFIMUM/SUPREMUM)
    write_le<uint16_t>(page_data.data(), 38, 2);

    // Set INFIMUM record at offset 54
    // ✅ 修复：设置正确的 INFIMUM 记录头（info_bits = 0x02 = REC_STATUS_INFIMUM）
    page_data[54] = dbrecovery::innodb::REC_STATUS_INFIMUM;  // info_bits
    write_le<uint16_t>(page_data.data(), 55, 99);  // next_record_offset

    // Set SUPREMUM record at offset 99
    // ✅ 修复：设置正确的 SUPREMUM 记录头（info_bits = 0x03 = REC_STATUS_SUPREMUM）
    page_data[99] = dbrecovery::innodb::REC_STATUS_SUPREMUM;  // info_bits
    write_le<uint16_t>(page_data.data(), 100, 0xFFFF);  // next_record_offset = end

    // Set n_heap (4 records: INFIMUM, SUPREMUM, plus 2 user records)
    write_le<uint16_t>(page_data.data(), 42, 4);

    // Create table schema with 2 columns
    catalog::TableSchema schema;
    schema.table_name = "test_table";
    schema.row_format = 1;  // COMPACT

    catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = mysql_types::LONGLONG;
    col1.is_nullable = false;
    schema.columns[1] = col1;

    catalog::ColumnInfo col2;
    col2.name = "name";
    col2.mysql_type_id = mysql_types::VARCHAR;
    col2.column_length = 100;
    col2.is_nullable = true;
    schema.columns[2] = col2;

    // Parse the page
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    // Parse records
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records.size(), 0);  // No user records in this minimal page
}

// Test Case 2: parse_records with REDUNDANT format
TEST(ParseRecordsTest, ValidRedundantFormat) {
    std::vector<uint8_t> page_data(16384, 0);

    // Set page type to INDEX
    write_le<uint16_t>(page_data.data(), 16, 9969);

    // Set page level to 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // Set flags with REDUNDANT format (bit 0-2 = 0)
    write_le<uint16_t>(page_data.data(), 48, 0x0000);

    // Set heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // Set n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // Set INFIMUM record
    page_data[54] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 99);

    // Set SUPREMUM record
    page_data[99] = dbrecovery::innodb::REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 100, 0xFFFF);

    // Set n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // Create table schema
    catalog::TableSchema schema;
    schema.table_name = "test_table";
    schema.row_format = 0;  // REDUNDANT

    catalog::ColumnInfo col;
    col.name = "id";
    col.mysql_type_id = mysql_types::LONGLONG;
    col.is_nullable = false;
    schema.columns[1] = col;

    // Parse the page
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    // Parse records
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records.size(), 0);
}

// Test Case 3: parse_records with DYNAMIC format and BLOB column
TEST(ParseRecordsTest, ValidDynamicFormatWithBlob) {
    std::vector<uint8_t> page_data(16384, 0);

    // Set page type to INDEX
    write_le<uint16_t>(page_data.data(), 16, 9969);

    // Set page level to 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // Set flags with DYNAMIC format (bit 0-2 = 2)
    write_le<uint16_t>(page_data.data(), 48, 0x0002);

    // Set heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // Set n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // Set INFIMUM record
    page_data[54] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 99);

    // Set SUPREMUM record
    page_data[99] = dbrecovery::innodb::REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 100, 0xFFFF);

    // Set n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // Create table schema with BLOB column
    catalog::TableSchema schema;
    schema.table_name = "blob_table";
    schema.row_format = 2;  // DYNAMIC

    catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = mysql_types::LONGLONG;
    col1.is_nullable = false;
    schema.columns[1] = col1;

    catalog::ColumnInfo col2;
    col2.name = "data";
    col2.mysql_type_id = mysql_types::MEDIUM_BLOB;
    col2.is_nullable = true;
    schema.columns[2] = col2;

    // Parse the page
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    // Parse records
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records.size(), 0);
}

// Test Case 4: parse_records with deleted records
TEST(ParseRecordsTest, DeletedRecords) {
    std::vector<uint8_t> page_data(16384, 0);

    // Set page type to INDEX
    write_le<uint16_t>(page_data.data(), 16, 9969);

    // Set page level to 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // Set flags with COMPACT format
    write_le<uint16_t>(page_data.data(), 48, 0x0701);

    // Set heap_top
    write_le<uint16_t>(page_data.data(), 40, 150);

    // Set n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 3);

    // Set INFIMUM record
    page_data[54] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 75);  // next_record_offset = 75 (user record 1)

    // Set user record 1 (deleted)
    page_data[75] = dbrecovery::innodb::REC_INFO_DELETED_FLAG;  // Mark as deleted
    write_le<uint16_t>(page_data.data(), 76, 99);  // next_record_offset = 99 (user record 2)

    // Set user record 2 (valid)
    page_data[99] = 0x00;  // info_bits (not deleted)
    write_le<uint16_t>(page_data.data(), 100, 120);  // next_record_offset = 120 (SUPREMUM)

    // Set SUPREMUM record at offset 120
    page_data[120] = dbrecovery::innodb::REC_STATUS_SUPREMUM;  // info_bits
    write_le<uint16_t>(page_data.data(), 121, 0xFFFF);  // next_record_offset = end

    // Set n_heap
    write_le<uint16_t>(page_data.data(), 42, 5);

    // Create table schema
    catalog::TableSchema schema;
    schema.table_name = "deleted_table";
    schema.row_format = 1;  // COMPACT

    catalog::ColumnInfo col;
    col.name = "id";
    col.mysql_type_id = mysql_types::LONGLONG;
    col.is_nullable = false;
    schema.columns[1] = col;

    // Parse the page
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    // Parse records - deleted record should be skipped
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records.size(), 1);  // One valid record (user record 2)
}

// Test Case 5: parse_records with truncated page
TEST(ParseRecordsTest, TruncatedPage) {
    // Create a minimal but valid page for testing
    std::vector<uint8_t> page_data(128, 0);  // Minimal valid page with space for directory

    // Set page type to INDEX
    write_le<uint16_t>(page_data.data(), 16, 9969);

    // Set page level to 0 (at BTR_N_LEVELS_ABS = 38 + 4 = 42)
    // BTR_N_LEVELS is 4 bytes (uint32) but we only need to set lower 2 bytes
    page_data[42] = 0;
    page_data[43] = 0;
    page_data[44] = 0;
    page_data[45] = 0;

    // Set flags with COMPACT format (bit 0-2 = 1)
    // flags at BTR header offset 10, absolute 38 + 10 = 48
    // Write 0x01 (COMPACT format)
    page_data[48] = 0x01;
    page_data[49] = 0x00;  // high byte should be 0 for valid format

    // Set heap_top at offset 40
    write_le<uint16_t>(page_data.data(), 40, 100);

    // Set n_dir_slots at offset 38
    write_le<uint16_t>(page_data.data(), 38, 2);

    // Set INFIMUM record at offset 54 (REC_STATUS_INFIMUM = 0x02)
    page_data[54] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    // Set INFIMUM next_offset to point to SUPREMUM at offset 99
    write_le<uint16_t>(page_data.data(), 55, 99);

    // Set SUPREMUM record at offset 99 (REC_STATUS_SUPREMUM = 0x03)
    page_data[99] = dbrecovery::innodb::REC_STATUS_SUPREMUM;
    // Set SUPREMUM next_offset to 0xFFFF (end of chain)
    write_le<uint16_t>(page_data.data(), 100, 0xFFFF);

    // Set n_heap at offset 42 (same as page_level for this minimal test)
    // This overwrites page_level, but the parser reads as uint16
    write_le<uint16_t>(page_data.data(), 42, 4);

    // Set directory slots at offset 96 (heap_top - n_dir_slots*2 = 100 - 4 = 96)
    write_le<uint16_t>(page_data.data(), 96, 54);  // Point to INFIMUM
    write_le<uint16_t>(page_data.data(), 98, 99);  // Point to SUPREMUM

    // Create table schema
    catalog::TableSchema schema;
    schema.table_name = "truncated_table";
    schema.row_format = 1;  // COMPACT

    catalog::ColumnInfo col;
    col.name = "id";
    col.mysql_type_id = mysql_types::LONGLONG;
    col.is_nullable = false;
    schema.columns[1] = col;

    // Parse the page - should succeed for minimal valid pages
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    // Parse records - should return 0 user records (only system records)
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records.size(), 0);
}

// Test Case 6: parse_records with DATETIME2 column
TEST(ParseRecordsTest, Datetime2Column) {
    std::vector<uint8_t> page_data(16384, 0);

    // Set page type to INDEX
    write_le<uint16_t>(page_data.data(), 16, 9969);

    // Set page level to 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // Set flags with COMPACT format
    write_le<uint16_t>(page_data.data(), 48, 0x0701);

    // Set heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // Set n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // Set INFIMUM record
    page_data[54] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 99);

    // Set SUPREMUM record
    page_data[99] = dbrecovery::innodb::REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 100, 0xFFFF);

    // Set n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // Create table schema with DATETIME2 column
    catalog::TableSchema schema;
    schema.table_name = "datetime_table";
    schema.row_format = 1;  // COMPACT

    catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = mysql_types::LONGLONG;
    col1.is_nullable = false;
    schema.columns[1] = col1;

    catalog::ColumnInfo col2;
    col2.name = "created_at";
    col2.mysql_type_id = mysql_types::DATETIME2;
    col2.datetime_precision = 6;
    col2.is_nullable = true;
    schema.columns[2] = col2;

    // Parse the page
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    // Parse records
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records.size(), 0);
}

// Test Case 7: parse_records with JSON column
TEST(ParseRecordsTest, JsonColumn) {
    std::vector<uint8_t> page_data(16384, 0);

    // Set page type to INDEX
    write_le<uint16_t>(page_data.data(), 16, 9969);

    // Set page level to 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // Set flags with COMPACT format
    write_le<uint16_t>(page_data.data(), 48, 0x0701);

    // Set heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // Set n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // Set INFIMUM record
    page_data[54] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 99);

    // Set SUPREMUM record
    page_data[99] = dbrecovery::innodb::REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 100, 0xFFFF);

    // Set n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // Create table schema with JSON column
    catalog::TableSchema schema;
    schema.table_name = "json_table";
    schema.row_format = 1;  // COMPACT

    catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = mysql_types::LONGLONG;
    col1.is_nullable = false;
    schema.columns[1] = col1;

    catalog::ColumnInfo col2;
    col2.name = "data";
    col2.mysql_type_id = mysql_types::JSON;
    col2.is_nullable = true;
    schema.columns[2] = col2;

    // Parse the page
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    // Parse records
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records.size(), 0);
}

// Test Case 8: parse_records with multiple nullable columns
TEST(ParseRecordsTest, MultipleNullableColumns) {
    std::vector<uint8_t> page_data(16384, 0);

    // Set page type to INDEX
    write_le<uint16_t>(page_data.data(), 16, 9969);

    // Set page level to 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // Set flags with COMPACT format
    write_le<uint16_t>(page_data.data(), 48, 0x0701);

    // Set heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // Set n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // Set INFIMUM record
    page_data[54] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 99);

    // Set SUPREMUM record
    page_data[99] = dbrecovery::innodb::REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 100, 0xFFFF);

    // Set n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // Create table schema with multiple nullable columns
    catalog::TableSchema schema;
    schema.table_name = "nullable_table";
    schema.row_format = 1;  // COMPACT

    for (int i = 0; i < 10; ++i) {
        catalog::ColumnInfo col;
        col.name = "col_" + std::to_string(i);
        col.mysql_type_id = mysql_types::VARCHAR;
        col.column_length = 100;
        col.is_nullable = true;
        schema.columns[i + 1] = col;
    }

    // Parse the page
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    // Parse records
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records.size(), 0);
}

// Test Case 9: parse_records with page not parsed
TEST(ParseRecordsTest, PageNotParsed) {
    std::vector<uint8_t> page_data(16384, 0);

    // Create table schema
    catalog::TableSchema schema;
    schema.table_name = "test_table";
    schema.row_format = 1;  // COMPACT

    catalog::ColumnInfo col;
    col.name = "id";
    col.mysql_type_id = mysql_types::LONGLONG;
    col.is_nullable = false;
    schema.columns[1] = col;

    // Create parser but don't parse
    BPlusTreeParser parser(page_data);

    // Try to parse records without parsing page first
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records.size(), 0);
}

// Test Case 10: parse_records with empty schema
TEST(ParseRecordsTest, EmptySchema) {
    std::vector<uint8_t> page_data(16384, 0);

    // Set page type to INDEX
    write_le<uint16_t>(page_data.data(), 16, 9969);

    // Set page level to 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // Set flags with COMPACT format
    write_le<uint16_t>(page_data.data(), 48, 0x0701);

    // Set heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // Set n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // Set INFIMUM record
    page_data[54] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 99);

    // Set SUPREMUM record
    page_data[99] = dbrecovery::innodb::REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 100, 0xFFFF);

    // Set n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // Create empty schema
    catalog::TableSchema schema;
    schema.table_name = "empty_schema_table";
    schema.row_format = 1;  // COMPACT

    // Parse the page
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    // Parse records with empty schema
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records.size(), 0);
}

} // namespace innodb
} // namespace dbrecovery
