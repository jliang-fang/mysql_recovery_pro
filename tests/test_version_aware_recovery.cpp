/**
 * @file test_version_aware_recovery.cpp
 * @brief Unit tests for the version-aware recovery engine safety contract
 *
 * Phase 2 hardening: UNKNOWN version must be capability-free, automatic
 * unresolved detection must fail closed (no structured records), and forced
 * versions must route to the corresponding parser. Synthetic pages use
 * canonical big-endian InnoDB FIL headers.
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <fstream>
#include <filesystem>

#include "dbrecovery/recovery/version_aware_recovery.hpp"
#include "dbrecovery/common/version_compat.hpp"
#include "dbrecovery/common/mysql_version_detector.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

using namespace dbrecovery;
using namespace dbrecovery::recovery;

// ==================== Canonical page builder ====================

/// @brief Build a synthetic INDEX page with canonical big-endian FIL metadata.
/// Non-INDEX pages that need a specific page type must pass it explicitly.
std::vector<uint8_t> make_index_page(
    uint16_t page_type,
    uint64_t flush_lsn = 0,
    uint16_t page_version = 0,
    uint32_t space_id = 1
) {
    std::vector<uint8_t> page(16384, 0);

    innodb::write_be<uint32_t>(page.data(), innodb::FIL_PAGE_OFFSET, 0);
    innodb::write_be<uint32_t>(page.data(), innodb::FIL_PAGE_PREV_ABS, 0);
    innodb::write_be<uint32_t>(page.data(), innodb::FIL_PAGE_NEXT_ABS, 0);
    innodb::write_be<uint64_t>(page.data(), innodb::FIL_PAGE_LSN_ABS, 0);
    innodb::write_be<uint16_t>(page.data(), innodb::FIL_PAGE_TYPE_ABS, page_type);
    innodb::write_be<uint32_t>(page.data(), innodb::FIL_PAGE_SPACE_ID, space_id);
    // FLUSH_LSN at offset 26 (FIL_PAGE_TYPE_ABS + 2), 8 bytes big-endian.
    innodb::write_be<uint64_t>(page.data(), innodb::FIL_PAGE_TYPE_ABS + 2, flush_lsn);
    // Checksum at offset 0 (big-endian).
    innodb::write_be<uint32_t>(page.data(), 0, 0x1000000);

    // PAGE_N_DIR_SLOTS at offset 38 (big-endian) doubles as the 8.0+
    // page_version field for INDEX pages.
    innodb::write_be<uint16_t>(page.data(), innodb::FIL_PAGE_OFFSET_N_DIR_SLOTS, page_version);

    return page;
}

/// @brief Build a typed and signed SDI page, the only automatic 8.0-family marker.
std::vector<uint8_t> make_mysql80_page() {
    auto page = make_index_page(innodb::FIL_PAGE_SDI, 0, 0);
    constexpr uint8_t sdi_signature[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};
    std::memcpy(page.data() + 64, sdi_signature, sizeof(sdi_signature));
    return page;
}

/// @brief Offset-38 content is page-header data, never version evidence.
std::vector<uint8_t> make_mysql84_page() {
    return make_index_page(innodb::FIL_PAGE_INDEX, 0x100000000ULL, 2);
}

std::vector<uint8_t> make_mysql90_page() {
    return make_index_page(innodb::FIL_PAGE_INDEX, 0x100000000ULL, 3);
}

/// @brief Build an INDEX page with no 8.x markers: no FLUSH_LSN, no page
/// version. The hardened detector must NOT classify this.
std::vector<uint8_t> make_ambiguous_index_page() {
    return make_index_page(innodb::FIL_PAGE_INDEX, 0, 0);
}

// ==================== VersionConfig: UNKNOWN must be capability-free ====================

TEST(VersionConfigTest, UnknownIsCapabilityFree) {
    auto config = get_version_config(MySQLVersion::UNKNOWN);

    EXPECT_EQ(config.version, MySQLVersion::UNKNOWN);
    EXPECT_EQ(config.name, "Unknown");
    EXPECT_EQ(config.default_page_size, 0);
    EXPECT_FALSE(config.supports_sdi);
    EXPECT_FALSE(config.supports_instant_ddl);
    EXPECT_FALSE(config.supports_encryption);
    EXPECT_FALSE(config.supports_temporary_pages);
    EXPECT_FALSE(config.supports_new_checksum);
    EXPECT_FALSE(config.supports_invisible_columns);
    EXPECT_FALSE(config.supports_json_type);
    EXPECT_FALSE(config.supports_redundant);
    EXPECT_FALSE(config.supports_compact);
    EXPECT_FALSE(config.supports_dynamic);
    EXPECT_FALSE(config.supports_compressed);
}

TEST(VersionConfigTest, UnknownRowFormatIsNone) {
    VersionDetector detector;
    EXPECT_EQ(detector.get_default_row_format(MySQLVersion::UNKNOWN), RowFormat::REDUNDANT);
}

TEST(VersionConfigTest, UnknownCapabilityGetters) {
    EXPECT_FALSE(VersionDetector::supports_sdi(MySQLVersion::UNKNOWN));
    EXPECT_FALSE(VersionDetector::supports_encryption(MySQLVersion::UNKNOWN));
    EXPECT_FALSE(VersionDetector::supports_instant_ddl(MySQLVersion::UNKNOWN));
}

// ==================== Detection: ambiguous pages are UNKNOWN ====================

TEST(VersionDetectionTest, AmbiguousIndexPageIsUnknown) {
    auto page = make_ambiguous_index_page();
    MySQLVersion v = MySQLVersionDetector::detect(page.data(), page.size());
    EXPECT_EQ(v, MySQLVersion::UNKNOWN);
}

TEST(VersionDetectionTest, AllZeroPageIsUnknown) {
    std::vector<uint8_t> page(16384, 0);
    MySQLVersion v = MySQLVersionDetector::detect(page.data(), page.size());
    EXPECT_EQ(v, MySQLVersion::UNKNOWN);
}

TEST(VersionDetectionTest, DetectsMySQL80) {
    auto page = make_mysql80_page();
    MySQLVersion v = MySQLVersionDetector::detect(page.data(), page.size());
    EXPECT_EQ(v, MySQLVersion::MYSQL_80);
}

TEST(VersionDetectionTest, HeaderBytesDoNotIdentifyVersions) {
    auto v84 = make_mysql84_page();
    auto v90 = make_mysql90_page();
    EXPECT_EQ(MySQLVersionDetector::detect(v84.data(), v84.size()),
              MySQLVersion::UNKNOWN);
    EXPECT_EQ(MySQLVersionDetector::detect(v90.data(), v90.size()),
              MySQLVersion::UNKNOWN);
}

// Non-INDEX pages must not be classified via the page-version field.
TEST(VersionDetectionTest, NonIndexPageWithHighVersionFieldIsUnknown) {
    // UNDO_LOG page type with a bogus high value at offset 38.
    auto page = make_index_page(innodb::FIL_PAGE_UNDO_LOG, 0x100000000ULL, 3);
    MySQLVersion v = MySQLVersionDetector::detect(page.data(), page.size());
    EXPECT_EQ(v, MySQLVersion::UNKNOWN);
}

TEST(VersionDetectionTest, FlushLsnWithoutIndexVersionIsUnknown) {
    auto page = make_index_page(innodb::FIL_PAGE_INDEX, 0x100000000ULL, 0);
    EXPECT_EQ(MySQLVersionDetector::detect(page.data(), page.size()),
              MySQLVersion::UNKNOWN);
}

TEST(VersionDetectionTest, UnsignedSdiPageIsUnknown) {
    auto page = make_index_page(innodb::FIL_PAGE_SDI, 0x100000000ULL, 0);
    EXPECT_EQ(MySQLVersionDetector::detect(page.data(), page.size()),
              MySQLVersion::UNKNOWN);
}

TEST(VersionDetectionTest, UnsignedCompressedSdiPageIsUnknown) {
    auto page = make_index_page(innodb::FIL_PAGE_SDI_ZIP, 0x100000000ULL, 0);
    EXPECT_EQ(MySQLVersionDetector::detect(page.data(), page.size()),
              MySQLVersion::UNKNOWN);
}

TEST(VersionDetectionTest, SignedSdiIdentifiesMySQL80Family) {
    auto page = make_mysql80_page();
    EXPECT_EQ(MySQLVersionDetector::detect(page.data(), page.size()),
              MySQLVersion::MYSQL_80);
}

TEST(VersionDetectionTest, UnsupportedPageTypeIsUnknown) {
    auto page = make_index_page(0xFFFF, 0x100000000ULL, 3);
    EXPECT_EQ(MySQLVersionDetector::detect(page.data(), page.size()),
              MySQLVersion::UNKNOWN);
}

TEST(VersionDetectionTest, TruncatedPageIsUnknown) {
    auto page = make_mysql90_page();
    EXPECT_EQ(MySQLVersionDetector::detect(page.data(), 37),
              MySQLVersion::UNKNOWN);
}

TEST(VersionDetectionTest, FspFlagsIdentifyMySQL57) {
    auto fsp = make_index_page(innodb::FIL_PAGE_TYPE_FSP_HDR, 0, 0);
    innodb::write_be<uint32_t>(fsp.data(), innodb::FIL_HEADER_SIZE + 4, 0x21);
    EXPECT_EQ(MySQLVersionDetector::detect(fsp.data(), fsp.size()),
              MySQLVersion::MYSQL_57);

    auto undo = make_index_page(innodb::FIL_PAGE_UNDO_LOG, 0, 0);
    innodb::write_be<uint16_t>(undo.data(), innodb::FIL_HEADER_SIZE, 1);
    EXPECT_EQ(MySQLVersionDetector::detect(undo.data(), undo.size()),
              MySQLVersion::UNKNOWN);
}

TEST(VersionDetectionTest, MatchesVersionRequiresCompleteEvidence) {
    auto lsn_only = make_index_page(innodb::FIL_PAGE_INDEX, 0x100000000ULL, 0);
    EXPECT_FALSE(MySQLVersionDetector::matches_version(
        lsn_only.data(), lsn_only.size(), MySQLVersion::MYSQL_80));

    auto unsigned_sdi = make_index_page(innodb::FIL_PAGE_SDI, 0x100000000ULL, 0);
    EXPECT_FALSE(MySQLVersionDetector::matches_version(
        unsigned_sdi.data(), unsigned_sdi.size(), MySQLVersion::MYSQL_80));
}

// ==================== DetailedVersionDetector alignment ====================

TEST(DetailedVersionDetectorTest, AlignedWithCanonicalDetector) {
    VersionDetectionFactory& factory = VersionDetectionFactory::instance();

    auto ambiguous = make_ambiguous_index_page();
    auto sdi = make_mysql80_page();
    auto header_bytes_84 = make_mysql84_page();
    auto header_bytes_90 = make_mysql90_page();

    EXPECT_EQ(factory.detect_version_detailed(ambiguous.data(), ambiguous.size()).version,
              MySQLVersion::UNKNOWN);
    EXPECT_EQ(factory.detect_version_detailed(sdi.data(), sdi.size()).version,
              MySQLVersion::MYSQL_80);
    EXPECT_EQ(factory.detect_version_detailed(header_bytes_84.data(), header_bytes_84.size()).version,
              MySQLVersion::UNKNOWN);
    EXPECT_EQ(factory.detect_version_detailed(header_bytes_90.data(), header_bytes_90.size()).version,
              MySQLVersion::UNKNOWN);
}

// ==================== Engine: unresolved automatic detection fails closed ====================

namespace {
std::string make_temp_file(const std::vector<uint8_t>& data) {
    auto path = std::filesystem::temp_directory_path() / "dbrec_phase2_unknown.bin";
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    out.close();
    return path.string();
}
} // namespace

TEST(VersionAwareRecoveryEngineTest, UnresolvedDetectionProducesNoRecords) {
    VersionAwareRecoveryEngine engine;
    engine.set_input_file(make_temp_file(make_ambiguous_index_page()));
    engine.set_output_dir(std::filesystem::temp_directory_path().string());

    auto records = engine.recover();
    EXPECT_TRUE(records.empty()) << "UNKNOWN version must not produce structured records";
    EXPECT_EQ(engine.get_stats().recovered_records, 0);
    EXPECT_EQ(engine.get_detected_version(), MySQLVersion::UNKNOWN);
}

TEST(VersionAwareRecoveryEngineTest, ForcedVersionRoutesToParser) {
    VersionAwareRecoveryEngine engine;
    engine.set_force_version(MySQLVersion::MYSQL_80);
    // A page with no 8.x markers would be UNKNOWN if auto-detected, but the
    // forced version must route to the 8.0 parser instead.
    engine.set_input_file(make_temp_file(make_ambiguous_index_page()));
    engine.set_output_dir(std::filesystem::temp_directory_path().string());

    engine.recover();
    EXPECT_EQ(engine.get_detected_version(), MySQLVersion::MYSQL_80);
    EXPECT_EQ(engine.get_stats().version_detection_method, "Forced version");
}

// ==================== Main ====================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}