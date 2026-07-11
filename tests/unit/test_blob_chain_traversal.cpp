/**
 * @file test_blob_chain_traversal.cpp
 * @brief Unit tests for BLOB chain traversal from ibd2sql patterns
 */

#include <gtest/gtest.h>
#include "dbrecovery/innodb/blob_scanner.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

using namespace dbrecovery::innodb;

class BlobChainTraversalTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ==================== Off-Page Flag Detection Tests ====================

TEST_F(BlobChainTraversalTest, OffPageFlagDynamic) {
    // DYNAMIC row format uses 16404 as off-page flag
    EXPECT_TRUE(BlobScanner::is_off_page(16404, row_formats::DYNAMIC, 80000));
    EXPECT_FALSE(BlobScanner::is_off_page(16384, row_formats::DYNAMIC, 80000));  // Wrong value
    EXPECT_FALSE(BlobScanner::is_off_page(100, row_formats::DYNAMIC, 80000));  // Normal size
}

TEST_F(BlobChainTraversalTest, OffPageFlagCompact) {
    // COMPACT row format with MySQL < 8.0 uses 17172
    EXPECT_TRUE(BlobScanner::is_off_page(17172, row_formats::COMPACT, 50700));
    EXPECT_FALSE(BlobScanner::is_off_page(16404, row_formats::COMPACT, 50700));  // Wrong for COMPACT
}

TEST_F(BlobChainTraversalTest, OffPageFlagCompressed) {
    // COMPRESSED row format uses DYNAMIC flag
    EXPECT_TRUE(BlobScanner::is_off_page(16404, row_formats::COMPRESSED, 80000));
}

TEST_F(BlobChainTraversalTest, OffPageFlagMySQL8) {
    // MySQL 8.0+ uses DYNAMIC flag for all formats
    EXPECT_TRUE(BlobScanner::is_off_page(16404, row_formats::COMPACT, 80000));
}

// ==================== BLOB Entry Chain Constants Tests ====================

TEST_F(BlobChainTraversalTest, BlobEntryHeaderConstants) {
    EXPECT_EQ(BLOB_ENTRY_HEADER_OFFSET, 96);
    EXPECT_EQ(BLOB_ENTRY_HEADER_SIZE, 60);
    EXPECT_EQ(BLOB_ENTRY_NEXT_PAGE_OFFSET, 6);
    EXPECT_EQ(BLOB_ENTRY_NEXT_OFFSET_OFFSET, 10);
    EXPECT_EQ(BLOB_ENTRY_TRAILER_SIZE, 12);
    EXPECT_EQ(BLOB_ENTRY_DATALEN_SHIFT, 16);
}

TEST_F(BlobChainTraversalTest, BlobDataOffsetConstants) {
    // First page data at offset 696 for 16KB page
    EXPECT_EQ(BLOB_FIRST_PAGE_DATA_OFFSET, 696);
    // Subsequent pages data at offset 49
    EXPECT_EQ(BLOB_SUBSEQUENT_PAGE_DATA_OFFSET, 49);
}

// ==================== FIRST_BLOB Chain Traversal Tests ====================

TEST_F(BlobChainTraversalTest, FirstBlobChainLegacy) {
    // Test legacy format for MySQL 5.6 / early 5.7
    std::vector<uint8_t> page1(16384, 0);
    std::vector<uint8_t> page2(16384, 0);

    // Setup legacy BLOB page format
    // REAL_SIZE at offset 38-41
    page1[38] = 0; page1[39] = 0; page1[40] = 0; page1[41] = 10;  // 10 bytes
    // Next page at offset 42-45
    page1[42] = 0; page1[43] = 0; page1[44] = 0; page1[45] = 2;  // Page 2
    // Data at offset 46
    for (int i = 0; i < 10; i++) {
        page1[46 + i] = 'A' + i;
    }

    // Page 2 - end of chain
    page2[38] = 0; page2[39] = 0; page2[40] = 0; page2[41] = 5;  // 5 bytes
    page2[42] = 0xFF; page2[43] = 0xFF; page2[44] = 0xFF; page2[45] = 0xFF;  // FIL_NULL
    for (int i = 0; i < 5; i++) {
        page2[46 + i] = 'K' + i;
    }

    auto page_reader = [&](uint32_t page_no) -> std::vector<uint8_t> {
        if (page_no == 1) return page1;
        if (page_no == 2) return page2;
        return {};
    };

    BlobScanner scanner;
    auto result = scanner.read_first_blob_chain(page_reader, 1, 50600);  // MySQL 5.6

    EXPECT_EQ(result.size(), 15);
    if (result.size() >= 10) {
        EXPECT_EQ(result[0], 'A');
        EXPECT_EQ(result[9], 'J');
    }
}

