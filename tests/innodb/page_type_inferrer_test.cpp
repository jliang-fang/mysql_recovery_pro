#include <gtest/gtest.h>

// Make private members accessible for testing
#define private public
#include "dbrecovery/innodb/page_type_inferrer.hpp"
#undef private
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include <fstream>
#include <vector>
#include <cstring>
#include <cstdio>

namespace dbrecovery {
namespace innodb {

// ==================== Helper Functions ====================

// Build a synthetic INDEX page that passes all 10 validation checks.
// Returns a page_size-byte buffer.
static std::vector<uint8_t> create_valid_index_page(
    size_t page_size = 16384,
    uint32_t page_no = 0,
    uint32_t space_id = 1,
    uint64_t lsn = 12345678,
    uint64_t index_id = 42,
    uint16_t n_dir_slots = 2,
    uint16_t heap_top = 200,
    uint16_t n_heap = 3) {

    std::vector<uint8_t> page(page_size, 0);

    // --- FIL header (big-endian) ---
    // Offset 4: page_no
    write_be<uint32_t>(page.data(), 4, page_no);
    // Offset 8: prev_page = 0xFFFFFFFF (none)
    write_be<uint32_t>(page.data(), 8, 0xFFFFFFFF);
    // Offset 12: next_page = 0xFFFFFFFF (none)
    write_be<uint32_t>(page.data(), 12, 0xFFFFFFFF);
    // Offset 16: LSN
    write_be<uint64_t>(page.data(), 16, lsn);
    // Offset 24: page_type = FIL_PAGE_INDEX (0x45BF)
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);
    // Offset 26: page_level = 0 (leaf)
    write_be<uint16_t>(page.data(), 26, 0);
    // Offset 34: space_id
    write_be<uint32_t>(page.data(), 34, space_id);

    // --- Page header (at offset 38) ---
    // Offset 38: n_dir_slots
    write_be<uint16_t>(page.data(), 38, n_dir_slots);
    // Offset 40: heap_top
    write_be<uint16_t>(page.data(), 40, heap_top);
    // Offset 42: n_heap
    write_be<uint16_t>(page.data(), 42, n_heap);
    // Offset 44: index_id (8 bytes BE)
    write_be<uint64_t>(page.data(), 44, index_id);

    // --- INFIMUM record at offset 54 ---
    // byte 4 (offset 58): status = REC_STATUS_INFIMUM (0x02) in bits 4-5
    // (0x02 << 4) = 0x20
    page[58] = 0x20;

    // SUPREMUM at offset 67 (54 + 13 bytes)
    uint16_t supremum_offset = 67;
    // INFIMUM next_record at bytes 59-60 (BE)
    write_be<uint16_t>(page.data(), 59, supremum_offset);

    // --- SUPREMUM record at offset 67 ---
    // byte 4 (offset 71): status = REC_STATUS_SUPREMUM (0x03) in bits 4-5
    // (0x03 << 4) = 0x30
    page[71] = 0x30;

    // --- Checksum algorithm ID at page_size - 4 ---
    // Set to CHECKSUM_NONE_MAGIC so Level 1 scan can find it
    write_le<uint32_t>(page.data(), page_size - 4, CHECKSUM_NONE_MAGIC);

    // --- Page directory (at end of page, before checksum trailer) ---
    // The validate_offset_table code computes:
    //   dir_start = page_size - n_dir_slots * 2
    // For page_size=16384, n_dir_slots=2: dir_start = 16380
    // Slot 0: points to INFIMUM (54 LE)
    // Slot 1: points to SUPREMUM (67 LE)
    // NOTE: directory overlaps with checksum at page_size-4; write directory AFTER
    // checksum so valid slot values (54, 67) overwrite the checksum bytes.
    size_t dir_start = page_size - static_cast<size_t>(n_dir_slots) * 2;
    write_le<uint16_t>(page.data(), dir_start, 54);
    write_le<uint16_t>(page.data(), dir_start + 2, supremum_offset);

