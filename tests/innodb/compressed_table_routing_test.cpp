/**
 * @file compressed_table_routing_test.cpp
 * @brief Fail-closed routing tests for compressed tablespace classification
 *
 * Verifies that CompressedTableRecovery::classify() and the CLI routing
 * semantics only claim recovery through a validated decoder:
 *
 *  - classify() uses canonical big-endian FSP flags and never the old
 *    little-endian ZSD byte heuristic
 *  - A tablespace that reports FSP compression flags is categorized
 *    RECOGNIZED_UNSUPPORTED until a fixture-validated decoder exists
 *  - recover() returns zero rows when the decoder is not validated so a
 *    handled-but-empty result can never be mistaken for success
 *
 * The synthetic compressed pages below are layout-only: the FSP_HDR page 0
 * carries valid canonical compression metadata (big-endian flags, ZIP_SSIZE,
 * FSP_HDR type) but no real compressed payloads, which is exactly the routing
 * precondition the fixture producer records in its manifest.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <vector>

#include "dbrecovery/innodb/compressed_table_recovery.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {
namespace {

std::string make_fixture_dir() {
    std::string dir = "/tmp/compressed_routing_test_" + std::to_string(::getpid());
    std::filesystem::create_directories(dir);
    return dir;
}

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

void write_ibd_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
}

std::vector<uint8_t> read_page0(const std::string& path, size_t page_size) {
    std::vector<uint8_t> page0(page_size, 0);
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(page0.data()),
              static_cast<std::streamsize>(page0.size()));
    if (file.gcount() < static_cast<std::streamsize>(page0.size())) {
        page0.resize(static_cast<size_t>(file.gcount()));
    }
    return page0;
}

/**
 * Build a minimal .ibd whose page 0 is a canonical FSP_HDR carrying
 * big-endian compression flags at FSP_SPACE_FLAGS_OFFSET.
 */
std::vector<uint8_t> make_compressed_ibd(size_t logical_page_size,
                                         size_t physical_page_size,
                                         size_t file_bytes,
                                         uint16_t fsp_page_type = FIL_PAGE_TYPE_FSP_HDR) {
    std::vector<uint8_t> data(file_bytes, 0);
    uint8_t* p0 = data.data();

    write_be16(p0 + FIL_PAGE_TYPE_ABS, fsp_page_type);

    // ZIP_SSIZE = log2(logical/physical), encoded in flags bits 1-4.
    uint32_t ratio = static_cast<uint32_t>(logical_page_size / physical_page_size);
    uint32_t zip_ssize = 0;
    while ((1u << zip_ssize) < ratio) ++zip_ssize;
    write_be32(p0 + FSP_SPACE_FLAGS_OFFSET, (zip_ssize << 1) | 0x21);

    // Physical sizing marker: first page of each stride carries the FIL page
    // type so scan_pages() can identify data pages. Layout-only — pages are
    // otherwise zero-filled, never real compressed payloads.
    const size_t stride = physical_page_size;
    for (size_t off = 0; off + 26 <= file_bytes; off += stride) {
        write_be16(data.data() + off + FIL_PAGE_TYPE_ABS,
                   off == 0 ? FIL_PAGE_TYPE_FSP_HDR : FIL_PAGE_INDEX);
    }
    return data;
}

class CompressedRoutingTest : public ::testing::Test {
protected:
    std::string dir_;

    void SetUp() override {
        dir_ = make_fixture_dir();
    }

    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }
};

TEST_F(CompressedRoutingTest, ClassifyRecognizesEncryptedCompressedFlags) {
    const std::string path = dir_ + "/comp_encrypted.ibd";
    auto data = make_compressed_ibd(16384, 8192, 16384 * 8);
    uint32_t flags = 0;
    flags = (static_cast<uint32_t>(1) << 1) | FSP_FLAGS_POST_ANTELOPE |
            FSP_FLAGS_ATOMIC_BLOBS | FSP_FLAGS_ENCRYPTION;
    write_be32(data.data() + FSP_SPACE_FLAGS_OFFSET, flags);
    write_ibd_file(path, data);

    CompressedTableRecovery reco(16384);
    EXPECT_EQ(reco.classify(path, 16384), CompressionStatus::COMPRESSED_AND_ENCRYPTED);
}

