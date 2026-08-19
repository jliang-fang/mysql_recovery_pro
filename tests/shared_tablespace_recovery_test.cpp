/**
 * @file shared_tablespace_recovery_test.cpp
 * @brief Phase 5e tests for SharedTablespaceRecovery streaming scan,
 *        page-bundle manifest, checksum verification, and zero-page ban.
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstring>

#include "dbrecovery/innodb/shared_tablespace_recovery.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"

using namespace dbrecovery;
using namespace dbrecovery::innodb;

namespace {

/// @brief Build a minimal page with valid FIL header and checksum.
/// @param page_no Page number to write at offset 4 (big-endian)
/// @param space_id Space ID to write at offset 34 (big-endian)
/// @param page_type Page type to write at offset 24 (big-endian)
/// @param add_checksum If true, compute and append a valid CRC32C checksum
std::vector<uint8_t> build_page(uint32_t page_no, uint32_t space_id,
                                 uint16_t page_type, bool add_checksum = true) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint32_t>(page.data(), 4, page_no);
    write_be<uint32_t>(page.data(), 34, space_id);
    write_be<uint16_t>(page.data(), 24, page_type);

    // Page header fields (FIL_HEADER_SIZE = 38): n_dir_slots, heap_top, n_heap
    write_le<uint16_t>(page.data(), 38, 3);     // n_dir_slots
    write_le<uint16_t>(page.data(), 40, 150);   // heap_top
    write_le<uint16_t>(page.data(), 42, 5);     // n_heap

    // INFIMUM and SUPREMUM
    page[54] = REC_STATUS_INFIMUM;
    write_le<uint16_t>(page.data(), 55, 120);
    page[120] = REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page.data(), 121, 0xFFFF);

    if (add_checksum) {
        write_page_checksum(page.data(), page.size(), ChecksumAlgo::CRC32C);
    }
    return page;
}

std::string make_temp_file(const std::string& name, const std::vector<uint8_t>& data) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    out.close();
    return path.string();
}

/// @brief Build a multi-page shared tablespace file from page vectors
std::string build_shared_tablespace(const std::string& name,
                                     const std::vector<std::vector<uint8_t>>& pages) {
    std::vector<uint8_t> combined;
    for (const auto& p : pages) {
        combined.insert(combined.end(), p.begin(), p.end());
    }
    return make_temp_file(name, combined);
}

}  // namespace

// ==================== Phase 5e: Streaming scan returns manifest ====================

TEST(SharedTablespaceRecoveryTest, StreamingScanReturnsManifest) {
    auto p0 = build_page(0, 1, FIL_PAGE_SDI, true);
    auto p1 = build_page(1, 1, FIL_PAGE_INDEX, true);
    auto p2 = build_page(2, 2, FIL_PAGE_INDEX, true);  // different space_id

    std::vector<std::vector<uint8_t>> pages = {p0, p1, p2};
    std::string ts_path = build_shared_tablespace("dbrec_ts_manifest.ibd", pages);

    SharedTablespaceRecovery engine;
    engine.set_tablespace_path(ts_path);

    // Scan all spaces
    auto manifest = engine.scan_pages(0);
    EXPECT_EQ(manifest.pages_scanned, 3);
    EXPECT_EQ(manifest.total_file_pages, 3);
    EXPECT_GE(manifest.pages_with_valid_checksum, 3);
    EXPECT_EQ(manifest.pages_identity_matched, 3);  // filter=0 means all match

    // Check entry details
    ASSERT_EQ(manifest.entries.size(), 3);
    EXPECT_EQ(manifest.entries[0].page_no, 0);
    EXPECT_EQ(manifest.entries[0].space_id, 1);
    EXPECT_EQ(manifest.entries[0].page_type, FIL_PAGE_SDI);
    EXPECT_TRUE(manifest.entries[0].checksum_valid);
    EXPECT_TRUE(manifest.entries[0].identity_matched);

    EXPECT_EQ(manifest.entries[1].page_no, 1);
    EXPECT_EQ(manifest.entries[1].space_id, 1);
    EXPECT_EQ(manifest.entries[1].page_type, FIL_PAGE_INDEX);

    EXPECT_EQ(manifest.entries[2].page_no, 2);
    EXPECT_EQ(manifest.entries[2].space_id, 2);
    EXPECT_EQ(manifest.entries[2].page_type, FIL_PAGE_INDEX);

    // Multiple spaces detected
    EXPECT_EQ(manifest.pages_per_space.size(), 2);
    EXPECT_EQ(manifest.pages_per_space[1], 2);
    EXPECT_EQ(manifest.pages_per_space[2], 1);

    // Verdict should be IMPORTABLE (no holes, no mismatches since filter=0)
    EXPECT_EQ(manifest.verdict, ImportabilityVerdict::IMPORTABLE);
}

// ==================== Phase 5e: Checksum verification ====================

TEST(SharedTablespaceRecoveryTest, ChecksumInvalidPageDetected) {
    // Build a page with valid CRC32C checksum, then corrupt the body
    auto p0 = build_page(0, 1, FIL_PAGE_INDEX, true);
    auto p1 = build_page(1, 1, FIL_PAGE_INDEX, true);  // has valid CRC32C checksum
    p1[100] = 0xFF;  // corrupt page body AFTER checksum was written

    // Strict validation detects the corruption
    auto csr_strict = validate_page_checksum(p1.data(), p1.size(), true);
    EXPECT_FALSE(csr_strict.is_valid);

    // Non-strict also detects it — CRC32C-tagged pages do NOT fall back to NONE
    auto csr_lenient = validate_page_checksum(p1.data(), p1.size(), false);
    EXPECT_FALSE(csr_lenient.is_valid);

    std::vector<std::vector<uint8_t>> pages = {p0, p1};
    std::string ts_path = build_shared_tablespace("dbrec_ts_cksum.ibd", pages);

    SharedTablespaceRecovery engine;
    engine.set_tablespace_path(ts_path);
    auto manifest = engine.scan_pages(0);

    EXPECT_EQ(manifest.pages_scanned, 2);
    // p0 has valid CRC32C, p1 has corrupted body → checksum fails
    EXPECT_EQ(manifest.pages_with_valid_checksum, 1);
    EXPECT_EQ(manifest.pages_with_invalid_checksum, 1);
    EXPECT_EQ(manifest.verdict, ImportabilityVerdict::CHECKSUM_FAILURES);

    // Entry-level: first page has CRC32C valid, second page has CRC32C invalid
    ASSERT_EQ(manifest.entries.size(), 2);
    EXPECT_TRUE(manifest.entries[0].checksum_valid);
    EXPECT_EQ(manifest.entries[0].checksum_algorithm, ChecksumAlgo::CRC32C);
    EXPECT_FALSE(manifest.entries[1].checksum_valid);
    EXPECT_EQ(manifest.entries[1].checksum_algorithm, ChecksumAlgo::CRC32C);
}

TEST(SharedTablespaceRecoveryTest, ChecksumVerificationCanBeDisabled) {
    auto p0 = build_page(0, 1, FIL_PAGE_INDEX, false);
    p0[100] = 0xFF;  // corrupted

    std::vector<std::vector<uint8_t>> pages = {p0};
    std::string ts_path = build_shared_tablespace("dbrec_ts_nocsum.ibd", pages);

    SharedTablespaceScanConfig cfg;
    cfg.verify_checksums = false;

    SharedTablespaceRecovery engine;
    engine.set_tablespace_path(ts_path);
    engine.set_scan_config(cfg);
    auto manifest = engine.scan_pages(0);

    // When verification is disabled, all pages are treated as valid
    EXPECT_EQ(manifest.pages_scanned, 1);
    EXPECT_EQ(manifest.pages_with_valid_checksum, 1);
    EXPECT_EQ(manifest.pages_with_invalid_checksum, 0);
}

// ==================== Phase 5e: Identity filter ====================

TEST(SharedTablespaceRecoveryTest, IdentityFilterRejectsMismatch) {
    auto p0 = build_page(0, 10, FIL_PAGE_INDEX, true);
    auto p1 = build_page(1, 10, FIL_PAGE_INDEX, true);
    auto p2 = build_page(2, 20, FIL_PAGE_INDEX, true);  // wrong space_id

    std::vector<std::vector<uint8_t>> pages = {p0, p1, p2};
    std::string ts_path = build_shared_tablespace("dbrec_ts_filter.ibd", pages);

    SharedTablespaceRecovery engine;
    engine.set_tablespace_path(ts_path);
    auto manifest = engine.scan_pages(10);  // filter for space_id=10

    EXPECT_EQ(manifest.pages_scanned, 3);
    EXPECT_EQ(manifest.pages_identity_matched, 2);   // p0, p1
    EXPECT_EQ(manifest.pages_identity_mismatched, 1); // p2
    EXPECT_EQ(manifest.mismatches_found, 1);

    // Entry-level identity tracking
    ASSERT_EQ(manifest.entries.size(), 3);
    EXPECT_TRUE(manifest.entries[0].identity_matched);
    EXPECT_TRUE(manifest.entries[1].identity_matched);
    EXPECT_FALSE(manifest.entries[2].identity_matched);

    EXPECT_EQ(manifest.verdict, ImportabilityVerdict::IDENTITY_MISMATCH);
}

// ==================== Phase 5e: Zero-page ban ====================

TEST(SharedTablespaceRecoveryTest, ExtractTableNoZeroPages) {
    // Simulate a shared tablespace with pages for space_id=1 at pages 0,2,5
    // (non-contiguous). extract_table must NOT write zero-filled holes.
    auto p0 = build_page(0, 1, FIL_PAGE_INDEX, true);
    auto p1 = build_page(1, 2, FIL_PAGE_INDEX, true);   // different space, skipped
    auto p2 = build_page(2, 1, FIL_PAGE_INDEX, true);   // space_id=1, included
    auto p3 = build_page(3, 3, FIL_PAGE_SDI, true);     // different space, skipped
    auto p4 = build_page(4, 3, FIL_PAGE_INDEX, true);   // different space, skipped
    auto p5 = build_page(5, 1, FIL_PAGE_INDEX, true);   // space_id=1, included

    std::vector<std::vector<uint8_t>> pages = {p0, p1, p2, p3, p4, p5};
    std::string ts_path = build_shared_tablespace("dbrec_ts_noholes.ibd", pages);

    std::string out_path = std::filesystem::temp_directory_path() / "dbrec_ts_extracted.ibd";

    SharedTablespaceRecovery engine;
    engine.set_tablespace_path(ts_path);
    engine.set_output_dir(std::filesystem::temp_directory_path().string());
    bool result = engine.extract_table(1, out_path);

    EXPECT_TRUE(result);

    // Read back the extracted file
    std::ifstream in(out_path, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(in);
    uint64_t extracted_size = in.tellg();
    in.seekg(0, std::ios::beg);

    // Only 3 pages (p0, p2, p5) — no zero-fill holes
    uint64_t expected_size = 3 * 16384ULL;
    EXPECT_EQ(extracted_size, expected_size);

    // Verify each page is the original content, not zero-filled
    std::vector<uint8_t> buf(16384);
    for (uint32_t i = 0; i < 3; i++) {
        in.read(reinterpret_cast<char*>(buf.data()), 16384);
        ASSERT_EQ(in.gcount(), 16384);

        // Page should have non-zero FIL header
        uint32_t sp_id = read_be<uint32_t>(buf.data(), FIL_PAGE_SPACE_ID);
        EXPECT_EQ(sp_id, 1) << "Page " << i << " has wrong space_id";
    }

    // Check manifest was written
    std::string manifest_path = out_path + ".manifest.json";
    std::ifstream mf(manifest_path);
    ASSERT_TRUE(mf);
    std::string manifest_content((std::istreambuf_iterator<char>(mf)),
                                  std::istreambuf_iterator<char>());
    EXPECT_FALSE(manifest_content.empty());
    // Verify manifest contains "HAS_HOLES" since pages were non-contiguous
    EXPECT_NE(manifest_content.find("HAS_HOLES"), std::string::npos);

    std::filesystem::remove(out_path);
    std::filesystem::remove(manifest_path);
}

TEST(SharedTablespaceRecoveryTest, ExtractTableNoMatchingSpaceReturnsFalse) {
    auto p0 = build_page(0, 1, FIL_PAGE_INDEX, true);
    std::vector<std::vector<uint8_t>> pages = {p0};
    std::string ts_path = build_shared_tablespace("dbrec_ts_nomatch.ibd", pages);

    std::string out_path = std::filesystem::temp_directory_path() / "dbrec_ts_nomatch_out.ibd";

    SharedTablespaceRecovery engine;
    engine.set_tablespace_path(ts_path);
    engine.set_output_dir(std::filesystem::temp_directory_path().string());
    bool result = engine.extract_table(99, out_path);

    EXPECT_FALSE(result);
    EXPECT_FALSE(std::filesystem::exists(out_path));
}

// ==================== Phase 5e: Manifest JSON output ====================

TEST(SharedTablespaceRecoveryTest, ManifestToJson) {
    auto p0 = build_page(0, 10, FIL_PAGE_INDEX, true);
    auto p1 = build_page(1, 10, FIL_PAGE_SDI, true);

    std::vector<std::vector<uint8_t>> pages = {p0, p1};
    std::string ts_path = build_shared_tablespace("dbrec_ts_json.ibd", pages);

    SharedTablespaceRecovery engine;
    engine.set_tablespace_path(ts_path);
    auto manifest = engine.scan_pages(10);

    std::string json = manifest.to_json();
    EXPECT_FALSE(json.empty());

    // Verify JSON contains key fields
    EXPECT_NE(json.find("\"pages_scanned\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"verdict\": \"IMPORTABLE\""), std::string::npos);
    EXPECT_NE(json.find("\"page_size\": 16384"), std::string::npos);

    // Should have entries array
    EXPECT_NE(json.find("\"entries\""), std::string::npos);
    EXPECT_NE(json.find("\"page_no\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"page_no\": 1"), std::string::npos);
}

// ==================== Phase 5e: Memory limit ====================

TEST(SharedTablespaceRecoveryTest, ManifestMemoryLimitTruncatesEntries) {
    // Build 10 pages
    std::vector<std::vector<uint8_t>> pages;
    for (uint32_t i = 0; i < 10; i++) {
        pages.push_back(build_page(i, 1, FIL_PAGE_INDEX, true));
    }
    std::string ts_path = build_shared_tablespace("dbrec_ts_mem.ibd", pages);

    SharedTablespaceScanConfig cfg;
    cfg.max_memory_bytes = 256;  // tiny — only ~3 entries fit

    SharedTablespaceRecovery engine;
    engine.set_tablespace_path(ts_path);
    engine.set_scan_config(cfg);
    auto manifest = engine.scan_pages(0);

    // All 10 pages scanned, but entries truncated
    EXPECT_EQ(manifest.pages_scanned, 10);
    EXPECT_LT(manifest.entries.size(), 10);  // entries truncated due to memory limit
    EXPECT_EQ(manifest.verdict, ImportabilityVerdict::IMPORTABLE);
}

// ==================== Phase 5e: Empty / missing file ====================

TEST(SharedTablespaceRecoveryTest, EmptyPathReturnsEmptyManifest) {
    SharedTablespaceRecovery engine;
    auto manifest = engine.scan_pages(0);

    EXPECT_EQ(manifest.pages_scanned, 0);
    EXPECT_EQ(manifest.entries.size(), 0);
    EXPECT_EQ(manifest.verdict, ImportabilityVerdict::UNKNOWN);
}

TEST(SharedTablespaceRecoveryTest, MissingFileReturnsEmptyManifest) {
    SharedTablespaceRecovery engine;
    engine.set_tablespace_path("/nonexistent/shared.ibd");
    auto manifest = engine.scan_pages(0);

    EXPECT_EQ(manifest.pages_scanned, 0);
    EXPECT_EQ(manifest.verdict, ImportabilityVerdict::UNKNOWN);
}

// ==================== Main ====================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}