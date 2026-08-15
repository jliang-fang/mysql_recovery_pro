#include <gtest/gtest.h>

#include <vector>

#include "dbrecovery/innodb/page_defs.hpp"

namespace {

void write_be16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

void write_be32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

std::vector<uint8_t> make_fsp_page(size_t page_size, uint8_t zip_ssize) {
    std::vector<uint8_t> page(page_size, 0);
    write_be16(page.data() + dbrecovery::innodb::FIL_PAGE_TYPE_ABS,
               dbrecovery::innodb::FIL_PAGE_TYPE_FSP_HDR);
    write_be32(page.data() + dbrecovery::innodb::FSP_SPACE_FLAGS_OFFSET,
               static_cast<uint32_t>(zip_ssize) << 1);
    return page;
}

} // namespace

TEST(FSPCompressionMetadataTest, ParsesCanonicalCompressedFlags) {
    const auto page = make_fsp_page(16384, 2);

    const auto metadata = dbrecovery::innodb::parse_fsp_compression_metadata(
        page.data(), page.size(), 16384);

    ASSERT_TRUE(metadata.has_value());
    EXPECT_TRUE(metadata->is_compressed());
    EXPECT_EQ(metadata->zip_ssize, 2);
    EXPECT_EQ(metadata->logical_page_size, 16384u);
    EXPECT_EQ(metadata->physical_page_size, 4096u);
}

TEST(FSPCompressionMetadataTest, AcceptsValidUncompressedTablespace) {
    const auto page = make_fsp_page(8192, 0);

    const auto metadata = dbrecovery::innodb::parse_fsp_compression_metadata(
        page.data(), page.size(), 8192);

    ASSERT_TRUE(metadata.has_value());
    EXPECT_FALSE(metadata->is_compressed());
    EXPECT_EQ(metadata->physical_page_size, 8192u);
}

TEST(FSPCompressionMetadataTest, RejectsMalformedMetadata) {
    auto page = make_fsp_page(16384, 5);
    EXPECT_FALSE(dbrecovery::innodb::parse_fsp_compression_metadata(
        page.data(), page.size(), 16384).has_value());

    page = make_fsp_page(16384, 1);
    write_be16(page.data() + dbrecovery::innodb::FIL_PAGE_TYPE_ABS,
               dbrecovery::innodb::FIL_PAGE_INDEX);
    EXPECT_FALSE(dbrecovery::innodb::parse_fsp_compression_metadata(
        page.data(), page.size(), 16384).has_value());

    EXPECT_FALSE(dbrecovery::innodb::parse_fsp_compression_metadata(
        nullptr, 0, 16384).has_value());
}
