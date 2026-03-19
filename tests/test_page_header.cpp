#include <gtest/gtest.h>
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== PageHeader 解析测试 ====================

TEST(PageHeader, ParseTruncatedData) {
    // 创建一个只有 10 字节的截断数据
    std::vector<uint8_t> data(10, 0);
    write_le<uint16_t>(data.data(), 0, 5);   // n_dir_slots
    write_le<uint16_t>(data.data(), 2, 100); // heap_top

    auto header = PageHeader::parse(data.data(), data.size());
    EXPECT_FALSE(header.has_value()) << "Truncated page header should not be parsed";
}

TEST(PageHeader, ParseFullHeader) {
    // 创建一个完整的页头数据 (38 字节)
    std::vector<uint8_t> data(38, 0);
    
    write_le<uint16_t>(data.data(), 0, 5);   // n_dir_slots
    write_le<uint16_t>(data.data(), 2, 100); // heap_top
    write_le<uint16_t>(data.data(), 4, 10);  // n_heap
    write_le<uint16_t>(data.data(), 6, 50);  // free_offset
    write_le<uint16_t>(data.data(), 8, 60);  // garbage_end
    write_le<uint16_t>(data.data(), 10, 70); // last_insert
    write_le<uint16_t>(data.data(), 12, 8);  // direction
    write_le<uint16_t>(data.data(), 14, 0);  // n_direct_pages

    auto header = PageHeader::parse(data.data(), data.size());
    
    EXPECT_TRUE(header.has_value()) << "Full page header should be parsed";
    EXPECT_EQ(header->n_dir_slots, 5);
    EXPECT_EQ(header->heap_top, 100);
    EXPECT_EQ(header->free_offset, 50);
    EXPECT_EQ(header->direction, 8);
}

TEST(PageHeader, ToString) {
    std::vector<uint8_t> data(38, 0);
    write_le<uint16_t>(data.data(), 0, 10); // n_dir_slots

    auto header = PageHeader::parse(data.data(), data.size());
    EXPECT_TRUE(header.has_value());

    std::string str = header->to_string();
    EXPECT_NE(str.find("n_dir_slots=10"), std::string::npos);
}

// ==================== Record 格式化测试（新增）====================
TEST(Record, ToStringFormatting) {
    std::vector<uint8_t> data(6, 0);
    write_le<uint16_t>(data.data(), 0, 5);   // next_record_offset
    data[4] = 0x02;                          // heap_no low byte
    
    auto record = Record::parse(data.data(), 38, 6);
    EXPECT_TRUE(record.has_value());
    
    std::string str = record->to_string();
    // 验证 info_bits 格式化为两位十六进制（补零）
    EXPECT_NE(str.find("info_bits=0x"), std::string::npos);
}

// ==================== RecordHeader 解析测试（新增）====================
TEST(RecordHeader, ParseCompactFormat) {
    std::vector<uint8_t> data(5, 0);
    data[0] = 0x12; // info_bits
    write_le<uint16_t>(data.data(), 1, 70); // next_record_offset (bits 0-14)
    data[3] = 0x01; // heap_no low byte
    data[4] = 0x00; // heap_no high byte

    auto header = RecordHeader::parse(data.data(), data.size());
    EXPECT_TRUE(header.has_value());
    
    EXPECT_EQ(header->get_next_record_offset(), 70);
    EXPECT_FALSE(header->is_deleted());
    EXPECT_FALSE(header->is_min_rec());
    EXPECT_EQ(header->get_heap_no(), 1);
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
