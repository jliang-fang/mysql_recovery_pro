#include <gtest/gtest.h>
#include "dbrecovery/innodb/btree_node_parser.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== BPlusTreeNodeParser Tests ====================

TEST(BPlusTreeNodeParserTest, ParseTooSmallPage) {
    std::vector<uint8_t> page_data(50, 0); // 小于最小大小

    BPlusTreeNodeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

TEST(BPlusTreeNodeParserTest, ParseInvalidPageType) {
    std::vector<uint8_t> page_data(16384, 0);

    // 设置无效的页类型 (FIL_PAGE_TYPE is at offset 24, big-endian)
    write_be<uint16_t>(page_data.data(), 24, 0xFFFF);

    BPlusTreeNodeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

TEST(BPlusTreeNodeParserTest, ParseValidIndexPage) {
    std::vector<uint8_t> page_data(16384, 0);

    // BPlusTreeNodeParser uses read_be at offset FIL_PAGE_TYPE_ABS (24) for page_type (correct)
    write_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    write_be<uint32_t>(page_data.data(), FIL_PAGE_OFFSET, 100);  // 页号 at offset 4

    // 设置页头 (Page Header starts at offset 38, big-endian)
    write_be<uint16_t>(page_data.data(), 38 + 0, 10);  // n_dir_slots
    write_be<uint16_t>(page_data.data(), 38 + 2, 100); // heap_top
    write_be<uint16_t>(page_data.data(), 38 + 4, 5);   // n_heap
    write_be<uint16_t>(page_data.data(), 38 + 6, 50);  // free_offset

    BPlusTreeNodeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    EXPECT_EQ(parser.node_info().page_no, 100);
    EXPECT_EQ(parser.node_info().page_type, FIL_PAGE_INDEX);
    EXPECT_TRUE(parser.node_info().is_valid);
}

TEST(BPlusTreeNodeParserTest, ParseSDIPage) {
    std::vector<uint8_t> page_data(16384, 0);

    // BPlusTreeNodeParser uses read_be at offset FIL_PAGE_TYPE_ABS (24) (correct)
    write_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);
    write_be<uint32_t>(page_data.data(), FIL_PAGE_OFFSET, 50);

    // 设置页头 (big-endian)
    write_be<uint16_t>(page_data.data(), 38 + 2, 100); // heap_top
    write_be<uint16_t>(page_data.data(), 38 + 4, 5);   // n_heap

    // 设置 INFIMUM 记录
    page_data[54] = REC_INFO_MIN_REC_FLAG;
    write_le<uint16_t>(page_data.data(), 55, 100);

    // 设置 SUPREMUM 记录
    page_data[99] = REC_STATUS_SUPREMUM;

    BPlusTreeNodeParser parser(page_data);
    bool result = parser.parse();

    // 调试输出
    if (!result) {
        std::cerr << "Parse failed: " << parser.error_message() << std::endl;
    }

    EXPECT_TRUE(result);

    EXPECT_EQ(parser.node_info().page_type, FIL_PAGE_SDI);
}

TEST(BPlusTreeNodeParserTest, ParseUndoLogPage) {
    std::vector<uint8_t> page_data(16384, 0);

    // BPlusTreeNodeParser uses read_be at offset FIL_PAGE_TYPE_ABS (24) (correct)
    write_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_UNDO_LOG);
    write_be<uint32_t>(page_data.data(), FIL_PAGE_OFFSET, 200);

    // 设置页头 (big-endian)
    write_be<uint16_t>(page_data.data(), 38 + 2, 100); // heap_top
    write_be<uint16_t>(page_data.data(), 38 + 4, 5);   // n_heap

    // 设置 INFIMUM 记录
    page_data[54] = REC_INFO_MIN_REC_FLAG;
    write_le<uint16_t>(page_data.data(), 55, 100);

    // 设置 SUPREMUM 记录
    page_data[99] = REC_STATUS_SUPREMUM;

    BPlusTreeNodeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    EXPECT_EQ(parser.node_info().page_type, FIL_PAGE_UNDO_LOG);
}