TEST_F(BlobChainTraversalTest, FirstBlobChainModern) {
    // Test modern format for MySQL 5.7.44+
    std::vector<uint8_t> page1(16384, 0);

    // Setup entry header at offset 96 (60 bytes)
    // Entry trailer: pageno, datalen, lobversion at entry[-12:]
    size_t entry_start = BLOB_ENTRY_HEADER_OFFSET;
    size_t trailer_offset = entry_start + BLOB_ENTRY_HEADER_SIZE - BLOB_ENTRY_TRAILER_SIZE;

    // pageno = first_page (1) at entry[-12:-8]
    page1[trailer_offset] = 0; page1[trailer_offset + 1] = 0;
    page1[trailer_offset + 2] = 0; page1[trailer_offset + 3] = 1;

    // datalen = 20 (shifted by 16) at entry[-8:-4]
    page1[trailer_offset + 4] = 0; page1[trailer_offset + 5] = 0;
    page1[trailer_offset + 6] = 0; page1[trailer_offset + 7] = 20 << 16;  // Will be wrong, need correct encoding

    // Actually: upper 16 bits = length, lower 16 bits = version
    // So datalen_raw = (20 << 16) | version
    uint32_t datalen_raw = (20 << 16);  // 20 bytes, version 0
    page1[trailer_offset + 4] = (datalen_raw >> 24) & 0xFF;
    page1[trailer_offset + 5] = (datalen_raw >> 16) & 0xFF;
    page1[trailer_offset + 6] = (datalen_raw >> 8) & 0xFF;
    page1[trailer_offset + 7] = datalen_raw & 0xFF;

    // Next entry: end of chain (0 at entry[6:10])
    page1[entry_start + BLOB_ENTRY_NEXT_PAGE_OFFSET] = 0;
    page1[entry_start + BLOB_ENTRY_NEXT_PAGE_OFFSET + 1] = 0;
    page1[entry_start + BLOB_ENTRY_NEXT_PAGE_OFFSET + 2] = 0;
    page1[entry_start + BLOB_ENTRY_NEXT_PAGE_OFFSET + 3] = 0;

    // Data at offset 696
    for (int i = 0; i < 20; i++) {
        page1[BLOB_FIRST_PAGE_DATA_OFFSET + i] = 'X' + i;
    }

    auto page_reader = [&](uint32_t page_no) -> std::vector<uint8_t> {
        if (page_no == 1) return page1;
        return {};
    };

    BlobScanner scanner;
    auto result = scanner.read_first_blob_chain(page_reader, 1, 50800);  // MySQL 5.7.44+

    EXPECT_EQ(result.size(), 20);
    if (result.size() >= 20) {
        EXPECT_EQ(result[0], 'X');
    }
}

// ==================== External Field Reference Tests ====================

TEST_F(BlobChainTraversalTest, BlobFieldParse) {
    // Test 20-byte BLOB field reference parsing
    std::vector<uint8_t> field_ref(20, 0);

    // space_id = 1
    field_ref[3] = 1;
    // page_no = 100
    field_ref[7] = 100;
    // offset = 50
    field_ref[11] = 50;
    // length = 1000
    field_ref[15] = 0xE8;  // 1000 = 0x3E8
    field_ref[14] = 0x03;

    auto blob_field = BLOBField::parse(field_ref.data());

    EXPECT_EQ(blob_field.space_id, 1);
    EXPECT_EQ(blob_field.page_no, 100);
    EXPECT_EQ(blob_field.offset, 50);
    EXPECT_EQ(blob_field.total_len, 1000);
}

TEST_F(BlobChainTraversalTest, IsOffPageFlag) {
    // Verify the off-page flag values match ibd2sql
    EXPECT_EQ(OFF_PAGE_FLAG_DYNAMIC, 16404);
    EXPECT_EQ(OFF_PAGE_FLAG_ANTLELOPE, 17172);
    EXPECT_EQ(OFF_PAGE_FLAG_COMPACT, 17172);
}

// ==================== Constants Verification ====================

TEST_F(BlobChainTraversalTest, BtrExternConstants) {
    EXPECT_EQ(BTR_EXTERN_SPACE_ID, 0);
    EXPECT_EQ(BTR_EXTERN_PAGE_NO, 4);
    EXPECT_EQ(BTR_EXTERN_OFFSET, 8);
    EXPECT_EQ(BTR_EXTERN_LEN, 12);
    EXPECT_EQ(BTR_EXTERN_FIELD_REF_SIZE, 20);
    EXPECT_EQ(FIL_NULL, 0xFFFFFFFF);
}

TEST_F(BlobChainTraversalTest, BlobHeaderConstants) {
    EXPECT_EQ(BTR_BLOB_HDR_PART_LEN, 0);
    EXPECT_EQ(BTR_BLOB_HDR_NEXT_PAGE_NO, 4);
    EXPECT_EQ(BTR_BLOB_HDR_SIZE, 8);
}

TEST_F(BlobChainTraversalTest, RedundantOffsetConstants) {
    EXPECT_EQ(REC_OLD_SHORT_MASK, 0x01);
    EXPECT_EQ(REC_1BYTE_OFFS_LIMIT, 0x7FUL);
    EXPECT_EQ(REC_2BYTE_EXTERN_MASK, 0x4000);
}
