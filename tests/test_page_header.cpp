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
    // InnoDB stores Page Header fields in BIG-ENDIAN format
    std::vector<uint8_t> data(38, 0);

    write_be<uint16_t>(data.data(), 0, 5);   // n_dir_slots
    write_be<uint16_t>(data.data(), 2, 100); // heap_top
    write_be<uint16_t>(data.data(), 4, 10);  // n_heap
    write_be<uint16_t>(data.data(), 6, 50);  // free_offset
    write_be<uint16_t>(data.data(), 8, 60);  // garbage_end
    write_be<uint16_t>(data.data(), 10, 70); // last_insert
    write_be<uint16_t>(data.data(), 12, 8);  // direction
    write_be<uint16_t>(data.data(), 14, 0);  // n_direct_pages

    auto header = PageHeader::parse(data.data(), data.size());

    EXPECT_TRUE(header.has_value()) << "Full page header should be parsed";
    EXPECT_EQ(header->n_dir_slots, 5);
    EXPECT_EQ(header->heap_top, 100);
    EXPECT_EQ(header->free_offset, 50);
    EXPECT_EQ(header->direction, 8);
}

TEST(PageHeader, ToString) {
    std::vector<uint8_t> data(38, 0);
    write_be<uint16_t>(data.data(), 0, 10); // n_dir_slots

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
    // RecordHeader layout (ibd2sql big-endian format):
    // byte0-2: info_and_heap (24 bits big-endian)
    //   bit 23: instant_flag
    //   bit 22: version_flag
    //   bit 21: deleted_flag
    //   bit 20: min_rec_flag
    //   bits 19-16: n_owned
    //   bits 15-3: heap_no (13 bits)
    //   bits 2-0: rec_type
    // byte3-4: next_record_offset (big-endian 16-bit signed)

    // get_heap_no() = ((byte1 << 8) | byte2) >> 3
    // For heap_no = 1: (byte1 << 8) | byte2 = 1 << 3 = 8
    // So byte1 = 0, byte2 = 8
    data[0] = 0x00;  // info_bits
    data[1] = 0x00;  // heap_no high bits
    data[2] = 0x08;  // heap_no = 1 (bits 15-3 = 8, so heap_no = 8 >> 3 = 1)
    // Set next_record_offset = 70 (big-endian)
    data[3] = 0x00;  // high byte
    data[4] = 70;    // low byte (70 = 0x46)

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
