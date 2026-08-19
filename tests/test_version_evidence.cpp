/**
 * @file test_version_evidence.cpp
 * @brief Tests for multi-page version evidence aggregation (Phase 2 P0)
 *
 * Verifies that multi-page detection:
 * - Distinguishes strong evidence (SDI) from corroborating evidence (FSP/LSN)
 * - Never fabricates a precise version from checksum or page-header bytes
 * - Returns UNKNOWN with explicit rejection reasons when evidence is
 *   insufficient or conflicting
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>

#include "dbrecovery/common/mysql_version_detector.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

using namespace dbrecovery;

namespace {

/// @brief Build a page with canonical big-endian FIL metadata.
std::vector<uint8_t> build_page(
    uint16_t page_type, uint64_t flush_lsn = 0, uint32_t space_id = 1
) {
    std::vector<uint8_t> page(16384, 0);
    innodb::write_be<uint32_t>(page.data(), innodb::FIL_PAGE_OFFSET, 0);
    innodb::write_be<uint16_t>(page.data(), innodb::FIL_PAGE_TYPE_ABS, page_type);
    innodb::write_be<uint32_t>(page.data(), innodb::FIL_PAGE_SPACE_ID, space_id);
    innodb::write_be<uint64_t>(page.data(), innodb::FIL_PAGE_TYPE_ABS + 2, flush_lsn);
    return page;
}

/// @brief Sign a page as SDI (page type + signature at offset 64).
void sign_as_sdi(std::vector<uint8_t>& page) {
    innodb::write_be<uint16_t>(page.data(), innodb::FIL_PAGE_TYPE_ABS,
                               innodb::FIL_PAGE_SDI);
    constexpr uint8_t sdi_sig[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};
    std::memcpy(page.data() + 64, sdi_sig, 8);
}

/// @brief Set FSP flags on an FSP_HDR page (corroborating only).
void set_fsp_flags(std::vector<uint8_t>& page, uint32_t flags) {
    innodb::write_be<uint16_t>(page.data(), innodb::FIL_PAGE_TYPE_ABS,
                               innodb::FIL_PAGE_TYPE_FSP_HDR);
    innodb::write_be<uint32_t>(page.data(), innodb::FSP_SPACE_FLAGS_OFFSET, flags);
}

} // namespace

// ==================== Strong evidence: SDI ====================

TEST(MultiPageEvidenceTest, SingleSignedSdiIsStrong) {
    auto sdi = build_page(innodb::FIL_PAGE_INDEX, 0, 1);
    sign_as_sdi(sdi);

    auto result = MySQLVersionDetector::detect_pages_with_evidence({sdi.data()}, sdi.size());
    EXPECT_EQ(result.version, MySQLVersion::MYSQL_80);
    EXPECT_EQ(result.evidence, MultiPageEvidence::STRONG);
    EXPECT_EQ(result.strong_evidence_pages, 1u);
    EXPECT_TRUE(result.is_strong());
}

TEST(MultiPageEvidenceTest, SdiAmongAmbiguousPagesIsStrong) {
    auto sdi = build_page(innodb::FIL_PAGE_INDEX, 0, 1);
    sign_as_sdi(sdi);
    auto ambiguous = build_page(innodb::FIL_PAGE_INDEX, 0, 1);

    auto result = MySQLVersionDetector::detect_pages_with_evidence(
        {ambiguous.data(), sdi.data()}, sdi.size());
    EXPECT_EQ(result.version, MySQLVersion::MYSQL_80);
    EXPECT_EQ(result.evidence, MultiPageEvidence::STRONG);
    EXPECT_EQ(result.strong_evidence_pages, 1u);
    EXPECT_EQ(result.unknown_pages, 1u);
    EXPECT_EQ(result.total_pages, 2u);
}

TEST(MultiPageEvidenceTest, UnsignedSdiPageIsNotEvidence) {
    // SDI page TYPE without the 8-byte signature is not SDI evidence.
    auto unsigned_sdi = build_page(innodb::FIL_PAGE_SDI, 0, 1);

    auto result = MySQLVersionDetector::detect_pages_with_evidence(
        {unsigned_sdi.data()}, unsigned_sdi.size());
    EXPECT_EQ(result.version, MySQLVersion::UNKNOWN);
    EXPECT_EQ(result.evidence, MultiPageEvidence::INSUFFICIENT);
    EXPECT_EQ(result.strong_evidence_pages, 0u);
    EXPECT_FALSE(result.is_known());
}

// ==================== Corroborating evidence is not decisive ====================

TEST(MultiPageEvidenceTest, FspFlagsAloneIsInsufficient) {
    // FSP flags (POST_ANTELOPE/ATOMIC_BLOBS) only give a 5.7+ lower bound,
    // shared by 5.7 and 8.0+. Must not select a version.
    auto fsp = build_page(innodb::FIL_PAGE_INDEX, 0, 1);
    set_fsp_flags(fsp, innodb::FSP_FLAGS_POST_ANTELOPE | innodb::FSP_FLAGS_ATOMIC_BLOBS);

    auto result = MySQLVersionDetector::detect_pages_with_evidence({fsp.data()}, fsp.size());
    EXPECT_EQ(result.version, MySQLVersion::UNKNOWN);
    EXPECT_EQ(result.evidence, MultiPageEvidence::INSUFFICIENT);
    EXPECT_EQ(result.corroborating_pages, 1u);
    EXPECT_FALSE(result.is_known());
    ASSERT_FALSE(result.rejection_reasons.empty());
    EXPECT_NE(result.rejection_reasons.front().find("lower bound"),
              std::string::npos);
}

TEST(MultiPageEvidenceTest, CorroboratingOnlyPagesAreRejected) {
    auto fsp = build_page(innodb::FIL_PAGE_INDEX, 0, 1);
    set_fsp_flags(fsp, innodb::FSP_FLAGS_POST_ANTELOPE);
    auto lsn_page = build_page(innodb::FIL_PAGE_INDEX, 0x100000000ULL, 1);

    auto result = MySQLVersionDetector::detect_pages_with_evidence(
        {fsp.data(), lsn_page.data()}, fsp.size());
    EXPECT_EQ(result.version, MySQLVersion::UNKNOWN);
    EXPECT_EQ(result.evidence, MultiPageEvidence::INSUFFICIENT);
    EXPECT_GE(result.corroborating_pages, 1u);
    EXPECT_FALSE(result.is_known());
}

// ==================== Insufficient / empty evidence ====================

TEST(MultiPageEvidenceTest, EmptyInputIsInsufficient) {
    auto result = MySQLVersionDetector::detect_pages_with_evidence({}, 16384);
    EXPECT_EQ(result.version, MySQLVersion::UNKNOWN);
    EXPECT_EQ(result.evidence, MultiPageEvidence::INSUFFICIENT);
    EXPECT_EQ(result.total_pages, 0u);
    EXPECT_FALSE(result.is_known());
    EXPECT_FALSE(result.is_conflicting());
    EXPECT_TRUE(result.is_insufficient());
}

TEST(MultiPageEvidenceTest, AmbiguousPagesOnlyIsInsufficient) {
    auto ambiguous = build_page(innodb::FIL_PAGE_INDEX, 0, 1);

    auto result = MySQLVersionDetector::detect_pages_with_evidence(
        {ambiguous.data(), ambiguous.data()}, ambiguous.size());
    EXPECT_EQ(result.version, MySQLVersion::UNKNOWN);
    EXPECT_EQ(result.evidence, MultiPageEvidence::INSUFFICIENT);
    EXPECT_EQ(result.unknown_pages, 2u);
    EXPECT_EQ(result.corroborating_pages, 0u);
}

TEST(MultiPageEvidenceTest, NullPageIsInsufficient) {
    auto result = MySQLVersionDetector::detect_pages_with_evidence({nullptr}, 16384);
    EXPECT_EQ(result.version, MySQLVersion::UNKNOWN);
    EXPECT_EQ(result.unknown_pages, 1u);
}

// ==================== API consistency ====================

TEST(MultiPageEvidenceTest, LegacyApiDelegatesToEvidence) {
    auto sdi = build_page(innodb::FIL_PAGE_INDEX, 0, 1);
    sign_as_sdi(sdi);
    auto ambiguous = build_page(innodb::FIL_PAGE_INDEX, 0, 1);

    // Single strong page → MYSQL_80
    EXPECT_EQ(MySQLVersionDetector::detect_from_multiple_pages({sdi.data()}, sdi.size()),
              MySQLVersion::MYSQL_80);

    // Only ambiguous pages → UNKNOWN
    EXPECT_EQ(MySQLVersionDetector::detect_from_multiple_pages(
                  {ambiguous.data(), ambiguous.data()}, ambiguous.size()),
              MySQLVersion::UNKNOWN);

    // Mixture with strong evidence → MYSQL_80
    EXPECT_EQ(MySQLVersionDetector::detect_from_multiple_pages(
                  {ambiguous.data(), sdi.data()}, sdi.size()),
              MySQLVersion::MYSQL_80);
}

TEST(MultiPageEvidenceTest, FactoryReturnsEvidenceBreakdown) {
    auto sdi = build_page(innodb::FIL_PAGE_INDEX, 0, 1);
    sign_as_sdi(sdi);

    auto result = VersionDetectionFactory::instance()
                      .detect_version_from_pages_with_evidence({sdi.data()}, sdi.size());
    EXPECT_EQ(result.version, MySQLVersion::MYSQL_80);
    EXPECT_EQ(result.evidence, MultiPageEvidence::STRONG);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}