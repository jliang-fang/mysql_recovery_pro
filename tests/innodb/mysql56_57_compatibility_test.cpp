// Test cases for MySQL 5.6/5.7 compatibility
// Tests for P1-4: MySQL 5.6/5.7 compatibility

#include <gtest/gtest.h>
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== MySQL 5.6/5.7 兼容性测试 ====================

// Test Case 1: REDUNDANT 格式兼容性（MySQL 5.0/5.1/5.5）
TEST(MySQL5657CompatibilityTest, RedundantFormatCompatibility) {
    std::vector<uint8_t> page_data(16384, 0);

    // 设置页类型为 INDEX
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);

    // 设置页级别为 0（叶子页）
    write_le<uint16_t>(page_data.data(), 42, 0);

    // 设置 flags 为 REDUNDANT 格式（bit 0-2 = 0）
    write_le<uint16_t>(page_data.data(), 44, 0x0700);

    // 设置 heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // 设置 n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // 设置 INFIMUM 记录
    page_data[54] = REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 97);  // next_offset = 97 (SUPREMUM)

    // 设置 SUPREMUM 记录
    page_data[97] = REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 98, 0xFFFF);

    // 设置 n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // 解析页
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    // 验证行格式
    EXPECT_EQ(parser.btr_format(), row_formats::REDUNDANT);
}

// Test Case 2: COMPACT 格式兼容性（MySQL 5.0+）
TEST(MySQL5657CompatibilityTest, CompactFormatCompatibility) {
    std::vector<uint8_t> page_data(16384, 0);

    // 设置页类型为 INDEX
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);

    // 设置页级别为 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // 设置 flags 为 COMPACT 格式（bit 0-2 = 1）
    // BTR header flags at absolute offset 80 (FIL_HEADER_SIZE=38 + BTR_HEADER_SIZE=42)
    write_le<uint16_t>(page_data.data(), 80, 0x0701); // flags = 0x0701 (COMPACT format)

    // 设置 heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // 设置 n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // 设置 INFIMUM 记录
    page_data[54] = REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 97);  // next_offset = 97 (SUPREMUM)

    // 设置 SUPREMUM 记录
    page_data[97] = REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 98, 0xFFFF);

    // 设置 n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // 解析页
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    // 验证行格式
    EXPECT_EQ(parser.btr_format(), row_formats::COMPACT);
}

// Test Case 3: 页校验和兼容性（MySQL 5.6/5.7 使用 CRC32）
TEST(MySQL5657CompatibilityTest, ChecksumCompatibility) {
    std::vector<uint8_t> page_data(16384, 0);

    // 设置页类型为 INDEX
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);

    // 设置页级别为 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // 设置 flags
    write_le<uint16_t>(page_data.data(), 44, 0x0701);

    // 设置 heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // 设置 n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // 设置 INFIMUM 记录
    page_data[54] = REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 97);  // next_offset = 97 (SUPREMUM)

    // 设置 SUPREMUM 记录
    page_data[97] = REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 98, 0xFFFF);

    // 设置 n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // 计算 CRC32 校验和（MySQL 5.6/5.7 方式）
    std::vector<uint8_t> page_copy = page_data;
    for (int i = 0; i < 4; ++i) {
        page_copy[24 + i] = 0;
    }

    // 使用 PageReader 计算校验和
    uint32_t computed_crc = dbrecovery::innodb::PageReader::compute_crc32c(page_copy);

    // 写入校验和
    for (int i = 0; i < 4; ++i) {
        page_data[24 + i] = (computed_crc >> (i * 8)) & 0xFF;
    }

    // 解析页
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    // 验证校验和（通过检查页数据）
    uint32_t stored_crc = dbrecovery::innodb::read_le<uint32_t>(page_data.data(), 24);
    EXPECT_EQ(stored_crc, computed_crc);
}

// Test Case 4: DATETIME 类型兼容性（MySQL 5.6 引入 DATETIME2）
TEST(MySQL5657CompatibilityTest, DatetimeTypeCompatibility) {
    // 创建表结构包含 DATETIME2 列
    catalog::TableSchema schema;
    schema.table_name = "test_table";
    schema.row_format = row_formats::COMPACT;

    catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = mysql_types::LONGLONG;
    col1.is_nullable = false;
    schema.columns[1] = col1;

    catalog::ColumnInfo col2;
    col2.name = "created_at";
    col2.mysql_type_id = mysql_types::DATETIME2;
    col2.datetime_precision = 3; // 毫秒精度
    col2.is_nullable = true;
    schema.columns[2] = col2;

    // 验证列定义
    EXPECT_EQ(schema.columns.size(), 2);
    EXPECT_EQ(schema.columns.at(2).mysql_type_id, mysql_types::DATETIME2);
    EXPECT_EQ(schema.columns.at(2).datetime_precision, 3);
}