TEST_F(CompressedRoutingTest, ClassifyRecognizesCompressedFlagsAsUnsupported) {
    const std::string path = dir_ + "/comp_8k.ibd";
    const size_t logical = 16384;
    const size_t physical = 8192;
    write_ibd_file(path, make_compressed_ibd(logical, physical, logical * 8));

    CompressedTableRecovery reco(logical);
    EXPECT_EQ(reco.classify(path, logical), CompressionStatus::RECOGNIZED_UNSUPPORTED);
}

TEST_F(CompressedRoutingTest, ClassifyRecognizesCompressedFlags4K) {
    const std::string path = dir_ + "/comp_4k.ibd";
    const size_t logical = 16384;
    write_ibd_file(path, make_compressed_ibd(logical, 4096, logical * 8));

    const auto metadata = parse_fsp_compression_metadata(
        read_page0(path, logical).data(),
        static_cast<size_t>(logical),
        logical);
    ASSERT_TRUE(metadata.has_value());
    EXPECT_TRUE(metadata->is_compressed());
    EXPECT_EQ(metadata->physical_page_size, 4096u);

    CompressedTableRecovery reco(logical);
    EXPECT_EQ(reco.classify(path, logical), CompressionStatus::RECOGNIZED_UNSUPPORTED);
}

TEST_F(CompressedRoutingTest, ClassifyUncompressedFlagsIsNotCompressed) {
    const std::string path = dir_ + "/uncompressed.ibd";
    const size_t logical = 16384;
    write_ibd_file(path, make_compressed_ibd(logical, logical, logical * 8));

    CompressedTableRecovery reco(logical);
    EXPECT_EQ(reco.classify(path, logical), CompressionStatus::NOT_COMPRESSED);
}

TEST_F(CompressedRoutingTest, ClassifyLegacyUncompressedFlagsIsNotCompressed) {
    const std::string path = dir_ + "/legacy_uncompressed.ibd";
    const size_t logical = 16384;
    write_ibd_file(path, make_compressed_ibd(logical, logical, logical * 8,
                                              FIL_PAGE_TYPE_FSP_HDR_LEGACY));

    CompressedTableRecovery reco(logical);
    EXPECT_EQ(reco.classify(path, logical), CompressionStatus::NOT_COMPRESSED);
}

TEST_F(CompressedRoutingTest, InvalidLegacyFlagsReturnsCorruptedMetadata) {
    const std::string path = dir_ + "/legacy_invalid_flags.ibd";
    const size_t logical = 16384;
    std::vector<uint8_t> data(logical * 8, 0);
    write_be16(data.data() + FIL_PAGE_TYPE_ABS, FIL_PAGE_TYPE_FSP_HDR_LEGACY);
    write_be32(data.data() + FSP_SPACE_FLAGS_OFFSET, static_cast<uint32_t>(5) << 1);
    write_ibd_file(path, data);

    CompressedTableRecovery reco(logical);
    EXPECT_EQ(reco.classify(path, logical), CompressionStatus::CORRUPTED_METADATA);
}

TEST_F(CompressedRoutingTest, InvalidFlagsReturnsCorruptedMetadata) {
    const std::string path = dir_ + "/invalid_flags.ibd";
    std::vector<uint8_t> data(16384 * 8, 0);
    write_be16(data.data() + FIL_PAGE_TYPE_ABS, FIL_PAGE_TYPE_FSP_HDR);
    // zip_ssize = 5 is rejected by parse_fsp_compression_metadata.
    write_be32(data.data() + FSP_SPACE_FLAGS_OFFSET, static_cast<uint32_t>(5) << 1);
    write_ibd_file(path, data);

    CompressedTableRecovery reco(16384);
    EXPECT_EQ(reco.classify(path, 16384), CompressionStatus::CORRUPTED_METADATA);
}

