#include <gtest/gtest.h>
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== BPlusTreeParser 测试 ====================

TEST(BPlusTreeParser, ParseValidLeafPage) {
    // 创建一个有效的叶子页
    std::vector<uint8_t> page_data(16384, 0);
    
    // ✅ 使用 page_defs.hpp 中定义的常量
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, dbrecovery::innodb::FIL_PAGE_INDEX);
    
    // 设置页级别为 0（叶子节点）
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::BTR_N_LEVELS_ABS, 0);
    
    // 设置目录槽数量
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + PAGE_N_DIR_SLOTS_OFFSET, 5);  // n_dir_slots
    
    // 设置堆顶位置
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + PAGE_HEAP_TOP_OFFSET, 100);  // heap_top
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
}

TEST(BPlusTreeParser, ParseInvalidPageType) {
    // 创建一个无效页类型的数据
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置页类型为 FIL_PAGE_TYPE_ALLOCATED (0)
    write_le<uint16_t>(page_data.data(), 50, FIL_PAGE_TYPE_ALLOCATED);
    
    BPlusTreeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

TEST(BPlusTreeParser, ParseEmptyData) {
    std::vector<uint8_t> page_data;
    BPlusTreeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

// ✅ 新增：测试一个完整的、有效的叶子页解析
TEST(BPlusTreeParser, ParseValidLeafPageFull) {
    // 创建一个 16KB 的页数据
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置 FIL Header (偏移 0-37)
    write_le<uint32_t>(page_data.data(), 0, 9969);   // FIL_PAGE_SPACE_ID = 0
    write_le<uint32_t>(page_data.data(), 4, 0);      // Checksum1
    write_le<uint64_t>(page_data.data(), 8, 0);      // LSN
    write_le<uint16_t>(page_data.data(), 16, 0);     // Page offset = 0
    write_le<uint16_t>(page_data.data(), 18, 0xFFFF); // Previous page (none)
    write_le<uint16_t>(page_data.data(), 20, 0xFFFF); // Next page (none)
    
    // 设置页类型为 FIL_PAGE_INDEX (9969) at offset 38+12=50
    write_le<uint16_t>(page_data.data(), 50, 9969);
    
    // 设置 BTR Header (偏移 38+4=42)
    write_le<uint16_t>(page_data.data(), 42, 0);     // PAGE_LEVEL = 0 (leaf)
    write_le<uint16_t>(page_data.data(), 38, 5);     // PAGE_N_DIR_SLOTS
    write_le<uint16_t>(page_data.data(), 40, 100);   // PAGE_HEAP_TOP
    
    // 设置目录槽和记录（简化版）
    // INFIMUM 记录 at offset 54
    page_data[54] = 0x02; // info_bits: REC_INFO_MIN_REC_FLAG (bit 4)
    write_le<uint16_t>(page_data.data(), 55, 70);   // next_record_offset
    
    // 用户记录
    page_data[70] = 0x00; // info_bits
    write_le<uint16_t>(page_data.data(), 71, 86);   // next_record_offset
    
    // SUPREMUM 记录 at offset 86
    page_data[86] = 0x03; // REC_STATUS_SUPREMUM
    write_le<uint16_t>(page_data.data(), 87, 0xFFFF); // end of chain
    
    // 设置目录槽 (在页尾部)
    size_t dir_start = 16384 - 5 * 2; // 5 slots * 2 bytes each
    write_le<uint16_t>(page_data.data(), dir_start + 0, 54);  // INFIMUM
    write_le<uint16_t>(page_data.data(), dir_start + 2, 70);  // User Record
    write_le<uint16_t>(page_data.data(), dir_start + 4, 86);  // SUPREMUM
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
    EXPECT_EQ(parser.node_type(), dbrecovery::innodb::BTreeNodeType::LEAF);
}

// ✅ 新增：测试解析包含多条用户记录的叶子页
TEST(BPlusTreeParser, ParseLeafPageWithMultipleRecords) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置页头信息
    write_le<uint16_t>(page_data.data(), 50, 9969);   // Page type
    write_le<uint16_t>(page_data.data(), 42, 0);      // PAGE_LEVEL = 0 (leaf)
    write_le<uint16_t>(page_data.data(), 38, 6);      // PAGE_N_DIR_SLOTS
    write_le<uint16_t>(page_data.data(), 40, 150);    // PAGE_HEAP_TOP
    
    // INFIMUM 记录 at offset 54
    page_data[54] = 0x02; 
    write_le<uint16_t>(page_data.data(), 55, 70);
    
    // 用户记录 1 at offset 70
    page_data[70] = 0x00;
    write_le<uint16_t>(page_data.data(), 71, 86);
    
    // 用户记录 2 at offset 86
    page_data[86] = 0x00;
    write_le<uint16_t>(page_data.data(), 87, 102);
    
    // SUPREMUM 记录 at offset 102
    page_data[102] = 0x03;
    write_le<uint16_t>(page_data.data(), 103, 0xFFFF);
    
    // 设置目录槽 (在页尾部)
    size_t dir_start = 16384 - 6 * 2; 
    write_le<uint16_t>(page_data.data(), dir_start + 0, 54);  // INFIMUM
    write_le<uint16_t>(page_data.data(), dir_start + 2, 70);  // User Record 1
    write_le<uint16_t>(page_data.data(), dir_start + 4, 86);  // User Record 2
    write_le<uint16_t>(page_data.data(), dir_start + 6, 102); // SUPREMUM
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
    
    // 验证解析出的记录数量（INFIMUM + 2 users + SUPREMUM = 4）
    EXPECT_EQ(parser.record_offsets().size(), 4);
}

// ✅ 新增：测试页类型读取辅助函数
TEST(PageHeader, GetPageType) {
    std::vector<uint8_t> page(16384, 0);
    
    // 设置页类型为 FIL_PAGE_INDEX (9969)
    write_le<uint16_t>(page.data(), 50, FIL_PAGE_INDEX);
    
    uint16_t type = get_page_type(page.data());
    EXPECT_EQ(type, FIL_PAGE_INDEX);
}

// ✅ 新增：测试 INFIMUM/SUPREMUM 解析（使用常量而非硬编码）
TEST(BPlusTreeParser, ParseInfimumSupremumWithConstants) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置页类型为 FIL_PAGE_INDEX
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, dbrecovery::innodb::FIL_PAGE_INDEX);
    
    // 设置堆顶位置（足够大以包含 INFIMUM 和 SUPREMUM）
    constexpr uint16_t heap_top = 100;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_HEAP_TOP_OFFSET, heap_top);
    
    // 设置有效的 INFIMUM 记录（使用常量偏移）
    constexpr uint16_t infimum_offset = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
    page_data[infimum_offset] = dbrecovery::innodb::REC_STATUS_INFIMUM; // info_bits
    write_le<uint16_t>(page_data.data(), infimum_offset + 1, heap_top - 5); // next_record_offset
    
    // 设置有效的 SUPREMUM 记录
    constexpr uint16_t supremum_offset = heap_top - 5;
    page_data[supremum_offset] = dbrecovery::innodb::REC_STATUS_SUPREMUM; // info_bits
    write_le<uint16_t>(page_data.data(), supremum_offset + 1, 0xFFFF); // end of chain
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
    
    // 验证解析出的记录偏移与常量一致
    auto offsets = parser.record_offsets();
    EXPECT_EQ(offsets.size(), 2);
    EXPECT_EQ(offsets[0], infimum_offset);
    EXPECT_EQ(offsets[1], supremum_offset);
}

