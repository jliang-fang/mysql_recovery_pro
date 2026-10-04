#include <gtest/gtest.h>

#include <cstring>
#include <vector>
#include <zlib.h>

#define private public
#include "dbrecovery/innodb/compressed_table_recovery.hpp"
#undef private

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

std::vector<uint8_t> make_fsp_page(size_t page_size, uint8_t zip_ssize,
                                   uint16_t page_type = dbrecovery::innodb::FIL_PAGE_TYPE_FSP_HDR) {
    std::vector<uint8_t> page(page_size, 0);
    write_be16(page.data() + dbrecovery::innodb::FIL_PAGE_TYPE_ABS, page_type);
    write_be32(page.data() + dbrecovery::innodb::FSP_SPACE_FLAGS_OFFSET,
               (static_cast<uint32_t>(zip_ssize) << 1) | 0x21);
    return page;
}

std::vector<uint8_t> make_zlib_frame(size_t expected_size) {
    std::vector<uint8_t> source(expected_size, 0x5a);
    uLong compressed_bound = compressBound(static_cast<uLong>(source.size()));
    std::vector<uint8_t> compressed(compressed_bound);
    uLong compressed_size = compressed_bound;
    EXPECT_EQ(compress2(compressed.data(), &compressed_size,
                        source.data(), static_cast<uLong>(source.size()),
                        Z_BEST_SPEED), Z_OK);
    compressed.resize(static_cast<size_t>(compressed_size));

    std::vector<uint8_t> frame(32 + compressed.size(), 0);
    frame[8] = static_cast<uint8_t>(compressed_size);
    frame[9] = static_cast<uint8_t>(compressed_size >> 8);
    std::memcpy(frame.data() + 32, compressed.data(), compressed.size());
    return frame;
}

} // namespace

TEST(CompressedTableRecoverySafetyTest, AcceptsLargeDeclaredLengthWithoutIntegerWrap) {
    dbrecovery::innodb::CompressedTableRecovery recovery;
    auto frame = make_zlib_frame(16384);
    frame[8] = 0xff;
    frame[9] = 0xff;

    const auto decompressed = recovery.decompress_page_data(
        frame.data(), frame.size(), 16384);

    ASSERT_EQ(decompressed.size(), 16384u);
    EXPECT_EQ(decompressed.front(), 0x5a);
    EXPECT_EQ(decompressed.back(), 0x5a);
}

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

TEST(FSPCompressionMetadataTest, AcceptsLegacyUncompressedTablespace) {
    const auto page = make_fsp_page(
        16384, 0, dbrecovery::innodb::FIL_PAGE_TYPE_FSP_HDR_LEGACY);

    const auto metadata = dbrecovery::innodb::parse_fsp_compression_metadata(
        page.data(), page.size(), 16384);

    ASSERT_TRUE(metadata.has_value());
    EXPECT_FALSE(metadata->is_compressed());
    EXPECT_EQ(metadata->physical_page_size, 16384u);
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
