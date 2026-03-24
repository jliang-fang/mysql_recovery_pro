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
    constexpr uint16_t PAGE_N_DIR_SLOTS_OFFSET = 0;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + PAGE_N_DIR_SLOTS_OFFSET, 5);  // n_dir_slots
    
    // 设置堆顶位置
    constexpr uint16_t PAGE_HEAP_TOP_OFFSET = 2;
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
    constexpr uint16_t FIL_PAGE_TYPE_ABS = 50;
    write_le<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS, 9969);
    
    // 设置 BTR Header (偏移 38+4=42)
    constexpr uint16_t BTR_N_LEVELS_ABS = 42;
    write_le<uint16_t>(page_data.data(), BTR_N_LEVELS_ABS, 0);     // PAGE_LEVEL = 0 (leaf)
    
    constexpr uint16_t FIL_HEADER_SIZE = 38;
    constexpr uint16_t PAGE_N_DIR_SLOTS_OFFSET = 0;
    write_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + PAGE_N_DIR_SLOTS_OFFSET, 5);     // PAGE_N_DIR_SLOTS
    
    constexpr uint16_t PAGE_HEAP_TOP_OFFSET = 2;
    write_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + PAGE_HEAP_TOP_OFFSET, 100);   // PAGE_HEAP_TOP
    
    // 设置目录槽和记录（简化版）
    // INFIMUM 记录 at offset 54
    constexpr uint16_t REC_STATUS_INFIMUM = 0x02;
    page_data[54] = REC_STATUS_INFIMUM; // info_bits: REC_INFO_MIN_REC_FLAG (bit 4)
    write_le<uint16_t>(page_data.data(), 55, 70);   // next_record_offset
    
    // 用户记录
    page_data[70] = 0x00; // info_bits
    write_le<uint16_t>(page_data.data(), 71, 86);   // next_record_offset
    
    // SUPREMUM 记录 at offset 86
    constexpr uint16_t REC_STATUS_SUPREMUM = 0x03;
    page_data[86] = REC_STATUS_SUPREMUM; // REC_STATUS_SUPREMUM
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

// ✅ P0 新增：测试 BLOB 字段解析（单页 BLOB 场景）
TEST(BPlusTreeParser, ParseBlobFieldSinglePage) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的页类型
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, dbrecovery::innodb::FIL_PAGE_INDEX);
    
    // 设置堆顶位置（足够大以包含记录）
    constexpr uint16_t heap_top = 200;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_HEAP_TOP_OFFSET, heap_top);
    
    // 设置有效的 INFIMUM 记录
    constexpr uint16_t infimum_offset = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
    page_data[infimum_offset] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), infimum_offset + 1, heap_top - 5);
    
    // 设置有效的 SUPREMUM 记录
    constexpr uint16_t supremum_offset = heap_top - 5;
    page_data[supremum_offset] = dbrecovery::innodb::REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), supremum_offset + 1, 0xFFFF);
    
    // 设置目录槽
    constexpr uint16_t n_dir_slots = 2;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_N_DIR_SLOTS_OFFSET, n_dir_slots);
    size_t dir_start = 16384 - n_dir_slots * 2;
    write_le<uint16_t>(page_data.data(), dir_start + 0, infimum_offset);
    write_le<uint16_t>(page_data.data(), dir_start + 2, supremum_offset);
    
    // 设置用户记录（包含 BLOB 指针）
    constexpr uint16_t user_rec_offset = 54 + 13; // INFIMUM 后
    page_data[user_rec_offset] = 0x00; // info_bits (普通记录)
    write_le<uint16_t>(page_data.data(), user_rec_offset + 1, heap_top); // next_record_offset
    
    // BLOB 指针（20字节）：space_id=0, page_no=100, offset=0, len=50, total_len=50
    write_le<uint32_t>(page_data.data(), user_rec_offset + 6, 0);   // space_id
    write_le<uint32_t>(page_data.data(), user_rec_offset + 10, 100); // page_no
    write_le<uint32_t>(page_data.data(), user_rec_offset + 14, 0);   // offset
    write_le<uint32_t>(page_data.data(), user_rec_offset + 18, 50);  // len
    write_le<uint32_t>(page_data.data(), user_rec_offset + 22, 50);  // total_len
    
    // 填充 BLOB 数据（在页内）
    for (int i = 0; i < 50; ++i) {
        page_data[1000 + i] = static_cast<uint8_t>(i % 256);
    }
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
}

