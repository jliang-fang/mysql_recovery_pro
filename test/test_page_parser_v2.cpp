#include <gtest/gtest.h>
#include "dbrecovery/innodb/page_parser_v2.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== PageParserFactory Tests ====================

TEST(PageParserFactoryTest, DetectMySQL80FromSDI) {
    // Create SDI page data (FIL_PAGE_SDI = 9971)
    std::vector<uint8_t> sdi_page(16384, 0);
    write_le<uint16_t>(sdi_page.data(), 16, FIL_PAGE_SDI);  // FIL_PAGE_TYPE at offset 16
    
    auto version = PageParserFactory::detect_mysql_version(sdi_page.data(), sdi_page.size());
    EXPECT_EQ(version, MySQLVersion::MYSQL_80);
}

TEST(PageParserFactoryTest, DetectMySQL57FromIndexPage) {
    // Create index page data (no SDI)
    std::vector<uint8_t> index_page(16384, 0);
    write_le<uint16_t>(index_page.data(), 16, FIL_PAGE_INDEX);  // FIL_PAGE_TYPE at offset 16
    
    auto version = PageParserFactory::detect_mysql_version(index_page.data(), index_page.size());
    EXPECT_EQ(version, MySQLVersion::MYSQL_57);
}

TEST(PageParserFactoryTest, DetectRowFormatFromFlags) {
    std::vector<uint8_t> page(16384, 0);
    // Set flags at offset 38 + 42 = 80
    write_le<uint16_t>(page.data(), 80, 0x0002); // DYNAMIC format (flags & 0x07 = 2)
    
    auto format = PageParserFactory::detect_row_format(page.data(), page.size());
    EXPECT_EQ(format, RowFormat::DYNAMIC);
}

TEST(PageParserFactoryTest, DetectRowFormatRedundant) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 80, 0x0000); // REDUNDANT format
    
    auto format = PageParserFactory::detect_row_format(page.data(), page.size());
    EXPECT_EQ(format, RowFormat::REDUNDANT);
}

TEST(PageParserFactoryTest, DetectRowFormatCompact) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 80, 0x0001); // COMPACT format
    
    auto format = PageParserFactory::detect_row_format(page.data(), page.size());
    EXPECT_EQ(format, RowFormat::COMPACT);
}

TEST(PageParserFactoryTest, CreateMySQL80Parser) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_SDI);
    
    auto parser = PageParserFactory::create_parser(page.data(), page.size());
    EXPECT_NE(parser, nullptr);
    EXPECT_EQ(parser->get_mysql_version(), MySQLVersion::MYSQL_80);
}

TEST(PageParserFactoryTest, CreateMySQL57Parser) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    auto parser = PageParserFactory::create_parser(page.data(), page.size());
    EXPECT_NE(parser, nullptr);
    EXPECT_EQ(parser->get_mysql_version(), MySQLVersion::MYSQL_57);
}

// ==================== PageParserV2 Tests ====================

TEST(PageParserV2Test, ParseSDIPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_SDI);  // SDI page type
    
    PageParserV2 parser(page.data(), page.size());
    auto result = parser.parse();
    
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.page_type, FIL_PAGE_SDI);
    EXPECT_EQ(result.version, MySQLVersion::MYSQL_80);
}

TEST(PageParserV2Test, ParseIndexPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);  // Index page type
    
    // Set page header fields
    write_le<uint16_t>(page.data(), 38, 2);  // n_dir_slots
    write_le<uint16_t>(page.data(), 40, 100);  // heap_top
    write_le<uint16_t>(page.data(), 42, 4);  // n_heap (2 records + 2 system)
    write_le<uint16_t>(page.data(), 44, 50);  // free_offset
    
    // Set BTR header
    write_le<uint16_t>(page.data(), 42, 0);  // page_level (leaf)
    
    PageParserV2 parser(page.data(), page.size());
    auto result = parser.parse();
    
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.page_type, FIL_PAGE_INDEX);
}

TEST(PageParserV2Test, ParseWithInvalidSize) {
    std::vector<uint8_t> page(100, 0);  // Too small
    
    PageParserV2 parser(page.data(), page.size());
    auto result = parser.parse();
    
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_message.empty());
}

TEST(PageParserV2Test, ParseWithInvalidPageType) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, 0xFFFF);  // Invalid page type
    
    PageParserV2 parser(page.data(), page.size());
    auto result = parser.parse();
    
    // Should still succeed but with unknown page type
    EXPECT_TRUE(result.success);
}

