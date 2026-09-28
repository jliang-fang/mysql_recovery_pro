#include <gtest/gtest.h>
#include "dbrecovery/innodb/page_types_v2.hpp"
#include "dbrecovery/innodb/auto_detector.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== PageHeaderV2 Tests ====================

TEST(PageHeaderV2Test, ParseBasicHeader) {
    std::vector<uint8_t> page(16384, 0);
    
    // Set FIL Header
    write_le<uint32_t>(page.data(), 0, 100);   // prev_page
    write_le<uint32_t>(page.data(), 4, 102);   // next_page
    write_le<uint64_t>(page.data(), 8, 1000);  // lsn
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);  // page_type
    write_le<uint32_t>(page.data(), 18, 1);    // space_id
    
    // Set PAGE Header
    write_le<uint16_t>(page.data(), 22, 0);    // level (leaf)
    write_le<uint16_t>(page.data(), 24, 4);    // n_dir_slots
    write_le<uint16_t>(page.data(), 26, 100);  // heap_top
    write_le<uint16_t>(page.data(), 28, 6);    // n_heap (4 records + 2 system)
    write_le<uint16_t>(page.data(), 30, 50);   // free
    write_le<uint16_t>(page.data(), 32, 10);   // garbage
    write_le<uint16_t>(page.data(), 34, 60);   // last_insert
    write_le<uint16_t>(page.data(), 36, 1);    // direction (right)
    write_le<uint16_t>(page.data(), 38, 3);    // n_direction
    write_le<uint16_t>(page.data(), 40, 4);    // n_recs
    
    // Set max_trx_id
    page[42] = 0x00; page[43] = 0x00; page[44] = 0x00;
    page[45] = 0x00; page[46] = 0x00; page[47] = 0x01;
    
    PageHeaderV2 header;
    bool success = header.parse(page.data(), page.size());
    
    EXPECT_TRUE(success);
    EXPECT_EQ(header.prev_page, 100);
    EXPECT_EQ(header.next_page, 102);
    EXPECT_EQ(header.lsn, 1000);
    EXPECT_EQ(header.type, FIL_PAGE_INDEX);
    EXPECT_EQ(header.space_id, 1);
    EXPECT_EQ(header.level, 0);
    EXPECT_EQ(header.n_dir_slots, 4);
    EXPECT_EQ(header.heap_top, 100);
    EXPECT_EQ(header.n_heap, 6);
    EXPECT_EQ(header.n_recs, 4);
}

TEST(PageHeaderV2Test, ParseMySQL80Page) {
    std::vector<uint8_t> page(16384, 0);
    
    // MySQL 8.0+ 特征
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(page.data(), 18, 4294967295U);  // space_id = 0xFFFFFFFF
    write_le<uint16_t>(page.data(), 22, 2);  // level
    
    PageHeaderV2 header;
    header.parse(page.data(), page.size());
    
    EXPECT_EQ(header.version_detected, MySQLVersion::MYSQL_80);
    EXPECT_EQ(header.space_id, 4294967295U);
}

TEST(PageHeaderV2Test, ParseCompressedPage) {
    std::vector<uint8_t> page(16384, 0);
    
    // 压缩页特征
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    write_le<uint16_t>(page.data(), 60, 0x1000);  // zip_size = 4096
    
    PageHeaderV2 header;
    header.parse(page.data(), page.size());
    
    EXPECT_TRUE(header.is_compressed);
    EXPECT_EQ(header.zip_size, 4096);
    EXPECT_EQ(header.get_page_size(), 4096);
}

TEST(PageHeaderV2Test, IsIndexPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    PageHeaderV2 header;
    header.parse(page.data(), page.size());
    
    EXPECT_TRUE(header.is_index_page());
}

TEST(PageHeaderV2Test, IsUndoPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_UNDO_LOG);
    
    PageHeaderV2 header;
    header.parse(page.data(), page.size());
    
    EXPECT_TRUE(header.is_undo_page());
}

TEST(PageHeaderV2Test, IsSDIPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_SDI);
    
    PageHeaderV2 header;
    header.parse(page.data(), page.size());
    
    EXPECT_TRUE(header.is_sdi_page());
}

TEST(PageHeaderV2Test, ParseSDI2Page) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, 0x5a5a);  // SDI2
    
    PageHeaderV2 header;
    header.parse(page.data(), page.size());
    
    EXPECT_TRUE(header.is_sdi_page());
}