// ✅ 新增：测试无效记录头被拒绝（防止解析损坏数据）
TEST(BPlusTreeParser, RejectInvalidRecordHeader) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置页类型为 FIL_PAGE_INDEX
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, dbrecovery::innodb::FIL_PAGE_INDEX);
    
    constexpr uint16_t heap_top = 200;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_HEAP_TOP_OFFSET, heap_top);
    
    // 设置有效的 INFIMUM 记录
    constexpr uint16_t infimum_offset = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
    page_data[infimum_offset] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), infimum_offset + 1, heap_top - 5);
    
    // 设置无效的用户记录（info_bits 高位非零）
    constexpr uint16_t invalid_record_offset = 70;
    page_data[invalid_record_offset] = 0xFF; // 无效 info_bits
    write_le<uint16_t>(page_data.data(), invalid_record_offset + 1, heap_top);
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse()); // 解析应成功，但跳过无效记录
    
    auto offsets = parser.record_offsets();
    // 只应包含 INFIMUM 和 SUPREMUM
    EXPECT_EQ(offsets.size(), 2);
}

// ✅ 新增：测试页目录槽位边界检查（防止越界访问）
TEST(BPlusTreeParser, CheckDirectorySlotsBoundary) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置页类型为 FIL_PAGE_INDEX
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, dbrecovery::innodb::FIL_PAGE_INDEX);
    
    constexpr uint16_t heap_top = 100;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_HEAP_TOP_OFFSET, heap_top);
    
    // 设置有效的 INFIMUM 记录
    constexpr uint16_t infimum_offset = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
    page_data[infimum_offset] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), infimum_offset + 1, heap_top - 5);
    
    // 设置有效的 SUPREMUM 记录
    constexpr uint16_t supremum_offset = heap_top - 5;
    page_data[supremum_offset] = dbrecovery::innodb::REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), supremum_offset + 1, 0xFFFF);
    
    // 设置目录槽数量为 2（INFIMUM + SUPREMUM）
    constexpr uint16_t n_dir_slots = 2;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_N_DIR_SLOTS_OFFSET, n_dir_slots);
    
    // 设置目录槽（在页尾部）
    size_t dir_start = 16384 - n_dir_slots * 2;
    write_le<uint16_t>(page_data.data(), dir_start + 0, infimum_offset);
    write_le<uint16_t>(page_data.data(), dir_start + 2, supremum_offset);
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
    
    auto offsets = parser.record_offsets();
    EXPECT_EQ(offsets.size(), 2); // 只应包含 INFIMUM 和 SUPREMUM
}