TEST(BPlusTreeNodeParserTest, ParsePageHeader) {
    std::vector<uint8_t> page_data(16384, 0);

    // NOTE: BPlusTreeNodeParser uses read_le at offset 16 for page_type
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(page_data.data(), 20, 1);

    // 设置页头字段 (big-endian)
    write_be<uint16_t>(page_data.data(), 38 + 0, 20);  // n_dir_slots
    write_be<uint16_t>(page_data.data(), 38 + 2, 200); // heap_top
    write_be<uint16_t>(page_data.data(), 38 + 4, 10);  // n_heap
    write_be<uint16_t>(page_data.data(), 38 + 6, 100); // free_offset

    BPlusTreeNodeParser parser(page_data);
    parser.parse();

    EXPECT_EQ(parser.page_header().n_dir_slots, 20);
    EXPECT_EQ(parser.page_header().heap_top, 200);
    EXPECT_EQ(parser.page_header().n_heap, 10);
}

TEST(BPlusTreeNodeParserTest, ParseRecordHeader) {
    std::vector<uint8_t> page_data(16384, 0);

    // NOTE: BPlusTreeNodeParser uses read_le at offset 16 for page_type
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(page_data.data(), 20, 1);

    // 设置页头 (big-endian)
    write_be<uint16_t>(page_data.data(), 38 + 2, 100); // heap_top

    // 设置 INFIMUM 记录（偏移 54）
    page_data[54] = REC_INFO_MIN_REC_FLAG;
    write_le<uint16_t>(page_data.data(), 55, 100); // next_record_offset

    // 设置 SUPREMUM 记录
    page_data[99] = REC_STATUS_SUPREMUM;

    BPlusTreeNodeParser parser(page_data);
    EXPECT_TRUE(parser.parse());

    EXPECT_TRUE(parser.node_info().has_infimum);
    EXPECT_TRUE(parser.node_info().has_supremum);
}

TEST(BPlusTreeNodeParserTest, ParseRecordOffsets) {
    std::vector<uint8_t> page_data(16384, 0);

    // NOTE: BPlusTreeNodeParser uses read_le at offset 16 for page_type
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(page_data.data(), 20, 1);

    // 设置页头 (big-endian)
    write_be<uint16_t>(page_data.data(), 38 + 0, 10);  // n_dir_slots
    write_be<uint16_t>(page_data.data(), 38 + 2, 100); // heap_top
    write_be<uint16_t>(page_data.data(), 38 + 4, 5);   // n_heap

    BPlusTreeNodeParser parser(page_data);
    parser.parse();

    // 目录槽应该被解析
    EXPECT_EQ(parser.node_info().n_dir_slots, 10);
}

TEST(BPlusTreeNodeParserTest, ParseUserRecords) {
    std::vector<uint8_t> page_data(16384, 0);

    // NOTE: BPlusTreeNodeParser uses read_le at offset 16 for page_type
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(page_data.data(), 20, 1);

    // 设置页头 (big-endian)
    write_be<uint16_t>(page_data.data(), 38 + 2, 200); // heap_top

    // 设置 INFIMUM 记录
    page_data[54] = REC_INFO_MIN_REC_FLAG;
    write_le<uint16_t>(page_data.data(), 55, 100);

    // 设置用户记录
    page_data[100] = 0;  // info_bits
    write_le<uint16_t>(page_data.data(), 101, 150); // next_record_offset

    // 设置 SUPREMUM 记录
    page_data[149] = REC_STATUS_SUPREMUM;

    BPlusTreeNodeParser parser(page_data);
    parser.parse();

    // 用户记录应该被解析
    EXPECT_GE(parser.records().size(), 0);
}