TEST(PageHeaderV2Test, ParseSmallPage) {
    std::vector<uint8_t> page(100, 0);
    
    PageHeaderV2 header;
    bool success = header.parse(page.data(), page.size());
    
    EXPECT_FALSE(success);
}

TEST(PageHeaderV2Test, ParseHeaderToString) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    PageHeaderV2 header;
    header.parse(page.data(), page.size());
    
    std::string str = header.to_string();
    EXPECT_FALSE(str.empty());
    EXPECT_NE(str.find("PageHeaderV2"), std::string::npos);
}

// ==================== RecordHeaderV2 Tests ====================

TEST(RecordHeaderV2Test, ParseCompactRecord) {
    std::vector<uint8_t> record(6, 0);
    
    // Compact record header (5 bytes)
    record[0] = 0x00;  // n_owned = 0
    record[1] = 0x01;  // heap_no low
    record[2] = 0x00;  // record_type = 0, next_offset high
    record[3] = 0x42;  // next_offset low
    record[4] = 0x05;  // n_fields
    
    RecordHeaderV2 header;
    bool success = header.parse(record.data(), record.size(), RowFormat::COMPACT);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(header.get_next_record_offset(), 0x42);
    EXPECT_EQ(header.get_heap_no(), 1);
    EXPECT_EQ(header.n_fields, 5);
}

TEST(RecordHeaderV2Test, ParseRedundantRecord) {
    std::vector<uint8_t> record(7, 0);
    
    // Redundant record header (6 bytes)
    record[0] = 0x00;
    record[1] = 0x01;
    record[2] = 0x00;
    record[3] = 0x42;
    record[4] = 0x05;  // n_fields
    record[5] = 0x06;  // extra_bytes = 6 for redundant
    
    RecordHeaderV2 header;
    bool success = header.parse(record.data(), record.size(), RowFormat::REDUNDANT);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(header.extra_bytes, 6);
}

TEST(RecordHeaderV2Test, IsInfimum) {
    std::vector<uint8_t> record(5, 0);
    record[2] = 0x02;  // record_type = INFIMUM
    
    RecordHeaderV2 header;
    header.parse(record.data(), record.size(), RowFormat::COMPACT);
    
    EXPECT_TRUE(header.is_infimum());
}

TEST(RecordHeaderV2Test, IsSupremum) {
    std::vector<uint8_t> record(5, 0);
    record[2] = 0x03;  // record_type = SUPREMUM
    
    RecordHeaderV2 header;
    header.parse(record.data(), record.size(), RowFormat::COMPACT);
    
    EXPECT_TRUE(header.is_supremum());
}

TEST(RecordHeaderV2Test, ParseDeletedRecord) {
    std::vector<uint8_t> record(5, 0);
    record[3] = 0x80;  // is_deleted flag
    
    RecordHeaderV2 header;
    header.parse(record.data(), record.size(), RowFormat::COMPACT);
    
    EXPECT_TRUE(header.is_deleted);
}

TEST(RecordHeaderV2Test, ParseRecordToString) {
    std::vector<uint8_t> record(5, 0);
    record[0] = 0x00;
    record[1] = 0x01;
    record[2] = 0x00;
    record[3] = 0x42;
    record[4] = 0x05;
    
    RecordHeaderV2 header;
    header.parse(record.data(), record.size(), RowFormat::COMPACT);
    
    std::string str = header.to_string();
    EXPECT_FALSE(str.empty());
}

// ==================== CompactRow Tests ====================

TEST(CompactRowTest, ParseBasicRow) {
    std::vector<uint8_t> data(20, 0);
    
    // 变长字段长度列表
    data[0] = 0x05;  // field 0 length = 5
    data[1] = 0x03;  // field 1 length = 3
    
    // NULL 位图
    data[2] = 0x00;  // no nulls
    
    // 列数据
    for (int i = 0; i < 5; i++) data[3 + i] = 'A' + i;
    for (int i = 0; i < 3; i++) data[8 + i] = 'a' + i;
    
    // 事务ID
    data[11] = 0x00; data[12] = 0x00; data[13] = 0x00;
    data[14] = 0x00; data[15] = 0x00; data[16] = 0x01;
    
    // roll_pointer (7 bytes)
    data[17] = 0x00; data[18] = 0x00; data[19] = 0x00;
    
    CompactRow row;
    std::vector<bool> null_flags = {false, false};
    bool success = row.parse(data.data(), 2, null_flags, true);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(row.columns.size(), 2);
    EXPECT_EQ(row.columns[0].length, 5);
    EXPECT_EQ(row.columns[1].length, 3);
}