    return page;
}

// Build a buffer with N valid INDEX pages at exact page_size boundaries
static std::vector<uint8_t> create_multi_page_buffer(
    size_t num_pages, size_t page_size = 16384) {
    std::vector<uint8_t> buffer(num_pages * page_size, 0);
    for (size_t i = 0; i < num_pages; i++) {
        auto page = create_valid_index_page(
            page_size, static_cast<uint32_t>(i), 1,
            12345678 + i * 1000, 42 + i);
        std::memcpy(buffer.data() + i * page_size, page.data(), page_size);
    }
    return buffer;
}

// Build a minimal page that passes quick_prefilter but has no INFIMUM/SUPREMUM
static std::vector<uint8_t> create_minimal_header_page(
    size_t page_size = 16384,
    uint32_t page_no = 0,
    uint32_t space_id = 1,
    uint64_t lsn = 12345678,
    uint16_t page_type = FIL_PAGE_INDEX) {

    std::vector<uint8_t> page(page_size, 0xFF);  // Fill with 0xFF to prevent accidental CHECKSUM_NONE_MAGIC matches
    write_be<uint32_t>(page.data(), 4, page_no);
    write_be<uint32_t>(page.data(), 8, 0xFFFFFFFF);
    write_be<uint32_t>(page.data(), 12, 0xFFFFFFFF);
    write_be<uint64_t>(page.data(), 16, lsn);
    write_be<uint16_t>(page.data(), 24, page_type);
    // Set offset 26 (FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID) to 0xFFFF = invalid page type
    // to prevent false ALLOCATED page detection when scanning from non-zero offsets
    write_be<uint16_t>(page.data(), 26, 0xFFFF);
    write_be<uint32_t>(page.data(), 34, space_id);
    // set tail to non-0xFF
    write_le<uint32_t>(page.data(), page_size - 4, 0);
    return page;
}

// Build a buffer with a UUID pattern for Level 3 tests
static std::vector<uint8_t> create_uuid_pattern_buffer(size_t page_size = 16384) {
    std::vector<uint8_t> buf(page_size, 0);
    // Place a UUID string at offset 100
    const char* uuid = "550e8400-e29b-41d4-a716-446655440000";
    std::memcpy(buf.data() + 100, uuid, 36);
    // Also place a timestamp at offset 200
    write_le<uint32_t>(buf.data(), 200, 1700000000);  // 2023-11-14
    write_le<uint32_t>(buf.data(), 204, 1700000001);
    write_le<uint32_t>(buf.data(), 208, 1700000002);
    write_le<uint32_t>(buf.data(), 212, 1700000003);
    return buf;
}

// ==================== Quick Prefilter Tests ====================

TEST(PageTypeInferrerTest, QuickPrefilterValid) {
    auto page = create_valid_index_page();
    EXPECT_TRUE(PageTypeInferrer::quick_prefilter(page.data(), page.size()));
}

TEST(PageTypeInferrerTest, QuickPrefilterInvalidPageType) {
    auto page = create_valid_index_page();
    write_be<uint16_t>(page.data(), 24, 0x0001);  // invalid type
    EXPECT_FALSE(PageTypeInferrer::quick_prefilter(page.data(), page.size()));
}

TEST(PageTypeInferrerTest, QuickPrefilterBadSpaceId) {
    auto page = create_valid_index_page();
    write_be<uint32_t>(page.data(), 34, 0x1000000);  // > 0xFFFFFF
    EXPECT_FALSE(PageTypeInferrer::quick_prefilter(page.data(), page.size()));
}

TEST(PageTypeInferrerTest, QuickPrefilterBadPageNo) {
    auto page = create_valid_index_page();
    write_be<uint32_t>(page.data(), 4, 0xFFFFFFF1);  // > 0xFFFFFFF0
    EXPECT_FALSE(PageTypeInferrer::quick_prefilter(page.data(), page.size()));
}

TEST(PageTypeInferrerTest, QuickPrefilterZeroLsn) {
    auto page = create_valid_index_page();
    write_be<uint64_t>(page.data(), 16, 0);
    EXPECT_FALSE(PageTypeInferrer::quick_prefilter(page.data(), page.size()));
}

TEST(PageTypeInferrerTest, QuickPrefilterAllFFLsn) {
    auto page = create_valid_index_page();
    write_be<uint64_t>(page.data(), 16, 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_FALSE(PageTypeInferrer::quick_prefilter(page.data(), page.size()));
}

TEST(PageTypeInferrerTest, QuickPrefilterAllFFTail) {
    auto page = create_valid_index_page();
    write_le<uint32_t>(page.data(), page.size() - 4, 0xFFFFFFFF);
    EXPECT_FALSE(PageTypeInferrer::quick_prefilter(page.data(), page.size()));
}

TEST(PageTypeInferrerTest, QuickPrefilterNullData) {
    EXPECT_FALSE(PageTypeInferrer::quick_prefilter(nullptr, 16384));
}

TEST(PageTypeInferrerTest, QuickPrefilterSmallBuffer) {
    auto page = create_valid_index_page();
    EXPECT_FALSE(PageTypeInferrer::quick_prefilter(page.data(), 20));
}

// ==================== read_candidate_header Tests ====================

TEST(PageTypeInferrerTest, ReadCandidateHeader) {
    auto page = create_valid_index_page(16384, 5, 3, 987654, 99);
    auto cand = PageTypeInferrer::read_candidate_header(page.data(), 0, 16384);

    EXPECT_EQ(cand.file_offset, 0u);
    EXPECT_EQ(cand.detected_size, 16384u);
    EXPECT_EQ(cand.page_no, 5u);
    EXPECT_EQ(cand.space_id, 3u);
    EXPECT_EQ(cand.lsn, 987654u);
    EXPECT_EQ(cand.page_type, FIL_PAGE_INDEX);
    EXPECT_EQ(cand.index_id, 99u);
}

TEST(PageTypeInferrerTest, ReadCandidateHeaderNullData) {
    auto cand = PageTypeInferrer::read_candidate_header(nullptr, 100, 16384);
    EXPECT_EQ(cand.file_offset, 100u);
    EXPECT_EQ(cand.detected_size, 16384u);
    EXPECT_EQ(cand.page_no, 0u);
    EXPECT_EQ(cand.space_id, 0u);
}

// ==================== Level 0: Exact Boundary Tests ====================

TEST(PageTypeInferrerTest, Level0ScanExactBoundary) {
    auto buffer = create_multi_page_buffer(3, 16384);
    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.enable_level1 = false;
    config.enable_level2 = false;
    config.enable_level3 = false;
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto candidates = inferrer.scan_level(buffer.data(), buffer.size(), ScanLevel::LEVEL_0_EXACT_BOUNDARY);
    EXPECT_EQ(candidates.size(), 3u);
    EXPECT_EQ(candidates[0].page_no, 0u);
    EXPECT_EQ(candidates[1].page_no, 1u);
    EXPECT_EQ(candidates[2].page_no, 2u);
}

TEST(PageTypeInferrerTest, Level0ScanMultiplePageSizes) {
    auto buffer = create_multi_page_buffer(2, 8192);
    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.page_sizes = {8192, 4096};
    config.enable_level1 = false;
    config.enable_level2 = false;
    config.enable_level3 = false;
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto candidates = inferrer.scan_level(buffer.data(), buffer.size(), ScanLevel::LEVEL_0_EXACT_BOUNDARY);
    EXPECT_EQ(candidates.size(), 2u);
}

TEST(PageTypeInferrerTest, Level0ScanNoValidPages) {
    std::vector<uint8_t> buffer(32768, 0xFF);  // all 0xFF, no valid pages
    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.enable_level1 = false;
    config.enable_level2 = false;
    config.enable_level3 = false;
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto candidates = inferrer.scan_level(buffer.data(), buffer.size(), ScanLevel::LEVEL_0_EXACT_BOUNDARY);
    EXPECT_TRUE(candidates.empty());
}

TEST(PageTypeInferrerTest, Level0ScanSmallBuffer) {
    std::vector<uint8_t> buffer(30, 0);
    PageTypeInferrer inferrer;
    auto candidates = inferrer.scan_level(buffer.data(), buffer.size(), ScanLevel::LEVEL_0_EXACT_BOUNDARY);
    EXPECT_TRUE(candidates.empty());
}

TEST(PageTypeInferrerTest, Level0ScanNullData) {
    PageTypeInferrer inferrer;
    auto candidates = inferrer.scan_level(nullptr, 0, ScanLevel::LEVEL_0_EXACT_BOUNDARY);
    EXPECT_TRUE(candidates.empty());
}

// ==================== Level 1: Fuzzy Boundary Tests ====================

TEST(PageTypeInferrerTest, Level1ScanFuzzyBoundary) {
    // Create a buffer with one valid page at offset 0 and one at offset 512
    auto page = create_valid_index_page(16384, 0, 1, 12345678, 42);
    std::vector<uint8_t> buffer(32768, 0);
    std::memcpy(buffer.data(), page.data(), 16384);

    // Put another valid page at offset 512 with CHECKSUM_NONE_MAGIC at page_size-4
    auto page2 = create_valid_index_page(16384, 1, 1, 23456789, 43);
    // Ensure the algo ID is set at page_size-4
    write_le<uint32_t>(page2.data(), 16384 - 4, CHECKSUM_NONE_MAGIC);
    std::memcpy(buffer.data() + 512, page2.data(), 16384);

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.enable_level0 = false;
    config.enable_level2 = false;
    config.enable_level3 = false;
    config.verify_checksum = true;  // Level 1 uses algo ID check
    config.level1_step = 512;
    inferrer.set_config(config);

    auto candidates = inferrer.scan_level(buffer.data(), buffer.size(), ScanLevel::LEVEL_1_FUZZY_BOUNDARY);
    EXPECT_GE(candidates.size(), 1u);
}

TEST(PageTypeInferrerTest, Level1ScanNoAlgoId) {
    auto page = create_minimal_header_page(16384, 0, 1, 12345678);
    // No valid algo ID at page_size - 4
    write_le<uint32_t>(page.data(), 16384 - 4, 0xDEADBEEF);

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.enable_level0 = false;
    config.enable_level2 = false;
    config.enable_level3 = false;
    config.verify_checksum = true;
    config.level1_step = 512;
    inferrer.set_config(config);

    auto candidates = inferrer.scan_level(page.data(), page.size(), ScanLevel::LEVEL_1_FUZZY_BOUNDARY);
    EXPECT_TRUE(candidates.empty());
}

// ==================== Level 2: Content-Based Tests ====================

TEST(PageTypeInferrerTest, Level2ScanContentBased) {
    auto page = create_valid_index_page(16384, 0, 1, 12345678, 42);
    // Ensure INFIMUM and SUPREMUM are set
    page[58] = 0x20;  // INFIMUM status
    write_be<uint16_t>(page.data(), 59, 67);  // next_record -> SUPREMUM
    page[71] = 0x30;  // SUPREMUM status

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.enable_level0 = false;
    config.enable_level1 = false;
    config.enable_level3 = false;
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto candidates = inferrer.scan_level(page.data(), page.size(), ScanLevel::LEVEL_2_CONTENT_BASED);
    // Level 2 scans byte-by-byte, so it should find the page at offset 0
    EXPECT_GE(candidates.size(), 1u);
}

TEST(PageTypeInferrerTest, Level2ScanNoInfimum) {
    auto page = create_minimal_header_page(16384, 0, 1, 12345678, FIL_PAGE_INDEX);
    // No INFIMUM/SUPREMUM set

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.enable_level0 = false;
    config.enable_level1 = false;
    config.enable_level3 = false;
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto candidates = inferrer.scan_level(page.data(), page.size(), ScanLevel::LEVEL_2_CONTENT_BASED);
    // Should not find the page because INFIMUM/SUPREMUM check fails for INDEX pages
    EXPECT_TRUE(candidates.empty());
}

// ==================== Level 3: Data Pattern Tests ====================

TEST(PageTypeInferrerTest, Level3ScanUuidPattern) {
    // Create a buffer with UUID and timestamp patterns but no valid FIL header
    auto buf = create_uuid_pattern_buffer();
    // Set a valid page type at offset 0 to pass the prefilter
    write_be<uint16_t>(buf.data(), 24, FIL_PAGE_INDEX);
    write_be<uint32_t>(buf.data(), 4, 0);  // page_no
    write_be<uint32_t>(buf.data(), 34, 1); // space_id
    write_be<uint64_t>(buf.data(), 16, 12345678);  // LSN
    write_le<uint32_t>(buf.data(), 16384 - 4, 0);  // tail

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.enable_level0 = false;
    config.enable_level1 = false;
    config.enable_level2 = false;
    config.verify_checksum = false;
    config.promote_threshold = 0.3;
    inferrer.set_config(config);

    auto candidates = inferrer.scan_level(buf.data(), buf.size(), ScanLevel::LEVEL_3_DATA_PATTERN);
    // Should find at least one candidate with UUID+timestamp patterns
    EXPECT_GE(candidates.size(), 1u);
}

TEST(PageTypeInferrerTest, Level3ScanNoPatterns) {
    std::vector<uint8_t> buf(16384, 0);
    // Set a valid FIL header
    write_be<uint16_t>(buf.data(), 24, FIL_PAGE_INDEX);
    write_be<uint32_t>(buf.data(), 4, 0);
    write_be<uint32_t>(buf.data(), 34, 1);
    write_be<uint64_t>(buf.data(), 16, 12345678);
    write_le<uint32_t>(buf.data(), 16384 - 4, 0);

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.enable_level0 = false;
    config.enable_level1 = false;
    config.enable_level2 = false;
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto candidates = inferrer.scan_level(buf.data(), buf.size(), ScanLevel::LEVEL_3_DATA_PATTERN);
    EXPECT_TRUE(candidates.empty());
}

// ==================== Full Scan Pipeline Tests ====================

TEST(PageTypeInferrerTest, ScanFullPipeline) {
    auto buffer = create_multi_page_buffer(3, 16384);
    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto candidates = inferrer.scan(buffer.data(), buffer.size());
    EXPECT_GE(candidates.size(), 3u);
}

TEST(PageTypeInferrerTest, ScanEmptyBuffer) {
    PageTypeInferrer inferrer;
    auto candidates = inferrer.scan(nullptr, 0);
    EXPECT_TRUE(candidates.empty());
}

TEST(PageTypeInferrerTest, ScanFile) {
    // Create a temp file with valid pages
    auto buffer = create_multi_page_buffer(2, 16384);
    std::string path = "/tmp/test_page_inferrer_scan.bin";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    }

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto candidates = inferrer.scan_file(path);
    EXPECT_GE(candidates.size(), 2u);

    std::remove(path.c_str());
}

TEST(PageTypeInferrerTest, ScanFileNonexistent) {
    PageTypeInferrer inferrer;
    auto candidates = inferrer.scan_file("/nonexistent/file.ibd");
    EXPECT_TRUE(candidates.empty());
}

// ==================== validate_page Tests ====================

TEST(PageTypeInferrerTest, ValidatePageAllChecksPass) {
    auto page = create_valid_index_page();
    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto flags = inferrer.validate_page(page.data(), page.size(), 0, nullptr);
    EXPECT_EQ(flags.passed_checks(), 10u);
    EXPECT_DOUBLE_EQ(flags.confidence(), 1.0);
    EXPECT_TRUE(flags.checksum_valid);
    EXPECT_TRUE(flags.space_id_consistent);
    EXPECT_TRUE(flags.page_no_consistent);
    EXPECT_TRUE(flags.type_content_match);
    EXPECT_TRUE(flags.lsn_continuous);
    EXPECT_TRUE(flags.index_id_consistent);
    EXPECT_TRUE(flags.has_infimum);
    EXPECT_TRUE(flags.has_supremum);
    EXPECT_TRUE(flags.offset_table_ok);
    EXPECT_TRUE(flags.chain_links_ok);
}

TEST(PageTypeInferrerTest, ValidatePageNullData) {
    PageTypeInferrer inferrer;
    auto flags = inferrer.validate_page(nullptr, 0, 0, nullptr);
    EXPECT_EQ(flags.passed_checks(), 0u);
}

TEST(PageTypeInferrerTest, ValidatePageSmallBuffer) {
    std::vector<uint8_t> buf(20, 0);
    PageTypeInferrer inferrer;
    auto flags = inferrer.validate_page(buf.data(), 20, 0, nullptr);
    EXPECT_EQ(flags.passed_checks(), 0u);
}

TEST(PageTypeInferrerTest, ValidatePageBadSpaceId) {
    auto page = create_valid_index_page();
    write_be<uint32_t>(page.data(), 34, 0x1000000);  // > 0xFFFFFF

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto flags = inferrer.validate_page(page.data(), page.size(), 0, nullptr);
    EXPECT_FALSE(flags.space_id_consistent);
}

TEST(PageTypeInferrerTest, ValidatePageBadLsn) {
    auto page = create_valid_index_page();
    write_be<uint64_t>(page.data(), 16, 0);

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto flags = inferrer.validate_page(page.data(), page.size(), 0, nullptr);
    EXPECT_FALSE(flags.lsn_continuous);
}

TEST(PageTypeInferrerTest, ValidatePageNoInfimum) {
    auto page = create_valid_index_page();
    page[58] = 0x00;  // Clear INFIMUM status

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto flags = inferrer.validate_page(page.data(), page.size(), 0, nullptr);
    EXPECT_FALSE(flags.has_infimum);
    EXPECT_FALSE(flags.type_content_match);
}

TEST(PageTypeInferrerTest, ValidatePageNoSupremum) {
    auto page = create_valid_index_page();
    page[71] = 0x00;  // Clear SUPREMUM status

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto flags = inferrer.validate_page(page.data(), page.size(), 0, nullptr);
    EXPECT_FALSE(flags.has_supremum);
    EXPECT_FALSE(flags.type_content_match);
}

TEST(PageTypeInferrerTest, ValidatePageChainLinksManyCandidates) {
    auto page = create_valid_index_page();
    // Set prev_page to a known candidate
    write_be<uint32_t>(page.data(), 8, 5);  // prev_page = 5

    std::vector<PageCandidate> existing;
    PageCandidate c1;
    c1.page_no = 5;
    existing.push_back(c1);
    // Add 10+ candidates to trigger chain check
    for (uint32_t i = 0; i < 15; i++) {
        PageCandidate c;
        c.page_no = i;
        existing.push_back(c);
    }

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto flags = inferrer.validate_page(page.data(), page.size(), 0, &existing);
    // prev_page 5 is found in existing candidates
    EXPECT_TRUE(flags.chain_links_ok);
}

// ==================== Deduplicate Tests ====================

TEST(PageTypeInferrerTest, DeduplicateOverlapping) {
    PageCandidate a, b;
    a.file_offset = 0;
    a.detected_size = 16384;
    a.confidence = 0.8;
    b.file_offset = 100;
    b.detected_size = 16384;
    b.confidence = 0.9;

    PageTypeInferrer inferrer;
    auto result = inferrer.deduplicate({a, b});
    EXPECT_EQ(result.size(), 1u);
    EXPECT_DOUBLE_EQ(result[0].confidence, 0.9);  // higher confidence kept
}

TEST(PageTypeInferrerTest, DeduplicateNonOverlapping) {
    PageCandidate a, b;
    a.file_offset = 0;
    a.detected_size = 16384;
    b.file_offset = 16384;
    b.detected_size = 16384;

    PageTypeInferrer inferrer;
    auto result = inferrer.deduplicate({a, b});
    EXPECT_EQ(result.size(), 2u);
}

TEST(PageTypeInferrerTest, DeduplicateSameOffset) {
    PageCandidate a, b;
    a.file_offset = 0;
    a.detected_size = 16384;
    a.confidence = 0.7;
    b.file_offset = 0;
    b.detected_size = 16384;
    b.confidence = 0.9;

    PageTypeInferrer inferrer;
    auto result = inferrer.deduplicate({a, b});
    EXPECT_EQ(result.size(), 1u);
    EXPECT_DOUBLE_EQ(result[0].confidence, 0.9);
}

TEST(PageTypeInferrerTest, DeduplicateEmpty) {
    PageTypeInferrer inferrer;
    auto result = inferrer.deduplicate({});
    EXPECT_TRUE(result.empty());
}

// ==================== filter_by_confidence Tests ====================

TEST(PageTypeInferrerTest, FilterByConfidence) {
    PageCandidate a, b, c;
    a.confidence = 0.9;
    b.confidence = 0.5;
    c.confidence = 0.7;

    PageTypeInferrer inferrer;
    auto result = inferrer.filter_by_confidence({a, b, c}, 0.6);
    EXPECT_EQ(result.size(), 2u);
    EXPECT_DOUBLE_EQ(result[0].confidence, 0.9);
    EXPECT_DOUBLE_EQ(result[1].confidence, 0.7);
}

TEST(PageTypeInferrerTest, FilterByConfidenceAllBelow) {
    PageCandidate a, b;
    a.confidence = 0.2;
    b.confidence = 0.4;

    PageTypeInferrer inferrer;
    auto result = inferrer.filter_by_confidence({a, b}, 0.6);
    EXPECT_TRUE(result.empty());
}

TEST(PageTypeInferrerTest, FilterByConfidenceEmpty) {
    PageTypeInferrer inferrer;
    auto result = inferrer.filter_by_confidence({}, 0.6);
    EXPECT_TRUE(result.empty());
}

// ==================== sort_by_offset Tests ====================

TEST(PageTypeInferrerTest, SortByOffset) {
    PageCandidate a, b, c;
    a.file_offset = 32768;
    b.file_offset = 0;
    c.file_offset = 16384;

    auto result = PageTypeInferrer::sort_by_offset({a, b, c});
    EXPECT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0].file_offset, 0u);
    EXPECT_EQ(result[1].file_offset, 16384u);
    EXPECT_EQ(result[2].file_offset, 32768u);
}

