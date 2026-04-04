#include <gtest/gtest.h>
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== PageScanner Tests ====================

TEST(PageScannerTest, ScanValidInnodbPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);  // 页类型
    write_le<uint32_t>(page.data(), 20, 0);               // 页号
    write_le<uint32_t>(page.data(), 0, 0xFFFFFFFF);       // prev
    write_le<uint32_t>(page.data(), 4, 0xFFFFFFFF);       // next

    EXPECT_TRUE(PageScanner::is_innodb_page(page.data()));
    EXPECT_TRUE(PageScanner::is_valid_page_type(FIL_PAGE_INDEX));
}

TEST(PageScannerTest, ScanSDIPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_SDI);

    EXPECT_TRUE(PageScanner::is_innodb_page(page.data()));
    EXPECT_TRUE(PageScanner::is_valid_page_type(FIL_PAGE_SDI));
}

TEST(PageScannerTest, ScanInvalidPageType) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, 0xFFFF);  // Invalid

    EXPECT_FALSE(PageScanner::is_innodb_page(page.data()));
    EXPECT_FALSE(PageScanner::is_valid_page_type(0xFFFF));
}

TEST(PageScannerTest, ScanBufferMultiplePages) {
    std::vector<uint8_t> buffer(32768, 0);  // 2 pages

    // Page 0: INDEX
    write_le<uint16_t>(buffer.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(buffer.data(), 20, 0);

    // Page 1: SDI
    write_le<uint16_t>(buffer.data() + 16384, 16, FIL_PAGE_SDI);
    write_le<uint32_t>(buffer.data() + 16384, 20, 1);

    PageScanner scanner(16384);
    auto pages = scanner.scan_buffer(buffer.data(), buffer.size());

    EXPECT_EQ(pages.size(), 2);
    EXPECT_EQ(pages[0].type, FIL_PAGE_INDEX);
    EXPECT_EQ(pages[1].type, FIL_PAGE_SDI);
    EXPECT_EQ(scanner.stats().total_pages, 2);
    EXPECT_EQ(scanner.stats().valid_pages, 2);
}

TEST(PageScannerTest, ScanBufferWithInvalidPages) {
    std::vector<uint8_t> buffer(49152, 0);  // 3 pages

    // Page 0: INDEX (valid)
    write_le<uint16_t>(buffer.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(buffer.data(), 20, 0);

    // Page 1: Invalid type
    write_le<uint16_t>(buffer.data() + 16384, 16, 0xFFFF);

    // Page 2: UNDO_LOG (valid)
    write_le<uint16_t>(buffer.data() + 32768, 16, FIL_PAGE_UNDO_LOG);
    write_le<uint32_t>(buffer.data() + 32768, 20, 2);

    PageScanner scanner(16384);
    auto pages = scanner.scan_buffer(buffer.data(), buffer.size());

    EXPECT_EQ(pages.size(), 3);
    EXPECT_EQ(scanner.stats().valid_pages, 2);
    EXPECT_EQ(scanner.stats().invalid_pages, 1);
    EXPECT_EQ(scanner.stats().index_pages, 1);
    EXPECT_EQ(scanner.stats().undo_pages, 1);
}

TEST(PageScannerTest, ScanPageChain) {
    std::vector<uint8_t> buffer(49152, 0);  // 3 pages

    // Page 0 -> Page 1 -> Page 2
    write_le<uint16_t>(buffer.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(buffer.data(), 20, 0);
    write_le<uint32_t>(buffer.data(), 4, 1);  // next = 1

    write_le<uint16_t>(buffer.data() + 16384, 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(buffer.data() + 16384, 20, 1);
    write_le<uint32_t>(buffer.data() + 16384, 0, 0);  // prev = 0
    write_le<uint32_t>(buffer.data() + 16384, 4, 2);  // next = 2

    write_le<uint16_t>(buffer.data() + 32768, 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(buffer.data() + 32768, 20, 2);
    write_le<uint32_t>(buffer.data() + 32768, 0, 1);  // prev = 1

    PageScanner scanner(16384);
    auto pages = scanner.scan_buffer(buffer.data(), buffer.size());

    EXPECT_EQ(pages.size(), 3);
    EXPECT_EQ(pages[0].next_page, 1);
    EXPECT_EQ(pages[1].prev_page, 0);
    EXPECT_EQ(pages[1].next_page, 2);
    EXPECT_EQ(pages[2].prev_page, 1);
}

TEST(PageScannerTest, ScanStats) {
    std::vector<uint8_t> buffer(49152, 0);  // 3 pages

    // Page 0: INDEX
    write_le<uint16_t>(buffer.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(buffer.data(), 20, 0);

    // Page 1: SDI
    write_le<uint16_t>(buffer.data() + 16384, 16, FIL_PAGE_SDI);
    write_le<uint32_t>(buffer.data() + 16384, 20, 1);

    // Page 2: UNDO_LOG
    write_le<uint16_t>(buffer.data() + 32768, 16, FIL_PAGE_UNDO_LOG);
    write_le<uint32_t>(buffer.data() + 32768, 20, 2);

    PageScanner scanner(16384);
    auto pages = scanner.scan_buffer(buffer.data(), buffer.size());

    const auto& stats = scanner.stats();
    EXPECT_EQ(stats.total_pages, 3);
    EXPECT_EQ(stats.valid_pages, 3);
    EXPECT_EQ(stats.index_pages, 1);
    EXPECT_EQ(stats.sdi_pages, 1);
    EXPECT_EQ(stats.undo_pages, 1);
}

TEST(PageScannerTest, ScanLargeFile) {
    // 创建 64KB 文件 (4 页)
    std::vector<uint8_t> buffer(65536, 0);

    for (int i = 0; i < 4; i++) {
        size_t offset = i * 16384;
        write_le<uint16_t>(buffer.data() + offset, 16, FIL_PAGE_INDEX);
        write_le<uint32_t>(buffer.data() + offset, 20, i);
    }

    PageScanner scanner(16384);
    auto pages = scanner.scan_buffer(buffer.data(), buffer.size());

    EXPECT_EQ(pages.size(), 4);
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(pages[i].page_no, i);
        EXPECT_EQ(pages[i].type, FIL_PAGE_INDEX);
    }
}

TEST(PageScannerTest, ScanWithDifferentPageSizes) {
    // 4KB 页
    std::vector<uint8_t> page4k(4096, 0);
    write_le<uint16_t>(page4k.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(page4k.data(), 20, 0);

    PageScanner scanner4k(4096);
    auto pages4k = scanner4k.scan_buffer(page4k.data(), page4k.size());
    EXPECT_EQ(pages4k.size(), 1);

    // 32KB 页
    std::vector<uint8_t> page32k(32768, 0);
    write_le<uint16_t>(page32k.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(page32k.data(), 20, 0);

    PageScanner scanner32k(32768);
    auto pages32k = scanner32k.scan_buffer(page32k.data(), page32k.size());
    EXPECT_EQ(pages32k.size(), 1);
}

TEST(PageScannerTest, ScanEmptyBuffer) {
    std::vector<uint8_t> buffer(0, 0);

    PageScanner scanner(16384);
    auto pages = scanner.scan_buffer(buffer.data(), buffer.size());

    EXPECT_EQ(pages.size(), 0);
    EXPECT_EQ(scanner.stats().total_pages, 0);
}

TEST(PageScannerTest, ScanSmallBuffer) {
    std::vector<uint8_t> buffer(100, 0);  // Less than page size

    PageScanner scanner(16384);
    auto pages = scanner.scan_buffer(buffer.data(), buffer.size());

    EXPECT_EQ(pages.size(), 0);
}

TEST(PageScannerTest, ScannedPageToString) {
    ScannedPage page{};
    page.offset = 0;
    page.type = FIL_PAGE_INDEX;
    page.page_no = 0;
    page.prev_page = 0xFFFFFFFF;
    page.next_page = 1;
    page.is_valid = true;
    page.lsn = 1000;
    page.space_id = 1;

    std::string str = page.to_string();
    EXPECT_NE(str.find("offset=0"), std::string::npos);
    EXPECT_NE(str.find("type=0x26f1"), std::string::npos);
    EXPECT_NE(str.find("valid=1"), std::string::npos);
}

TEST(PageScannerTest, ScanStatsToString) {
    ScanStats stats{};
    stats.total_pages = 10;
    stats.valid_pages = 8;
    stats.index_pages = 5;
    stats.sdi_pages = 2;
    stats.undo_pages = 1;
    stats.blob_pages = 0;
    stats.allocated_pages = 0;  // Fixed: use correct field name
    stats.invalid_pages = 2;

    std::string str = stats.to_string();
    EXPECT_NE(str.find("total=10"), std::string::npos);
    EXPECT_NE(str.find("valid=8"), std::string::npos);
}

} // namespace innodb
} // namespace dbrecovery