TEST(CompactRowTest, ParseExternalColumn) {
    std::vector<uint8_t> data(30, 0);
    
    // 外部存储标志 (0x80)
    data[0] = 0x85;  // external column, 0x80 | 5
    
    // NULL 位图
    data[1] = 0x00;
    
    // 列数据 (5 bytes)
    for (int i = 0; i < 5; i++) data[2 + i] = 'X';
    
    // 外部指针 (20 bytes)
    for (int i = 0; i < 20; i++) data[7 + i] = 0xFF;
    
    CompactRow row;
    std::vector<bool> null_flags = {false};
    bool success = row.parse(data.data(), 1, null_flags, true);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(row.columns[0].is_external, true);
    EXPECT_EQ(row.columns[0].length, 5);
}

TEST(CompactRowTest, ParseNullColumn) {
    std::vector<uint8_t> data(5, 0);
    
    // NULL 位图
    data[0] = 0x01;  // first column is null
    
    CompactRow row;
    std::vector<bool> null_flags = {true};
    bool success = row.parse(data.data(), 1, null_flags, false);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(row.columns[0].is_null, true);
}

// ==================== DynamicRow Tests ====================

TEST(DynamicRowTest, ParseBasicRow) {
    std::vector<uint8_t> data(20, 0);
    
    DynamicRow row;
    std::vector<bool> null_flags = {false, false};
    bool success = row.parse(data.data(), 2, null_flags, true);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(row.format, RowFormat::DYNAMIC);
}

TEST(DynamicRowTest, ParseInstantDDL) {
    std::vector<uint8_t> data(20, 0);
    
    DynamicRow row;
    std::vector<bool> null_flags = {false};
    row.parse(data.data(), 1, null_flags, true);
    
    // Instant DDL 元数据应该被初始化
    EXPECT_EQ(row.instant_n_cols, 0);
    EXPECT_EQ(row.default_n_cols, 0);
}

// ==================== RedundantRow Tests ====================

TEST(RedundantRowTest, ParseBasicRow) {
    std::vector<uint8_t> data(10, 0);
    
    // 变长字段长度列表 (2字节/字段)
    write_le<uint16_t>(data.data(), 0, 5);
    write_le<uint16_t>(data.data(), 2, 3);
    
    RedundantRow row;
    bool success = row.parse(data.data(), 2, true);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(row.format, RowFormat::REDUNDANT);
    EXPECT_EQ(row.n_fields, 2);
}

TEST(RedundantRowTest, ParseWithNulls) {
    std::vector<uint8_t> data(10, 0);
    
    write_le<uint16_t>(data.data(), 0, 5);
    write_le<uint16_t>(data.data(), 2, 3);
    
    RedundantRow row;
    bool success = row.parse(data.data(), 2, true);
    
    EXPECT_TRUE(success);
}

// ==================== IndexPageV2 Tests ====================

TEST(IndexPageV2Test, ParseBasicIndexPage) {
    std::vector<uint8_t> page(16384, 0);
    
    // FIL Header
    write_le<uint32_t>(page.data(), 0, 100);   // prev_page
    write_le<uint32_t>(page.data(), 4, 102);   // next_page
    write_le<uint64_t>(page.data(), 8, 1000);  // lsn
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(page.data(), 18, 1);    // space_id
    
    // PAGE Header
    write_le<uint16_t>(page.data(), 22, 0);    // level
    write_le<uint16_t>(page.data(), 24, 4);    // n_dir_slots
    write_le<uint16_t>(page.data(), 26, 100);  // heap_top
    write_le<uint16_t>(page.data(), 28, 6);    // n_heap
    
    IndexPageV2 index_page;
    bool success = index_page.parse(page.data(), page.size(), RowFormat::COMPACT);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(index_page.header.type, FIL_PAGE_INDEX);
    EXPECT_EQ(index_page.level, 0);
}