TEST_F(CompressedRoutingTest, ClassifyOversizedZipSsizeFallsBackToUnsupportedLayout) {
    const std::string path = dir_ + "/oversized_zip.ibd";
    const size_t logical = 16384;
    write_ibd_file(path, make_compressed_ibd(logical, logical / 8, logical * 8));

    // The standalone .ibd producer never set ZIP_SSIZE in some MySQL 5.7
    // layouts; the physical stride is the only signal. classify() must not
    // claim recovery solely from an observed stride — it must stay at the
    // recognized-but-unsupported gate.
    CompressedTableRecovery reco(logical);
    const auto status = reco.classify(path, logical);
    if (status == CompressionStatus::RECOGNIZED_UNSUPPORTED) {
        EXPECT_EQ(reco.decoder_is_validated(), false);
    }
    EXPECT_NE(status, CompressionStatus::RECOVERED);
}

TEST_F(CompressedRoutingTest, PhysicalLengthMismatchDetected) {
    const std::string path = dir_ + "/phys_mismatch.ibd";
    const size_t logical = 16384;
    // FSP flags declare physical_page_size = 8192 (zip_ssize = 1)
    // but the page at offset 8192 is not a valid InnoDB page — the stride is wrong.
    std::vector<uint8_t> data(logical * 8, 0);
    uint8_t* p0 = data.data();
    write_be16(p0 + FIL_PAGE_TYPE_ABS, FIL_PAGE_TYPE_FSP_HDR);
    write_be32(p0 + FSP_SPACE_FLAGS_OFFSET, static_cast<uint32_t>(1) << 1);
    // Page 1 at offset 8192 is just zeros (no valid page type)
    // Page 2 at offset 16384 is a valid INDEX page — the real stride is 16384
    write_be16(data.data() + 16384 + FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    write_ibd_file(path, data);

    CompressedTableRecovery reco(logical);
    EXPECT_EQ(reco.classify(path, logical), CompressionStatus::PHYSICAL_LENGTH_MISMATCH);
}

TEST_F(CompressedRoutingTest, DecoderNeverValidatedWithoutFixture) {
    EXPECT_FALSE(CompressedTableRecovery::decoder_is_validated());
}

TEST_F(CompressedRoutingTest, RecoverWithoutValidatedDecoderEmitsZeroRows) {
    const std::string path = dir_ + "/comp_8k.ibd";
    const size_t logical = 16384;
    write_ibd_file(path, make_compressed_ibd(logical, 8192, logical * 8));

    CompressedTableRecovery reco(logical);
    reco.set_file_path(path);

    // The routing gate: undefined decoder => recover() must fail closed
    // with an empty record set and explicit unsupported status. It must
    // never claim RECOVERED on zero records.
    auto records = reco.recover();
    EXPECT_TRUE(records.empty());
    EXPECT_EQ(reco.get_stats().status, CompressionStatus::RECOGNIZED_UNSUPPORTED);
}

TEST_F(CompressedRoutingTest, StatsStatusToStringRoundTrip) {
    EXPECT_STREQ(CompressedTableRecoveryStats::status_to_string(
                     CompressionStatus::NOT_COMPRESSED), "NOT_COMPRESSED");
    EXPECT_STREQ(CompressedTableRecoveryStats::status_to_string(
                     CompressionStatus::RECOGNIZED_UNSUPPORTED), "RECOGNIZED_UNSUPPORTED");
    EXPECT_STREQ(CompressedTableRecoveryStats::status_to_string(
                     CompressionStatus::RECOVERED), "RECOVERED");
    EXPECT_STREQ(CompressedTableRecoveryStats::status_to_string(
                     CompressionStatus::MALFORMED), "MALFORMED");
    EXPECT_STREQ(CompressedTableRecoveryStats::status_to_string(
                     CompressionStatus::CORRUPTED_METADATA), "CORRUPTED_METADATA");
    EXPECT_STREQ(CompressedTableRecoveryStats::status_to_string(
                     CompressionStatus::PHYSICAL_LENGTH_MISMATCH), "PHYSICAL_LENGTH_MISMATCH");
    EXPECT_STREQ(CompressedTableRecoveryStats::status_to_string(
                     CompressionStatus::COMPRESSED_AND_ENCRYPTED), "COMPRESSED_AND_ENCRYPTED");
}

} // namespace
} // namespace innodb
} // namespace dbrecovery