TEST(PageParserV2Test, ParseAsMySQL80) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    PageParserV2 parser(page.data(), page.size());
    auto result = parser.parse_as(MySQLVersion::MYSQL_80);
    
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.version, MySQLVersion::MYSQL_80);
}

TEST(PageParserV2Test, ParseAsMySQL57) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    PageParserV2 parser(page.data(), page.size());
    auto result = parser.parse_as(MySQLVersion::MYSQL_57);
    
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.version, MySQLVersion::MYSQL_57);
}

// ==================== PageScanner Tests ====================

TEST(PageScannerTest, ScanBufferWithSDIPage) {
    std::vector<uint8_t> buffer(32768, 0);  // 2 pages
    
    // Page 0: SDI page
    write_le<uint16_t>(buffer.data(), 16, FIL_PAGE_SDI);
    
    // Page 1: Index page
    write_le<uint16_t>(buffer.data() + 16384, 16, FIL_PAGE_INDEX);
    
    PageScanner scanner(16384);
    auto results = scanner.scan_buffer(buffer.data(), buffer.size());
    
    EXPECT_EQ(results.size(), 2);
    EXPECT_EQ(results[0].page_type, FIL_PAGE_SDI);
    EXPECT_EQ(results[1].page_type, FIL_PAGE_INDEX);
}

TEST(PageScannerTest, IsInnodbPageSDI) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_SDI);
    
    EXPECT_TRUE(PageScanner::is_innodb_page(page.data(), page.size()));
}

TEST(PageScannerTest, IsInnodbPageIndex) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    EXPECT_TRUE(PageScanner::is_innodb_page(page.data(), page.size()));
}

TEST(PageScannerTest, IsInnodbPageUndoLog) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_UNDO_LOG);
    
    EXPECT_TRUE(PageScanner::is_innodb_page(page.data(), page.size()));
}

TEST(PageScannerTest, IsNotInnodbPageInvalidType) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, 0xFFFF);
    
    EXPECT_FALSE(PageScanner::is_innodb_page(page.data(), page.size()));
}

// ==================== PageChainReconstructor Tests ====================

TEST(PageChainReconstructorTest, ReconstructValidChain) {
    PageChainReconstructor reconstructor;
    
    reconstructor.add_page(0, std::vector<uint8_t>(16384, 0));
    reconstructor.add_page(1, std::vector<uint8_t>(16384, 0));
    reconstructor.add_page(2, std::vector<uint8_t>(16384, 0));
    
    bool success = reconstructor.reconstruct();
    
    EXPECT_TRUE(success);
    EXPECT_EQ(reconstructor.stats().valid_chain, 3);
}

TEST(PageChainReconstructorTest, DetectBrokenChain) {
    PageChainReconstructor reconstructor;
    
    reconstructor.add_page(0, std::vector<uint8_t>(16384, 0));
    reconstructor.add_page(2, std::vector<uint8_t>(16384, 0));  // Gap at page 1
    reconstructor.add_page(3, std::vector<uint8_t>(16384, 0));
    
    bool success = reconstructor.reconstruct();
    
    EXPECT_FALSE(success);
    EXPECT_GT(reconstructor.stats().broken_chain, 0);
}

TEST(PageChainReconstructorTest, DetectCircularReference) {
    PageChainReconstructor reconstructor;
    
    reconstructor.add_page(0, std::vector<uint8_t>(16384, 0));
    reconstructor.add_page(1, std::vector<uint8_t>(16384, 0));
    reconstructor.add_page(0, std::vector<uint8_t>(16384, 0));  // Duplicate
    
    bool success = reconstructor.reconstruct();
    
    EXPECT_FALSE(success);
    EXPECT_GT(reconstructor.stats().circular_reference, 0);
}

// ==================== PageHealthChecker Tests ====================

TEST(PageHealthCheckerTest, CheckValidPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    auto info = PageHealthChecker::check(page.data(), page.size());
    
    EXPECT_TRUE(info.is_valid);
}

TEST(PageHealthCheckerTest, CheckInvalidPageSize) {
    std::vector<uint8_t> page(100, 0);  // Too small
    
    auto info = PageHealthChecker::check(page.data(), page.size());
    
    EXPECT_FALSE(info.is_valid);
}

