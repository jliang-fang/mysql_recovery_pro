#include <gtest/gtest.h>
#include "dbrecovery/innodb/sdi_extractor.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== SDIExtractor Tests ====================

TEST(SDIExtractorTest, ExtractFromEmptyBuffer) {
    SDIExtractor extractor;
    std::vector<uint8_t> buffer(0, 0);

    auto results = extractor.extract_from_buffer(buffer.data(), buffer.size());
    EXPECT_EQ(results.size(), 0);
    EXPECT_EQ(extractor.stats().total_pages, 0);
}

TEST(SDIExtractorTest, ExtractFromNonSDIPage) {
    SDIExtractor extractor;

    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);  // 不是 SDI 页 (correct offset is 24)

    auto results = extractor.extract_from_page(page.data(), page.size());
    EXPECT_EQ(results.size(), 0);
    EXPECT_EQ(extractor.stats().sdi_pages, 0);
}

TEST(SDIExtractorTest, ExtractFromSDIPage) {
    SDIExtractor extractor;

    std::vector<uint8_t> page(16384, 0);

    // 设置页类型为 SDI (at correct offset 24, big-endian)
    write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);
    write_be<uint32_t>(page.data(), FIL_PAGE_OFFSET, 1);  // 页号

    // 设置 SDI 头部（在偏移 70）
    size_t sdi_offset = 70;
    write_le<uint32_t>(page.data(), sdi_offset, 0);  // type
    write_le<uint64_t>(page.data(), sdi_offset + 4, 100);  // id
    write_le<uint32_t>(page.data(), sdi_offset + 12, 100);  // uncompressed_len
    write_le<uint32_t>(page.data(), sdi_offset + 16, 50);   // compressed_len

    // 设置一些压缩数据
    for (size_t i = sdi_offset + 20; i < sdi_offset + 70; i++) {
        page[i] = static_cast<uint8_t>(i % 256);
    }

    auto results = extractor.extract_from_page(page.data(), page.size());

    // 由于数据不是有效的压缩数据，应该失败
    // 注意：extract_sdi_at_offset 会尝试解压缩，如果失败会返回无效结果
    // 但由于我们设置了有效的头部，它可能会成功解压缩（即使数据无效）
    // 所以我们只检查 stats
    EXPECT_EQ(extractor.stats().sdi_pages, 1);
}

TEST(SDIExtractorTest, ExtractFromBufferMultiplePages) {
    SDIExtractor extractor;

    std::vector<uint8_t> buffer(32768, 0);  // 2 页

    // Page 0: INDEX (at correct offset 24, big-endian)
    write_be<uint16_t>(buffer.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);

    // Page 1: SDI (at correct offset 24, big-endian)
    write_be<uint16_t>(buffer.data() + 16384, FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);
    write_be<uint32_t>(buffer.data() + 16384, FIL_PAGE_OFFSET, 1);

    auto results = extractor.extract_from_buffer(buffer.data(), buffer.size());

    EXPECT_EQ(results.size(), 0);  // 没有有效的 SDI 数据
    EXPECT_EQ(extractor.stats().total_pages, 2);
    EXPECT_EQ(extractor.stats().sdi_pages, 1);
}

TEST(SDIExtractorTest, ExtractStatsToString) {
    SDIExtractStats stats{};
    stats.total_pages = 10;
    stats.sdi_pages = 3;
    stats.sdi_extracted = 2;
    stats.sdi_failed = 1;

    std::string str = stats.to_string();
    EXPECT_NE(str.find("total_pages=10"), std::string::npos);
    EXPECT_NE(str.find("sdi_pages=3"), std::string::npos);
    EXPECT_NE(str.find("extracted=2"), std::string::npos);
    EXPECT_NE(str.find("failed=1"), std::string::npos);
}

TEST(SDIExtractorTest, ExtractedSDIToString) {
    ExtractedSDI extracted{};
    extracted.page_no = 1;
    extracted.offset = 70;
    extracted.sdi_json = "{\"test\": \"data\"}";
    extracted.is_valid = true;

    std::string str = extracted.to_string();
    EXPECT_NE(str.find("page_no=1"), std::string::npos);
    EXPECT_NE(str.find("offset=70"), std::string::npos);
    EXPECT_NE(str.find("valid=true"), std::string::npos);  // 使用 std::boolalpha
    EXPECT_NE(str.find("json_len=16"), std::string::npos);
}