TEST(PageTypeInferrerTest, SortByOffsetEmpty) {
    auto result = PageTypeInferrer::sort_by_offset({});
    EXPECT_TRUE(result.empty());
}

// ==================== group_by_type Tests ====================

TEST(PageTypeInferrerTest, GroupByType) {
    PageCandidate a, b, c;
    a.page_type = FIL_PAGE_INDEX;
    b.page_type = FIL_PAGE_UNDO_LOG;
    c.page_type = FIL_PAGE_INDEX;

    auto groups = PageTypeInferrer::group_by_type({a, b, c});
    EXPECT_EQ(groups.size(), 2u);
    EXPECT_EQ(groups[FIL_PAGE_INDEX].size(), 2u);
    EXPECT_EQ(groups[FIL_PAGE_UNDO_LOG].size(), 1u);
}

TEST(PageTypeInferrerTest, GroupByTypeEmpty) {
    auto groups = PageTypeInferrer::group_by_type({});
    EXPECT_TRUE(groups.empty());
}

// ==================== detect_page_size Tests ====================

TEST(PageTypeInferrerTest, DetectPageSize) {
    auto page = create_valid_index_page(16384);
    auto result = PageTypeInferrer::detect_page_size(page.data(), page.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 16384u);
}

TEST(PageTypeInferrerTest, DetectPageSizeInvalid) {
    std::vector<uint8_t> buf(16384, 0xFF);  // all 0xFF, no valid page
    auto result = PageTypeInferrer::detect_page_size(buf.data(), buf.size());
    EXPECT_FALSE(result.has_value());
}