TEST(PageHealthCheckerTest, CheckInvalidPageType) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, 0xFFFF);
    
    auto info = PageHealthChecker::check(page.data(), page.size());
    
    EXPECT_TRUE(info.is_valid);  // Unknown type is not fatal
    EXPECT_FALSE(info.status_message.empty());
}

// ==================== PageTypeDetector Tests ====================

TEST(PageTypeDetectorTest, DetectSDIPageType) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_SDI);
    
    auto type = PageTypeDetector::detect_page_type(page.data(), page.size());
    EXPECT_EQ(type, FIL_PAGE_SDI);
}

TEST(PageTypeDetectorTest, DetectIndexPageType) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    auto type = PageTypeDetector::detect_page_type(page.data(), page.size());
    EXPECT_EQ(type, FIL_PAGE_INDEX);
}

TEST(PageTypeDetectorTest, DetectVersionFromSDI) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_SDI);
    
    auto version = PageTypeDetector::detect_version(FIL_PAGE_SDI, page.data(), page.size());
    EXPECT_EQ(version, MySQLVersion::MYSQL_80);
}

TEST(PageTypeDetectorTest, PageTypeToString) {
    EXPECT_EQ(PageTypeDetector::page_type_to_string(FIL_PAGE_INDEX), "FIL_PAGE_INDEX");
    EXPECT_EQ(PageTypeDetector::page_type_to_string(FIL_PAGE_SDI), "FIL_PAGE_SDI");
    EXPECT_EQ(PageTypeDetector::page_type_to_string(FIL_PAGE_UNDO_LOG), "FIL_PAGE_UNDO_LOG");
}

// ==================== GenericPageParser Tests ====================

TEST(GenericPageParserTest, ParseHeader) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    write_le<uint32_t>(page.data(), 20, 0);  // page_offset
    
    GenericPageParser parser(MySQLVersion::MYSQL_80, RowFormat::DYNAMIC);
    
    bool success = parser.parse_header(page.data(), page.size());
    
    EXPECT_TRUE(success);
    EXPECT_EQ(parser.get_page_type(), FIL_PAGE_INDEX);
}

TEST(GenericPageParserTest, GetPageChain) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint32_t>(page.data(), 24, 5);   // prev_page
    write_le<uint32_t>(page.data(), 28, 10);  // next_page
    
    GenericPageParser parser(MySQLVersion::MYSQL_80, RowFormat::DYNAMIC);
    parser.parse_header(page.data(), page.size());
    
    auto chain = parser.get_page_chain();
    EXPECT_EQ(chain.first, 5);
    EXPECT_EQ(chain.second, 10);
}

// ==================== MySQL56PageParser Tests ====================

TEST(MySQL56PageParserTest, ParseHeader) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    MySQL56PageParser parser(RowFormat::COMPACT);
    bool success = parser.parse_header(page.data(), page.size());
    
    EXPECT_TRUE(success);
    EXPECT_EQ(parser.get_mysql_version(), MySQLVersion::MYSQL_56);
}

TEST(MySQL56PageParserTest, NoSDIMetadata) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    MySQL56PageParser parser(RowFormat::COMPACT);
    auto schema = parser.parse_sdi_metadata(page.data(), page.size());
    
    EXPECT_FALSE(schema.has_value());
}

// ==================== MySQL80PageParser Tests ====================

TEST(MySQL80PageParserTest, ParseHeader) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    MySQL80PageParser parser(RowFormat::DYNAMIC);
    bool success = parser.parse_header(page.data(), page.size());
    
    EXPECT_TRUE(success);
    EXPECT_EQ(parser.get_mysql_version(), MySQLVersion::MYSQL_80);
}

TEST(MySQL80PageParserTest, ParseInstantDDL) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    write_le<uint64_t>(page.data(), 86, 1);  // metadata_version at offset 38 + 44 + 4
    
    MySQL80PageParser parser(RowFormat::DYNAMIC);
    parser.parse_header(page.data(), page.size());
    
    // metadata_version_ should be set
    EXPECT_EQ(parser.get_metadata_version(), 1);
}

// ==================== MySQL84PageParser Tests ====================

TEST(MySQL84PageParserTest, ParseHeader) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    MySQL84PageParser parser(RowFormat::DYNAMIC);
    bool success = parser.parse_header(page.data(), page.size());
    
    EXPECT_TRUE(success);
    EXPECT_EQ(parser.get_mysql_version(), MySQLVersion::MYSQL_80);  // Returns base version
}

