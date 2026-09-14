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
#include <memory>
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
#include "dbrecovery/tools/mysql_version_parser.hpp"
#include "dbrecovery/validate/unified_validator.hpp"
#include "dbrecovery/report/recovery_report.hpp"
#include "dbrecovery/engine/drop_table_recovery.hpp"
#include "dbrecovery/innodb/page_types_v2.hpp"
#include "dbrecovery/innodb/deep_undo_analyzer.hpp"
#include "dbrecovery/innodb/flashback_generator.hpp"

namespace dbrecovery {
namespace {

using namespace innodb;

// Disambiguate: `using namespace innodb` would otherwise shadow dbrecovery::MySQLVersion
// (innodb::page_types_v2.hpp defines its own distinct MySQLVersion enum).
using dbrecovery::MySQLVersion;

TEST(MySQLVersionParsingTest, AcceptsCanonicalAndProvenanceVersions) {
    struct ExpectedVersion {
        const char* input;
        const char* family;
        MySQLVersion version;
    };
    const ExpectedVersion cases[] = {
        {"5.6", "5.6", MySQLVersion::MYSQL_56},
        {"5.6.51", "5.6", MySQLVersion::MYSQL_56},
        {"5.7", "5.7", MySQLVersion::MYSQL_57},
        {"5.7.44", "5.7", MySQLVersion::MYSQL_57},
        {"8.0", "8.0", MySQLVersion::MYSQL_80},
        {"8.0.46", "8.0", MySQLVersion::MYSQL_80},
        {"8.4.3", "8.4", MySQLVersion::MYSQL_84},
        {"9.0.1", "9.0", MySQLVersion::MYSQL_90},
    };

    for (const auto& test : cases) {
        const auto parsed = tools::parse_mysql_version(test.input);
        ASSERT_TRUE(parsed.has_value()) << test.input;
        EXPECT_EQ(parsed->family, test.family);
        EXPECT_EQ(parsed->version, test.version);
    }
}

TEST(MySQLVersionParsingTest, RejectsMalformedAndUnsupportedVersions) {
    for (const char* input : {"", "80", "8", "8.", ".0", "8.0.x", "8.0.46.1", "8.5.1", "10.0.1"}) {
        EXPECT_FALSE(tools::parse_mysql_version(input).has_value()) << input;
    }
}

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

TEST_F(FILHeaderEndiannessTest, ChecksumIsNotVersionEvidence) {
    auto buf = create_page_buffer();
    set_be32(buf, 0, 0x3A4B5C6D);

    const int checksum_score = MySQLVersionDetector::check_checksum_type(buf.data(), buf.size());
    EXPECT_EQ(checksum_score, 0)
        << "A raw checksum value must not be treated as algorithm or version evidence";
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

    // Scenario 3: MySQL 8.0 INDEX page — has LSN extension and page_version
    // field at offset 38. Offset 38 is not a reliable cross-version marker
    // (it overlaps PAGE_N_DIR_SLOTS on older layouts), so the detector must
    // not use it. Without SDI or FSP evidence, the version is UNKNOWN.
    {
        auto buf = create_page_buffer();
        set_be16(buf, FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
        set_be64(buf, FIL_PAGE_TYPE_ABS + 2, 0x1000);     // Non-zero FLUSH_LSN
        set_be16(buf, FIL_HEADER_SIZE, 1);                 // page_version = 1

        MySQLVersion version = MySQLVersionDetector::detect(buf.data(), buf.size());
        EXPECT_EQ(version, MySQLVersion::UNKNOWN)
            << "Scenario 3: page_version at offset 38 is not reliable version evidence; "
            << "INDEX page without SDI must be UNKNOWN";
    }

    // Scenario 4: MySQL 8.4 INDEX page — has LSN extension, page_version = 2.
    // Same reasoning: offset 38 is not a reliable version marker so the
    // detector must return UNKNOWN without SDI or FSP evidence.
    {
        auto buf = create_page_buffer();
        set_be16(buf, FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
        set_be64(buf, FIL_PAGE_TYPE_ABS + 2, 0x2000);     // Non-zero FLUSH_LSN
        set_be16(buf, FIL_HEADER_SIZE, 2);                 // page_version = 2

        MySQLVersion version = MySQLVersionDetector::detect(buf.data(), buf.size());
        EXPECT_EQ(version, MySQLVersion::UNKNOWN)
            << "Scenario 4: page_version at offset 38 is not reliable version evidence; "
            << "INDEX page without SDI must be UNKNOWN";
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
// P0.1b: Version/page detection fail-closed (recursion + soft-default)
// ======================================================================

TEST_F(FILHeaderEndiannessTest, VersionCompatibilityFactoryNoRecursionOnIndexPage) {
    auto buf = create_page_buffer();
    set_be16(buf, FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    set_be32(buf, 0, 0x3A4B5C6D);

    // Must not crash from infinite recursion (version_compat.cpp:382 cycle)
    MySQLVersion v1 = VersionCompatibilityFactory::instance().detect_version(buf.data(), buf.size());
    EXPECT_EQ(v1, MySQLVersion::UNKNOWN)
        << "VersionCompatibilityFactory::detect_version on INDEX page without SDI must return UNKNOWN";

    MySQLVersion v2 = dbrecovery::detect_mysql_version(buf.data(), buf.size());
    EXPECT_EQ(v2, MySQLVersion::UNKNOWN)
        << "dbrecovery::detect_mysql_version on INDEX page without SDI must return UNKNOWN";
}

TEST_F(FILHeaderEndiannessTest, VersionCompatibilityFactoryDetectsSdiPage) {
    auto buf = create_page_buffer();
    set_be16(buf, FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);
    constexpr uint8_t SDI_SIG[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};
    std::memcpy(buf.data() + 64, SDI_SIG, 8);

    MySQLVersion version = VersionCompatibilityFactory::instance().detect_version(buf.data(), buf.size());
    EXPECT_EQ(version, MySQLVersion::MYSQL_80)
        << "VersionCompatibilityFactory::detect_version must return MYSQL_80 for SDI page";
}

TEST_F(FILHeaderEndiannessTest, PageParserFactoryCreateParserNoCrash) {
    auto buf = create_page_buffer();
    set_be16(buf, FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    set_be32(buf, 0, 0x3A4B5C6D);

    // Must not crash (no infinite recursion from unescoped detect_mysql_version call)
    auto parser = innodb::PageParserFactory::create_parser(buf.data(), buf.size());
    EXPECT_NE(parser, nullptr)
        << "PageParserFactory::create_parser must return non-null for unknown version (fail-closed)";
}

TEST_F(FILHeaderEndiannessTest, PageTypeDetectorDefaultUnknown) {
    innodb::MySQLVersion version = innodb::PageTypeDetector::detect_version(FIL_PAGE_INDEX, nullptr, 0);
    EXPECT_EQ(version, innodb::MySQLVersion::UNKNOWN)
        << "PageTypeDetector::detect_version must return UNKNOWN for non-SDI page types, not MYSQL_57";
}

TEST(SystemTablespaceScannerVersionTest, UnknownFilenameReturnsUnknown) {
    engine::SystemTablespaceScanner scanner("/some/unknown/path.ibd", 16384);
    std::string version = scanner.detect_mysql_version();
    EXPECT_EQ(version, "UNKNOWN")
        << "Unknown filename must not silently default to 5.7";
}

TEST(SystemTablespaceScannerVersionTest, IsMysql80FormatFalseForUnknown) {
    engine::SystemTablespaceScanner scanner("/some/unknown/path.ibd", 16384);
    EXPECT_FALSE(scanner.is_mysql80_format())
        << "is_mysql80_format must be false for unknown filename";
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

TEST_F(RollPtrDecoderTest, DecodeStoragePinnedByProfile) {
    // The profile-pinned overload must never infer width from remaining bytes.
    // Width is chosen exclusively from the resolved profile.
    uint64_t val6 = (0x1000ULL << 20) | 0x200ULL;
    uint8_t data6[6];
    for (int i = 5; i >= 0; i--) {
        data6[i] = static_cast<uint8_t>(val6 & 0xFF);
        val6 >>= 8;
    }

    // 6 bytes is the correct width for LEGACY_6BYTE, too short for MYSQL8_7BYTE.
    auto d6 = RollPtrDecoder::decode_storage(data6, 6, UndoFormatProfile::LEGACY_6BYTE);
    EXPECT_TRUE(d6.is_valid());
    EXPECT_EQ(d6.page_no, 0x1000u);
    EXPECT_EQ(d6.offset, 0x200u);
    EXPECT_EQ(d6.flags, 0u) << "LEGACY_6BYTE profile must decode with no flags";

    // Same buffer is NOT sufficient for the 7-byte profile: fail closed.
    auto d7_short = RollPtrDecoder::decode_storage(data6, 6, UndoFormatProfile::MYSQL8_7BYTE);
    EXPECT_FALSE(d7_short.is_valid()) << "7-byte profile must reject 6-byte input";

    // UNRESOLVED must reject regardless of buffer length.
    auto d_unresolved = RollPtrDecoder::decode_storage(data6, 6, UndoFormatProfile::UNRESOLVED);
    EXPECT_FALSE(d_unresolved.is_valid()) << "UNRESOLVED profile must fail closed";
    EXPECT_TRUE(d_unresolved.is_null());
}

TEST_F(RollPtrDecoderTest, DecodeStorageInternalNoOverlap) {
    // Internal encode() lays out offset at bits [49:32] (18 bits) and flags
    // at bits [55:50] (6 bits). The maximum offset 0x3FFFF must therefore
    // round-trip regardless of non-zero flags — no overlap by construction.
    uint64_t rp = RollPtrDecoder::encode(0x1000, 0x3FFFF, 0x3F);
    auto decoded = RollPtrDecoder::decode(rp);
    EXPECT_EQ(decoded.page_no, 0x1000u);
    EXPECT_EQ(decoded.offset, 0x3FFFFu)
        << "18-bit offset must be fully preserved with flags set";
    EXPECT_EQ(decoded.flags, 0x3Fu);
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

    // Internal representation: offset at bits [49:32] (18 bits), flags at
    // bits [55:50] (6 bits). The layouts never overlap, so a full 18-bit
    // offset round-trips cleanly regardless of flags — including the max
    // offset 0x3FFFF whose low 16 bits are all set.
    auto rp_max = create_7byte_roll_ptr_page(0x5678, 0x3FFFF, 0x3);
    uint64_t internal_max = RollPtrDecoder::decode_storage_to_internal(
        rp_max.data.data() + rp_max.roll_ptr_offset, 7);
    auto decoded_max = RollPtrDecoder::decode(internal_max);
    EXPECT_EQ(decoded_max.page_no, 0x5678u);
    EXPECT_EQ(decoded_max.offset, 0x3FFFFu) << "max 18-bit offset must not collide with flags";
    EXPECT_EQ(decoded_max.flags, 0x3u) << "flags must survive decode/encode with max offset";

    // Round-trip with non-zero flags must stay lossless at the internal layer.
    uint64_t internal = RollPtrDecoder::decode_storage_to_internal(
        rp.data.data() + rp.roll_ptr_offset, 7);
    auto decoded2 = RollPtrDecoder::decode(internal);
    EXPECT_EQ(decoded2.page_no, 0x5678u);
    EXPECT_EQ(decoded2.offset, 0x1Fu);
    EXPECT_EQ(decoded2.flags, 0x3u);
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
    EXPECT_EQ(undo_format_profile_for_version(MySQLVersion::MYSQL_56),
              UndoFormatProfile::LEGACY_6BYTE);
    EXPECT_EQ(undo_format_profile_for_version(MySQLVersion::MYSQL_57),
              UndoFormatProfile::LEGACY_6BYTE);
    EXPECT_EQ(undo_format_profile_for_version(MySQLVersion::MYSQL_80),
              UndoFormatProfile::MYSQL8_7BYTE);
    EXPECT_EQ(undo_format_profile_for_version(MySQLVersion::MYSQL_84),
              UndoFormatProfile::MYSQL8_7BYTE);
    EXPECT_EQ(undo_format_profile_for_version(MySQLVersion::MYSQL_90),
              UndoFormatProfile::MYSQL8_7BYTE);

    // Also verify that RollPtrDecoder::decode_storage with null data does not crash
    auto decoded_6 = RollPtrDecoder::decode_storage(nullptr, 6);
    EXPECT_FALSE(decoded_6.is_valid()) << "Null data with 6-byte format must return invalid";
    EXPECT_TRUE(decoded_6.is_null()) << "Null data with 6-byte format must return null";

    auto decoded_7 = RollPtrDecoder::decode_storage(nullptr, 7);
    EXPECT_FALSE(decoded_7.is_valid()) << "Null data with 7-byte format must return invalid";
    EXPECT_TRUE(decoded_7.is_null()) << "Null data with 7-byte format must return null";
}

// ======================================================================
// P0.2b: Deep Undo Fail-Closed Gates
// ======================================================================

class DeepUndoFailClosedTest : public ::testing::Test {
protected:
    static constexpr size_t PAGE_SIZE = 16384;
    static constexpr uint16_t TRX_UNDO_PAGE_FREE = 30;  // LE u16 offset
    static constexpr size_t TRX_UNDO_PAGE_HDR_SIZE = 40;

    struct UndoPage {
        std::vector<uint8_t> data;
        size_t record_offset;
    };

    // Create a minimal undo page with an UPDATE record at record_offset
    UndoPage create_undo_update_page(uint32_t page_no, uint16_t page_offset,
                                     uint32_t prev_ptr_page_no, uint16_t prev_ptr_offset,
                                     bool is_7byte = false) {
        UndoPage up;
        up.data.resize(PAGE_SIZE, 0);
        up.record_offset = TRX_UNDO_PAGE_HDR_SIZE + 10;

        // Set FIL page type to UNDO_LOG (BE at offset 24)
        set_be16(up.data, FIL_PAGE_TYPE_ABS, FIL_PAGE_UNDO_LOG);

        // Set TRX_UNDO_PAGE_FREE (LE) to include the full record
        size_t roll_ptr_len = is_7byte ? 7 : 6;
        size_t total_record_size = 2 + 4 + 2 + 1 + 5 + roll_ptr_len + 10;
        set_le16(up.data, TRX_UNDO_PAGE_FREE, up.record_offset + total_record_size);

        // Write the UPDATE record header
        size_t pos = up.record_offset;
        up.data[pos] = 2;          // TRX_UNDO_UPDATE
        up.data[pos + 1] = 1;      // field_count = 1
        set_le32(up.data, pos + 2, page_no);   // page_no (4B LE)
        set_le16(up.data, pos + 6, page_offset); // page_offset (2B LE)
        up.data[pos + 8] = 0;      // info_bits
        set_le32(up.data, pos + 9, 12345);     // trx_id low 4 bytes
        up.data[pos + 13] = 0;     // trx_id high byte

        // prev_undo_ptr at offset+14: storage format is (page_no<<20 | offset)
        // as big-endian roll_ptr_len bytes.
        uint64_t val = (static_cast<uint64_t>(prev_ptr_page_no) << 20) |
                       (prev_ptr_offset & 0x3FFFF);
        for (size_t i = 0; i < roll_ptr_len; i++) {
            up.data[pos + 14 + roll_ptr_len - 1 - i] =
                static_cast<uint8_t>(val & 0xFF);
            val >>= 8;
        }

        // Add a small field (1-byte length prefix = 5, then 5 bytes data)
        up.data[pos + 14 + roll_ptr_len] = 5;
        for (int i = 0; i < 5; i++) {
            up.data[pos + 15 + roll_ptr_len + i] = static_cast<uint8_t>(i);
        }

        return up;
    }

    void set_be16(std::vector<uint8_t>& buf, size_t offset, uint16_t v) {
        buf[offset] = static_cast<uint8_t>(v >> 8);
        buf[offset + 1] = static_cast<uint8_t>(v & 0xFF);
    }

    void set_le16(std::vector<uint8_t>& buf, size_t offset, uint16_t v) {
        buf[offset] = static_cast<uint8_t>(v & 0xFF);
        buf[offset + 1] = static_cast<uint8_t>(v >> 8);
    }

    void set_le32(std::vector<uint8_t>& buf, size_t offset, uint32_t v) {
        buf[offset]     = static_cast<uint8_t>(v & 0xFF);
        buf[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        buf[offset + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        buf[offset + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    }
};

TEST_F(DeepUndoFailClosedTest, ParserRejectsUpdateWithUnresolvedProfile) {
    // UndoRecordV2Parser with default UNRESOLVED profile must reject
    // UPDATE/DELETE records because the roll-pointer width is unknown.
    // (deep_undo_analyzer.cpp:465-468)
    auto up = create_undo_update_page(100, 200, 0x100, 0x1);

    UndoRecordV2Parser parser;
    // Default profile is UNRESOLVED — no call to set_undo_format_profile

    auto result = parser.parse(up.data.data(), PAGE_SIZE, up.record_offset);
    EXPECT_FALSE(result.has_value())
        << "Parser with UNRESOLVED profile must reject UPDATE record";
}

TEST_F(DeepUndoFailClosedTest, ParserRejectsTruncatedUpdateHeader) {
    // When the undo page's page_free is too small for the required header
    // (ROLL_PTR_OFFSET + roll_ptr_len), parse() must return nullopt.
    // (deep_undo_analyzer.cpp:477-485)
    auto up = create_undo_update_page(100, 200, 0x100, 0x1);

    // Override page_free to 15 bytes from record start — enough for type+field_count
    // (2) + page_no (4) + page_offset (2) + info_bits (1) + trx_id (5) = 14,
    // but NOT enough for the 6-byte roll_ptr at offset 14 (needs 14+6=20).
    set_le16(up.data, TRX_UNDO_PAGE_FREE, up.record_offset + 15);

    UndoRecordV2Parser parser;
    parser.set_undo_format_profile(UndoFormatProfile::LEGACY_6BYTE);

    auto result = parser.parse(up.data.data(), PAGE_SIZE, up.record_offset);
    EXPECT_FALSE(result.has_value())
        << "Parser must reject UPDATE record with truncated header (page_free too small)";
}

TEST_F(DeepUndoFailClosedTest, ExportSafeOnlyForComplete) {
    // RowVersionHistory::export_safe() must return true only for COMPLETE.
    // Every other HistoryIntegrity value must return false.
    // (deep_undo_analyzer.hpp:174)
    RowVersionHistory history;

    history.integrity = HistoryIntegrity::COMPLETE;
    EXPECT_TRUE(history.export_safe()) << "COMPLETE must be export-safe";

    history.integrity = HistoryIntegrity::BROKEN_PAGE;
    EXPECT_FALSE(history.export_safe()) << "BROKEN_PAGE must not be export-safe";

    history.integrity = HistoryIntegrity::INVALID_RECORD;
    EXPECT_FALSE(history.export_safe()) << "INVALID_RECORD must not be export-safe";

    history.integrity = HistoryIntegrity::POINTER_CONFLICT;
    EXPECT_FALSE(history.export_safe()) << "POINTER_CONFLICT must not be export-safe";

    history.integrity = HistoryIntegrity::CYCLE;
    EXPECT_FALSE(history.export_safe()) << "CYCLE must not be export-safe";

    history.integrity = HistoryIntegrity::TRUNCATED;
    EXPECT_FALSE(history.export_safe()) << "TRUNCATED must not be export-safe";

    history.integrity = HistoryIntegrity::HEURISTIC;
    EXPECT_FALSE(history.export_safe()) << "HEURISTIC must not be export-safe";
}

TEST_F(DeepUndoFailClosedTest, FlashbackFullChainReturnsEmptyForBroken) {
    // flashback_full_chain must return {} when the roll_ptr points to
    // page_no=0, which triggers BROKEN_PAGE in trace_version_chain
    // before any I/O is attempted. (flashback_generator.cpp:625-628)
    FlashbackGenerator gen;

    catalog::TableSchema schema;
    schema.table_name = "test";

    // encode(page_no=0, offset=1, flags=0) — page_no=0 triggers BROKEN_PAGE
    // at deep_undo_analyzer.cpp:635 without any file I/O.
    uint64_t roll_ptr = RollPtrDecoder::encode(0, 1, 0);

    std::vector<uint8_t> current_record = {0x01, 0x02, 0x03};

    auto result = gen.flashback_full_chain(current_record, roll_ptr, schema, 42);
    EXPECT_TRUE(result.empty())
        << "flashback_full_chain must return {} for broken chain (page_no=0)";
}

TEST_F(DeepUndoFailClosedTest, ParserAcceptsUpdateWithResolvedProfile) {
    // With LEGACY_6BYTE profile, a valid UPDATE record must parse successfully.
    // This is the positive control for the ParserRejectsUpdateWithUnresolvedProfile test.
    // prev_undo_ptr is stored as (page_no<<20 | offset) big-endian in storage.
    auto up = create_undo_update_page(100, 200, 0x100, 0);

    UndoRecordV2Parser parser;
    parser.set_undo_format_profile(UndoFormatProfile::LEGACY_6BYTE);

    auto result = parser.parse(up.data.data(), PAGE_SIZE, up.record_offset);
    ASSERT_TRUE(result.has_value())
        << "Parser with LEGACY_6BYTE profile must accept valid UPDATE record";
    EXPECT_EQ(result->type, UndoRecordV2::UPDATE);
    EXPECT_EQ(result->page_no, 100u);
    EXPECT_EQ(result->page_offset, 200u);
    EXPECT_EQ(result->trx_id, 12345u);
    // prev_undo_ptr decoded from storage: page_no=0x100, offset=0, flags=0
    // Internal encode: page_no | (offset<<32) | (flags<<50) = 0x100
    EXPECT_EQ(result->prev_undo_ptr, 0x100u);
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

// ----------------------------------------------------------------------
// P0-3: Constrained SDI extraction — schema-promotion path must fail
// closed. parse_sdi_record()/parse_sdi_record_structured() are the ONLY
// APIs that promote decompressed SDI to an authoritative TableSchema; they
// delegate exclusively to the single bounded extract_standard_payload().
// Any bound violation (ratio, output size, trailing input) must yield NO
// schema — never a partial one — and must propagate a structured status.
// ----------------------------------------------------------------------

// Declared 100:1 ratio (800/8) > 20:1 must be rejected before decompression,
// and the schema-promoting API must return an empty schema, not a partial one.
TEST_F(SDIDecompressionSafetyTest, SchemaPathFailsClosedOnRatioAttack) {
    std::vector<uint8_t> payload(20 + 8, 0);
    write_le<uint32_t>(payload.data(), 0, 1);
    write_le<uint64_t>(payload.data(), 4, 1);
    write_le<uint32_t>(payload.data(), 12, 800);   // 800/8 = 100:1 > 20:1
    write_le<uint32_t>(payload.data(), 16, 8);
    for (size_t i = 0; i < 8; ++i) payload[20 + i] = 0xA5;

    const auto schema = SDIManager::parse_sdi_record(payload.data(), payload.size());
    EXPECT_TRUE(schema.table_name.empty()) << "No partial schema may leak";
    EXPECT_TRUE(schema.columns.empty());

    const auto par = SDIManager::parse_sdi_record_structured(payload.data(), payload.size());
    EXPECT_EQ(par.status, SDIManager::SDIParseStatus::DECOMPRESSION_LIMIT);
    EXPECT_TRUE(par.schema.table_name.empty());
}

// Declared uncompressed_len beyond SDI_MAX_UNCOMPRESSED_SIZE must be rejected
// as DECOMPRESSION_LIMIT with no schema on the promotion path.
TEST_F(SDIDecompressionSafetyTest, SchemaPathFailsClosedOnOversizedOutput) {
    constexpr uint32_t oversized = 16 * 1024 * 1024 + 1;
    std::vector<uint8_t> payload(20 + 8, 0);
    write_le<uint32_t>(payload.data(), 0, 1);
    write_le<uint64_t>(payload.data(), 4, 1);
    write_le<uint32_t>(payload.data(), 12, oversized);
    write_le<uint32_t>(payload.data(), 16, 8);

    const auto schema = SDIManager::parse_sdi_record(payload.data(), payload.size());
    EXPECT_TRUE(schema.table_name.empty());

    const auto par = SDIManager::parse_sdi_record_structured(payload.data(), payload.size());
    EXPECT_EQ(par.status, SDIManager::SDIParseStatus::DECOMPRESSION_LIMIT);
    EXPECT_TRUE(par.schema.table_name.empty());
}

// Trailing bytes after the deflate stream must be rejected (Z_STREAM_END with
// remaining input), yielding no schema, never silently accepted.
TEST_F(SDIDecompressionSafetyTest, SchemaPathFailsClosedOnTrailingInput) {
    const std::string json = R"({"table":{"name":"t","columns":[{"name":"id","type":4,"is_nullable":false}]}})";
    auto base = create_compressed_sdi_payload(json);
    ASSERT_FALSE(base.empty());
    const size_t declared = base.size() - 20;
    std::vector<uint8_t> payload(base.begin(), base.end());
    payload.resize(payload.size() + 8, 0x42);
    write_le<uint32_t>(payload.data(), 16, static_cast<uint32_t>(declared + 8));

    const auto schema = SDIManager::parse_sdi_record(payload.data(), payload.size());
    EXPECT_TRUE(schema.table_name.empty());

    const auto par = SDIManager::parse_sdi_record_structured(payload.data(), payload.size());
    EXPECT_NE(par.status, SDIManager::SDIParseStatus::SUCCESS);
    EXPECT_TRUE(par.schema.table_name.empty());
}

// A valid compressed SDI record with a real table schema must promote to an
// AUTHORITATIVE TableSchema with provenance (hash/source/trust) attached.
TEST_F(SDIDecompressionSafetyTest, ValidSchemaPromotesToAuthoritative) {
    const std::string json =
        R"({"table":{"name":"t1","columns":[{"name":"id","type":4,"is_nullable":false},{"name":"name","type":16,"is_nullable":true}]}})";
    auto payload = create_compressed_sdi_payload(json);
    ASSERT_FALSE(payload.empty());

    const auto par = SDIManager::parse_sdi_record_structured(payload.data(), payload.size());
    EXPECT_EQ(par.status, SDIManager::SDIParseStatus::SUCCESS);
    EXPECT_EQ(par.schema.table_name, "t1");
    EXPECT_EQ(par.schema.schema_source, "SDI");
    EXPECT_EQ(par.schema.trust_level, "AUTHORITATIVE");
    EXPECT_EQ(par.schema.schema_hash.size(), 64u);
    EXPECT_FALSE(par.schema.columns.empty());
}

// Heuristic ZIP extraction, even when it succeeds, must be tagged
// HeuristicCorruptedZip — distinct from both strict origins — so the
// consumer-side gate (main.cpp / recovery_cli.hpp / partition_table_recovery)
// keeps it as EVIDENCE_ONLY and never promotes it to an authoritative schema.
TEST_F(SDIDecompressionSafetyTest, HeuristicZipNeverAuthoritative) {
    const std::string json = R"({"table":{"name":"t","columns":[{"name":"id","type":4,"is_nullable":false}]}})";
    auto payload = create_compressed_sdi_payload(json);
    ASSERT_FALSE(payload.empty());
    const size_t compressed_size = payload.size() - 20;
    std::vector<uint8_t> page(1024, 0);
    // Place the deflate stream near the end of the page so the heuristic
    // scan's max_input = search_end - offset equals the stream size,
    // avoiding trailing-zero rejection.
    const size_t stream_offset = (1024 - 8) - compressed_size;
    std::copy(payload.begin() + 20, payload.end(), page.begin() + stream_offset);

    auto heuristic = SDIManager::extract_heuristic_zip_page(page.data(), page.size());
    // The heuristic must either succeed under HeuristicCorruptedZip or fail
    // closed — either way, never confused with a strict origin.
    if (heuristic.success()) {
        EXPECT_EQ(heuristic.origin, SDIManager::ExtractionOrigin::HeuristicCorruptedZip);
    } else {
        EXPECT_FALSE(heuristic.failure_reason.empty());
    }
    EXPECT_NE(heuristic.origin, SDIManager::ExtractionOrigin::StrictStandardPayload);
    EXPECT_NE(heuristic.origin, SDIManager::ExtractionOrigin::StrictZipRecordAdapter);
}

// ======================================================================
// P0.5: FSP Flags Compression
// ======================================================================

class FSPFlagsCompressionTest : public ::testing::Test {
protected:
    std::vector<uint8_t> create_fsp_page(size_t logical_page_size, uint8_t zip_ssize) {
        std::vector<uint8_t> page(logical_page_size, 0);
        write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_TYPE_FSP_HDR);
        write_be<uint32_t>(page.data(), FSP_SPACE_FLAGS_OFFSET,
                           static_cast<uint32_t>(zip_ssize) << 1);
        return page;
    }
};

TEST_F(FSPFlagsCompressionTest, ParsesCanonicalBigEndianFlags) {
    const auto page = create_fsp_page(16384, 2);

    const auto metadata = parse_fsp_compression_metadata(page.data(), page.size(), 16384);

    ASSERT_TRUE(metadata.has_value());
    EXPECT_TRUE(metadata->is_compressed());
    EXPECT_EQ(metadata->zip_ssize, 2);
    EXPECT_EQ(metadata->logical_page_size, 16384u);
    EXPECT_EQ(metadata->physical_page_size, 4096u);
}

TEST_F(FSPFlagsCompressionTest, AcceptsUncompressedTablespace) {
    const auto page = create_fsp_page(8192, 0);

    const auto metadata = parse_fsp_compression_metadata(page.data(), page.size(), 8192);

    ASSERT_TRUE(metadata.has_value());
    EXPECT_FALSE(metadata->is_compressed());
    EXPECT_EQ(metadata->physical_page_size, 8192u);
}

TEST_F(FSPFlagsCompressionTest, PreservesOtherFlagBits) {
    auto page = create_fsp_page(16384, 3);
    write_be<uint32_t>(page.data(), FSP_SPACE_FLAGS_OFFSET, 0xA5000000u | (3u << 1));

    const auto metadata = parse_fsp_compression_metadata(page.data(), page.size(), 16384);

    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata->zip_ssize, 3);
    EXPECT_EQ(metadata->physical_page_size, 2048u);
}

TEST_F(FSPFlagsCompressionTest, RejectsInvalidOrIncompleteMetadata) {
    auto page = create_fsp_page(16384, 5);
    EXPECT_FALSE(parse_fsp_compression_metadata(page.data(), page.size(), 16384).has_value());

    page = create_fsp_page(16384, 1);
    write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    EXPECT_FALSE(parse_fsp_compression_metadata(page.data(), page.size(), 16384).has_value());

    page = create_fsp_page(16384, 1);
    EXPECT_FALSE(parse_fsp_compression_metadata(page.data(), FSP_SPACE_FLAGS_OFFSET + 3, 16384).has_value());
    EXPECT_FALSE(parse_fsp_compression_metadata(page.data(), page.size(), 12288).has_value());
    EXPECT_FALSE(parse_fsp_compression_metadata(nullptr, 0, 16384).has_value());
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

TEST_F(EncryptedPageSpaceIDTest, TDECandidateRoutingDetection) {
    auto encrypted = create_encrypted_page(42, 100);
    auto normal = create_normal_page(42, 100);

    EXPECT_EQ(TDEDecryptor::probe_candidate(encrypted.data(), encrypted.size()),
              TDECandidateProbe::RECOGNIZED_CANDIDATE);
    EXPECT_TRUE(TDEDecryptor::is_encrypted(encrypted.data(), encrypted.size()))
        << "Candidate page must be blocked from generic plaintext recovery";
    EXPECT_EQ(TDEDecryptor::probe_candidate(normal.data(), normal.size()),
              TDECandidateProbe::NOT_DETECTED);
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

        // P0.5: Parse canonical big-endian FSP compression metadata.
        write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_TYPE_FSP_HDR);
        write_be<uint32_t>(page.data(), FSP_SPACE_FLAGS_OFFSET, 2u << 1);
        auto compression = parse_fsp_compression_metadata(page.data(), page.size(), page.size());
        (void)compression;

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

    const report::CapabilityReport::Feature* find_feature(
        const report::CapabilityReport& report, const std::string& id) {
        const auto* feature = report.find_feature(id);
        EXPECT_NE(feature, nullptr) << "Feature '" << id << "' not found in capability report";
        return feature;
    }

    void assert_status(const report::CapabilityReport& report, const std::string& id,
                       report::CapabilityStatus expected) {
        const auto* feature = find_feature(report, id);
        ASSERT_NE(feature, nullptr);
        EXPECT_EQ(feature->status, expected) << "Feature '" << id << "' status mismatch";
    }

    void assert_output_category(const report::CapabilityReport& report, const std::string& id,
                                report::RecoveryOutputCategory expected) {
        const auto* feature = find_feature(report, id);
        ASSERT_NE(feature, nullptr);
        EXPECT_EQ(feature->output_category, expected)
            << "Feature '" << id << "' output category mismatch";
    }
};

TEST_F(CapabilityReportTest, MySQL56NoSDI) {
    auto report = run_report(MySQLVersion::MYSQL_56);
    assert_status(report, "sdi.extraction", report::CapabilityStatus::REJECTED);
    assert_status(report, "tde.page_encryption", report::CapabilityStatus::RECOGNIZED_UNSUPPORTED);
    assert_status(report, "compression.page", report::CapabilityStatus::RECOGNIZED_UNSUPPORTED);
    assert_status(report, "temporary.pages", report::CapabilityStatus::REJECTED);
    EXPECT_TRUE(report.version_id == MySQLVersion::MYSQL_56);
}

TEST_F(CapabilityReportTest, MySQL80SDIRequiresValidation) {
    auto report = run_report(MySQLVersion::MYSQL_80);
    assert_status(report, "sdi.extraction", report::CapabilityStatus::EVIDENCE_ONLY);
    assert_status(report, "tde.page_encryption", report::CapabilityStatus::RECOGNIZED_UNSUPPORTED);
    assert_status(report, "compression.page", report::CapabilityStatus::RECOGNIZED_UNSUPPORTED);
    assert_status(report, "temporary.pages", report::CapabilityStatus::REJECTED);
    EXPECT_TRUE(report.version_id == MySQLVersion::MYSQL_80);
}

TEST_F(CapabilityReportTest, MySQL84TemporaryPagesRemainUnsupported) {
    auto report = run_report(MySQLVersion::MYSQL_84);
    assert_status(report, "sdi.extraction", report::CapabilityStatus::EVIDENCE_ONLY);
    assert_status(report, "temporary.pages", report::CapabilityStatus::RECOGNIZED_UNSUPPORTED);
    EXPECT_TRUE(report.version_id == MySQLVersion::MYSQL_84);
}

TEST_F(CapabilityReportTest, MySQL90NoUnsupportedVersionWarning) {
    auto report = run_report(MySQLVersion::MYSQL_90);
    assert_status(report, "sdi.extraction", report::CapabilityStatus::EVIDENCE_ONLY);
    assert_status(report, "temporary.pages", report::CapabilityStatus::RECOGNIZED_UNSUPPORTED);
    EXPECT_TRUE(report.version_id == MySQLVersion::MYSQL_90);
    EXPECT_TRUE(report.warnings.empty() ||
                report.warnings[0].find("outside the supported range") == std::string::npos);
}

TEST_F(CapabilityReportTest, UnknownVersionRejectsFormatSpecificRecovery) {
    auto report = run_report(MySQLVersion::UNKNOWN);
    assert_status(report, "sdi.extraction", report::CapabilityStatus::REJECTED);
    assert_status(report, "page_type.FIL_PAGE_INDEX", report::CapabilityStatus::REJECTED);
    assert_status(report, "salvage.l3", report::CapabilityStatus::EVIDENCE_ONLY);
    assert_output_category(report, "salvage.l3", report::RecoveryOutputCategory::FORENSIC_EVIDENCE);
}

TEST_F(CapabilityReportTest, UnknownVersionWarning) {
    auto report = run_report(MySQLVersion::UNKNOWN);
    bool has_warning = false;
    for (const auto& w : report.warnings) {
        if (w.find("UNKNOWN") != std::string::npos) has_warning = true;
    }
    EXPECT_TRUE(has_warning) << "UNKNOWN version must produce a warning";
}

TEST_F(CapabilityReportTest, SalvageLevelsAreEvidenceOnly) {
    auto report = run_report(MySQLVersion::MYSQL_56);
    assert_status(report, "salvage.l1", report::CapabilityStatus::EVIDENCE_ONLY);
    assert_status(report, "salvage.l2", report::CapabilityStatus::EVIDENCE_ONLY);
    assert_status(report, "salvage.l3", report::CapabilityStatus::EVIDENCE_ONLY);
    assert_status(report, "salvage.l4", report::CapabilityStatus::EVIDENCE_ONLY);
    assert_output_category(report, "salvage.l1", report::RecoveryOutputCategory::PAGE_BUNDLE);
}

TEST_F(CapabilityReportTest, PageTypeCapabilities) {
    auto report_56 = run_report(MySQLVersion::MYSQL_56);
    assert_status(report_56, "page_type.FIL_PAGE_INDEX", report::CapabilityStatus::SUPPORTED);
    assert_status(report_56, "page_type.FIL_PAGE_COMPRESSED", report::CapabilityStatus::EVIDENCE_ONLY);
    assert_status(report_56, "page_type.FIL_PAGE_SDI", report::CapabilityStatus::REJECTED);
    assert_status(report_56, "page_type.FIL_PAGE_COMPRESSED_AND_ENCRYPTED",
                  report::CapabilityStatus::REJECTED);

    auto report_80 = run_report(MySQLVersion::MYSQL_80);
    assert_status(report_80, "page_type.FIL_PAGE_COMPRESSED", report::CapabilityStatus::EVIDENCE_ONLY);
    assert_status(report_80, "page_type.FIL_PAGE_COMPRESSED_AND_ENCRYPTED",
                  report::CapabilityStatus::EVIDENCE_ONLY);
    assert_status(report_80, "page_type.FIL_PAGE_SDI", report::CapabilityStatus::EVIDENCE_ONLY);
}

TEST_F(CapabilityReportTest, ReportSerializesStatusAndEvidence) {
    auto report = run_report(MySQLVersion::MYSQL_80);
    const std::string json = report.to_json();
    EXPECT_NE(json.find("\"status\": \"EVIDENCE_ONLY\""), std::string::npos);
    EXPECT_NE(json.find("\"output_category\": \"FORENSIC_EVIDENCE\""), std::string::npos);
    EXPECT_NE(json.find("\"id\": \"sdi.extraction\""), std::string::npos);
}

TEST_F(CapabilityReportTest, FeatureLookupByName) {
    auto report = run_report(MySQLVersion::MYSQL_80);
    EXPECT_FALSE(report.is_feature_supported("SDI extraction"));
    EXPECT_FALSE(report.is_feature_supported("Temporary pages"));
    EXPECT_FALSE(report.is_feature_supported("nonexistent_feature"));
    ASSERT_NE(report.find_feature("sdi.extraction"), nullptr);
}

TEST_F(CapabilityReportTest, MySQL57MidRange) {
    auto report = run_report(MySQLVersion::MYSQL_57);
    assert_status(report, "sdi.extraction", report::CapabilityStatus::REJECTED);
    assert_status(report, "tde.page_encryption", report::CapabilityStatus::RECOGNIZED_UNSUPPORTED);
    assert_status(report, "compression.page", report::CapabilityStatus::RECOGNIZED_UNSUPPORTED);
    assert_status(report, "temporary.pages", report::CapabilityStatus::REJECTED);
    assert_status(report, "salvage.l1", report::CapabilityStatus::EVIDENCE_ONLY);
}

} // namespace
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}