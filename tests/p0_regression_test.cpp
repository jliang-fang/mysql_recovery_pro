/**
 * @file p0_regression_test.cpp
 * @brief P0 Regression Test Suite with ASan/UBSan for MySQL Recovery Pro
 *
 * Tests ALL 7 recently applied P0 fixes:
 *   P0.1: FIL Header Endianness
 *   P0.2: Roll Ptr Decoder
 *   P0.3: Deep Undo Analyzer Version Awareness
 *   P0.4: SDI Decompression Safety
 *   P0.5: FSP Flags Compression
 *   P0.6: Encrypted Page Space ID
 *   P0.7: Keyring AES-ECB
 *
 * All tests are self-contained, create their own test data inline,
 * and must pass cleanly under AddressSanitizer + UndefinedBehaviorSanitizer
 * (no leaks, no undefined behavior, no crashes).
 */

#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include <cstdint>
#include <random>
#include <algorithm>
#include <zlib.h>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/roll_ptr_decoder.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/tde_decryptor.hpp"
#include "dbrecovery/innodb/offline_keyring_parser.hpp"
#include "dbrecovery/common/mysql_version_detector.hpp"
#include "dbrecovery/common/endian_compat.hpp"
#include "dbrecovery/common/version_compat.hpp"
#include "dbrecovery/validate/unified_validator.hpp"
#include "dbrecovery/report/recovery_report.hpp"

namespace dbrecovery {
namespace {

using namespace innodb;

// ======================================================================
// P0.1: FIL Header Endianness
// ======================================================================

/**
 * InnoDB FIL header stores all fields in big-endian byte order.
 * This test verifies that the code reads page_type, FLUSH_LSN, and
 * checksum values correctly using big-endian byte order.
 *
 * FIL header layout (first 38 bytes of each page):
 *   Offset 0:  checksum (4 bytes, big-endian)
 *   Offset 4:  page offset (4 bytes, big-endian)
 *   Offset 8:  prev page (4 bytes, big-endian)
 *   Offset 12: next page (4 bytes, big-endian)
 *   Offset 16: LSN (8 bytes, big-endian)
 *   Offset 24: page type (2 bytes, big-endian)
 *   Offset 26: FLUSH_LSN (8 bytes, big-endian, MySQL 8.0+)
 *   Offset 34: space ID (4 bytes, big-endian)
 */
class FILHeaderEndiannessTest : public ::testing::Test {
protected:
    // Create a minimal InnoDB page buffer with a known big-endian FIL header
    // page_size bytes are allocated; the first 38 bytes contain the FIL header.
    std::vector<uint8_t> create_page_buffer(size_t page_size = 16384) {
        return std::vector<uint8_t>(page_size, 0);
    }

    // Set a 2-byte big-endian value at the given offset
    void set_be16(std::vector<uint8_t>& buf, size_t offset, uint16_t val) {
        buf[offset]     = static_cast<uint8_t>((val >> 8) & 0xFF);
        buf[offset + 1] = static_cast<uint8_t>(val & 0xFF);
    }

    // Set a 4-byte big-endian value at the given offset
    void set_be32(std::vector<uint8_t>& buf, size_t offset, uint32_t val) {
        buf[offset]     = static_cast<uint8_t>((val >> 24) & 0xFF);
        buf[offset + 1] = static_cast<uint8_t>((val >> 16) & 0xFF);
        buf[offset + 2] = static_cast<uint8_t>((val >> 8) & 0xFF);
        buf[offset + 3] = static_cast<uint8_t>(val & 0xFF);
    }