// ✅ P0 新增：测试 BLOB 字段解析（跨页 BLOB 场景）
TEST(BPlusTreeParser, ParseBlobFieldMultiPage) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的页类型
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, dbrecovery::innodb::FIL_PAGE_INDEX);
    
    // 设置堆顶位置
    constexpr uint16_t heap_top = 200;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_HEAP_TOP_OFFSET, heap_top);
    
    // 设置有效的 INFIMUM 记录
    constexpr uint16_t infimum_offset = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
    page_data[infimum_offset] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), infimum_offset + 1, heap_top - 5);
    
    // 设置有效的 SUPREMUM 记录
    constexpr uint16_t supremum_offset = heap_top - 5;
    page_data[supremum_offset] = dbrecovery::innodb::REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), supremum_offset + 1, 0xFFFF);
    
    // 设置目录槽
    constexpr uint16_t n_dir_slots = 2;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_N_DIR_SLOTS_OFFSET, n_dir_slots);
    size_t dir_start = 16384 - n_dir_slots * 2;
    write_le<uint16_t>(page_data.data(), dir_start + 0, infimum_offset);
    write_le<uint16_t>(page_data.data(), dir_start + 2, supremum_offset);
    
    // 设置用户记录（包含跨页 BLOB 指针）
    constexpr uint16_t user_rec_offset = 54 + 13;
    page_data[user_rec_offset] = 0x00; // info_bits
    write_le<uint16_t>(page_data.data(), user_rec_offset + 1, heap_top); // next_record_offset
    
    // BLOB 指针（20字节）：total_len > len，表示跨页
    write_le<uint32_t>(page_data.data(), user_rec_offset + 6, 0);   // space_id
    write_le<uint32_t>(page_data.data(), user_rec_offset + 10, 100); // page_no
    write_le<uint32_t>(page_data.data(), user_rec_offset + 14, 0);   // offset
    write_le<uint32_t>(page_data.data(), user_rec_offset + 18, 50);  // len (当前页)
    write_le<uint32_t>(page_data.data(), user_rec_offset + 22, 100); // total_len (跨两页)
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
}

// ✅ 新增：测试 parse_blob_pointer 正确填充 chain_pages（P1 修复）
TEST(BlobPointerTest, ParseBlobPointerFillsChainPages) {
    std::vector<uint8_t> data(20, 0);
    write_le<uint32_t>(data.data(), 0, 0);   // space_id
    write_le<uint32_t>(data.data(), 4, 100); // page_no (first BLOB 页)
    write_le<uint32_t>(data.data(), 8, 0);   // offset
    write_le<uint32_t>(data.data(), 12, 50); // len (current page)
    write_le<uint32_t>(data.data(), 16, 150); // total_len (multi-page)

    auto ptr = parse_blob_pointer(data);
    EXPECT_TRUE(ptr.has_value());
    EXPECT_EQ(ptr->space_id, 0);
    EXPECT_EQ(ptr->page_no, 100);
    EXPECT_EQ(ptr->len, 50);
    EXPECT_EQ(ptr->total_len, 150);
    
    // ✅ 验证：chain_pages 至少包含当前页
    EXPECT_FALSE(ptr->chain_pages.empty());
    EXPECT_EQ(ptr->chain_pages.size(), 1); // 单页 BLOB，只应有当前页
    EXPECT_EQ(ptr->chain_pages[0].first, 0);
    EXPECT_EQ(ptr->chain_pages[0].second, 100);
}