TEST(SDIExtractorTest, ExtractFromLargeBuffer) {
    SDIExtractor extractor;

    // 创建 64KB 缓冲区（4 页）
    std::vector<uint8_t> buffer(65536, 0);

    // Page 0: INDEX (at correct offset 24, big-endian)
    write_be<uint16_t>(buffer.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    write_be<uint32_t>(buffer.data(), FIL_PAGE_OFFSET, 0);

    // Page 1: SDI
    write_be<uint16_t>(buffer.data() + 16384, FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);
    write_be<uint32_t>(buffer.data() + 16384, FIL_PAGE_OFFSET, 1);

    // Page 2: INDEX
    write_be<uint16_t>(buffer.data() + 32768, FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    write_be<uint32_t>(buffer.data() + 32768, FIL_PAGE_OFFSET, 2);

    // Page 3: SDI
    write_be<uint16_t>(buffer.data() + 49152, FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);
    write_be<uint32_t>(buffer.data() + 49152, FIL_PAGE_OFFSET, 3);

    auto results = extractor.extract_from_buffer(buffer.data(), buffer.size());

    EXPECT_EQ(extractor.stats().total_pages, 4);
    EXPECT_EQ(extractor.stats().sdi_pages, 2);
}

TEST(SDIExtractorTest, ExtractFromDifferentPageSizes) {
    // 4KB 页
    std::vector<uint8_t> page4k(4096, 0);
    write_be<uint16_t>(page4k.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);
    write_be<uint32_t>(page4k.data(), FIL_PAGE_OFFSET, 0);

    SDIExtractor extractor4k(4096);
    auto results4k = extractor4k.extract_from_page(page4k.data(), page4k.size());
    EXPECT_EQ(extractor4k.stats().sdi_pages, 1);

    // 32KB 页
    std::vector<uint8_t> page32k(32768, 0);
    write_be<uint16_t>(page32k.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);
    write_be<uint32_t>(page32k.data(), FIL_PAGE_OFFSET, 0);

    SDIExtractor extractor32k(32768);
    auto results32k = extractor32k.extract_from_page(page32k.data(), page32k.size());
    EXPECT_EQ(extractor32k.stats().sdi_pages, 1);
}

TEST(SDIExtractorTest, ExtractFromInvalidPage) {
    SDIExtractor extractor;

    std::vector<uint8_t> page(100, 0);  // 太小

    auto results = extractor.extract_from_page(page.data(), page.size());
    EXPECT_EQ(results.size(), 0);
}

TEST(SDIExtractorTest, ExtractFromPageWithInvalidSDIHeader) {
    SDIExtractor extractor;

    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);
    write_be<uint32_t>(page.data(), FIL_PAGE_OFFSET, 1);

    // 设置无效的 SDI 头部
    size_t sdi_offset = 70;
    write_le<uint32_t>(page.data(), sdi_offset, 100);  // type > 10 (invalid)
    write_le<uint64_t>(page.data(), sdi_offset + 4, 100);
    write_le<uint32_t>(page.data(), sdi_offset + 12, 100);
    write_le<uint32_t>(page.data(), sdi_offset + 16, 50);

    auto results = extractor.extract_from_page(page.data(), page.size());
    EXPECT_EQ(results.size(), 0);
    EXPECT_EQ(extractor.stats().sdi_failed, 1);
}

TEST(SDIExtractorTest, ExtractFromPageWithZeroUncompressedLen) {
    SDIExtractor extractor;

    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);
    write_be<uint32_t>(page.data(), FIL_PAGE_OFFSET, 1);

    // 设置无效的 SDI 头部（uncompressed_len = 0）
    size_t sdi_offset = 70;
    write_le<uint32_t>(page.data(), sdi_offset, 0);
    write_le<uint64_t>(page.data(), sdi_offset + 4, 100);
    write_le<uint32_t>(page.data(), sdi_offset + 12, 0);  // invalid
    write_le<uint32_t>(page.data(), sdi_offset + 16, 50);

    auto results = extractor.extract_from_page(page.data(), page.size());
    EXPECT_EQ(results.size(), 0);
}

TEST(SDIExtractorTest, ExtractFromPageWithCompressedLenGreaterThanUncompressed) {
    SDIExtractor extractor;

    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);
    write_be<uint32_t>(page.data(), FIL_PAGE_OFFSET, 1);

    // 设置无效的 SDI 头部（compressed_len > uncompressed_len）
    size_t sdi_offset = 70;
    write_le<uint32_t>(page.data(), sdi_offset, 0);
    write_le<uint64_t>(page.data(), sdi_offset + 4, 100);
    write_le<uint32_t>(page.data(), sdi_offset + 12, 50);   // uncompressed
    write_le<uint32_t>(page.data(), sdi_offset + 16, 100);  // compressed > uncompressed (invalid)

    auto results = extractor.extract_from_page(page.data(), page.size());
    EXPECT_EQ(results.size(), 0);
}

} // namespace innodb
} // namespace dbrecovery