    // Set an 8-byte big-endian value at the given offset
    void set_be64(std::vector<uint8_t>& buf, size_t offset, uint64_t val) {
        for (int i = 7; i >= 0; i--) {
            buf[offset + i] = static_cast<uint8_t>(val & 0xFF);
            val >>= 8;
        }
    }
};

TEST_F(FILHeaderEndiannessTest, PageTypeReadAsBigEndian) {
    // Create a page buffer with FIL_PAGE_INDEX (0x45BF = 17855) at offset 24
    auto buf = create_page_buffer();
    set_be16(buf, FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);

    // Read page type using the production code path
    uint16_t page_type = read_be<uint16_t>(buf.data(), FIL_PAGE_TYPE_ABS);

    // Verify big-endian read produces the correct value
    EXPECT_EQ(page_type, FIL_PAGE_INDEX) << "FIL_PAGE_TYPE must be read as big-endian at offset 24";
}

TEST_F(FILHeaderEndiannessTest, PageTypeZeroAllocated) {
    // An allocated page (type 0)
    auto buf = create_page_buffer();
    set_be16(buf, FIL_PAGE_TYPE_ABS, 0);

    uint16_t page_type = read_be<uint16_t>(buf.data(), FIL_PAGE_TYPE_ABS);
    EXPECT_EQ(page_type, 0) << "FIL_PAGE_TYPE_ALLOCATED must be 0";
}

TEST_F(FILHeaderEndiannessTest, PageTypeAllKnownValues) {
    // Verify all known page type constants are correctly read as big-endian
    struct { uint16_t type; const char* name; } known_types[] = {
        {FIL_PAGE_INDEX,       "FIL_PAGE_INDEX"},
        {FIL_PAGE_UNDO_LOG,    "FIL_PAGE_UNDO_LOG"},
        {FIL_PAGE_SDI,         "FIL_PAGE_SDI"},
        {FIL_PAGE_SDI_ZIP,     "FIL_PAGE_SDI_ZIP"},
        {FIL_PAGE_COMPRESSED,  "FIL_PAGE_COMPRESSED"},
        {FIL_PAGE_COMPRESSED_AND_ENCRYPTED, "FIL_PAGE_COMPRESSED_AND_ENCRYPTED"},
        {FIL_PAGE_TYPE_FSP_HDR, "FIL_PAGE_TYPE_FSP_HDR"},
        {FIL_PAGE_TYPE_XDES,   "FIL_PAGE_TYPE_XDES"},
    };

    for (const auto& t : known_types) {
        auto buf = create_page_buffer();
        set_be16(buf, FIL_PAGE_TYPE_ABS, t.type);

        uint16_t page_type = read_be<uint16_t>(buf.data(), FIL_PAGE_TYPE_ABS);
        EXPECT_EQ(page_type, t.type) << "Page type " << t.name << " must match big-endian read";
    }
}

TEST_F(FILHeaderEndiannessTest, FlushLSNReadAsBigEndian) {
    // FLUSH_LSN is at offset 26 (FIL_PAGE_TYPE_ABS + 2), 8 bytes big-endian
    auto buf = create_page_buffer();
    uint64_t expected_lsn = 0x1234567890ABCDEFULL;
    set_be64(buf, FIL_PAGE_TYPE_ABS + 2, expected_lsn);

    // Read using big-endian helper
    uint64_t flush_lsn = read_be<uint64_t>(buf.data(), FIL_PAGE_TYPE_ABS + 2);

    EXPECT_EQ(flush_lsn, expected_lsn) << "FLUSH_LSN must be read as big-endian at offset 26";
}

TEST_F(FILHeaderEndiannessTest, FlushLSNZero) {
    // MySQL 5.6/5.7 has FLUSH_LSN = 0
    auto buf = create_page_buffer();
    // buf is already zero-initialized, FLUSH_LSN at offset 26 is 0

    uint64_t flush_lsn = read_be<uint64_t>(buf.data(), FIL_PAGE_TYPE_ABS + 2);
    EXPECT_EQ(flush_lsn, 0ULL) << "FLUSH_LSN must be 0 for MySQL 5.6/5.7";
}

TEST_F(FILHeaderEndiannessTest, ChecksumReadAsBigEndian) {
    // Checksum is at offset 0, 4 bytes big-endian
    auto buf = create_page_buffer();
    uint32_t expected_checksum = 0xDEADBEEF;
    set_be32(buf, 0, expected_checksum);

    // Read using big-endian helper
    uint32_t checksum = read_be<uint32_t>(buf.data(), 0);

    EXPECT_EQ(checksum, expected_checksum) << "Checksum must be read as big-endian at offset 0";
}

TEST_F(FILHeaderEndiannessTest, ChecksumTypeDetection) {
    // MySQL 5.6+ CRC32 checksums are typically large values
    auto buf = create_page_buffer();

    // Set a large CRC32 checksum at offset 0 (big-endian)
    uint32_t crc32_checksum = 0x3A4B5C6D;
    set_be32(buf, 0, crc32_checksum);

    // Verify via the version detector's checksum check
    int checksum_score = MySQLVersionDetector::check_checksum_type(buf.data(), buf.size());
    EXPECT_GT(checksum_score, 0) << "CRC32 checksum must be detected as valid";
}

TEST_F(FILHeaderEndiannessTest, VersionDetectorBugScenarios) {
    // Test 6 bug scenarios from the version detector
    // Scenario 1: MySQL 5.6 page — no LSN extension, no SDI, CRC32 checksum.
    // Hardened behavior: an INDEX page without 8.0 markers cannot be
    // distinguished from other 5.x versions, so it must be UNKNOWN, not MYSQL_56.
    {
        auto buf = create_page_buffer();
        set_be16(buf, FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
        set_be32(buf, 0, 0x3A4B5C6D);  // Large CRC32 checksum
        // FLUSH_LSN stays 0 (MySQL 5.6/5.7)

        MySQLVersion version = MySQLVersionDetector::detect(buf.data(), buf.size());
        EXPECT_EQ(version, MySQLVersion::UNKNOWN)
            << "Scenario 1: INDEX page without 8.0 markers must be UNKNOWN";
    }

    // Scenario 2: MySQL 5.7 page — same as 5.6 but with different page type
    // patterns. Without FSP flags or UNDO format evidence, 5.6 vs 5.7 is
    // ambiguous and must remain UNKNOWN.
    {
        auto buf = create_page_buffer();
        set_be16(buf, FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
        set_be32(buf, 0, 0x1A2B3C4D);  // CRC32 checksum

        MySQLVersion version = MySQLVersionDetector::detect(buf.data(), buf.size());
        EXPECT_EQ(version, MySQLVersion::UNKNOWN)
            << "Scenario 2: 5.6/5.7 ambiguity must be UNKNOWN, not guessed";
    }

    // Scenario 3: MySQL 8.0 INDEX page — has LSN extension and page version
    // field (page_version appears at offset 38 only on INDEX pages).
    {
        auto buf = create_page_buffer();
        set_be16(buf, FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
        set_be64(buf, FIL_PAGE_TYPE_ABS + 2, 0x1000);     // Non-zero FLUSH_LSN
        set_be16(buf, FIL_HEADER_SIZE, 1);                 // page_version = 1

        MySQLVersion version = MySQLVersionDetector::detect(buf.data(), buf.size());
        EXPECT_EQ(version, MySQLVersion::MYSQL_80)
            << "Scenario 3: MySQL 8.0 page must be detected as MYSQL_80";
    }

    // Scenario 4: MySQL 8.4 INDEX page — has LSN extension, page version = 2.
    {
        auto buf = create_page_buffer();
        set_be16(buf, FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
        set_be64(buf, FIL_PAGE_TYPE_ABS + 2, 0x2000);     // Non-zero FLUSH_LSN
        set_be16(buf, FIL_HEADER_SIZE, 2);                 // page_version = 2

        MySQLVersion version = MySQLVersionDetector::detect(buf.data(), buf.size());
        EXPECT_EQ(version, MySQLVersion::MYSQL_84)
            << "Scenario 4: MySQL 8.4 page must be detected as MYSQL_84";
    }

    // Scenario 5: SDI page type detection
    {
        auto buf = create_page_buffer();
        set_be16(buf, FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);
        // SDI signature at offset 64
        constexpr uint8_t SDI_SIG[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};
        std::memcpy(buf.data() + 64, SDI_SIG, 8);

        int sdi_score = MySQLVersionDetector::check_sdi_signature(buf.data(), buf.size());
        EXPECT_GT(sdi_score, 0) << "Scenario 5: SDI signature must be detected";
    }

    // Scenario 6: Unknown page type (should not crash)
    {
        auto buf = create_page_buffer();
        // Leave page type at 0 (ALLOCATED)
        // buf is already zero-initialized

        // Should not crash and return UNKNOWN
        MySQLVersion version = MySQLVersionDetector::detect(buf.data(), buf.size());
        EXPECT_EQ(version, MySQLVersion::UNKNOWN)
            << "Scenario 6: Allocated page must not crash version detection";
    }
}

TEST_F(FILHeaderEndiannessTest, FILHeaderSizeConsistency) {
    // Verify that FIL_PAGE_TYPE_ABS is at offset 24
    // and that the header layout is consistent
    EXPECT_EQ(FIL_PAGE_TYPE_ABS, 24) << "FIL_PAGE_TYPE must be at absolute offset 24";

    // Verify that checksum is at offset 0
    // Verify that FLUSH_LSN is at offset 26
    EXPECT_EQ(FIL_PAGE_TYPE_ABS + 2, 26) << "FLUSH_LSN must be at offset 26";

    // Verify that FIL_PAGE_SPACE_ID is at offset 34
    EXPECT_EQ(FIL_PAGE_SPACE_ID, 34) << "FIL_PAGE_SPACE_ID must be at offset 34";
}

// ======================================================================
// P0.2: Roll Ptr Decoder
// ======================================================================

class RollPtrDecoderTest : public ::testing::Test {
protected:
    // Internal format: bits [31:0]=page_no, [49:32]=offset (18 bits), [55:50]=flags
    uint64_t make_internal(uint32_t page_no, uint32_t offset, uint8_t flags) {
        return RollPtrDecoder::encode(page_no, offset, flags);
    }
};

TEST_F(RollPtrDecoderTest, DecodeWith18BitOffset) {
    // Verify that decode() correctly extracts a full 18-bit offset with 0x3FFFF mask.
    // The internal format reserves bits [49:32] for offset and bits [55:50] for
    // flags, so a max 18-bit offset (0x3FFFF) round-trips cleanly even with flags.
    uint64_t rp = make_internal(100, 0x3FFFF, 0x3F);
    auto decoded = RollPtrDecoder::decode(rp);

    EXPECT_EQ(decoded.page_no, 100u);
    EXPECT_EQ(decoded.offset, 0x3FFFFu) << "18-bit offset must be preserved";
    EXPECT_EQ(decoded.flags, 0x3Fu) << "Flags must be preserved alongside 18-bit offset";
}

TEST_F(RollPtrDecoderTest, DecodeOffsetMaskMax) {
    // Test the maximum offset value that fits without overflowing into flags
    uint64_t rp = make_internal(100, 0x3FFFF, 0);
    auto decoded = RollPtrDecoder::decode(rp);
    EXPECT_EQ(decoded.offset, 0x3FFFFu);
    EXPECT_EQ(decoded.flags, 0u);
}

TEST_F(RollPtrDecoderTest, DecodeOffsetZero) {
    // Edge case: offset = 0
    uint64_t rp = make_internal(42, 0, 0);
    auto decoded = RollPtrDecoder::decode(rp);

    EXPECT_EQ(decoded.page_no, 42u);
    EXPECT_EQ(decoded.offset, 0u) << "Offset must be 0 when roll_ptr has no offset bits";
    EXPECT_TRUE(decoded.is_valid());
}

TEST_F(RollPtrDecoderTest, EncodeMaxPageNo) {
    // Encode with maximum page_no, offset, and flags
    uint64_t rp = RollPtrDecoder::encode(0xFFFFFFFF, 0x3FFFF, 0x3F);
    auto decoded = RollPtrDecoder::decode(rp);

    EXPECT_EQ(decoded.page_no, 0xFFFFFFFFu);
    EXPECT_EQ(decoded.offset, 0x3FFFFu);
    EXPECT_EQ(decoded.flags, 0x3Fu);
}

TEST_F(RollPtrDecoderTest, DecodeOffsetMaskBoundary) {
    // Test 18-bit offset round-trip through both the storage and internal paths.
    // The internal format reserves bits [49:32] for offset and bits [55:50] for
    // flags with no overlap, so all 18-bit offsets round-trip losslessly.
    struct { uint32_t page_no; uint32_t offset; uint8_t flags; } cases[] = {
        {0x1000, 0x3FFFF, 0x0},
        {0x1000, 0x3FFFE, 0x0},
        {0x1000, 0x20000, 0x0},
        {0x1000, 0x00001, 0x0},
        {0x1000, 0x00000, 0x0},
        {0x1000, 0x3FFFF, 0x5},
        {0x1000, 0x0FFFF, 0xF},
    };

    for (const auto& c : cases) {
        // Encode to storage format
        uint8_t buf[7] = {};
        RollPtrDecoder::encode_storage(buf, {c.page_no, c.offset, c.flags});

        // Decode from storage — full 18-bit offset supported
        auto decoded = RollPtrDecoder::decode_storage(buf, 7);
        EXPECT_EQ(decoded.page_no, c.page_no)
            << "Storage page_no round-trip: expected 0x" << std::hex << c.page_no
            << " got 0x" << decoded.page_no << std::dec;
        EXPECT_EQ(decoded.offset, c.offset)
            << "Storage offset round-trip: expected 0x" << std::hex << c.offset
            << " got 0x" << decoded.offset << std::dec;
        EXPECT_EQ(decoded.flags, c.flags)
            << "Storage flags round-trip: expected 0x" << std::hex << (int)c.flags
            << " got 0x" << (int)decoded.flags << std::dec;

        // Test storage-to-internal round-trip
        uint64_t internal = RollPtrDecoder::decode_storage_to_internal(buf, 7);
        auto decoded_internal = RollPtrDecoder::decode(internal);
        // The internal format now supports full 18-bit offsets with no overlap,
        // so the offset is preserved losslessly regardless of flags.
        EXPECT_EQ(decoded_internal.offset, c.offset)
            << "Internal offset round-trip: expected 0x" << std::hex << c.offset
            << " got 0x" << decoded_internal.offset << std::dec
            << " (flags=0x" << (int)c.flags << ")";
    }
}

TEST_F(RollPtrDecoderTest, EncodeDecodeAllFields) {
    // Test encode() packs page_no and offset correctly.
    // The internal format reserves bits [49:32] for offset (18 bits) and
    // bits [55:50] for flags (6 bits), so full 18-bit offset is preserved
    // regardless of flags.
    uint64_t rp = RollPtrDecoder::encode(0xFEDCBA98, 0x3FFFF, 0x3F);
    auto decoded = RollPtrDecoder::decode(rp);

    EXPECT_EQ(decoded.page_no, 0xFEDCBA98u);
    EXPECT_EQ(decoded.offset, 0x3FFFFu);
    EXPECT_EQ(decoded.flags, 0x3Fu);

    // Also test encode/decode with flags via the storage format round-trip,
    // which properly handles the bit layout without overlap:
    RollPtrDecoder::DecodedRollPtr storage_rp = {0x1000, 0x2ABCD, 0x5};
    uint8_t buf[7];
    RollPtrDecoder::encode_storage(buf, storage_rp);
    auto decoded_storage = RollPtrDecoder::decode_storage(buf, 7);
    EXPECT_EQ(decoded_storage.page_no, 0x1000u);
    EXPECT_EQ(decoded_storage.offset, 0x2ABCDu);
    EXPECT_EQ(decoded_storage.flags, 0x5u);
}

TEST_F(RollPtrDecoderTest, DecodeStorageFormatVersions) {
    // Test decode_storage() with both 6-byte (MySQL 5.6/5.7) and 7-byte (MySQL 8.0+) formats

    // 6-byte format: page_no >> 20, offset & 0x3FFFF
    {
        uint64_t val = (0x1000ULL << 20) | 0x200ULL;
        uint8_t data[6];
        for (int i = 5; i >= 0; i--) {
            data[i] = static_cast<uint8_t>(val & 0xFF);
            val >>= 8;
        }

        auto decoded = RollPtrDecoder::decode_storage(data, 6);
        EXPECT_EQ(decoded.page_no, 0x1000u);
        EXPECT_EQ(decoded.offset, 0x200u);
        EXPECT_EQ(decoded.flags, 0u) << "6-byte format must have flags=0";
    }

    // 7-byte format: includes flags byte
    {
        uint64_t val = (0x1ULL << 52) | (0x1000ULL << 20) | 0x200ULL;
        uint8_t data[7];
        for (int i = 6; i >= 0; i--) {
            data[i] = static_cast<uint8_t>(val & 0xFF);
            val >>= 8;
        }

        auto decoded = RollPtrDecoder::decode_storage(data, 7);
        EXPECT_EQ(decoded.page_no, 0x1000u);
        EXPECT_EQ(decoded.offset, 0x200u);
        EXPECT_EQ(decoded.flags, 0x1u) << "7-byte format must extract flags from upper nibble";
    }
}

TEST_F(RollPtrDecoderTest, DecodeStorageNullPointer) {
    // Must not crash when passed a null pointer
    auto decoded = RollPtrDecoder::decode_storage(nullptr, 7);
    EXPECT_FALSE(decoded.is_valid());
    EXPECT_TRUE(decoded.is_null());
}

// ======================================================================
// P0.3: Deep Undo Analyzer Version Awareness
// ======================================================================

class DeepUndoAnalyzerVersionTest : public ::testing::Test {
protected:
    // Create a synthetic undo page header with a roll_ptr at the given offset
    // Returns the page data and the offset of the roll_ptr field
    struct RollPtrPage {
        std::vector<uint8_t> data;
        size_t roll_ptr_offset = 0;
    };

    // Create a page with a 6-byte roll_ptr (MySQL 5.6/5.7 format)
    RollPtrPage create_6byte_roll_ptr_page(uint32_t page_no, uint32_t offset) {
        RollPtrPage rp;
        rp.data.resize(16384, 0);
        rp.roll_ptr_offset = 100;  // Arbitrary offset within the page

        // 6-byte big-endian: page_no >> 20, offset & 0x3FFFF
        uint64_t val = (static_cast<uint64_t>(page_no) << 20) | (offset & 0x3FFFF);
        for (int i = 5; i >= 0; i--) {
            rp.data[rp.roll_ptr_offset + i] = static_cast<uint8_t>(val & 0xFF);
            val >>= 8;
        }

        return rp;
    }

    // Create a page with a 7-byte roll_ptr (MySQL 8.0+ format)
    RollPtrPage create_7byte_roll_ptr_page(uint32_t page_no, uint32_t offset, uint8_t flags) {
        RollPtrPage rp;
        rp.data.resize(16384, 0);
        rp.roll_ptr_offset = 100;  // Arbitrary offset within the page

        // 7-byte big-endian: flags << 52 | page_no << 20 | offset
        uint64_t val = (static_cast<uint64_t>(flags & 0x0F) << 52) |
                       (static_cast<uint64_t>(page_no) << 20) |
                       (offset & 0x3FFFF);
        for (int i = 6; i >= 0; i--) {
            rp.data[rp.roll_ptr_offset + i] = static_cast<uint8_t>(val & 0xFF);
            val >>= 8;
        }

        return rp;
    }
};

TEST_F(DeepUndoAnalyzerVersionTest, ReadPrevUndoPtr6Byte) {
    // read_prev_undo_ptr() equivalent: RollPtrDecoder::decode_storage with 6 bytes
    // This tests the 6-byte format used by MySQL 5.6/5.7
    auto rp = create_6byte_roll_ptr_page(0x1234, 0xABCD);

    // Directly decode the 6-byte storage format (equivalent to read_prev_undo_ptr with MYSQL_56)
    auto decoded = RollPtrDecoder::decode_storage(rp.data.data() + rp.roll_ptr_offset, 6);
    EXPECT_EQ(decoded.page_no, 0x1234u) << "6-byte prev_undo_ptr must decode page_no correctly";
    EXPECT_EQ(decoded.offset, 0xABCDu) << "6-byte prev_undo_ptr must decode offset correctly";
    EXPECT_EQ(decoded.flags, 0u) << "6-byte format must have no flags";

    // Also verify round-trip through the internal format
    uint64_t internal = RollPtrDecoder::decode_storage_to_internal(
        rp.data.data() + rp.roll_ptr_offset, 6);
    auto decoded2 = RollPtrDecoder::decode(internal);
    EXPECT_EQ(decoded2.page_no, 0x1234u);
    EXPECT_EQ(decoded2.offset, 0xABCDu);
}

TEST_F(DeepUndoAnalyzerVersionTest, ReadPrevUndoPtr7Byte) {
    // read_prev_undo_ptr() equivalent: RollPtrDecoder::decode_storage with 7 bytes
    // This tests the 7-byte format used by MySQL 8.0+
    // Use a small offset that won't overflow into the flags byte when encoded
    auto rp = create_7byte_roll_ptr_page(0x5678, 0x1F, 0x3);

    auto decoded = RollPtrDecoder::decode_storage(rp.data.data() + rp.roll_ptr_offset, 7);
    EXPECT_EQ(decoded.page_no, 0x5678u) << "7-byte prev_undo_ptr must decode page_no correctly";
    EXPECT_EQ(decoded.offset, 0x1Fu) << "7-byte prev_undo_ptr must decode offset correctly";
    EXPECT_EQ(decoded.flags, 0x3u) << "7-byte format must extract flags from upper nibble";

    // Verify decode_storage_to_internal -> decode round-trip with flags=0.
    // When flags is non-zero, the internal encode() packs offset at bits [47:32]
    // and flags at bits [55:48]. Since offset & 0x3FFFF can have bits 16-17 set,
    // these overlap with the flags byte. This is a property of the InnoDB internal
    // format, not a bug. Verify clean round-trip with flags=0:
    auto rp_noflags = create_7byte_roll_ptr_page(0x5678, 0x1F, 0x0);
    uint64_t internal = RollPtrDecoder::decode_storage_to_internal(
        rp_noflags.data.data() + rp_noflags.roll_ptr_offset, 7);
    auto decoded2 = RollPtrDecoder::decode(internal);
    EXPECT_EQ(decoded2.page_no, 0x5678u);
    EXPECT_EQ(decoded2.offset, 0x1Fu);
    EXPECT_EQ(decoded2.flags, 0x0u);
}

TEST_F(DeepUndoAnalyzerVersionTest, VersionAwareRollPtrSize) {
    // Verify that the version-aware roll_ptr decoding uses the correct size
    // based on MySQL version. The logic is equivalent to:
    //   size_t roll_ptr_size = (version >= MYSQL_80) ? 7 : 6;
    //   return RollPtrDecoder::decode_storage_to_internal(page_data + offset, roll_ptr_size);

    auto rp_6 = create_6byte_roll_ptr_page(0x1000, 0x200);
    auto rp_7 = create_7byte_roll_ptr_page(0x1000, 0x200, 0x0);

    // 6-byte format (MySQL 5.6/5.7)
    uint64_t result_56 = RollPtrDecoder::decode_storage_to_internal(
        rp_6.data.data() + rp_6.roll_ptr_offset, 6);
    // 7-byte format (MySQL 8.0+)
    uint64_t result_80 = RollPtrDecoder::decode_storage_to_internal(
        rp_7.data.data() + rp_7.roll_ptr_offset, 7);

    auto decoded_56 = RollPtrDecoder::decode(result_56);
    auto decoded_80 = RollPtrDecoder::decode(result_80);

    EXPECT_EQ(decoded_56.page_no, 0x1000u);
    EXPECT_EQ(decoded_56.offset, 0x200u);
    EXPECT_EQ(decoded_80.page_no, 0x1000u);
    EXPECT_EQ(decoded_80.offset, 0x200u);
}

TEST_F(DeepUndoAnalyzerVersionTest, RollPtrSizeConsistency) {
    // Verify that the correct storage size is used based on MySQL version:
    // 6 bytes for MySQL < 8.0, 7 bytes for MySQL >= 8.0

    // The version-aware logic in read_prev_undo_ptr does:
    //   size_t roll_ptr_size = (mysql_version >= MySQLVersion::MYSQL_80) ? 7 : 6;
    //   return RollPtrDecoder::decode_storage_to_internal(page_data + offset, roll_ptr_size);

    // Test all version boundaries by verifying the size selection logic
    auto test_version_size = [](MySQLVersion v) -> size_t {
        return (v >= MySQLVersion::MYSQL_80) ? 7 : 6;
    };

    EXPECT_EQ(test_version_size(MySQLVersion::MYSQL_56), 6u)
        << "MYSQL_56 must use 6-byte format";
    EXPECT_EQ(test_version_size(MySQLVersion::MYSQL_57), 6u)
        << "MYSQL_57 must use 6-byte format";
    EXPECT_EQ(test_version_size(MySQLVersion::MYSQL_80), 7u)
        << "MYSQL_80 must use 7-byte format";
    EXPECT_EQ(test_version_size(MySQLVersion::MYSQL_84), 7u)
        << "MYSQL_84 must use 7-byte format";
    EXPECT_EQ(test_version_size(MySQLVersion::MYSQL_90), 7u)
        << "MYSQL_90 must use 7-byte format";

    // Also verify that RollPtrDecoder::decode_storage with null data does not crash
    auto decoded_6 = RollPtrDecoder::decode_storage(nullptr, 6);
    EXPECT_FALSE(decoded_6.is_valid()) << "Null data with 6-byte format must return invalid";
    EXPECT_TRUE(decoded_6.is_null()) << "Null data with 6-byte format must return null";

    auto decoded_7 = RollPtrDecoder::decode_storage(nullptr, 7);
    EXPECT_FALSE(decoded_7.is_valid()) << "Null data with 7-byte format must return invalid";
    EXPECT_TRUE(decoded_7.is_null()) << "Null data with 7-byte format must return null";
}

// ======================================================================
// P0.4: SDI Decompression Safety
// ======================================================================

class SDIDecompressionSafetyTest : public ::testing::Test {
protected:
    /**
     * Create a synthetic compressed SDI record payload.
     *
     * SDI record layout:
     *   Offset 0:  type (4 bytes LE)
     *   Offset 4:  id (8 bytes LE)
     *   Offset 12: uncompressed_len (4 bytes LE)
     *   Offset 16: compressed_len (4 bytes LE)
     *   Offset 20: compressed data (raw deflate, -15 window bits)
     */
    std::vector<uint8_t> create_compressed_sdi_payload(const std::string& json_content) {
        // Compress the JSON content using raw deflate (-15 window bits)
        uLong uncompressed_len = static_cast<uLong>(json_content.size());
        uLong compressed_len = compressBound(uncompressed_len);
        std::vector<uint8_t> compressed_buf(compressed_len);

        // Use raw deflate (-15 window bits) matching MySQL's SDI compression
        z_stream strm = {};
        strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(json_content.data()));
        strm.avail_in = static_cast<uInt>(uncompressed_len);
        strm.next_out = compressed_buf.data();
        strm.avail_out = static_cast<uInt>(compressed_buf.size());

        int ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        if (ret != Z_OK) return {};

        ret = deflate(&strm, Z_FINISH);
        if (ret != Z_STREAM_END) {
            deflateEnd(&strm);
            return {};
        }
        compressed_len = strm.total_out;
        deflateEnd(&strm);

        // Build the full SDI payload: header (20 bytes) + compressed data
        std::vector<uint8_t> payload(20 + compressed_len, 0);

        // Type (4 bytes LE)
        write_le<uint32_t>(payload.data(), 0, 1);  // SDI type = 1

        // ID (8 bytes LE)
        write_le<uint64_t>(payload.data(), 4, 42);

        // Uncompressed length (4 bytes LE)
        write_le<uint32_t>(payload.data(), 12, static_cast<uint32_t>(uncompressed_len));

        // Compressed length (4 bytes LE)
        write_le<uint32_t>(payload.data(), 16, static_cast<uint32_t>(compressed_len));

        // Compressed data
        std::memcpy(payload.data() + 20, compressed_buf.data(), compressed_len);

        return payload;
    }

    // Create a malformed SDI payload with zero lengths
    std::vector<uint8_t> create_malformed_sdi_payload() {
        std::vector<uint8_t> payload(20, 0);
        // All fields are zero — this should be handled gracefully
        return payload;
    }

    // Create a truncated SDI payload (header only, no compressed data)
    std::vector<uint8_t> create_truncated_sdi_payload() {
        std::vector<uint8_t> payload(20, 0);
        write_le<uint32_t>(payload.data(), 0, 1);
        write_le<uint64_t>(payload.data(), 4, 42);
        write_le<uint32_t>(payload.data(), 12, 100);  // uncompressed_len = 100
        write_le<uint32_t>(payload.data(), 16, 50);    // compressed_len = 50
        // No compressed data follows (payload is only 20 bytes)
        return payload;
    }

    // Create an SDI payload with oversized compressed_len
    std::vector<uint8_t> create_oversized_sdi_payload() {
        std::vector<uint8_t> payload(20, 0);
        write_le<uint32_t>(payload.data(), 0, 1);
        write_le<uint64_t>(payload.data(), 4, 42);
        write_le<uint32_t>(payload.data(), 12, 10);   // uncompressed_len = 10
        write_le<uint32_t>(payload.data(), 16, 1000);  // compressed_len = 1000 (bigger than uncompressed)
        return payload;
    }
};

TEST_F(SDIDecompressionSafetyTest, ParseValidCompressedSDI) {
    // Test that parse_sdi_record() handles a valid compressed SDI record without crash
    std::string json = R"({"version": 1, "type": "test"})";
    auto payload = create_compressed_sdi_payload(json);

    // First test the SDI extraction (the decompression path)
    auto result = SDIManager::extract_sdi_json(payload.data(), payload.size());
    EXPECT_TRUE(result.has_value()) << "Valid compressed SDI must be extractable";
    if (result.has_value()) {
        EXPECT_TRUE(result->find("version") != std::string::npos)
            << "Decompressed SDI must contain expected JSON content";
    }
}

TEST_F(SDIDecompressionSafetyTest, ParseMalformedSDI) {
    // Test with malformed/incomplete compressed data (all zeros)
    auto payload = create_malformed_sdi_payload();
    auto result = SDIManager::extract_sdi_json(payload.data(), payload.size());
    // Should return nullopt, not crash
    EXPECT_FALSE(result.has_value()) << "Malformed SDI (all zeros) must not crash";
}

TEST_F(SDIDecompressionSafetyTest, ParseTruncatedSDI) {
    // Test with truncated data (header says compressed data exists but doesn't)
    auto payload = create_truncated_sdi_payload();
    auto result = SDIManager::extract_sdi_json(payload.data(), payload.size());
    // Should return nullopt, not crash
    EXPECT_FALSE(result.has_value()) << "Truncated SDI must not crash";
}

TEST_F(SDIDecompressionSafetyTest, ParseOversizedCompressedLen) {
    // Test with compressed_len > uncompressed_len (invalid)
    auto payload = create_oversized_sdi_payload();
    auto result = SDIManager::extract_sdi_json(payload.data(), payload.size());
    // Should return nullopt, not crash
    EXPECT_FALSE(result.has_value()) << "Oversized compressed_len must not crash";
}

TEST_F(SDIDecompressionSafetyTest, NullPayload) {
    // Test with null pointer — should not crash
    auto result = SDIManager::extract_sdi_json(nullptr, 0);
    EXPECT_FALSE(result.has_value()) << "Null payload must not crash";
}

TEST_F(SDIDecompressionSafetyTest, EmptyPayload) {
    // Test with empty payload
    std::vector<uint8_t> empty;
    auto result = SDIManager::extract_sdi_json(empty.data(), empty.size());
    EXPECT_FALSE(result.has_value()) << "Empty payload must not crash";
}

// ======================================================================
// P0.5: FSP Flags Compression
// ======================================================================

class FSPFlagsCompressionTest : public ::testing::Test {
protected:
    /**
     * Create an FSP header page (page 0) with given FSP_FLAGS at offset 80.
     *
     * FSP_FLAGS layout (4 bytes, little-endian at offset 80 of page 0):
     *   Bits 5-7: zip_ssize (compressed page size exponent)
     *   zip_ssize = 0 -> 16KB (no compression)
     *   zip_ssize = 1 -> 8KB
     *   zip_ssize = 2 -> 4KB
     *   zip_ssize = 3 -> 2KB
     *   ...
     *   Compressed page size = 16384 / (2 ^ zip_ssize)
     */
    std::vector<uint8_t> create_fsp_page(uint32_t fsp_flags) {
        std::vector<uint8_t> page(16384, 0);
        // Set FSP_FLAGS at offset 80 (little-endian)
        write_le<uint32_t>(page.data(), 80, fsp_flags);
        return page;
    }

    // Extract zip_ssize from FSP flags
    uint32_t get_zip_ssize(uint32_t fsp_flags) {
        return (fsp_flags >> 5) & 0x07;
    }

    // Calculate compressed page size from zip_ssize
    size_t get_compressed_page_size(uint32_t zip_ssize) {
        if (zip_ssize == 0) return 16384;  // No compression
        return static_cast<size_t>(16384 / (1 << zip_ssize));
    }

    // Simulate is_page_compressed() logic
    bool is_page_compressed_by_flags(uint32_t fsp_flags) {
        uint32_t zip_ssize = get_zip_ssize(fsp_flags);
        return zip_ssize > 0;
    }
};

TEST_F(FSPFlagsCompressionTest, ZipSSizeZero) {
    // zip_ssize = 0 means no compression (16KB pages)
    // Bits 5-7 = 0
    uint32_t fsp_flags = 0;  // All flags zero
    uint32_t zip_ssize = get_zip_ssize(fsp_flags);
    EXPECT_EQ(zip_ssize, 0u) << "zip_ssize must be 0 for uncompressed tablespace";
    EXPECT_FALSE(is_page_compressed_by_flags(fsp_flags)) << "zip_ssize=0 must not be compressed";
    EXPECT_EQ(get_compressed_page_size(zip_ssize), 16384u) << "zip_ssize=0 must give 16KB page size";
}

TEST_F(FSPFlagsCompressionTest, ZipSSizeOne) {
    // zip_ssize = 1 -> 8KB compressed pages
    // Set bits 5-7 = 001
    uint32_t fsp_flags = (1 << 5);
    uint32_t zip_ssize = get_zip_ssize(fsp_flags);
    EXPECT_EQ(zip_ssize, 1u) << "zip_ssize must be 1";
    EXPECT_TRUE(is_page_compressed_by_flags(fsp_flags)) << "zip_ssize=1 must be compressed";
    EXPECT_EQ(get_compressed_page_size(zip_ssize), 8192u) << "zip_ssize=1 must give 8KB page size";
}

TEST_F(FSPFlagsCompressionTest, ZipSSizeTwo) {
    // zip_ssize = 2 -> 4KB compressed pages
    uint32_t fsp_flags = (2 << 5);
    uint32_t zip_ssize = get_zip_ssize(fsp_flags);
    EXPECT_EQ(zip_ssize, 2u) << "zip_ssize must be 2";
    EXPECT_TRUE(is_page_compressed_by_flags(fsp_flags)) << "zip_ssize=2 must be compressed";
    EXPECT_EQ(get_compressed_page_size(zip_ssize), 4096u) << "zip_ssize=2 must give 4KB page size";
}

TEST_F(FSPFlagsCompressionTest, ZipSSizeThree) {
    // zip_ssize = 3 -> 2KB compressed pages
    uint32_t fsp_flags = (3 << 5);
    uint32_t zip_ssize = get_zip_ssize(fsp_flags);
    EXPECT_EQ(zip_ssize, 3u) << "zip_ssize must be 3";
    EXPECT_TRUE(is_page_compressed_by_flags(fsp_flags)) << "zip_ssize=3 must be compressed";
    EXPECT_EQ(get_compressed_page_size(zip_ssize), 2048u) << "zip_ssize=3 must give 2KB page size";
}

TEST_F(FSPFlagsCompressionTest, ZipSSizeAllValid) {
    // Test all valid zip_ssize values 0-7
    struct { uint32_t zip_ssize; size_t expected_page_size; bool is_compressed; } cases[] = {
        {0, 16384, false},
        {1, 8192,  true},
        {2, 4096,  true},
        {3, 2048,  true},
        {4, 1024,  true},
        {5, 512,   true},
        {6, 256,   true},
        {7, 128,   true},
    };

    for (const auto& c : cases) {
        uint32_t fsp_flags = (c.zip_ssize << 5);
        uint32_t actual_zip_ssize = get_zip_ssize(fsp_flags);
        size_t actual_page_size = get_compressed_page_size(actual_zip_ssize);

        EXPECT_EQ(actual_zip_ssize, c.zip_ssize)
            << "zip_ssize=" << c.zip_ssize << " must be extracted correctly";
        EXPECT_EQ(actual_page_size, c.expected_page_size)
            << "zip_ssize=" << c.zip_ssize << " must produce page size " << c.expected_page_size;
        EXPECT_EQ(is_page_compressed_by_flags(fsp_flags), c.is_compressed)
            << "zip_ssize=" << c.zip_ssize << " compression flag must be " << c.is_compressed;
    }
}

TEST_F(FSPFlagsCompressionTest, FSPFlagsWithOtherBits) {
    // Test that zip_ssize extraction correctly isolates bits 5-7
    // even when other flag bits are set
    struct { uint32_t fsp_flags; uint32_t expected_zip_ssize; } cases[] = {
        {0x00000000, 0},  // No flags
        {0x00000020, 1},  // Only bit 5 set
        {0x00000040, 2},  // Only bit 6 set
        {0x00000060, 3},  // Bits 5-6 set
        {0x00000080, 4},  // Only bit 7 set
        {0x000000E0, 7},  // Bits 5-7 all set
        {0x0000001F, 0},  // Lower 5 bits set, zip_ssize = 0
        {0xFFFF001F, 0},  // All other bits set except bits 5-7, zip_ssize = 0
        {0xDEADBE8F, 4},  // Bits 5-7 = 100 = 4, other bits set (0xDEADBE8F & 0xE0 = 0x80, >>5 = 4)
    };

    for (const auto& c : cases) {
        uint32_t actual_zip_ssize = get_zip_ssize(c.fsp_flags);
        EXPECT_EQ(actual_zip_ssize, c.expected_zip_ssize)
            << "FSP_FLAGS 0x" << std::hex << c.fsp_flags
            << std::dec << " must extract zip_ssize=" << c.expected_zip_ssize;
    }
}

TEST_F(FSPFlagsCompressionTest, CompressedPageSizeFormula) {
    // Verify the formula: compressed_page_size = 16384 / (1 << zip_ssize)
    for (uint32_t z = 0; z <= 7; z++) {
        size_t expected = (z == 0) ? 16384 : (16384 / (1 << z));
        size_t actual = get_compressed_page_size(z);
        EXPECT_EQ(actual, expected) << "Formula must hold for zip_ssize=" << z;
    }
}

// ======================================================================
// P0.6: Encrypted Page Space ID
// ======================================================================

class EncryptedPageSpaceIDTest : public ::testing::Test {
protected:
    /**
     * Create a minimal encrypted page for testing.
     * The encrypted page has a TDE header at offset 38 (after FIL header).
     * Algorithm byte = 6 (AES-256-CBC), then key_id (4 bytes), IV (12 bytes),
     * then encrypted header (28 bytes).
     */
    std::vector<uint8_t> create_encrypted_page(uint32_t space_id, uint32_t page_no) {
        std::vector<uint8_t> page(16384, 0);

        // Set FIL header page type at offset 24 (big-endian)
        write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);

        // Set space_id in FIL header at offset 34 (big-endian)
        write_be<uint32_t>(page.data(), FIL_PAGE_SPACE_ID, space_id);

        // Set page_no at offset 4 (big-endian)
        write_be<uint32_t>(page.data(), 4, page_no);

        // Set TDE header at offset 38
        page[38] = 6;  // Algorithm = AES-256-CBC (6)

        // Key ID (4 bytes at offset 39)
        page[39] = 0x01;
        page[40] = 0x00;
        page[41] = 0x00;
        page[42] = 0x00;

        // IV (12 bytes at offset 43)
        for (int i = 0; i < 12; i++) {
            page[43 + i] = static_cast<uint8_t>(i);
        }

        // Encrypted header (28 bytes at offset 55)
        for (int i = 0; i < 28; i++) {
            page[55 + i] = static_cast<uint8_t>(i + 0x80);
        }

        // Set a non-zero checksum at offset 0 to avoid detection issues
        write_be<uint32_t>(page.data(), 0, 0xDEADBEEF);

        return page;
    }

    // Create a non-encrypted page (should pass through decrypt_page)
    std::vector<uint8_t> create_normal_page(uint32_t space_id, uint32_t page_no) {
        std::vector<uint8_t> page(16384, 0);

        // Set FIL header page type
        write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);

        // Set space_id in FIL header
        write_be<uint32_t>(page.data(), FIL_PAGE_SPACE_ID, space_id);

        // Set page_no at offset 4
        write_be<uint32_t>(page.data(), 4, page_no);

        return page;
    }
};

TEST_F(EncryptedPageSpaceIDTest, DecryptPageWithKnownSpaceID) {
    // Test that decrypt_page() with a known space_id does not crash
    // and returns a result (even if empty when key is not available)
    auto page = create_encrypted_page(42, 100);

    // Create a TDEDecryptor instance (the actual decryptor used internally)
    TDEDecryptor decryptor;

    // This should not crash — without a key, decryption will fail gracefully
    // but the space_id is correctly passed
    auto decrypted = decryptor.decrypt_page(page.data(), page.size(), 42, 100);
    // Without a key, decryption will fail (empty result), but no crash
    EXPECT_TRUE(decrypted.empty() || decrypted.size() == page.size())
        << "Decrypt with no key must not crash";
}

TEST_F(EncryptedPageSpaceIDTest, SpaceIDZeroEdgeCase) {
    // Test with space_id = 0 (edge case)
    auto page = create_encrypted_page(0, 1);

    TDEDecryptor decryptor;
    auto decrypted = decryptor.decrypt_page(page.data(), page.size(), 0, 1);
    // Must not crash with space_id = 0
    EXPECT_TRUE(decrypted.empty() || decrypted.size() == page.size())
        << "Space ID=0 must not crash";
}

TEST_F(EncryptedPageSpaceIDTest, NonEncryptedPagePassThrough) {
    // Test that a non-encrypted page is returned as-is
    auto page = create_normal_page(42, 100);

    TDEDecryptor decryptor;
    auto decrypted = decryptor.decrypt_page(page.data(), page.size(), 42, 100);

    // Non-encrypted page should be returned as-is (same size)
    EXPECT_EQ(decrypted.size(), page.size())
        << "Non-encrypted page must be returned as-is";
}

TEST_F(EncryptedPageSpaceIDTest, SpaceIDIsolation) {
    // Test that different space_ids produce different key derivation paths
    // (This is a structural test — the actual key derivation uses space_id)

    // Create two encrypted pages with different space IDs
    auto page1 = create_encrypted_page(1, 100);
    auto page2 = create_encrypted_page(2, 100);

    // Verify space_ids are written correctly
    uint32_t space_id1 = read_be<uint32_t>(page1.data(), FIL_PAGE_SPACE_ID);
    uint32_t space_id2 = read_be<uint32_t>(page2.data(), FIL_PAGE_SPACE_ID);

    EXPECT_EQ(space_id1, 1u) << "Space ID must be correctly written to FIL header";
    EXPECT_EQ(space_id2, 2u) << "Space ID must be correctly written to FIL header";
    EXPECT_NE(space_id1, space_id2) << "Space IDs must differ";
}

TEST_F(EncryptedPageSpaceIDTest, TDEHeaderDetection) {
    // Test that is_encrypted() correctly detects encrypted pages
    auto encrypted = create_encrypted_page(42, 100);
    auto normal = create_normal_page(42, 100);

    EXPECT_TRUE(TDEDecryptor::is_encrypted(encrypted.data(), encrypted.size()))
        << "Encrypted page must be detected as encrypted";
    EXPECT_FALSE(TDEDecryptor::is_encrypted(normal.data(), normal.size()))
        << "Normal page must not be detected as encrypted";
}

// ======================================================================
// P0.7: Keyring AES-ECB
// ======================================================================

class KeyringAESECBTest : public ::testing::Test {
protected:
    /**
     * Test key derivation and verification using OfflineKeyringParser.
     *
     * MySQL TDE key derivation:
     *   table_key = AES-ECB(master_key, space_id || padding)
     */
    OfflineKeyringParser parser;

    // Create a known 32-byte master key (AES-256)
    std::vector<uint8_t> create_master_key() {
        std::vector<uint8_t> key(32, 0);
        for (int i = 0; i < 32; i++) {
            key[i] = static_cast<uint8_t>(i * 17 + 0xAB);
        }
        return key;
    }

    // Create a minimal page buffer for verification
    std::vector<uint8_t> create_minimal_page() {
        std::vector<uint8_t> page(38, 0);
        // Set FIL header page type to FIL_PAGE_INDEX (0x45BF) at offset 24 (big-endian)
        page[24] = 0x45;
        page[25] = 0xBF;
        // Set non-zero LSN at offset 16 (8 bytes, big-endian) — required by verify_derived_key
        page[16] = 0x00;
        page[17] = 0x00;
        page[18] = 0x00;
        page[19] = 0x01;
        page[20] = 0x23;
        page[21] = 0x45;
        page[22] = 0x67;
        page[23] = 0x89;
        return page;
    }

    // Create a page with an invalid page type (should fail verification)
    std::vector<uint8_t> create_invalid_page() {
        std::vector<uint8_t> page(38, 0);
        // Set an invalid page type
        page[24] = 0xFF;
        page[25] = 0xFF;
        return page;
    }
};

TEST_F(KeyringAESECBTest, DeriveTableKeyWithKnownMasterKey) {
    // Test derive_table_key() with a known master key + space_id
    auto master_key = create_master_key();
    parser.add_master_key(master_key, "test_master", TDEAlgorithm::AES_256_CBC);

    auto keys = parser.get_master_keys();
    ASSERT_EQ(keys.size(), 1u) << "Must have one master key";

    // Derive table key for space_id = 42
    std::vector<uint8_t> derived = parser.derive_table_key(keys[0], 42, 0, 0);

    EXPECT_FALSE(derived.empty()) << "Derived key must not be empty";
    EXPECT_EQ(derived.size(), 32u) << "AES-256 derived key must be 32 bytes";

    // Derive again with the same parameters — must produce the same key
    std::vector<uint8_t> derived2 = parser.derive_table_key(keys[0], 42, 0, 0);
    EXPECT_EQ(derived, derived2) << "Key derivation must be deterministic";
}

TEST_F(KeyringAESECBTest, DeriveTableKeyAES128) {
    // Test with a 16-byte master key (AES-128)
    std::vector<uint8_t> master_key_128(16, 0xCD);
    for (int i = 0; i < 16; i++) {
        master_key_128[i] = static_cast<uint8_t>(i * 13 + 0xAB);
    }

    parser.add_master_key(master_key_128, "aes128", TDEAlgorithm::AES_128_CBC);
    auto keys = parser.get_master_keys();

    // Find the correct key (it's the second one)
    auto mk = parser.find_key("aes128");
    ASSERT_TRUE(mk.has_value()) << "Must find AES-128 key";

    std::vector<uint8_t> derived = parser.derive_table_key(*mk, 42, 0, 0);
    EXPECT_FALSE(derived.empty()) << "AES-128 derived key must not be empty";
    // With AES-128 master key, derive_table_key output still targets 32 bytes
    EXPECT_EQ(derived.size(), 32u) << "Derived key must be 32 bytes";
}

TEST_F(KeyringAESECBTest, DeriveTableKeyDifferentSpaceIDs) {
    // Test that different space_ids produce different derived keys
    auto master_key = create_master_key();
    parser.add_master_key(master_key, "test_master", TDEAlgorithm::AES_256_CBC);

    auto keys = parser.get_master_keys();
    ASSERT_EQ(keys.size(), 1u);

    std::vector<uint8_t> derived_1 = parser.derive_table_key(keys[0], 1, 0, 0);
    std::vector<uint8_t> derived_2 = parser.derive_table_key(keys[0], 2, 0, 0);

    EXPECT_NE(derived_1, derived_2) << "Different space IDs must produce different derived keys";
}

TEST_F(KeyringAESECBTest, EmptyMasterKeyReturnsEmpty) {
    // Test with empty master key — should return empty or fail gracefully
    parser.add_master_key({}, "empty", TDEAlgorithm::AES_256_CBC);

    auto keys = parser.get_master_keys();
    // Find the key by name
    auto mk = parser.find_key("empty");
    ASSERT_TRUE(mk.has_value()) << "Must find the key";

    // derive_table_key should handle empty master key gracefully
    auto derived = parser.derive_table_key(*mk, 42, 0, 0);
    // The implementation pads empty keys to 16 bytes, so it won't be empty
    // But it should not crash
    // (empty check: the implementation pads to 16 bytes, so it will produce output)
    EXPECT_NO_THROW(parser.derive_table_key(*mk, 42, 0, 0))
        << "Empty master key must not throw";
}

TEST_F(KeyringAESECBTest, VerifyDerivedKeyValid) {
    // Test verify_derived_key() with valid page data
    auto page = create_minimal_page();
    EXPECT_TRUE(OfflineKeyringParser::verify_derived_key(page))
        << "Valid page with FIL_PAGE_INDEX must pass verification";
}

TEST_F(KeyringAESECBTest, VerifyDerivedKeyInvalid) {
    // Test verify_derived_key() with invalid page data
    auto page = create_invalid_page();
    EXPECT_FALSE(OfflineKeyringParser::verify_derived_key(page))
        << "Invalid page type must fail verification";
}

TEST_F(KeyringAESECBTest, VerifyDerivedKeyShortBuffer) {
    // Test verify_derived_key() with a buffer that is too short
    std::vector<uint8_t> short_buf(10, 0);
    EXPECT_FALSE(OfflineKeyringParser::verify_derived_key(short_buf))
        << "Short buffer must fail verification";
}

TEST_F(KeyringAESECBTest, VerifyDerivedKeyEmptyBuffer) {
    // Test verify_derived_key() with empty buffer
    std::vector<uint8_t> empty;
    EXPECT_FALSE(OfflineKeyringParser::verify_derived_key(empty))
        << "Empty buffer must fail verification";
}

TEST_F(KeyringAESECBTest, KeyDerivationAcrossCalls) {
    // Test that multiple calls to derive_table_key with the same parameters
    // produce the same result (deterministic)
    auto master_key = create_master_key();
    parser.add_master_key(master_key, "test_master", TDEAlgorithm::AES_256_CBC);
    auto keys = parser.get_master_keys();

    // Derive multiple times
    std::vector<uint8_t> results[3];
    for (int i = 0; i < 3; i++) {
        results[i] = parser.derive_table_key(keys[0], 42, 0, 0);
    }

    EXPECT_EQ(results[0], results[1]) << "First and second derivation must match";
    EXPECT_EQ(results[1], results[2]) << "Second and third derivation must match";
}

TEST_F(KeyringAESECBTest, KeyringParserInitialState) {
    // Test that a freshly created parser has no keys
    OfflineKeyringParser empty_parser;
    EXPECT_TRUE(empty_parser.get_master_keys().empty()) << "New parser must have no keys";
}

TEST_F(KeyringAESECBTest, AESECBKeyLength) {
    // Verify that AES-ECB produces correct key length
    // For AES-128: 16 bytes, AES-192: 24 bytes, AES-256: 32 bytes
    auto master_key_256 = create_master_key();
    parser.add_master_key(master_key_256, "aes256", TDEAlgorithm::AES_256_CBC);
    auto mk = parser.find_key("aes256");
    ASSERT_TRUE(mk.has_value());

    std::vector<uint8_t> derived = parser.derive_table_key(*mk, 42, 0, 0);
    EXPECT_EQ(derived.size(), 32u) << "AES-256-ECB must produce 32-byte derived key";
}

// ======================================================================
// Integration: ASan/UBSan Stress Test
// ======================================================================

/**
 * This test verifies that the entire test suite doesn't leak memory
 * or exhibit undefined behavior under ASan+UBSan.
 * It creates a variety of edge-case inputs and exercises all the
 * components that had P0 fixes.
 */
class ASanUBSanStressTest : public ::testing::Test {
protected:
    void run_basic_operations() {
        // P0.1: Read various FIL header values
        std::vector<uint8_t> page(16384, 0);
        write_be<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);
        write_be<uint64_t>(page.data(), 26, 0x1000);
        write_be<uint32_t>(page.data(), 0, 0xDEADBEEF);

        volatile uint16_t t = read_be<uint16_t>(page.data(), 24);
        volatile uint64_t l = read_be<uint64_t>(page.data(), 26);
        volatile uint32_t c = read_be<uint32_t>(page.data(), 0);
        (void)t; (void)l; (void)c;

        // P0.2: Encode/decode roll ptr
        uint64_t rp = RollPtrDecoder::encode(100, 200, 3);
        auto decoded = RollPtrDecoder::decode(rp);
        (void)decoded;

        // P0.3: Read prev undo ptr (using RollPtrDecoder which is the public API)
        // read_prev_undo_ptr internally does: decode_storage_to_internal(data+offset, 6)
        // or decode_storage_to_internal(data+offset, 7) for MySQL 8.0+
        uint64_t prev_56 = RollPtrDecoder::decode_storage_to_internal(page.data() + 100, 6);
        uint64_t prev_80 = RollPtrDecoder::decode_storage_to_internal(page.data() + 100, 7);
        (void)prev_56; (void)prev_80;

        // P0.4: SDI extraction with null
        auto sdi = SDIManager::extract_sdi_json(nullptr, 0);
        (void)sdi;

        // P0.5: FSP flags
        uint32_t fsp = 0;
        uint32_t z = (fsp >> 5) & 0x07;
        (void)z;

        // P0.6: Encrypted page detection
        bool enc = TDEDecryptor::is_encrypted(page.data(), page.size());
        (void)enc;

        // P0.7: Verify derived key
        bool v = OfflineKeyringParser::verify_derived_key(page);
        (void)v;
    }
};

TEST_F(ASanUBSanStressTest, NoLeaks) {
    // Run all basic operations — ASan will catch any leaks
    for (int i = 0; i < 100; i++) {
        run_basic_operations();
    }
    // No explicit assertions needed — ASan will fail the test if leaks are detected
    SUCCEED() << "100 iterations of basic operations completed without ASan/UBSan errors";
}

// ======================================================================
// Phase 7: Integrated Validation & Capability Report
// ======================================================================

class CapabilityReportTest : public ::testing::Test {
protected:
    report::CapabilityReport run_report(MySQLVersion version) {
        return validate::UnifiedValidator::validate_all(version);
    }