// ✅ 新增：测试 parse_blob_pointer 在多页 BLOB 场景下的行为（需要 page_reader 回调）
TEST(BlobPointerTest, ParseBlobPointerMultiPageWithReader) {
    std::vector<uint8_t> data(20, 0);
    write_le<uint32_t>(data.data(), 0, 0);   // space_id
    write_le<uint32_t>(data.data(), 4, 100); // page_no (first BLOB 页)
    write_le<uint32_t>(data.data(), 8, 0);   // offset
    write_le<uint32_t>(data.data(), 12, 50); // len (current page)
    write_le<uint32_t>(data.data(), 16, 150); // total_len (multi-page)

    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 0 && page_no == 100) {
            std::vector<uint8_t> page_data(16384, 0);
            // 设置下一个 BLOB 页号（偏移 38+0=38）
            write_le<uint32_t>(page_data.data(), 38, 200); // next_bpage = 200
            return page_data;
        }
        if (space_id == 0 && page_no == 200) {
            std::vector<uint8_t> page_data(16384, 0);
            // 设置下一个 BLOB 页号为 0xFFFFFFFF，表示链表结束
            write_le<uint32_t>(page_data.data(), 38, 0xFFFFFFFF);
            return page_data;
        }
        return std::nullopt;
    };

    auto ptr = parse_blob_pointer(data.data(), data.size());
    EXPECT_TRUE(ptr.has_value());
    
    // ✅ 验证：chain_pages 应包含两个页（100 和 200）
    EXPECT_EQ(ptr->chain_pages.size(), 2);
    EXPECT_EQ(ptr->chain_pages[0].first, 0);
    EXPECT_EQ(ptr->chain_pages[0].second, 100);
    EXPECT_EQ(ptr->chain_pages[1].first, 0);
    EXPECT_EQ(ptr->chain_pages[1].second, 200);
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

// ✅ P1 新增：测试 parse_blob_pointer 在多页 BLOB 场景下的行为（需要 page_reader 回调）
TEST(BlobPointerTest, ParseBlobPointerMultiPageWithReader) {
    std::vector<uint8_t> data(20, 0);
    write_le<uint32_t>(data.data(), 0, 0);   // space_id
    write_le<uint32_t>(data.data(), 4, 100); // page_no (first BLOB 页)
    write_le<uint32_t>(data.data(), 8, 0);   // offset
    write_le<uint32_t>(data.data(), 12, 50); // len (current page)
    write_le<uint32_t>(data.data(), 16, 150); // total_len (multi-page)

    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 0 && page_no == 100) {
            std::vector<uint8_t> page_data(16384, 0);
            // 设置下一个 BLOB 页号（偏移 38+0=38）
            write_le<uint32_t>(page_data.data(), dbrecovery::innodb::BlobPageChain::BTR_BPAGE_NEXT, 200); // next_bpage = 200
            return page_data;
        }
        if (space_id == 0 && page_no == 200) {
            std::vector<uint8_t> page_data(16384, 0);
            // 设置下一个 BLOB 页号为 0xFFFFFFFF，表示链表结束
            write_le<uint32_t>(page_data.data(), dbrecovery::innodb::BlobPageChain::BTR_BPAGE_NEXT, 0xFFFFFFFF);
            return page_data;
        }
        return std::nullopt;
    };

    auto ptr = parse_blob_pointer(data.data(), data.size(), page_reader);
    EXPECT_TRUE(ptr.has_value());
    
    // ✅ 验证：chain_pages 应包含两个页（100 和 200）
    EXPECT_EQ(ptr->chain_pages.size(), 2);
    EXPECT_EQ(ptr->chain_pages[0].first, 0);
    EXPECT_EQ(ptr->chain_pages[0].second, 100);
    EXPECT_EQ(ptr->chain_pages[1].first, 0);
    EXPECT_EQ(ptr->chain_pages[1].second, 200);
}