TEST(PageTypeInferrerTest, DetectPageSizeSmall) {
    std::vector<uint8_t> buf(100, 0);
    auto result = PageTypeInferrer::detect_page_size(buf.data(), buf.size());
    EXPECT_FALSE(result.has_value());
}

TEST(PageTypeInferrerTest, DetectPageSizeNull) {
    auto result = PageTypeInferrer::detect_page_size(nullptr, 0);
    EXPECT_FALSE(result.has_value());
}

// ==================== infer_page_sizes_from_checksum Tests ====================

TEST(PageTypeInferrerTest, InferPageSizesFromChecksum) {
    auto page = create_valid_index_page(16384);
    // Set CHECKSUM_NONE_MAGIC at page_size - 4
    write_le<uint32_t>(page.data(), 16384 - 4, CHECKSUM_NONE_MAGIC);

    auto sizes = PageTypeInferrer::infer_page_sizes_from_checksum(page.data(), page.size());
    EXPECT_GE(sizes.size(), 1u);
    bool found = false;
    for (auto s : sizes) {
        if (s == 16384) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(PageTypeInferrerTest, InferPageSizesFromChecksumNoMatch) {
    std::vector<uint8_t> buf(16384, 0);
    write_le<uint32_t>(buf.data(), 16384 - 4, 0xDEADBEEF);  // invalid algo ID

    auto sizes = PageTypeInferrer::infer_page_sizes_from_checksum(buf.data(), buf.size());
    EXPECT_TRUE(sizes.empty());
}

// ==================== Stats Tests ====================

TEST(PageTypeInferrerTest, StatsInitial) {
    PageTypeInferrer inferrer;
    const auto& stats = inferrer.stats();
    EXPECT_EQ(stats.total_bytes_scanned, 0u);
    EXPECT_EQ(stats.level0_candidates, 0u);
    EXPECT_EQ(stats.level1_candidates, 0u);
    EXPECT_EQ(stats.level2_candidates, 0u);
    EXPECT_EQ(stats.level3_candidates, 0u);
    EXPECT_EQ(stats.accepted_pages, 0u);
    EXPECT_EQ(stats.rejected_pages, 0u);
    EXPECT_EQ(stats.scan_time_ms, 0.0);
}

TEST(PageTypeInferrerTest, StatsAfterScan) {
    auto buffer = create_multi_page_buffer(2, 16384);
    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.verify_checksum = false;
    inferrer.set_config(config);

    inferrer.scan(buffer.data(), buffer.size());

    const auto& stats = inferrer.stats();
    EXPECT_GT(stats.total_bytes_scanned, 0u);
    EXPECT_GT(stats.level0_candidates, 0u);
    EXPECT_GT(stats.accepted_pages, 0u);
    EXPECT_GT(stats.scan_time_ms, 0.0);
}

TEST(PageTypeInferrerTest, StatsReset) {
    auto buffer = create_multi_page_buffer(2, 16384);
    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.verify_checksum = false;
    inferrer.set_config(config);

    inferrer.scan(buffer.data(), buffer.size());
    EXPECT_GT(inferrer.stats().total_bytes_scanned, 0u);

    inferrer.reset_stats();
    EXPECT_EQ(inferrer.stats().total_bytes_scanned, 0u);
    EXPECT_EQ(inferrer.stats().scan_time_ms, 0.0);
}

// ==================== Configuration Tests ====================

TEST(PageTypeInferrerTest, DefaultConfig) {
    PageTypeInferrer inferrer;
    const auto& cfg = inferrer.config();
    EXPECT_TRUE(cfg.enable_level0);
    EXPECT_TRUE(cfg.enable_level1);
    EXPECT_TRUE(cfg.enable_level2);
    EXPECT_TRUE(cfg.enable_level3);
    EXPECT_DOUBLE_EQ(cfg.promote_threshold, 0.3);
    EXPECT_DOUBLE_EQ(cfg.accept_threshold, 0.6);
    EXPECT_DOUBLE_EQ(cfg.reject_threshold, 0.1);
    EXPECT_EQ(cfg.level1_step, 512u);
    EXPECT_TRUE(cfg.verify_checksum);
}

TEST(PageTypeInferrerTest, SetConfig) {
    PageInferrerConfig cfg;
    cfg.enable_level0 = false;
    cfg.enable_level1 = false;
    cfg.enable_level2 = false;
    cfg.enable_level3 = false;
    cfg.verify_checksum = false;
    cfg.accept_threshold = 0.8;

    PageTypeInferrer inferrer(cfg);
    const auto& retrieved = inferrer.config();
    EXPECT_FALSE(retrieved.enable_level0);
    EXPECT_FALSE(retrieved.verify_checksum);
    EXPECT_DOUBLE_EQ(retrieved.accept_threshold, 0.8);
}

TEST(PageTypeInferrerTest, MutableConfig) {
    PageTypeInferrer inferrer;
    inferrer.mutable_config().accept_threshold = 0.95;
    EXPECT_DOUBLE_EQ(inferrer.config().accept_threshold, 0.95);
}

// ==================== Edge Cases ====================

TEST(PageTypeInferrerTest, AllLevelsDisabled) {
    auto buffer = create_multi_page_buffer(2, 16384);
    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.enable_level0 = false;
    config.enable_level1 = false;
    config.enable_level2 = false;
    config.enable_level3 = false;
    inferrer.set_config(config);

    auto candidates = inferrer.scan(buffer.data(), buffer.size());
    EXPECT_TRUE(candidates.empty());
}

TEST(PageTypeInferrerTest, ScanLevelUnknown) {
    PageTypeInferrer inferrer;
    auto candidates = inferrer.scan_level(nullptr, 0, static_cast<ScanLevel>(99));
    EXPECT_TRUE(candidates.empty());
}

TEST(PageTypeInferrerTest, ConfidenceField) {
    PageValidationFlags flags;
    EXPECT_EQ(flags.passed_checks(), 0u);
    EXPECT_EQ(flags.total_checks(), 10u);
    EXPECT_DOUBLE_EQ(flags.confidence(), 0.0);

    flags.checksum_valid = true;
    flags.space_id_consistent = true;
    flags.page_no_consistent = true;
    flags.type_content_match = true;
    flags.lsn_continuous = true;
    EXPECT_EQ(flags.passed_checks(), 5u);
    EXPECT_DOUBLE_EQ(flags.confidence(), 0.5);
}

TEST(PageTypeInferrerTest, PageCandidateToString) {
    PageCandidate cand;
    cand.file_offset = 16384;
    cand.page_no = 1;
    cand.space_id = 0;
    cand.page_type = FIL_PAGE_INDEX;
    cand.page_level = 0;
    cand.lsn = 12345678;
    cand.detected_size = 16384;
    cand.confidence = 0.95;
    cand.detected_at = ScanLevel::LEVEL_0_EXACT_BOUNDARY;

    std::string s = cand.to_string();
    EXPECT_NE(s.find("offset=16384"), std::string::npos);
    EXPECT_NE(s.find("page_no=1"), std::string::npos);
    EXPECT_NE(s.find("confidence=95.00%"), std::string::npos);
}

TEST(PageTypeInferrerTest, ScanLevelToString) {
    EXPECT_EQ(scan_level_to_string(ScanLevel::LEVEL_0_EXACT_BOUNDARY), "LEVEL_0_EXACT_BOUNDARY");
    EXPECT_EQ(scan_level_to_string(ScanLevel::LEVEL_1_FUZZY_BOUNDARY), "LEVEL_1_FUZZY_BOUNDARY");
    EXPECT_EQ(scan_level_to_string(ScanLevel::LEVEL_2_CONTENT_BASED), "LEVEL_2_CONTENT_BASED");
    EXPECT_EQ(scan_level_to_string(ScanLevel::LEVEL_3_DATA_PATTERN), "LEVEL_3_DATA_PATTERN");
    EXPECT_EQ(scan_level_to_string(static_cast<ScanLevel>(99)), "UNKNOWN");
}

TEST(PageTypeInferrerTest, StatsToString) {
    PageInferrerStats stats;
    stats.total_bytes_scanned = 49152;
    stats.level0_candidates = 3;
    stats.accepted_pages = 3;
    std::string s = stats.to_string();
    EXPECT_NE(s.find("scanned=49152"), std::string::npos);
    EXPECT_NE(s.find("L0=3"), std::string::npos);
}

TEST(PageTypeInferrerTest, ValidatePageNonIndexType) {
    // UNDO_LOG page should pass non-INDEX checks trivially
    auto page = create_minimal_header_page(16384, 0, 1, 12345678, FIL_PAGE_UNDO_LOG);
    // Set non-zero undo marker at offset 38
    write_be<uint32_t>(page.data(), 38, 0x01020304);

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto flags = inferrer.validate_page(page.data(), page.size(), 0, nullptr);
    // Non-INDEX checks (infimum, supremum, offset_table, index_id) are N/A → true
    EXPECT_TRUE(flags.has_infimum);
    EXPECT_TRUE(flags.has_supremum);
    EXPECT_TRUE(flags.offset_table_ok);
    EXPECT_TRUE(flags.index_id_consistent);
    // type_content_match should pass for UNDO_LOG with non-zero marker
    EXPECT_TRUE(flags.type_content_match);
    // Basic checks
    EXPECT_TRUE(flags.space_id_consistent);
    EXPECT_TRUE(flags.page_no_consistent);
    EXPECT_TRUE(flags.lsn_continuous);
}

TEST(PageTypeInferrerTest, ValidatePageOffsetTableBad) {
    auto page = create_valid_index_page();
    // Set n_dir_slots = 0 (invalid)
    write_be<uint16_t>(page.data(), 38, 0);

    PageTypeInferrer inferrer;
    auto config = inferrer.config();
    config.verify_checksum = false;
    inferrer.set_config(config);

    auto flags = inferrer.validate_page(page.data(), page.size(), 0, nullptr);
    EXPECT_FALSE(flags.offset_table_ok);
}

TEST(PageTypeInferrerTest, FilterByConfidenceEmptyInput) {
    PageTypeInferrer inferrer;
    auto result = inferrer.filter_by_confidence({}, 0.5);
    EXPECT_TRUE(result.empty());
}

TEST(PageTypeInferrerTest, HasValidInfimum) {
    auto page = create_valid_index_page();
    EXPECT_TRUE(PageTypeInferrer::has_valid_infimum(page.data(), page.size()));
}

TEST(PageTypeInferrerTest, HasValidInfimumNull) {
    EXPECT_FALSE(PageTypeInferrer::has_valid_infimum(nullptr, 0));
}

TEST(PageTypeInferrerTest, HasValidSupremum) {
    auto page = create_valid_index_page();
    EXPECT_TRUE(PageTypeInferrer::has_valid_supremum(page.data(), page.size()));
}

TEST(PageTypeInferrerTest, HasValidSupremumNull) {
    EXPECT_FALSE(PageTypeInferrer::has_valid_supremum(nullptr, 0));
}

TEST(PageTypeInferrerTest, ExtractIndexId) {
    auto page = create_valid_index_page(16384, 0, 1, 12345678, 99);
    EXPECT_EQ(PageTypeInferrer::extract_index_id(page.data(), page.size()), 99u);
}

TEST(PageTypeInferrerTest, ExtractIndexIdZero) {
    std::vector<uint8_t> buf(50, 0);
    EXPECT_EQ(PageTypeInferrer::extract_index_id(buf.data(), buf.size()), 0u);
}

TEST(PageTypeInferrerTest, ExtractIndexIdNull) {
    EXPECT_EQ(PageTypeInferrer::extract_index_id(nullptr, 0), 0u);
}

TEST(PageTypeInferrerTest, HasUuidPattern) {
    std::vector<uint8_t> buf(100, 0);
    const char* uuid = "550e8400-e29b-41d4-a716-446655440000";
    std::memcpy(buf.data(), uuid, 36);
    EXPECT_TRUE(PageTypeInferrer::has_uuid_pattern(buf.data(), buf.size()));
}

TEST(PageTypeInferrerTest, HasUuidPatternNoMatch) {
    std::vector<uint8_t> buf(100, 0);
    EXPECT_FALSE(PageTypeInferrer::has_uuid_pattern(buf.data(), buf.size()));
}

TEST(PageTypeInferrerTest, HasTimestampPattern) {
    std::vector<uint8_t> buf(100, 0);
    write_le<uint32_t>(buf.data(), 0, 1700000000);
    write_le<uint32_t>(buf.data(), 8, 1700000001);
    write_le<uint32_t>(buf.data(), 16, 1700000002);
    write_le<uint32_t>(buf.data(), 24, 1700000003);
    EXPECT_TRUE(PageTypeInferrer::has_timestamp_pattern(buf.data(), buf.size()));
}

TEST(PageTypeInferrerTest, HasTimestampPatternNoMatch) {
    std::vector<uint8_t> buf(100, 0);
    // Values outside valid timestamp range
    write_le<uint32_t>(buf.data(), 0, 100);
    write_le<uint32_t>(buf.data(), 8, 200);
    EXPECT_FALSE(PageTypeInferrer::has_timestamp_pattern(buf.data(), buf.size()));
}

TEST(PageTypeInferrerTest, HasAutoincPattern) {
    std::vector<uint8_t> buf(200, 0);
    // Three consecutive 8-byte values with small gaps
    write_le<uint64_t>(buf.data(), 0, 100);
    write_le<uint64_t>(buf.data(), 16, 101);
    write_le<uint64_t>(buf.data(), 32, 102);
    EXPECT_TRUE(PageTypeInferrer::has_autoinc_pattern(buf.data(), buf.size()));
}

TEST(PageTypeInferrerTest, HasAutoincPatternNoMatch) {
    std::vector<uint8_t> buf(200, 0);
    // Values far apart
    write_le<uint64_t>(buf.data(), 0, 100);
    write_le<uint64_t>(buf.data(), 16, 99999);
    EXPECT_FALSE(PageTypeInferrer::has_autoinc_pattern(buf.data(), buf.size()));
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}