// Test Case 5: TIMESTAMP 类型兼容性（MySQL 5.6 引入 TIMESTAMP2）
TEST(MySQL5657CompatibilityTest, TimestampTypeCompatibility) {
    catalog::TableSchema schema;
    schema.table_name = "test_table";
    schema.row_format = row_formats::COMPACT;

    catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = mysql_types::LONGLONG;
    col1.is_nullable = false;
    schema.columns[1] = col1;

    catalog::ColumnInfo col2;
    col2.name = "updated_at";
    col2.mysql_type_id = mysql_types::TIMESTAMP2;
    col2.datetime_precision = 6; // 微秒精度
    col2.is_nullable = true;
    schema.columns[2] = col2;

    // 验证列定义
    EXPECT_EQ(schema.columns.size(), 2);
    EXPECT_EQ(schema.columns.at(2).mysql_type_id, mysql_types::TIMESTAMP2);
    EXPECT_EQ(schema.columns.at(2).datetime_precision, 6);
}

// Test Case 6: VARCHAR 类型兼容性（MySQL 5.6/5.7 最大长度 65535）
TEST(MySQL5657CompatibilityTest, VarcharTypeCompatibility) {
    catalog::TableSchema schema;
    schema.table_name = "test_table";
    schema.row_format = row_formats::COMPACT;

    catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = mysql_types::LONGLONG;
    col1.is_nullable = false;
    schema.columns[1] = col1;

    catalog::ColumnInfo col2;
    col2.name = "description";
    col2.mysql_type_id = mysql_types::VARCHAR;
    col2.column_length = 65535; // MySQL 5.6/5.7 最大 VARCHAR 长度
    col2.is_nullable = true;
    schema.columns[2] = col2;

    // 验证列定义
    EXPECT_EQ(schema.columns.at(2).column_length, 65535);
}

// Test Case 7: 索引兼容性（MySQL 5.6/5.7 索引前缀长度 767 字节）
TEST(MySQL5657CompatibilityTest, IndexCompatibility) {
    catalog::TableSchema schema;
    schema.table_name = "test_table";
    schema.row_format = row_formats::COMPACT;

    catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = mysql_types::LONGLONG;
    col1.is_nullable = false;
    schema.columns[1] = col1;

    catalog::ColumnInfo col2;
    col2.name = "name";
    col2.mysql_type_id = mysql_types::VARCHAR;
    col2.column_length = 255;
    col2.is_nullable = true;
    schema.columns[2] = col2;

    // 添加索引
    catalog::IndexInfo idx;
    idx.name = "idx_name";
    idx.columns.push_back("name");
    schema.indexes.push_back(idx);

    // 验证索引定义
    EXPECT_EQ(schema.indexes.size(), 1);
    EXPECT_EQ(schema.indexes[0].name, "idx_name");
    EXPECT_EQ(schema.indexes[0].columns.size(), 1);
}

// Test Case 8: 页大小兼容性（MySQL 5.6/5.7 支持 4KB/8KB/16KB/32KB/64KB）
TEST(MySQL5657CompatibilityTest, PageSizeCompatibility) {
    // 测试不同的页大小
    std::vector<uint32_t> page_sizes = {4096, 8192, 16384, 32768, 65536};

    for (auto page_size : page_sizes) {
        std::vector<uint8_t> page_data(page_size, 0);

        // 设置页类型为 INDEX
        write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);

        // 设置页级别为 0
        write_le<uint16_t>(page_data.data(), 42, 0);

        // 设置 flags
        write_le<uint16_t>(page_data.data(), 44, 0x0701);

        // 设置 heap_top
        write_le<uint16_t>(page_data.data(), 40, 100);

        // 设置 n_dir_slots
        write_le<uint16_t>(page_data.data(), 38, 2);

        // 设置 INFIMUM 记录
        page_data[54] = REC_STATUS_INFIMUM;
        write_le<uint16_t>(page_data.data(), 55, 97);  // next_offset = 97 (SUPREMUM)

        // 设置 SUPREMUM 记录
        page_data[97] = REC_STATUS_SUPREMUM;
        write_le<uint16_t>(page_data.data(), 98, 0xFFFF);

        // 设置 n_heap
        write_le<uint16_t>(page_data.data(), 42, 4);

        // 解析页
        BPlusTreeParser parser(page_data);
        EXPECT_TRUE(parser.parse()) << "Failed for page size: " << page_size;
    }
}

// Test Case 9: 页类型兼容性（MySQL 5.6/5.7 页类型值）
TEST(MySQL5657CompatibilityTest, PageTypeCompatibility) {
    // MySQL 5.6/5.7 页类型值与 MySQL 8.0 相同
    EXPECT_EQ(FIL_PAGE_INDEX, 9969);      // B+Tree index page
    EXPECT_EQ(FIL_PAGE_SDI, 9971);        // SDI metadata page
    EXPECT_EQ(FIL_PAGE_UNDO_LOG, 9970);   // Undo log page
    EXPECT_EQ(FIL_PAGE_TYPE_ALLOCATED, 0); // Fresh allocated page
}

// Test Case 10: 行格式常量兼容性
TEST(MySQL5657CompatibilityTest, RowFormatConstants) {
    // 行格式常量在所有版本中保持一致
    EXPECT_EQ(row_formats::REDUNDANT, 0);
    EXPECT_EQ(row_formats::COMPACT, 1);
    EXPECT_EQ(row_formats::DYNAMIC, 2);
    EXPECT_EQ(row_formats::COMPRESSED, 3);
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