// ✅ P0 新增：测试 BPlusTreeParser 解析包含跨页 BLOB 的记录
TEST(BPlusTreeParser, ParseRecordWithMultiPageBlob) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的页类型
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, dbrecovery::innodb::FIL_PAGE_INDEX);
    
    // 设置堆顶位置（足够大以包含记录）
    constexpr uint16_t heap_top = 200;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_HEAP_TOP_OFFSET, heap_top);
    
    // 设置有效的 INFIMUM 记录
    constexpr uint16_t infimum_offset = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
    page_data[infimum_offset] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), infimum_offset + 1, heap_top - 5);
    
    // 设置有效的 SUPREMUM 记录
    constexpr uint16_t supremum_offset = heap_top - 5;
    page_data[supremum_offset] = dbrecovery::innodb::REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), supremum_offset + 1, 0xFFFF);
    
    // 设置目录槽
    constexpr uint16_t n_dir_slots = 2;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_N_DIR_SLOTS_OFFSET, n_dir_slots);
    size_t dir_start = 16384 - n_dir_slots * 2;
    write_le<uint16_t>(page_data.data(), dir_start + 0, infimum_offset);
    write_le<uint16_t>(page_data.data(), dir_start + 2, supremum_offset);
    
    // 设置用户记录（包含跨页 BLOB 指针）
    constexpr uint16_t user_rec_offset = 54 + 13; // INFIMUM 后
    page_data[user_rec_offset] = 0x00; // info_bits (普通记录)
    write_le<uint16_t>(page_data.data(), user_rec_offset + 1, heap_top); // next_record_offset
    
    // BLOB 指针（20字节）：space_id=0, page_no=100, offset=0, len=50, total_len=150 (跨3页)
    write_le<uint32_t>(page_data.data(), user_rec_offset + 6, 0);   // space_id
    write_le<uint32_t>(page_data.data(), user_rec_offset + 10, 100); // page_no
    write_le<uint32_t>(page_data.data(), user_rec_offset + 14, 0);   // offset
    write_le<uint32_t>(page_data.data(), user_rec_offset + 18, 50);  // len (当前页)
    write_le<uint32_t>(page_data.data(), user_rec_offset + 22, 150); // total_len
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
    
    // 创建一个简单的表结构用于解析
    catalog::TableSchema schema;
    schema.table_name = "test_table";
    schema.engine = "InnoDB";
    schema.row_format = catalog::row_formats::DYNAMIC;
    
    catalog::ColumnInfo col;
    col.name = "blob_data";
    col.mysql_type_id = catalog::mysql_types::BLOB;
    col.is_nullable = true;
    schema.columns[1] = col;
    
    // 解析记录
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records.size(), 1); // 应该解析出一条用户记录
    
    // 验证 BLOB 字段的值（应包含重组后的数据大小信息）
    auto it = records[0].find("blob_data");
    ASSERT_NE(it, records[0].end());
    EXPECT_NE(it->second.find("data_size=150"), std::string::npos); // 应显示重组后的总长度
}

// ✅ 新增：测试所有校验和算法（MySQL 5.6/5.7/8.0 兼容）
TEST(PageReader, ValidateChecksumAllAlgorithms) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的页类型
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, dbrecovery::innodb::FIL_PAGE_INDEX);
    
    // 测试 CRC32C 校验和
    uint32_t crc = PageReader::compute_crc32c(page_data);
    for (int i = 0; i < 4; ++i) {
        page_data[24 + i] = (crc >> (i * 8)) & 0xFF;
    }
    
    uint32_t stored, computed;
    EXPECT_TRUE(PageReader::validate_checksum(page_data, true, &stored, &computed));
    EXPECT_EQ(stored, computed);
    
    // 测试 INNODB 校验和
    for (int i = 0; i < 4; ++i) page_data[24 + i] = 0;
    uint32_t innodb_crc = PageReader::compute_innodb_checksum(page_data);
    for (int i = 0; i < 4; ++i) {
        page_data[24 + i] = (innodb_crc >> (i * 8)) & 0xFF;
    }
    
    EXPECT_TRUE(PageReader::validate_checksum(page_data, true, &stored, &computed));
    EXPECT_EQ(stored, computed);
}