TEST(IndexPageV2Test, ParseWithRecords) {
    std::vector<uint8_t> page(16384, 0);
    
    // FIL Header
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    // PAGE Header
    write_le<uint16_t>(page.data(), 24, 4);    // n_dir_slots
    write_le<uint16_t>(page.data(), 26, 100);  // heap_top
    write_le<uint16_t>(page.data(), 28, 6);    // n_heap
    
    // Directory slots
    write_le<uint16_t>(page.data(), 74, 54);   // INFIMUM
    write_le<uint16_t>(page.data(), 76, 99);   // SUPREMUM
    
    // INFIMUM record
    page[54] = 0x02;  // INFIMUM type
    write_le<uint16_t>(page.data(), 55, 99);   // next = SUPREMUM
    
    // SUPREMUM record
    page[99] = 0x03;  // SUPREMUM type
    
    IndexPageV2 index_page;
    bool success = index_page.parse(page.data(), page.size(), RowFormat::COMPACT);
    
    EXPECT_TRUE(success);
    EXPECT_GE(index_page.records.size(), 2);
}

TEST(IndexPageV2Test, GetNextPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint32_t>(page.data(), 4, 100);   // next_page
    
    IndexPageV2 index_page;
    index_page.parse(page.data(), page.size(), RowFormat::COMPACT);
    
    EXPECT_EQ(index_page.get_next_page(), 100);
}

TEST(IndexPageV2Test, GetPrevPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint32_t>(page.data(), 0, 50);    // prev_page
    
    IndexPageV2 index_page;
    index_page.parse(page.data(), page.size(), RowFormat::COMPACT);
    
    EXPECT_EQ(index_page.get_prev_page(), 50);
}

// ==================== BPlusTreeIterator Tests ====================

TEST(BPlusTreeIteratorTest, HasNext) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    IndexPageV2 index_page;
    index_page.parse(page.data(), page.size(), RowFormat::COMPACT);
    
    auto page_ptr = std::make_shared<IndexPageV2>(index_page);
    BPlusTreeIterator iter(page_ptr, true);
    
    // Empty page, no next
    EXPECT_FALSE(iter.has_next());
}

TEST(BPlusTreeIteratorTest, Next) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    IndexPageV2 index_page;
    index_page.parse(page.data(), page.size(), RowFormat::COMPACT);
    
    auto page_ptr = std::make_shared<IndexPageV2>(index_page);
    BPlusTreeIterator iter(page_ptr, true);
    
    EXPECT_EQ(iter.next(), nullptr);
}

// ==================== AutoDetector Tests ====================

TEST(AutoDetectorTest, DetectVersionFromSDI) {
    std::vector<uint8_t> sdi_page(16384, 0);
    write_be<uint16_t>(sdi_page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);

    auto version = AutoDetector::detect_version_from_sdi(sdi_page.data());
    EXPECT_EQ(version, MySQLVersion::MYSQL_80);
}