// ==================== MySQL90PageParser Tests ====================

TEST(MySQL90PageParserTest, ParseHeader) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    MySQL90PageParser parser(RowFormat::DYNAMIC);
    bool success = parser.parse_header(page.data(), page.size());
    
    EXPECT_TRUE(success);
    EXPECT_EQ(parser.get_mysql_version(), MySQLVersion::MYSQL_90);
}

// ==================== Edge Case Tests ====================

TEST(PageParserV2Test, ParseTooSmallPage) {
    std::vector<uint8_t> page(38, 0);  // Exactly FIL_HEADER_SIZE
    
    PageParserV2 parser(page.data(), page.size());
    auto result = parser.parse();
    
    // Should fail due to insufficient data for page header
    EXPECT_FALSE(result.success);
}

TEST(PageParserV2Test, ParseZeroSizePage) {
    std::vector<uint8_t> page(0, 0);
    
    PageParserV2 parser(page.data(), 0);
    auto result = parser.parse();
    
    EXPECT_FALSE(result.success);
}

TEST(PageParserV2Test, ParseLargePage) {
    std::vector<uint8_t> page(65536, 0);  // 64KB page
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    PageParserV2 parser(page.data(), page.size());
    auto result = parser.parse();
    
    EXPECT_TRUE(result.success);
}

TEST(PageParserV2Test, ParseNonInnodbPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, 0xFFFF);  // Invalid page type
    
    PageParserV2 parser(page.data(), page.size());
    auto result = parser.parse();
    
    // Should still succeed but with unknown page type
    EXPECT_TRUE(result.success);
}

// ==================== Record Parsing Tests ====================

TEST(PageParserV2Test, ParseRecords) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    // Set page header
    write_le<uint16_t>(page.data(), 38, 4);  // n_dir_slots
    write_le<uint16_t>(page.data(), 40, 100);  // heap_top
    write_le<uint16_t>(page.data(), 42, 6);  // n_heap (4 records + 2 system)
    
    // Set directory slots
    write_le<uint16_t>(page.data(), 74, 54);   // INFIMUM offset
    write_le<uint16_t>(page.data(), 76, 100);  // SUPREMUM offset
    
    // Set INFIMUM record
    page[54] = REC_STATUS_INFIMUM;  // info_bits
    write_le<uint16_t>(page.data(), 55, 100);  // next_record_offset
    
    // Set SUPREMUM record
    page[99] = REC_STATUS_SUPREMUM;  // info_bits
    
    PageParserV2 parser(page.data(), page.size());
    auto result = parser.parse();
    
    EXPECT_TRUE(result.success);
    EXPECT_GE(result.records.size(), 2);  // At least INFIMUM and SUPREMUM
}

// ==================== Checksum Validation Tests ====================

TEST(PageHealthCheckerTest, ValidateChecksum) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    // Access private method via friend class or public wrapper
    // For testing purposes, we'll use the public check method instead
    auto info = PageHealthChecker::check(page.data(), page.size());
    
    // For pages with zero checksum, validation should pass
    EXPECT_TRUE(info.checksum_match || !info.is_valid);  // Either match or not validated
}

// ==================== Utility Function Tests ====================

TEST(PageParserV2Test, GetPageTypeUtility) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_SDI);
    
    auto type = get_page_type(page.data());
    EXPECT_EQ(type, FIL_PAGE_SDI);
}

TEST(PageParserV2Test, GetPageLevelUtility) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 42, 3);  // page_level at offset 38 + 4
    
    auto level = get_page_level(page.data());
    EXPECT_EQ(level, 3);
}

TEST(PageParserV2Test, GetPageOffsetUtility) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint32_t>(page.data(), 20, 100);  // page_offset
    
    auto offset = get_page_offset(page.data());
    EXPECT_EQ(offset, 100);
}

TEST(PageParserV2Test, GetPageChainUtility) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint32_t>(page.data(), 24, 5);   // prev_page
    write_le<uint32_t>(page.data(), 28, 10);  // next_page
    
    auto chain = get_page_chain(page.data());
    EXPECT_EQ(chain.first, 5);
    EXPECT_EQ(chain.second, 10);
}

} // namespace innodb
} // namespace dbrecovery