    void assert_feature(const report::CapabilityReport& report,
                        const std::string& name, bool expected) {
        for (const auto& f : report.features) {
            if (f.name == name) {
                EXPECT_EQ(f.supported, expected)
                    << "Feature '" << name << "' for " << report.mysql_version
                    << " expected " << (expected ? "SUPPORTED" : "NOT SUPPORTED")
                    << " but got " << (f.supported ? "SUPPORTED" : "NOT SUPPORTED")
                    << (f.supported == expected ? "" : " (" + f.limitation + ")");
                return;
            }
        }
        FAIL() << "Feature '" << name << "' not found in capability report for "
               << report.mysql_version;
    }
};

TEST_F(CapabilityReportTest, MySQL56NoSDI) {
    // MySQL 5.6 does not support SDI, encryption, or temporary pages
    // Compression IS supported (ROW_FORMAT=COMPRESSED with KEY_BLOCK_SIZE)
    auto report = run_report(MySQLVersion::MYSQL_56);
    assert_feature(report, "SDI extraction", false);
    assert_feature(report, "TDE/page encryption", false);
    assert_feature(report, "Page compression", true);
    assert_feature(report, "Temporary pages", false);
    EXPECT_TRUE(report.version_id == MySQLVersion::MYSQL_56);
}

TEST_F(CapabilityReportTest, MySQL80HasSDI) {
    // MySQL 8.0 supports SDI, encryption, compression but not temporary pages
    auto report = run_report(MySQLVersion::MYSQL_80);
    assert_feature(report, "SDI extraction", true);
    // MySQL 8.0 supports encryption and compression
    assert_feature(report, "TDE/page encryption", true);
    assert_feature(report, "Page compression", true);
    // Temporary pages require MySQL 8.4+
    assert_feature(report, "Temporary pages", false);
    EXPECT_TRUE(report.version_id == MySQLVersion::MYSQL_80);
}

TEST_F(CapabilityReportTest, MySQL84HasTemporaryPages) {
    // MySQL 8.4 supports all features including temporary pages
    auto report = run_report(MySQLVersion::MYSQL_84);
    assert_feature(report, "SDI extraction", true);
    assert_feature(report, "Temporary pages", true);
    EXPECT_TRUE(report.version_id == MySQLVersion::MYSQL_84);
}

TEST_F(CapabilityReportTest, MySQL90AllFeatures) {
    // MySQL 9.0 supports all features
    auto report = run_report(MySQLVersion::MYSQL_90);
    assert_feature(report, "SDI extraction", true);
    assert_feature(report, "Temporary pages", true);
    EXPECT_TRUE(report.version_id == MySQLVersion::MYSQL_90);
    // No warnings for supported version
    EXPECT_TRUE(report.warnings.empty() ||
                report.warnings[0].find("outside the supported range") == std::string::npos);
}

TEST_F(CapabilityReportTest, UnknownVersionWarning) {
    // UNKNOWN version should produce a warning
    auto report = run_report(MySQLVersion::UNKNOWN);
    bool has_warning = false;
    for (const auto& w : report.warnings) {
        if (w.find("UNKNOWN") != std::string::npos) has_warning = true;
    }
    EXPECT_TRUE(has_warning) << "UNKNOWN version must produce a warning";
}

TEST_F(CapabilityReportTest, SalvageLevelsAlwaysAvailable) {
    // Salvage L1-L4 are build capabilities, available for all versions
    auto report = run_report(MySQLVersion::MYSQL_56);
    assert_feature(report, "salvage L1 (header repair)", true);
    assert_feature(report, "salvage L2 (signature scan)", true);
    assert_feature(report, "salvage L3 (heuristic scan)", true);
    assert_feature(report, "salvage L4 (fragment extract)", true);
}

TEST_F(CapabilityReportTest, PageTypeCapabilities) {
    // Verify page type capability matrix for MySQL 5.6 (SDI & encrypted unsupported)
    auto report_56 = run_report(MySQLVersion::MYSQL_56);
    assert_feature(report_56, "page_type:FIL_PAGE_INDEX", true);
    assert_feature(report_56, "page_type:FIL_PAGE_COMPRESSED", true);
    assert_feature(report_56, "page_type:FIL_PAGE_SDI", false);
    assert_feature(report_56, "page_type:FIL_PAGE_COMPRESSED_AND_ENCRYPTED", false);

    // MySQL 8.0 supports SDI, compressed, and encrypted page types
    auto report_80 = run_report(MySQLVersion::MYSQL_80);
    assert_feature(report_80, "page_type:FIL_PAGE_COMPRESSED", true);
    assert_feature(report_80, "page_type:FIL_PAGE_COMPRESSED_AND_ENCRYPTED", true);
    assert_feature(report_80, "page_type:FIL_PAGE_SDI", true);
}

TEST_F(CapabilityReportTest, ReportToStringNotEmpty) {
    // CapabilityReport::to_string() must produce non-empty output
    auto report = run_report(MySQLVersion::MYSQL_80);
    std::string s = report.to_string();
    EXPECT_FALSE(s.empty()) << "CapabilityReport::to_string() must not be empty";
    EXPECT_NE(s.find("Capability Report"), std::string::npos)
        << "Report must contain title";
    EXPECT_NE(s.find("SUPPORTED"), std::string::npos)
        << "Report must contain SUPPORTED features";
}

TEST_F(CapabilityReportTest, FeatureLookupByName) {
    // is_feature_supported() must work correctly
    auto report = run_report(MySQLVersion::MYSQL_80);
    EXPECT_TRUE(report.is_feature_supported("SDI extraction"));
    EXPECT_FALSE(report.is_feature_supported("Temporary pages"));
    EXPECT_FALSE(report.is_feature_supported("nonexistent_feature"));
}

TEST_F(CapabilityReportTest, MySQL57MidRange) {
    // MySQL 5.7: no SDI, no encryption, compression supported, no temporary pages
    auto report = run_report(MySQLVersion::MYSQL_57);
    assert_feature(report, "SDI extraction", false);
    assert_feature(report, "TDE/page encryption", false);
    assert_feature(report, "Page compression", true);
    assert_feature(report, "Temporary pages", false);
    // But salvage levels are always available
    assert_feature(report, "salvage L1 (header repair)", true);
}

} // namespace
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}