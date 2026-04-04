/**
 * @file page_parser_v3_test.cpp
 * @brief PageParserV3 单元测试
 * 
 * 测试覆盖：
 * - Redundant 行格式解析
 * - Compact 行格式解析
 * - Dynamic 行格式解析
 * - B+ 树遍历
 * - Undo Log 链条解析
 * - 版本探测
 * - 行格式识别
 */

#include "dbrecovery/innodb/page_parser_v3.hpp"
#include <gtest/gtest.h>
#include <vector>
#include <cstring>
#include <fstream>
#include <filesystem>

namespace dbrecovery {
namespace innodb {

// ==================== RedundantRowParser 测试 ====================

TEST(RedundantRowParserTest, ParseRecordHeader) {
    // 创建 Redundant 格式的记录头 (6 字节)
    uint8_t data[6] = {0x00, 0x0D, 0x00, 0x01, 0x00, 0x03};
    
    RedundantRowParser parser;
    RecordHeader header = parser.parse_record_header(data);
    
    EXPECT_EQ(header.get_next_record_offset(), 13);
    EXPECT_EQ(header.get_heap_no(), 1);
    EXPECT_EQ(header.info_bits, 0x00);
}

TEST(RedundantRowParserTest, ParseRecordData) {
    // 创建 Redundant 格式的记录数据
    // 6-byte header + 3 columns * 2 bytes length + data
    std::vector<uint8_t> data(30, 0);

    // Header
    data[0] = 0x00;  // info_bits
    data[1] = 0x0D;  // next_record_offset (low)
    data[2] = 0x00;  // next_record_offset (high)
    data[3] = 0x01;  // heap_no (low)
    data[4] = 0x00;  // heap_no (high)
    data[5] = 0x03;  // n_fields

    // Column lengths (2 bytes each)
    data[6] = 0x04; data[7] = 0x00;  // col1: 4 bytes
    data[8] = 0x05; data[9] = 0x00;  // col2: 5 bytes
    data[10] = 0x03; data[11] = 0x00; // col3: 3 bytes

    // Column data (4 + 5 + 3 = 12 bytes)
    // "test" (4) + "test2" (5) + "tes" (3) = 12 bytes
    std::strcpy(reinterpret_cast<char*>(data.data()) + 12, "testtest2tes");
    
    // 定义列元数据
    std::vector<catalog::ColumnInfo> columns;
    catalog::ColumnInfo col1, col2, col3;
    col1.name = "col1"; col1.mysql_type_id = 1; col1.column_length = 4; col1.is_variable_length = false;
    col2.name = "col2"; col2.mysql_type_id = 2; col2.column_length = 5; col2.is_variable_length = true;
    col3.name = "col3"; col3.mysql_type_id = 3; col3.column_length = 3; col3.is_variable_length = false;
    columns.push_back(col1);
    columns.push_back(col2);
    columns.push_back(col3);

    RedundantRowParser parser;
    RecordHeader header = parser.parse_record_header(data.data());
    auto values = parser.parse_record_data(data.data(), data.size(), header, columns);

    EXPECT_EQ(values.size(), 3);
    EXPECT_TRUE(values[0].has_value());
    EXPECT_EQ(values[0].value(), "test");
    EXPECT_TRUE(values[1].has_value());
    EXPECT_EQ(values[1].value(), "test2");
    EXPECT_TRUE(values[2].has_value());
    EXPECT_EQ(values[2].value(), "tes");
}

TEST(RedundantRowParserTest, IsDeleted) {
    uint8_t data[6] = {0x08, 0x00, 0x00, 0x00, 0x00, 0x00};
    
    RedundantRowParser parser;
    EXPECT_TRUE(parser.is_record_deleted(data));
}

TEST(RedundantRowParserTest, IsMinRecord) {
    uint8_t data[6] = {REC_STATUS_INFIMUM, 0x00, 0x00, 0x00, 0x00, 0x00};
    
    RedundantRowParser parser;
    EXPECT_TRUE(parser.is_min_record(data));
}

TEST(RedundantRowParserTest, IsMaxRecord) {
    uint8_t data[6] = {REC_STATUS_SUPREMUM, 0x00, 0x00, 0x00, 0x00, 0x00};
    
    RedundantRowParser parser;
    EXPECT_TRUE(parser.is_max_record(data));
}

// ==================== CompactRowParser 测试 ====================

TEST(CompactRowParserTest, ParseRecordHeader) {
    // 创建 Compact 格式的记录头 (5 字节)
    uint8_t data[5] = {0x18, 0x0D, 0x00, 0x01, 0x00};
    
    CompactRowParser parser;
    RecordHeader header = parser.parse_record_header(data);
    
    EXPECT_EQ(header.get_next_record_offset(), 13);
    EXPECT_EQ(header.get_heap_no(), 1);
    EXPECT_EQ(header.info_bits, 0x18);
}

TEST(CompactRowParserTest, ParseRecordData) {
    // 创建 Compact 格式的记录数据
    std::vector<uint8_t> data(20, 0);

    // Header (5 bytes)
    data[0] = 0x18;  // info_bits
    data[1] = 0x0D;  // next_record_offset (low)
    data[2] = 0x00;  // next_record_offset (high)
    data[3] = 0x01;  // heap_no (low)
    data[4] = 0x00;  // heap_no (high)

    // Variable length (1 byte)
    data[5] = 0x04;  // col1: 4 bytes

    // Column data
    std::strcpy(reinterpret_cast<char*>(data.data()) + 6, "test");

    std::vector<catalog::ColumnInfo> columns;
    catalog::ColumnInfo col1, col2;
    col1.name = "col1"; col1.mysql_type_id = 1; col1.column_length = 4; col1.is_variable_length = true;
    col2.name = "col2"; col2.mysql_type_id = 2; col2.column_length = 5; col2.is_variable_length = false;
    columns.push_back(col1);
    columns.push_back(col2);

    CompactRowParser parser;
    RecordHeader header = parser.parse_record_header(data.data());
    auto values = parser.parse_record_data(data.data(), data.size(), header, columns);
    EXPECT_EQ(values.size(), 2);
    EXPECT_TRUE(values[0].has_value());
    EXPECT_EQ(values[0].value(), "test");
}

TEST(CompactRowParserTest, IsDeleted) {
    uint8_t data[5] = {0x08, 0x00, 0x00, 0x00, 0x00};
    
    CompactRowParser parser;
    EXPECT_TRUE(parser.is_record_deleted(data));
}

// ==================== DynamicRowParser 测试 ====================

TEST(DynamicRowParserTest, ParseExternalPointer) {
    // 创建外部指针数据 (20 bytes)
    uint8_t data[20] = {0};
    
    // space_id = 1
    data[0] = 0x01; data[1] = 0x00; data[2] = 0x00; data[3] = 0x00;
    // page_no = 100
    data[4] = 0x64; data[5] = 0x00; data[6] = 0x00; data[7] = 0x00;
    // offset = 256
    data[8] = 0x00; data[9] = 0x01; data[10] = 0x00; data[11] = 0x00;
    // len = 4096
    data[12] = 0x00; data[13] = 0x10; data[14] = 0x00; data[15] = 0x00;
    
    DynamicRowParser parser;
    auto ptr = parser.parse_external_pointer(data, 20);
    
    EXPECT_TRUE(ptr.has_value());
    EXPECT_EQ(ptr->space_id, 1);
    EXPECT_EQ(ptr->page_no, 100);
    EXPECT_EQ(ptr->offset, 256);
    EXPECT_EQ(ptr->len, 4096);
}

// ==================== PageParserFactoryV3 测试 ====================

TEST(PageParserFactoryV3Test, DetectRowFormatRedundant) {
    // 创建 Redundant 格式的页数据
    std::vector<uint8_t> data(16384, 0);
    
    // FIL Header
    data[0] = 0x00; data[1] = 0x00; data[2] = 0x00; data[3] = 0x00;  // FIL_PAGE_PREV
    data[4] = 0x01; data[5] = 0x00; data[6] = 0x00; data[7] = 0x00;  // FIL_PAGE_NEXT
    data[8] = 0x00; data[9] = 0x00; data[10] = 0x00; data[11] = 0x00; // FIL_PAGE_LSN
    data[12] = 0x00; data[13] = 0x00; data[14] = 0x00; data[15] = 0x00; // FIL_PAGE_LSN
    
    // Page Type (B+ Tree Index = 0x4567)
    data[16] = 0x67; data[17] = 0x45;
    
    // Page Level (0 = leaf)
    data[42] = 0x00; data[43] = 0x00;
    
    // BTR Header flags (Redundant = 0)
    data[44] = 0x00; data[45] = 0x00;
    
    // First record (INFIMUM)
    data[54] = REC_STATUS_INFIMUM;  // info_bits
    data[55] = 0x0D; data[56] = 0x00;  // next_record_offset
    data[57] = 0x00; data[58] = 0x00;  // heap_no
    data[59] = 0x06;  // n_fields (6 for Redundant)
    
    RowFormat format = PageParserFactoryV3::detect_row_format_v3(data.data(), data.size());
    EXPECT_EQ(format, RowFormat::REDUNDANT);
}

TEST(PageParserFactoryV3Test, DetectRowFormatCompact) {
    // 创建 Compact 格式的页数据
    std::vector<uint8_t> data(16384, 0);

    // FIL Header
    data[16] = 0x67; data[17] = 0x45;  // Page Type

    // BTR Header flags (Compact = 1)
    // BTR header starts at FIL_HEADER_SIZE (38), flags at offset 42 relative to BTR header
    // So absolute offset is 38 + 42 = 80
    data[80] = 0x01; data[81] = 0x00;  // Row format = COMPACT (value 1 in page format)

    // First record (INFIMUM)
    data[54] = REC_STATUS_INFIMUM;  // info_bits
    data[55] = 0x0D; data[56] = 0x00;  // next_record_offset
    data[57] = 0x00; data[58] = 0x00;  // heap_no
    data[59] = 0x05;  // n_fields (5 for Compact)

    RowFormat format = PageParserFactoryV3::detect_row_format_v3(data.data(), data.size());
    
    // Debug output
    std::cout << "Detected format: " << (int)format << std::endl;
    
    EXPECT_EQ(format, RowFormat::COMPACT);
}

// ==================== BTreeTraverser 测试 ====================

TEST(BTreeTraverserTest, TraverseFromPage) {
    // 创建一个简单的 B+ 树页
    std::vector<uint8_t> data(16384, 0);
    
    // FIL Header
    data[16] = 0x67; data[17] = 0x45;  // Page Type
    data[42] = 0x00; data[43] = 0x00;  // Page Level (leaf)
    data[44] = 0x02; data[45] = 0x00;  // N_DIR_SLOTS
    
    // Directory slots
    data[38] = 0x36; data[39] = 0x00;  // INFIMUM offset
    data[40] = 0x43; data[41] = 0x00;  // SUPREMUM offset
    
    // INFIMUM record
    data[54] = REC_STATUS_INFIMUM;
    data[55] = 0x43; data[56] = 0x00;  // next_record_offset
    
    // SUPREMUM record
    data[67] = REC_STATUS_SUPREMUM;
    data[68] = 0x00; data[69] = 0x00;  // next_record_offset (end)
    
    BTreeTraverser traverser(data.data(), data.size());
    auto results = traverser.traverse_from_page(0);
    
    EXPECT_GT(results.size(), 0);
}

// ==================== UndoLogChainParser 测试 ====================

TEST(UndoLogChainParserTest, GetUndoLogPages) {
    // 创建 Undo Log 页数据
    std::vector<uint8_t> data(16384, 0);
    
    // Undo Log Header
    data[0] = 0x01; data[1] = 0x00;  // state = TRX_UNDO_ACTIVE
    data[2] = 0x01; data[3] = 0x00;  // direction = TRX_UNDO_INSERT
    data[4] = 0x00; data[5] = 0x00; data[6] = 0x00; data[7] = 0x00;  // last_page
    data[8] = 0x00; data[9] = 0x00;  // last_offset
    data[10] = 0xFF; data[11] = 0xFF; data[12] = 0xFF; data[13] = 0xFF;  // prev_page
    data[14] = 0x01; data[15] = 0x00; data[16] = 0x00; data[17] = 0x00;  // next_page
    
    UndoLogChainParser parser(data.data(), data.size());
    auto pages = parser.get_undo_log_pages();
    
    // No UNDO_LOG pages in test data
    EXPECT_EQ(pages.size(), 0);
}

// ==================== PageParserV3 集成测试 ====================

TEST(PageParserV3Test, ParseCompletePage) {
    // 创建完整的页数据
    std::vector<uint8_t> data(16384, 0);
    
    // FIL Header
    data[0] = 0xFF; data[1] = 0xFF; data[2] = 0xFF; data[3] = 0xFF;  // FIL_PAGE_PREV
    data[4] = 0xFF; data[5] = 0xFF; data[6] = 0xFF; data[7] = 0xFF;  // FIL_PAGE_NEXT
    data[8] = 0x00; data[9] = 0x00; data[10] = 0x00; data[11] = 0x00; // FIL_PAGE_LSN
    data[12] = 0x00; data[13] = 0x00; data[14] = 0x00; data[15] = 0x00; // FIL_PAGE_LSN
    
    // Page Type
    data[16] = 0x67; data[17] = 0x45;
    
    // Checksum
    data[18] = 0x00; data[19] = 0x00; data[20] = 0x00; data[21] = 0x00;
    
    // Page Number
    data[22] = 0x00; data[23] = 0x00; data[24] = 0x00; data[25] = 0x00;
    
    // Page Level
    data[42] = 0x00; data[43] = 0x00;
    
    // BTR Header
    data[44] = 0x00; data[45] = 0x00;
    
    // N_DIR_SLOTS
    data[38] = 0x02; data[39] = 0x00;
    
    // INFIMUM record (54 bytes)
    data[54] = REC_STATUS_INFIMUM;
    data[55] = 0x43; data[56] = 0x00;
    data[57] = 0x00; data[58] = 0x00;
    data[59] = 0x05;
    
    // SUPREMUM record (67 bytes)
    data[67] = REC_STATUS_SUPREMUM;
    data[68] = 0x00; data[69] = 0x00;
    data[70] = 0x01; data[71] = 0x00;
    data[72] = 0x05;
    
    PageParserV3 parser(data.data(), data.size());
    auto result = parser.parse();
    
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.page_type, 0x4567);
    EXPECT_EQ(result.page_level, 0);
}

// ==================== 实用工具函数测试 ====================

TEST(UtilsTest, CalculateRecordSize) {
    std::vector<uint8_t> data(100, 0);
    
    // Redundant: 6-byte header + 3 columns * 2 bytes length
    size_t size = calculate_record_size(data.data(), data.size(), RowFormat::REDUNDANT, 3);
    EXPECT_GT(size, 0);
}

TEST(UtilsTest, ExtractFieldValues) {
    std::vector<uint8_t> data(50, 0);
    
    // Header
    data[0] = 0x18;
    data[1] = 0x0D; data[2] = 0x00;
    data[3] = 0x01; data[4] = 0x00;
    
    // Variable length
    data[5] = 0x04;
    
    // Data
    std::strcpy(reinterpret_cast<char*>(data.data()) + 6, "test12345");
    
    std::vector<catalog::ColumnInfo> columns;
    catalog::ColumnInfo col1;
    col1.name = "col1";
    col1.mysql_type_id = 1;
    col1.is_nullable = true;
    col1.column_length = 4;
    col1.is_variable_length = true;
    columns.push_back(col1);
    
    auto values = extract_field_values(data.data(), data.size(), RowFormat::COMPACT, columns);
    EXPECT_EQ(values.size(), 1);
}

TEST(UtilsTest, GenerateCreateStatement) {
    std::vector<RowValue> sample_rows;
    std::vector<catalog::ColumnInfo> columns;
    
    catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = 4;
    col1.is_nullable = false;
    col1.column_length = 4;
    columns.push_back(col1);
    
    catalog::ColumnInfo col2;
    col2.name = "name";
    col2.mysql_type_id = 253;
    col2.is_nullable = false;
    col2.column_length = 64;
    columns.push_back(col2);
    
    std::string sql = generate_create_statement_heuristic(sample_rows, columns);
    EXPECT_FALSE(sql.empty());
    EXPECT_NE(sql.find("CREATE TABLE"), std::string::npos);
}

}  // namespace innodb
}  // namespace dbrecovery