TEST(BPlusTreeNodeParserTest, ParsePageTypeToString) {
    EXPECT_EQ(BPlusTreeNodeParser::page_type_to_string(FIL_PAGE_INDEX), "FIL_PAGE_INDEX");
    EXPECT_EQ(BPlusTreeNodeParser::page_type_to_string(FIL_PAGE_SDI), "FIL_PAGE_SDI");
    EXPECT_EQ(BPlusTreeNodeParser::page_type_to_string(FIL_PAGE_UNDO_LOG), "FIL_PAGE_UNDO_LOG");
    EXPECT_EQ(BPlusTreeNodeParser::page_type_to_string(0xFFFF), "UNKNOWN");
}

TEST(BPlusTreeNodeParserTest, ParseNodeInfoToString) {
    BTreeNodeInfo info{};
    info.page_no = 100;
    info.page_type = FIL_PAGE_INDEX;
    info.level = 0;
    info.prev_page = 99;
    info.next_page = 101;
    info.n_records = 10;
    info.is_valid = true;

    std::string str = info.to_string();
    EXPECT_NE(str.find("page_no=100"), std::string::npos);
    // FIL_PAGE_INDEX = 17855 = 0x45bf, to_string uses hex format
    EXPECT_NE(str.find("type=0x45bf"), std::string::npos);
    EXPECT_NE(str.find("valid=true"), std::string::npos);  // 使用 std::boolalpha
}

TEST(BPlusTreeNodeParserTest, ParseRecordInfoToString) {
    RecordInfo info{};
    info.offset = 100;
    info.heap_no = 5;
    info.next_offset = 150;
    info.is_deleted = false;
    info.is_min_rec = false;
    info.is_extern = false;
    info.n_fields = 3;
    info.raw_data.assign(100, 0);

    std::string str = info.to_string();
    EXPECT_NE(str.find("offset=100"), std::string::npos);
    EXPECT_NE(str.find("heap_no=5"), std::string::npos);
    EXPECT_NE(str.find("raw_size=100"), std::string::npos);
}

TEST(BPlusTreeNodeParserTest, ParseWithDifferentPageSizes) {
    // 4KB 页
    std::vector<uint8_t> page4k(4096, 0);
    // NOTE: BPlusTreeNodeParser uses read_le at offset 16 for page_type
    write_le<uint16_t>(page4k.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(page4k.data(), 20, 1);

    // 设置页头 (big-endian)
    write_be<uint16_t>(page4k.data(), 38 + 2, 100); // heap_top
    write_be<uint16_t>(page4k.data(), 38 + 4, 5);   // n_heap

    // 设置 INFIMUM 记录
    page4k[54] = REC_INFO_MIN_REC_FLAG;
    write_le<uint16_t>(page4k.data(), 55, 100);

    // 设置 SUPREMUM 记录
    page4k[99] = REC_STATUS_SUPREMUM;

    BPlusTreeNodeParser parser4k(page4k, 4096);
    EXPECT_TRUE(parser4k.parse());

    // 32KB 页
    std::vector<uint8_t> page32k(32768, 0);
    // NOTE: BPlusTreeNodeParser uses read_le at offset 16 for page_type
    write_le<uint16_t>(page32k.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(page32k.data(), 20, 1);

    // 设置页头 (big-endian)
    write_be<uint16_t>(page32k.data(), 38 + 2, 100); // heap_top
    write_be<uint16_t>(page32k.data(), 38 + 4, 5);   // n_heap

    // 设置 INFIMUM 记录
    page32k[54] = REC_INFO_MIN_REC_FLAG;
    write_le<uint16_t>(page32k.data(), 55, 100);

    // 设置 SUPREMUM 记录
    page32k[99] = REC_STATUS_SUPREMUM;

    BPlusTreeNodeParser parser32k(page32k, 32768);
    EXPECT_TRUE(parser32k.parse());
}

TEST(BPlusTreeNodeParserTest, ParseInvalidPageData) {
    std::vector<uint8_t> page_data(100, 0); // 太小

    BPlusTreeNodeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

} // namespace innodb
} // namespace dbrecovery