TEST(AutoDetectorTest, GenericPage0IsInconclusive) {
    std::vector<uint8_t> page0(16384, 0);
    write_be<uint16_t>(page0.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    write_be<uint32_t>(page0.data(), FIL_PAGE_SPACE_ID, 1);

    auto version = AutoDetector::detect_version_from_page0(page0.data(), page0.size());
    EXPECT_EQ(version, MySQLVersion::UNKNOWN);
}

TEST(AutoDetectorTest, DetectVersionFromPage0RejectsTruncatedInput) {
    std::vector<uint8_t> page0(FIL_PAGE_SPACE_ID + sizeof(uint32_t), 0);
    write_be<uint16_t>(page0.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    write_be<uint32_t>(page0.data(), FIL_PAGE_SPACE_ID, 1);

    EXPECT_EQ(AutoDetector::detect_version_from_page0(page0.data(), page0.size() - 1),
              MySQLVersion::UNKNOWN);
    EXPECT_EQ(AutoDetector::detect_version_from_page0(nullptr, 0), MySQLVersion::UNKNOWN);
}

TEST(AutoDetectorTest, DetectRowFormatFromHeader) {
    std::vector<uint8_t> record(6, 0);
    record[4] = 0x06;  // extra_bytes = 6 for redundant
    
    auto format = AutoDetector::detect_row_format_from_header(record.data());
    EXPECT_EQ(format, RowFormat::REDUNDANT);
}

TEST(AutoDetectorTest, DetectRowFormatCompact) {
    std::vector<uint8_t> record(5, 0);
    record[4] = 0x05;  // extra_bytes = 5 for compact
    
    auto format = AutoDetector::detect_row_format_from_header(record.data());
    EXPECT_EQ(format, RowFormat::COMPACT);
}

TEST(AutoDetectorTest, DetectVersionFromPageType) {
    EXPECT_EQ(AutoDetector::detect_version_from_page_type(FIL_PAGE_SDI),
              MySQLVersion::MYSQL_80);
    EXPECT_EQ(AutoDetector::detect_version_from_page_type(0x45ba),
              MySQLVersion::UNKNOWN);
}

TEST(AutoDetectorTest, DetectVersionFromSpaceID) {
    EXPECT_EQ(AutoDetector::detect_version_from_space_id(0),
              MySQLVersion::UNKNOWN);
    EXPECT_EQ(AutoDetector::detect_version_from_space_id(1),
              MySQLVersion::UNKNOWN);
}

TEST(AutoDetectorTest, DetectVersionFromPageSize) {
    EXPECT_EQ(AutoDetector::detect_version_from_page_size(16384),
              MySQLVersion::UNKNOWN);
    EXPECT_EQ(AutoDetector::detect_version_from_page_size(4096),
              MySQLVersion::UNKNOWN);
}

TEST(AutoDetectorTest, DetectFullFile) {
    std::vector<uint8_t> file_data(16384, 0);
    write_be<uint16_t>(file_data.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    write_be<uint32_t>(file_data.data(), FIL_PAGE_SPACE_ID, 1);

    auto result = AutoDetector::detect_from_file_header(file_data.data(), file_data.size());

    EXPECT_EQ(result.version, MySQLVersion::UNKNOWN);
    EXPECT_FALSE(result.has_sdi);
    EXPECT_EQ(result.row_format, RowFormat::COMPACT);
}

// ==================== Edge Case Tests ====================

TEST(PageHeaderV2Test, ParseNullData) {
    PageHeaderV2 header;
    bool success = header.parse(nullptr, 0);
    EXPECT_FALSE(success);
}

TEST(RecordHeaderV2Test, ParseNullData) {
    RecordHeaderV2 header;
    bool success = header.parse(nullptr, 0, RowFormat::COMPACT);
    EXPECT_FALSE(success);
}

TEST(IndexPageV2Test, ParseNonIndexPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_UNDO_LOG);  // Not index page
    
    IndexPageV2 index_page;
    bool success = index_page.parse(page.data(), page.size(), RowFormat::COMPACT);
    
    EXPECT_FALSE(success);
}

TEST(AutoDetectorTest, DetectFromEmptyFile) {
    auto result = AutoDetector::detect_from_file_header(nullptr, 0);
    EXPECT_EQ(result.version, MySQLVersion::UNKNOWN);
}

// ==================== Utility Function Tests ====================

TEST(PageTypesV2Test, PageTypeToString) {
    EXPECT_EQ(page_type_to_string(PageType::INDEX), "FIL_PAGE_INDEX");
    EXPECT_EQ(page_type_to_string(PageType::SDI), "FIL_PAGE_SDI");
    EXPECT_EQ(page_type_to_string(PageType::UNDO_LOG), "FIL_PAGE_UNDO_LOG");
}

TEST(PageTypesV2Test, RowFormatToString) {
    EXPECT_EQ(row_format_to_string(RowFormat::COMPACT), "COMPACT");
    EXPECT_EQ(row_format_to_string(RowFormat::DYNAMIC), "DYNAMIC");
    EXPECT_EQ(row_format_to_string(RowFormat::REDUNDANT), "REDUNDANT");
}

TEST(PageTypesV2Test, VersionToString) {
    EXPECT_EQ(version_to_string(MySQLVersion::MYSQL_56), "5.6");
    EXPECT_EQ(version_to_string(MySQLVersion::MYSQL_80), "8.0");
    EXPECT_EQ(version_to_string(MySQLVersion::MYSQL_90), "9.0");
}

TEST(PageTypesV2Test, TrxIDToString) {
    trx_id_t id;
    id.data[0] = 0x00; id.data[1] = 0x00; id.data[2] = 0x00;
    id.data[3] = 0x00; id.data[4] = 0x00; id.data[5] = 0x01;
    
    std::string str = id.to_string();
    EXPECT_FALSE(str.empty());
    EXPECT_NE(str.find("trx_id:"), std::string::npos);
}

} // namespace innodb
} // namespace dbrecovery