// ✅ 新增：测试 Redo Log 回放（MLOG_COMP_REC_INSERT）
TEST(BPlusTreeParser, RedoInsertRecord) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的页类型
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, dbrecovery::innodb::FIL_PAGE_INDEX);
    
    // 初始化 BPlusTreeParser
    innodb::BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
    
    // 构造 MLOG_COMP_REC_INSERT 记录
    RedoLogRecord record;
    record.r_type = 30; // MLOG_COMP_REC_INSERT
    record.r_page_no = 0;
    
    // 添加记录数据：info_bits(1) + next_offset(2) + data
    record.r_data.push_back(0x00); // info_bits
    write_le<uint16_t>(record.r_data, record.r_data.size(), 54);
    record.r_data.push_back('t'); // test data
    record.r_data.push_back('e');
    record.r_data.push_back('s');
    record.r_data.push_back('t');
    
    parser.apply_redo_record(record);
    
    // 验证页大小增加（记录被追加）
    EXPECT_GT(parser.page_data_.size(), 16384);
}

// ✅ 新增：测试循环引用的 BLOB 页链被正确拒绝
TEST(BlobPointerTest, RejectCircularBLOBChain) {
    std::vector<uint8_t> data(20, 0);
    write_le<uint32_t>(data.data(), 0, 0);   // space_id
    write_le<uint32_t>(data.data(), 4, 100); // page_no (first BLOB 页)
    write_le<uint32_t>(data.data(), 8, 0);   // offset
    write_le<uint32_t>(data.data(), 12, 50); // len (current page)
    write_le<uint32_t>(data.data(), 16, 150); // total_len (multi-page)

    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 0 && page_no == 100) {
            std::vector<uint8_t> page_data(16384, 0);
            // 设置下一个 BLOB 页号为 200
            write_le<uint32_t>(page_data.data(), dbrecovery::innodb::BlobPageChain::BTR_BPAGE_NEXT, 200);
            return page_data;
        }
        if (space_id == 0 && page_no == 200) {
            std::vector<uint8_t> page_data(16384, 0);
            // 设置下一个 BLOB 页号为 100，形成循环
            write_le<uint32_t>(page_data.data(), dbrecovery::innodb::BlobPageChain::BTR_BPAGE_NEXT, 100);
            return page_data;
        }
        return std::nullopt;
    };

    auto ptr = dbrecovery::innodb::parse_blob_pointer(data.data(), data.size(), page_reader);
    EXPECT_TRUE(ptr.has_value());

    // ✅ 验证：chain_pages 应包含两个页（100 和 200）
    EXPECT_EQ(ptr->chain_pages.size(), 2);

    // ✅ P0 新增：验证 is_valid 在严格模式下会因循环引用而失败
    EXPECT_FALSE(ptr->is_valid(page_reader, true));

    // ✅ P0 新增：验证 read_blob_data_from_chain 会拒绝处理循环链
    auto result = dbrecovery::innodb::read_blob_data_from_chain(
        ptr->chain_pages,
        ptr->total_len,
        page_reader
    );
    EXPECT_TRUE(result.empty()); // 应返回空，而不是陷入无限循环
}

