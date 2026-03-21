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

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