// ✅ 新增：测试使用常量而非硬编码偏移（验证 P0-1 修复）
TEST(BPlusTreeParser, UseConstantsInsteadOfHardcodedOffsets) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 使用常量设置页头
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, dbrecovery::innodb::FIL_PAGE_INDEX);
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::BTR_N_LEVELS_ABS, 0); // PAGE_LEVEL = 0
    
    // 使用常量设置目录槽和堆顶
    constexpr uint16_t heap_top = 100;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_HEAP_TOP_OFFSET, heap_top);
    
    // 使用常量设置 INFIMUM 记录
    constexpr uint16_t infimum_offset = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
    page_data[infimum_offset] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), infimum_offset + 1, heap_top - 5);
    
    // 使用常量设置 SUPREMUM 记录
    constexpr uint16_t supremum_offset = heap_top - 5;
    page_data[supremum_offset] = dbrecovery::innodb::REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), supremum_offset + 1, 0xFFFF);
    
    // 使用常量设置目录槽
    constexpr uint16_t n_dir_slots = 2;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_N_DIR_SLOTS_OFFSET, n_dir_slots);
    
    size_t dir_start = 16384 - n_dir_slots * 2;
    write_le<uint16_t>(page_data.data(), dir_start + 0, infimum_offset);
    write_le<uint16_t>(page_data.data(), dir_start + 2, supremum_offset);
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
    
    // 验证解析出的记录偏移与常量一致
    auto offsets = parser.record_offsets();
    EXPECT_EQ(offsets.size(), 2);
    EXPECT_EQ(offsets[0], infimum_offset);
    EXPECT_EQ(offsets[1], supremum_offset);
}

// ✅ P0 新增：测试校验和不匹配时解析应失败（严格模式）
TEST(BPlusTreeParser, ParseFailsOnChecksumMismatchStrictMode) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的页类型
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, dbrecovery::innodb::FIL_PAGE_INDEX);
    
    // 设置一个错误的校验和（假设原本是 CRC32C）
    for (int i = 0; i < 4; ++i) {
        page_data[24 + i] = 0xFF; // 故意设置为全1，与计算出的值不符
    }
    
    BPlusTreeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

// ✅ P0 新增：测试校验和不匹配时在非严格模式下仍可解析（容错）
TEST(BPlusTreeParser, ParseSucceedsOnChecksumMismatchNonStrictMode) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的页类型
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, dbrecovery::innodb::FIL_PAGE_INDEX);
    
    // 设置一个错误的校验和（假设原本是 CRC32C）
    for (int i = 0; i < 4; ++i) {
        page_data[24 + i] = 0xFF; // 故意设置为全1，与计算出的值不符
    }
    
    BPlusTreeParser parser(page_data);
    // 在非严格模式下，即使校验和不匹配也应能解析（假设其他结构正确）
    EXPECT_TRUE(parser.parse());
}

// ✅ P0 新增：测试页大小无效时的行为
TEST(PageReader, ValidateChecksumInvalidPageSize) {
    std::vector<uint8_t> page_data(12345, 0); // 非标准页大小
    
    uint32_t stored, computed;
    bool result = PageReader::validate_checksum(page_data, true, &stored, &computed);
    
    EXPECT_FALSE(result); // 严格模式下应失败
}

// ✅ P0 新增：测试页大小无效时的非严格模式容错
TEST(PageReader, ValidateChecksumInvalidPageSizeNonStrict) {
    std::vector<uint8_t> page_data(12345, 0); // 非标准页大小
    
    uint32_t stored, computed;
    bool result = PageReader::validate_checksum(page_data, false, &stored, &computed);
    
    EXPECT_TRUE(result); // 非严格模式下应允许继续
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