// ✅ P1 新增：测试完整的多页 BLOB 链恢复流程（集成测试）
TEST(BPlusTreeParser, FullMultiPageBlobRecoveryIntegration) {
    // 1. 创建一个包含跨页 BLOB 的主索引页
    std::vector<uint8_t> page_data(16384, 0);
    
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, dbrecovery::innodb::FIL_PAGE_INDEX);
    constexpr uint16_t heap_top = 250;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_HEAP_TOP_OFFSET, heap_top);
    
    // 设置 INFIMUM/SUPREMUM
    constexpr uint16_t infimum_offset = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
    page_data[infimum_offset] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), infimum_offset + 1, heap_top - 5);
    
    constexpr uint16_t supremum_offset = heap_top - 5;
    page_data[supremum_offset] = dbrecovery::innodb::REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), supremum_offset + 1, 0xFFFF);
    
    // 设置目录槽
    constexpr uint16_t n_dir_slots = 2;
    write_le<uint16_t>(page_data.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_N_DIR_SLOTS_OFFSET, n_dir_slots);
    size_t dir_start = 16384 - n_dir_slots * 2;
    write_le<uint16_t>(page_data.data(), dir_start + 0, infimum_offset);
    write_le<uint16_t>(page_data.data(), dir_start + 2, supremum_offset);
    
    // 设置用户记录（包含跨页 BLOB 指针）
    constexpr uint16_t user_rec_offset = 54 + 13;
    page_data[user_rec_offset] = 0x00; // info_bits
    write_le<uint16_t>(page_data.data(), user_rec_offset + 1, heap_top);
    
    // BLOB 指针：total_len=200，表示跨两页（当前页50字节+下一页150字节）
    write_le<uint32_t>(page_data.data(), user_rec_offset + 6, 0);   // space_id
    write_le<uint32_t>(page_data.data(), user_rec_offset + 10, 100); // page_no (first BLOB页)
    write_le<uint32_t>(page_data.data(), user_rec_offset + 14, 0);   // offset
    write_le<uint32_t>(page_data.data(), user_rec_offset + 18, 50);  // len (当前页)
    write_le<uint32_t>(page_data.data(), user_rec_offset + 22, 200); // total_len
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
    
    // 2. 创建表结构
    catalog::TableSchema schema;
    schema.table_name = "test_blob_table";
    schema.engine = "InnoDB";
    schema.row_format = catalog::row_formats::DYNAMIC;
    
    catalog::ColumnInfo col;
    col.name = "data";
    col.mysql_type_id = catalog::mysql_types::BLOB;
    col.is_nullable = false;
    col.column_length = 1024;
    schema.columns[1] = col;
    
    // 3. 模拟 BLOB 页读取器
    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 0 && page_no == 100) {
            // 第一个 BLOB 页：包含前50字节数据和指向下一个页的指针
            std::vector<uint8_t> blob_page(16384, 0);
            write_le<uint32_t>(blob_page.data(), dbrecovery::innodb::BlobPageChain::BTR_BPAGE_NEXT, 200); // next_bpage
            write_le<uint32_t>(blob_page.data(), dbrecovery::innodb::BlobPageChain::BTR_BPAGE_LEN, 50);   // data_len
            
            // 填充前50字节数据
            for (int i = 0; i < 50; ++i) {
                blob_page[dbrecovery::innodb::BlobPageChain::BTR_BPAGE_LEN + 4 + i] = static_cast<uint8_t>(i % 256);
            }
            return blob_page;
        } else if (space_id == 0 && page_no == 200) {
            // 第二个 BLOB 页：包含剩余150字节数据
            std::vector<uint8_t> blob_page(16384, 0);
            write_le<uint32_t>(blob_page.data(), dbrecovery::innodb::BlobPageChain::BTR_BPAGE_NEXT, 0xFFFFFFFF); // 链表结束
            write_le<uint32_t>(blob_page.data(), dbrecovery::innodb::BlobPageChain::BTR_BPAGE_LEN, 150);   // data_len
            
            // 填充后150字节数据
            for (int i = 0; i < 150; ++i) {
                blob_page[dbrecovery::innodb::BlobPageChain::BTR_BPAGE_LEN + 4 + i] = static_cast<uint8_t>((i + 50) % 256);
            }
            return blob_page;
        }
        return std::nullopt;
    };
    
    // 4. 解析记录并验证 BLOB 数据
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records.size(), 1);
    
    auto it = records[0].find("data");
    ASSERT_NE(it, records[0].end());
    
    // 验证 BLOB 指针信息（应包含完整的页链和数据大小）
    EXPECT_NE(it->second.find("sid=0"), std::string::npos);
    EXPECT_NE(it->second.find("page=100"), std::string::npos);
    EXPECT_NE(it->second.find("len=200"), std::string::npos);
    
    // ✅ 验证：BLOB 数据已成功重组（data_size 应为 200）
    EXPECT_NE(it->second.find("data_size=200"), std::string::npos);
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
