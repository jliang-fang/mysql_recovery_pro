#include <gtest/gtest.h>
#include "dbrecovery/recovery/raw_disk_carver.hpp"
#include "dbrecovery/safety/safety_gate.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <cstdint>
#include <vector>
#include <cstring>
#include <string>
#include <array>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace dbrecovery {
namespace recovery {

using namespace dbrecovery::innodb;

// ==================== Helpers ====================

// Create a 16KB page buffer with specified page type at offset 24 (big-endian)
static std::vector<uint8_t> create_page(uint16_t page_type, size_t page_size = 16384) {
    std::vector<uint8_t> page(page_size, 0);
    write_be<uint16_t>(page.data(), 24, page_type);
    return page;
}

// Create a page with only footer LSN written (for detect_page_size tests)
// This ensures the smaller page size checks (4096, 8192) fail because
// the footer LSN at those offsets is zero, while the correct size passes.
static std::vector<uint8_t> create_detectable_index_page(size_t page_size = 16384) {
    std::vector<uint8_t> page(page_size, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);       // page type
    write_be<uint64_t>(page.data(), 66, 100);                   // index_id != 0
    write_be<uint64_t>(page.data(), page_size - 8, 0xABCD000000001234ULL); // footer LSN only
    return page;
}
// Create a valid INDEX page for is_valid_page: needs index_id != 0, non-zero LSN
// NOTE: Writes BOTH header and footer LSN. For detect_page_size tests, use
// create_detectable_index_page() instead (footer-only) to avoid false 4096 match.
static std::vector<uint8_t> create_valid_index_page(size_t page_size = 16384) {
    std::vector<uint8_t> page(page_size, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);       // page type
    write_be<uint64_t>(page.data(), 66, 100);                   // index_id != 0
    write_be<uint64_t>(page.data(), 0, 0xABCD000000001234ULL);  // non-zero LSN (header)
    write_be<uint64_t>(page.data(), page_size - 8, 0xABCD000000001234ULL); // matching footer LSN
    return page;
}

// Create a page with strict LSN matching (for is_valid_page_strict)
static std::vector<uint8_t> create_page_strict_valid(uint16_t page_type, uint32_t checksum, uint32_t lsn_low, size_t page_size = 16384) {
    std::vector<uint8_t> page(page_size, 0);
    write_be<uint16_t>(page.data(), 24, page_type);             // page type
    write_be<uint32_t>(page.data(), 0, checksum);               // checksum header
    write_be<uint32_t>(page.data(), 4, lsn_low);                // LSN low header
    write_be<uint32_t>(page.data(), page_size - 8, checksum);   // checksum footer
    write_be<uint32_t>(page.data(), page_size - 4, lsn_low);    // LSN low footer
    if (page_type == FIL_PAGE_INDEX) {
        write_be<uint64_t>(page.data(), 66, 100);               // index_id != 0
    }
    return page;
}

TEST(RawDiskCarverTest, SafetyGateRequiresAllMandatoryFacts) {
    dbrecovery::safety::SafetyReport report;
    report.device_identity_verified = true;
    report.read_only_confirmed = true;
    report.exclusive_access = true;
    report.not_mounted_or_ok = true;
    EXPECT_TRUE(dbrecovery::safety::SafetyGate::permits_read_only_raw_access(report));

    const std::array<bool, 4> mandatory = {true, true, true, true};
    for (size_t i = 0; i < mandatory.size(); ++i) {
        auto candidate = report;
        if (i == 0) candidate.device_identity_verified = false;
        if (i == 1) candidate.read_only_confirmed = false;
        if (i == 2) candidate.exclusive_access = false;
        if (i == 3) candidate.not_mounted_or_ok = false;
        EXPECT_FALSE(dbrecovery::safety::SafetyGate::permits_read_only_raw_access(candidate));
    }
}

TEST(RawDiskCarverTest, SafetyGateDiagnosticsDoNotAuthorize) {
    dbrecovery::safety::SafetyReport report;
    report.confidence = 1.0;
    report.loopback_verified = true;
    EXPECT_FALSE(dbrecovery::safety::SafetyGate::permits_read_only_raw_access(report));
}

TEST(RawDiskCarverTest, AdmitSourceAcceptsRegularFileAndDirectory) {
    const auto root = std::filesystem::temp_directory_path() / "raw_disk_carver_admission_test";
    std::filesystem::create_directories(root);
    const auto file = root / "image.bin";
    std::ofstream(file, std::ios::binary).put('\0');

    EXPECT_TRUE(RawDiskCarver::admit_source(file.string()));
    EXPECT_TRUE(RawDiskCarver::admit_source(root.string()));

    std::filesystem::remove_all(root);
}

TEST(RawDiskCarverTest, AdmitSourceRejectsMissingAndUnsupportedSources) {
    std::string reason;
    EXPECT_FALSE(RawDiskCarver::admit_source(
        (std::filesystem::temp_directory_path() / "does-not-exist-raw-source").string(), &reason));
    EXPECT_FALSE(reason.empty());

#ifndef _WIN32
    const auto fifo = std::filesystem::temp_directory_path() / "raw_disk_carver_admission_fifo";
    std::filesystem::remove(fifo);
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    EXPECT_FALSE(RawDiskCarver::admit_source(fifo.string(), &reason));
    std::filesystem::remove(fifo);
#endif
}

// ==================== Config Tests ====================

TEST(RawDiskCarverTest, ConfigDefaultValues) {
    RawDiskCarver::Config config;
    EXPECT_EQ(config.page_size, 16384u);
    EXPECT_EQ(config.chunk_size, 256u * 1024u * 1024u);
    EXPECT_EQ(config.num_workers, 8u);
    EXPECT_EQ(config.offset_start, 0u);
    EXPECT_EQ(config.offset_stop, UINT64_MAX);
    EXPECT_EQ(config.scan_step, 4096u);
    EXPECT_EQ(config.fine_scan_step, 512u);
    EXPECT_TRUE(config.scan_all_index);
    EXPECT_TRUE(config.recover_deleted);
    EXPECT_FALSE(config.verbose);
    EXPECT_TRUE(config.auto_detect_page_size);
    EXPECT_TRUE(config.scan_frm_files);
    EXPECT_TRUE(config.scan_sdi_pages);
    EXPECT_TRUE(config.use_page_aligned_scan);
    EXPECT_TRUE(config.scan_data_patterns);
    EXPECT_EQ(config.progress_interval_ms, 1000u);
    EXPECT_FALSE(config.use_page_type_inferrer);
    EXPECT_TRUE(config.target_index_ids.empty());
    EXPECT_TRUE(config.exclude_index_ids.empty());
    EXPECT_TRUE(config.data_patterns.empty());
}

TEST(RawDiskCarverTest, ConfigCustomValues) {
    RawDiskCarver::Config config;
    config.page_size = 8192;
    config.chunk_size = 128 * 1024 * 1024;
    config.num_workers = 16;
    config.offset_start = 4096;
    config.offset_stop = 1024ULL * 1024 * 1024 * 1024;
    config.scan_step = 8192;
    config.fine_scan_step = 256;
    config.scan_all_index = false;
    config.recover_deleted = false;
    config.verbose = true;
    config.auto_detect_page_size = false;
    config.scan_frm_files = false;
    config.scan_sdi_pages = false;
    config.use_page_aligned_scan = false;
    config.scan_data_patterns = false;
    config.progress_interval_ms = 500;
    config.use_page_type_inferrer = true;
    config.target_index_ids = {1, 2, 3};
    config.exclude_index_ids = {999};
    config.data_patterns = {"pattern1", "pattern2"};

    EXPECT_EQ(config.page_size, 8192u);
    EXPECT_EQ(config.chunk_size, 128u * 1024u * 1024u);
    EXPECT_EQ(config.num_workers, 16u);
    EXPECT_EQ(config.offset_start, 4096u);
    EXPECT_EQ(config.offset_stop, 1024ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(config.scan_step, 8192u);
    EXPECT_EQ(config.fine_scan_step, 256u);
    EXPECT_FALSE(config.scan_all_index);
    EXPECT_FALSE(config.recover_deleted);
    EXPECT_TRUE(config.verbose);
    EXPECT_FALSE(config.auto_detect_page_size);
    EXPECT_FALSE(config.scan_frm_files);
    EXPECT_FALSE(config.scan_sdi_pages);
    EXPECT_FALSE(config.use_page_aligned_scan);
    EXPECT_FALSE(config.scan_data_patterns);
    EXPECT_EQ(config.progress_interval_ms, 500u);
    EXPECT_TRUE(config.use_page_type_inferrer);
    EXPECT_EQ(config.target_index_ids.size(), 3u);
    EXPECT_EQ(config.exclude_index_ids.size(), 1u);
    EXPECT_EQ(config.data_patterns.size(), 2u);
}

// ==================== PageType Enum Tests ====================

TEST(RawDiskCarverTest, PageTypeEnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(RawDiskCarver::PageType::UNKNOWN), 0u);
    EXPECT_EQ(static_cast<uint8_t>(RawDiskCarver::PageType::INDEX), 1u);
    EXPECT_EQ(static_cast<uint8_t>(RawDiskCarver::PageType::SDI), 2u);
    EXPECT_EQ(static_cast<uint8_t>(RawDiskCarver::PageType::SDI_ZIP), 3u);
    EXPECT_EQ(static_cast<uint8_t>(RawDiskCarver::PageType::UNDO_LOG), 4u);
    EXPECT_EQ(static_cast<uint8_t>(RawDiskCarver::PageType::BLOB), 5u);
    EXPECT_EQ(static_cast<uint8_t>(RawDiskCarver::PageType::FRM), 6u);
    EXPECT_EQ(static_cast<uint8_t>(RawDiskCarver::PageType::ALLOCATED), 7u);
}

// ==================== FoundPage Struct Tests ====================

TEST(RawDiskCarverTest, FoundPageDefaultConstruction) {
    RawDiskCarver::FoundPage page{};
    EXPECT_EQ(page.type, RawDiskCarver::PageType::UNKNOWN);
    EXPECT_EQ(page.offset, 0u);
    EXPECT_EQ(page.index_id, 0u);
    EXPECT_EQ(page.space_id, 0u);
    EXPECT_EQ(page.page_no, 0u);
    EXPECT_TRUE(page.data.empty());
}

TEST(RawDiskCarverTest, FoundPageFieldAssignment) {
    RawDiskCarver::FoundPage page;
    page.type = RawDiskCarver::PageType::INDEX;
    page.offset = 4096;
    page.index_id = 42;
    page.space_id = 0;
    page.page_no = 3;
    page.data = {0x01, 0x02, 0x03};

    EXPECT_EQ(page.type, RawDiskCarver::PageType::INDEX);
    EXPECT_EQ(page.offset, 4096u);
    EXPECT_EQ(page.index_id, 42u);
    EXPECT_EQ(page.space_id, 0u);
    EXPECT_EQ(page.page_no, 3u);
    ASSERT_EQ(page.data.size(), 3u);
    EXPECT_EQ(page.data[0], 0x01);
}

// ==================== ScanResult Struct Tests ====================

TEST(RawDiskCarverTest, ScanResultDefaultConstruction) {
    RawDiskCarver::ScanResult result;
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.total_pages_scanned, 0u);
    EXPECT_EQ(result.valid_pages_found, 0u);
    EXPECT_EQ(result.index_pages_found, 0u);
    EXPECT_EQ(result.sdi_pages_found, 0u);
    EXPECT_EQ(result.blob_pages_found, 0u);
    EXPECT_EQ(result.frm_files_found, 0u);
    EXPECT_EQ(result.elapsed_seconds, 0.0);
    EXPECT_TRUE(result.pages_by_index_id.empty());
    EXPECT_TRUE(result.all_found_pages.empty());
    EXPECT_TRUE(result.sdi_pages.empty());
    EXPECT_TRUE(result.frm_files.empty());
    EXPECT_TRUE(result.errors.empty());
}

TEST(RawDiskCarverTest, ScanResultFieldAssignment) {
    RawDiskCarver::ScanResult result;
    result.success = true;
    result.total_pages_scanned = 1000;
    result.valid_pages_found = 500;
    result.index_pages_found = 400;
    result.sdi_pages_found = 50;
    result.blob_pages_found = 30;
    result.frm_files_found = 2;
    result.elapsed_seconds = 1.5;

    result.pages_by_index_id[42] = {{1, 2, 3}};
    result.all_found_pages.push_back({RawDiskCarver::PageType::INDEX, 4096, 42, 0, 3, {}});
    result.sdi_pages[0] = {0x00};
    result.frm_files.push_back("frm_data");
    result.errors.push_back("some error");

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_pages_scanned, 1000u);
    EXPECT_EQ(result.valid_pages_found, 500u);
    EXPECT_EQ(result.index_pages_found, 400u);
    EXPECT_EQ(result.sdi_pages_found, 50u);
    EXPECT_EQ(result.blob_pages_found, 30u);
    EXPECT_EQ(result.frm_files_found, 2u);
    EXPECT_EQ(result.elapsed_seconds, 1.5);
    EXPECT_EQ(result.pages_by_index_id.size(), 1u);
    EXPECT_EQ(result.all_found_pages.size(), 1u);
    EXPECT_EQ(result.sdi_pages.size(), 1u);
    EXPECT_EQ(result.frm_files.size(), 1u);
    EXPECT_EQ(result.errors.size(), 1u);
}

// ==================== RecoveryResult Struct Tests ====================

TEST(RawDiskCarverTest, RecoveryResultDefaultConstruction) {
    RawDiskCarver::RecoveryResult result;
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.total_records, 0u);
    EXPECT_TRUE(result.records_by_index_id.empty());
    EXPECT_TRUE(result.inferred_schemas.empty());
    EXPECT_TRUE(result.output_files.empty());
    EXPECT_TRUE(result.errors.empty());
}

TEST(RawDiskCarverTest, RecoveryResultFieldAssignment) {
    RawDiskCarver::RecoveryResult result;
    result.success = true;
    result.total_records = 100;
    result.records_by_index_id[42] = {{{"col1", "val1"}}};
    result.output_files.push_back("/tmp/output.csv");
    result.errors.push_back("warning");

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_records, 100u);
    EXPECT_EQ(result.records_by_index_id.size(), 1u);
    EXPECT_EQ(result.output_files.size(), 1u);
    EXPECT_EQ(result.errors.size(), 1u);
}

// ==================== is_valid_page Tests ====================

TEST(RawDiskCarverTest, IsValidPageNullData) {
    EXPECT_FALSE(RawDiskCarver::is_valid_page(nullptr, 16384));
}

TEST(RawDiskCarverTest, IsValidPageSmallPageSize) {
    std::vector<uint8_t> small(16, 0);
    EXPECT_FALSE(RawDiskCarver::is_valid_page(small.data(), 16));
}

TEST(RawDiskCarverTest, IsValidPageValidIndex) {
    auto page = create_valid_index_page();
    EXPECT_TRUE(RawDiskCarver::is_valid_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageIndexZeroIndexId) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);
    write_be<uint64_t>(page.data(), 0, 0xABCD000000001234ULL);  // non-zero LSN
    write_be<uint64_t>(page.data(), 66, 0);                      // index_id = 0
    EXPECT_FALSE(RawDiskCarver::is_valid_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageIndexZeroLSN) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);
    write_be<uint64_t>(page.data(), 66, 100);  // index_id != 0
    // LSN is all zeros -> should fail
    EXPECT_FALSE(RawDiskCarver::is_valid_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageAllocated) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_TYPE_ALLOCATED);
    // Allocated always passes without LSN check
    EXPECT_TRUE(RawDiskCarver::is_valid_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageUndoLog) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_UNDO_LOG);
    write_be<uint64_t>(page.data(), 0, 0xABCD000000001234ULL);  // non-zero LSN
    EXPECT_TRUE(RawDiskCarver::is_valid_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageSDI) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_SDI);
    write_be<uint64_t>(page.data(), 0, 0xABCD000000001234ULL);  // non-zero LSN
    EXPECT_TRUE(RawDiskCarver::is_valid_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageSDIZip) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_SDI_ZIP);
    write_be<uint64_t>(page.data(), 0, 0xABCD000000001234ULL);  // non-zero LSN
    EXPECT_TRUE(RawDiskCarver::is_valid_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageInvalidType) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, 0xFFFF);  // invalid type
    write_be<uint64_t>(page.data(), 0, 0xABCD000000001234ULL);
    EXPECT_FALSE(RawDiskCarver::is_valid_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageLSNMismatchWithinTolerance) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);
    write_be<uint64_t>(page.data(), 66, 100);                    // index_id != 0
    write_be<uint64_t>(page.data(), 0, 0xABCD000000001234ULL);   // header LSN
    write_be<uint64_t>(page.data(), page.size() - 8, 0xABCD000000001234ULL); // footer LSN
    // LSN low halves match, diff=0 within tolerance
    EXPECT_TRUE(RawDiskCarver::is_valid_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageLSNLargeDiff) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);
    write_be<uint64_t>(page.data(), 66, 100);                    // index_id != 0
    // header LSN low = 0x00000001, footer LSN low = 0x00010000
    write_be<uint64_t>(page.data(), 0, 0x0000000000000001ULL);   // header LSN
    write_be<uint64_t>(page.data(), page.size() - 8, 0x0000000000010000ULL); // footer LSN
    // diff = 0xFFFF = 65535 > 10000, but still returns true (non-zero LSN values)
    EXPECT_TRUE(RawDiskCarver::is_valid_page(page.data(), page.size()));
}

// ==================== is_valid_page_strict Tests ====================

TEST(RawDiskCarverTest, IsValidPageStrictNullData) {
    EXPECT_FALSE(RawDiskCarver::is_valid_page_strict(nullptr, 16384));
}

TEST(RawDiskCarverTest, IsValidPageStrictSmallPageSize) {
    std::vector<uint8_t> small(16, 0);
    EXPECT_FALSE(RawDiskCarver::is_valid_page_strict(small.data(), 16));
}

TEST(RawDiskCarverTest, IsValidPageStrictValidIndex) {
    auto page = create_page_strict_valid(FIL_PAGE_INDEX, 0x12345678, 0x9ABCDEF0);
    EXPECT_TRUE(RawDiskCarver::is_valid_page_strict(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageStrictMismatchedLSNNonZero) {
    // checksum_header != checksum_footer but both non-zero -> relaxed check passes
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);
    write_be<uint64_t>(page.data(), 66, 100);
    write_be<uint32_t>(page.data(), 0, 0xAAAAAAAA);              // checksum header
    write_be<uint32_t>(page.data(), 4, 0x11111111);              // LSN low header
    write_be<uint32_t>(page.data(), page.size() - 8, 0xBBBBBBBB); // checksum footer (different)
    write_be<uint32_t>(page.data(), page.size() - 4, 0x22222222); // LSN low footer (different)
    // checksum_header != 0, so relaxed check passes
    EXPECT_TRUE(RawDiskCarver::is_valid_page_strict(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageStrictAllocated) {
    // ALLOCATED page type skips LSN check
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_TYPE_ALLOCATED);
    // All zeros is fine for ALLOCATED
    EXPECT_TRUE(RawDiskCarver::is_valid_page_strict(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageStrictInvalidType) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, 0xFFFF);  // invalid type
    EXPECT_FALSE(RawDiskCarver::is_valid_page_strict(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageStrictZeroChecksum) {
    // Both checksums zero -> strict match passes (0 == 0 is true)
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);
    write_be<uint64_t>(page.data(), 66, 100);
    // All LSN fields are 0 -> strict check: 0==0 and 0==0 passes
    EXPECT_TRUE(RawDiskCarver::is_valid_page_strict(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageStrictUndoLog) {
    auto page = create_page_strict_valid(FIL_PAGE_UNDO_LOG, 0x12345678, 0x9ABCDEF0);
    EXPECT_TRUE(RawDiskCarver::is_valid_page_strict(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageStrictSDI) {
    auto page = create_page_strict_valid(FIL_PAGE_SDI, 0x12345678, 0x9ABCDEF0);
    EXPECT_TRUE(RawDiskCarver::is_valid_page_strict(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageStrictSDIZip) {
    auto page = create_page_strict_valid(FIL_PAGE_SDI_ZIP, 0x12345678, 0x9ABCDEF0);
    EXPECT_TRUE(RawDiskCarver::is_valid_page_strict(page.data(), page.size()));
}

// ==================== is_index_page Tests ====================

TEST(RawDiskCarverTest, IsIndexPageNullData) {
    EXPECT_FALSE(RawDiskCarver::is_index_page(nullptr, 16384));
}

TEST(RawDiskCarverTest, IsIndexPageSmallBuffer) {
    std::vector<uint8_t> small(16, 0);
    EXPECT_FALSE(RawDiskCarver::is_index_page(small.data(), 16));
}

TEST(RawDiskCarverTest, IsIndexPageTrue) {
    auto page = create_page(FIL_PAGE_INDEX);
    EXPECT_TRUE(RawDiskCarver::is_index_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsIndexPageFalseForSDI) {
    auto page = create_page(FIL_PAGE_SDI);
    EXPECT_FALSE(RawDiskCarver::is_index_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsIndexPageFalseForUndoLog) {
    auto page = create_page(FIL_PAGE_UNDO_LOG);
    EXPECT_FALSE(RawDiskCarver::is_index_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsIndexPageFalseForAllocated) {
    auto page = create_page(FIL_PAGE_TYPE_ALLOCATED);
    EXPECT_FALSE(RawDiskCarver::is_index_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsIndexPageFalseForSDIZip) {
    auto page = create_page(FIL_PAGE_SDI_ZIP);
    EXPECT_FALSE(RawDiskCarver::is_index_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsIndexPageFalseForCompressed) {
    auto page = create_page(FIL_PAGE_COMPRESSED);
    EXPECT_FALSE(RawDiskCarver::is_index_page(page.data(), page.size()));
}

// ==================== is_sdi_page Tests ====================

TEST(RawDiskCarverTest, IsSdiPageNullData) {
    EXPECT_FALSE(RawDiskCarver::is_sdi_page(nullptr, 16384, 0));
}

TEST(RawDiskCarverTest, IsSdiPageSmallBuffer) {
    std::vector<uint8_t> small(16, 0);
    EXPECT_FALSE(RawDiskCarver::is_sdi_page(small.data(), 16, 0));
}

TEST(RawDiskCarverTest, IsSdiPageSDI) {
    auto page = create_page(FIL_PAGE_SDI);
    // Uncompressed SDI is always SDI regardless of page_no
    EXPECT_TRUE(RawDiskCarver::is_sdi_page(page.data(), page.size(), 0));
    EXPECT_TRUE(RawDiskCarver::is_sdi_page(page.data(), page.size(), 3));
    EXPECT_TRUE(RawDiskCarver::is_sdi_page(page.data(), page.size(), 99));
}

TEST(RawDiskCarverTest, IsSdiPageUnsignedSDIZipIsNotSdi) {
    auto page = create_page(FIL_PAGE_SDI_ZIP);
    EXPECT_FALSE(RawDiskCarver::is_sdi_page(page.data(), page.size(), 3));
    EXPECT_FALSE(RawDiskCarver::is_sdi_page(page.data(), page.size(), 4));
}

TEST(RawDiskCarverTest, IsSdiPageSDIZipNonStandardPosition) {
    auto page = create_page(FIL_PAGE_SDI_ZIP);
    // SDI_ZIP at non-standard position (0) without SDI signature is NOT SDI
    EXPECT_FALSE(RawDiskCarver::is_sdi_page(page.data(), page.size(), 0));
    EXPECT_FALSE(RawDiskCarver::is_sdi_page(page.data(), page.size(), 5));
    // SDI signature at offset 64-71 causes it to be recognized as SDI
    constexpr uint8_t SDI_SIG[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};
    std::memcpy(page.data() + 64, SDI_SIG, 8);
    EXPECT_TRUE(RawDiskCarver::is_sdi_page(page.data(), page.size(), 99));
}

TEST(RawDiskCarverTest, IsSdiPageFalseForIndex) {
    auto page = create_page(FIL_PAGE_INDEX);
    EXPECT_FALSE(RawDiskCarver::is_sdi_page(page.data(), page.size(), 0));
    EXPECT_FALSE(RawDiskCarver::is_sdi_page(page.data(), page.size(), 3));
}

TEST(RawDiskCarverTest, IsSdiPageFalseForUndoLog) {
    auto page = create_page(FIL_PAGE_UNDO_LOG);
    EXPECT_FALSE(RawDiskCarver::is_sdi_page(page.data(), page.size(), 0));
}

TEST(RawDiskCarverTest, IsSdiPageFalseForAllocated) {
    auto page = create_page(FIL_PAGE_TYPE_ALLOCATED);
    EXPECT_FALSE(RawDiskCarver::is_sdi_page(page.data(), page.size(), 0));
}

// ==================== is_blob_page Tests ====================

TEST(RawDiskCarverTest, IsBlobPageNullData) {
    EXPECT_FALSE(RawDiskCarver::is_blob_page(nullptr, 16384, 0));
}

TEST(RawDiskCarverTest, IsBlobPageSmallBuffer) {
    std::vector<uint8_t> small(16, 0);
    EXPECT_FALSE(RawDiskCarver::is_blob_page(small.data(), 16, 0));
}

TEST(RawDiskCarverTest, IsBlobPageCompressed) {
    auto page = create_page(FIL_PAGE_COMPRESSED);
    EXPECT_TRUE(RawDiskCarver::is_blob_page(page.data(), page.size(), 0));
    EXPECT_TRUE(RawDiskCarver::is_blob_page(page.data(), page.size(), 3));
}

TEST(RawDiskCarverTest, IsBlobPageSDIZipNonStandard) {
    // SDI_ZIP at non-standard position (0, 5) without SDI signature is BLOB
    auto page = create_page(FIL_PAGE_SDI_ZIP);
    EXPECT_TRUE(RawDiskCarver::is_blob_page(page.data(), page.size(), 0));
    EXPECT_TRUE(RawDiskCarver::is_blob_page(page.data(), page.size(), 5));
}

TEST(RawDiskCarverTest, IsBlobPageUnsignedSDIZipAtAnyPosition) {
    auto page = create_page(FIL_PAGE_SDI_ZIP);
    EXPECT_TRUE(RawDiskCarver::is_blob_page(page.data(), page.size(), 3));
    EXPECT_TRUE(RawDiskCarver::is_blob_page(page.data(), page.size(), 4));
}

TEST(RawDiskCarverTest, IsBlobPageSDIZipWithSignature) {
    // SDI_ZIP with SDI signature at offset 64-71 is NOT a BLOB
    auto page = create_page(FIL_PAGE_SDI_ZIP);
    constexpr uint8_t SDI_SIG[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};
    std::memcpy(page.data() + 64, SDI_SIG, 8);
    EXPECT_FALSE(RawDiskCarver::is_blob_page(page.data(), page.size(), 99));
}

TEST(RawDiskCarverTest, IsBlobPageFalseForIndex) {
    auto page = create_page(FIL_PAGE_INDEX);
    EXPECT_FALSE(RawDiskCarver::is_blob_page(page.data(), page.size(), 0));
}

TEST(RawDiskCarverTest, IsBlobPageFalseForSDI) {
    auto page = create_page(FIL_PAGE_SDI);
    EXPECT_FALSE(RawDiskCarver::is_blob_page(page.data(), page.size(), 0));
}

TEST(RawDiskCarverTest, IsBlobPageFalseForUndoLog) {
    auto page = create_page(FIL_PAGE_UNDO_LOG);
    EXPECT_FALSE(RawDiskCarver::is_blob_page(page.data(), page.size(), 0));
}

// ==================== is_frm_signature Tests ====================

TEST(RawDiskCarverTest, IsFrmSignatureNullData) {
    EXPECT_FALSE(RawDiskCarver::is_frm_signature(nullptr, 10));
}

TEST(RawDiskCarverTest, IsFrmSignatureTooShort) {
    uint8_t data[] = {0xfe, 0x01, 0x0a};
    EXPECT_FALSE(RawDiskCarver::is_frm_signature(data, 3));
}

TEST(RawDiskCarverTest, IsFrmSignatureValid) {
    uint8_t data[] = {0xfe, 0x01, 0x0a, 0x0c, 0x03, 0x00, 0x00, 0x10, 0x01, 0x00};
    EXPECT_TRUE(RawDiskCarver::is_frm_signature(data, 10));
}

TEST(RawDiskCarverTest, IsFrmSignatureWrongBytes) {
    uint8_t data[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    EXPECT_FALSE(RawDiskCarver::is_frm_signature(data, 10));
}

TEST(RawDiskCarverTest, IsFrmSignatureEmptyData) {
    EXPECT_FALSE(RawDiskCarver::is_frm_signature(nullptr, 0));
}

// ==================== extract_index_id Tests ====================

TEST(RawDiskCarverTest, ExtractIndexIdNullData) {
    EXPECT_EQ(RawDiskCarver::extract_index_id(nullptr, 16384), 0u);
}

TEST(RawDiskCarverTest, ExtractIndexIdSmallPageSize) {
    std::vector<uint8_t> small(70, 0);
    EXPECT_EQ(RawDiskCarver::extract_index_id(small.data(), 70), 0u);
}

TEST(RawDiskCarverTest, ExtractIndexIdValid) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint64_t>(page.data(), 66, 0xDEADBEEFCAFEULL);
    EXPECT_EQ(RawDiskCarver::extract_index_id(page.data(), page.size()), 0xDEADBEEFCAFEULL);
}

TEST(RawDiskCarverTest, ExtractIndexIdZero) {
    std::vector<uint8_t> page(16384, 0);
    EXPECT_EQ(RawDiskCarver::extract_index_id(page.data(), page.size()), 0u);
}

TEST(RawDiskCarverTest, ExtractIndexIdMaxValue) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint64_t>(page.data(), 66, 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_EQ(RawDiskCarver::extract_index_id(page.data(), page.size()), 0xFFFFFFFFFFFFFFFFULL);
}

// ==================== extract_space_id Tests ====================

TEST(RawDiskCarverTest, ExtractSpaceIdNullData) {
    EXPECT_EQ(RawDiskCarver::extract_space_id(nullptr, 16384), 0u);
}

TEST(RawDiskCarverTest, ExtractSpaceIdSmallPageSize) {
    std::vector<uint8_t> small(36, 0);
    EXPECT_EQ(RawDiskCarver::extract_space_id(small.data(), 36), 0u);
}

TEST(RawDiskCarverTest, ExtractSpaceIdValid) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint32_t>(page.data(), 34, 0x12345678);
    EXPECT_EQ(RawDiskCarver::extract_space_id(page.data(), page.size()), 0x12345678u);
}

TEST(RawDiskCarverTest, ExtractSpaceIdZero) {
    std::vector<uint8_t> page(16384, 0);
    EXPECT_EQ(RawDiskCarver::extract_space_id(page.data(), page.size()), 0u);
}

// ==================== extract_page_no Tests ====================

TEST(RawDiskCarverTest, ExtractPageNoNullData) {
    EXPECT_EQ(RawDiskCarver::extract_page_no(nullptr, 16384), 0u);
}

TEST(RawDiskCarverTest, ExtractPageNoSmallPageSize) {
    std::vector<uint8_t> small(6, 0);
    EXPECT_EQ(RawDiskCarver::extract_page_no(small.data(), 6), 0u);
}

TEST(RawDiskCarverTest, ExtractPageNoValid) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint32_t>(page.data(), 4, 42);
    EXPECT_EQ(RawDiskCarver::extract_page_no(page.data(), page.size()), 42u);
}

TEST(RawDiskCarverTest, ExtractPageNoZero) {
    std::vector<uint8_t> page(16384, 0);
    EXPECT_EQ(RawDiskCarver::extract_page_no(page.data(), page.size()), 0u);
}

TEST(RawDiskCarverTest, ExtractPageNoMaxValue) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint32_t>(page.data(), 4, 0xFFFFFFFF);
    EXPECT_EQ(RawDiskCarver::extract_page_no(page.data(), page.size()), 0xFFFFFFFFu);
}

// ==================== detect_page_size Tests ====================

TEST(RawDiskCarverTest, DetectPageSizeNullData) {
    EXPECT_EQ(RawDiskCarver::detect_page_size(nullptr, 0), 0u);
}

TEST(RawDiskCarverTest, DetectPageSizeTooSmall) {
    std::vector<uint8_t> small(100, 0);
    EXPECT_EQ(RawDiskCarver::detect_page_size(small.data(), small.size()), 0u);
}

TEST(RawDiskCarverTest, DetectPageSize16K) {
    auto page = create_detectable_index_page(16384);
    uint32_t detected = RawDiskCarver::detect_page_size(page.data(), page.size());
    EXPECT_EQ(detected, 16384u);
}

TEST(RawDiskCarverTest, DetectPageSize8K) {
    auto page = create_detectable_index_page(8192);
    uint32_t detected = RawDiskCarver::detect_page_size(page.data(), page.size());
    EXPECT_EQ(detected, 8192u);
}

TEST(RawDiskCarverTest, DetectPageSize4K) {
    auto page = create_detectable_index_page(4096);
    uint32_t detected = RawDiskCarver::detect_page_size(page.data(), page.size());
    EXPECT_EQ(detected, 4096u);
}

TEST(RawDiskCarverTest, DetectPageSize32K) {
    auto page = create_detectable_index_page(32768);
    uint32_t detected = RawDiskCarver::detect_page_size(page.data(), page.size());
    EXPECT_EQ(detected, 32768u);
}

TEST(RawDiskCarverTest, DetectPageSizeInvalidData) {
    // All zeros -> page type 0x0000 = FIL_PAGE_TYPE_ALLOCATED, which passes
    // validation at the smallest size (4096). The implementation returns the
    // first valid size rather than 0.
    std::vector<uint8_t> data(65536, 0);
    EXPECT_EQ(RawDiskCarver::detect_page_size(data.data(), data.size()), 4096u);
}

// ==================== SetConfig and SetExternalSchema Tests ====================

TEST(RawDiskCarverTest, SetConfig) {
    RawDiskCarver carver;
    RawDiskCarver::Config config;
    config.page_size = 8192;
    config.num_workers = 4;
    config.verbose = true;

    carver.set_config(config);

    // Verify get_last_scan_result is initially empty
    const auto& result = carver.get_last_scan_result();
    EXPECT_FALSE(result.success);
}

TEST(RawDiskCarverTest, SetExternalSchema) {
    RawDiskCarver carver;
    catalog::TableSchema schema;
    schema.table_name = "test_table";
    schema.table_id = 100;

    carver.set_external_schema(schema);

    // No direct getter for external schema, just verify no crash
    SUCCEED();
}

// ==================== ReassemblyStats Type Alias ====================

TEST(RawDiskCarverTest, ReassemblyStatsAlias) {
    // Verify the type alias exists and can be used
    ReassemblyStats stats;
    EXPECT_EQ(stats.total_pages_found, 0u);
    EXPECT_EQ(stats.total_pages_used, 0u);
    EXPECT_EQ(stats.total_gaps, 0u);
    EXPECT_EQ(stats.critical_gaps, 0u);
    EXPECT_EQ(stats.spaces_reconstructed, 0u);
    EXPECT_EQ(stats.total_bytes_output, 0u);
    EXPECT_DOUBLE_EQ(stats.completeness_pct, 0.0);
    EXPECT_DOUBLE_EQ(stats.time_seconds, 0.0);
}

// ==================== Edge Cases ====================

TEST(RawDiskCarverTest, PageTypeInferrerConfigInSetConfig) {
    RawDiskCarver carver;
    RawDiskCarver::Config config;
    config.use_page_type_inferrer = true;
    config.page_inferrer_config.verify_checksum = true;
    config.page_inferrer_config.promote_threshold = 0.5;
    config.page_inferrer_config.accept_threshold = 0.8;

    carver.set_config(config);
    SUCCEED();
}

TEST(RawDiskCarverTest, IsValidPageInodeType) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, 0x0003);  // FIL_PAGE_INODE
    write_be<uint64_t>(page.data(), 0, 0xABCD000000001234ULL);  // non-zero LSN
    EXPECT_TRUE(RawDiskCarver::is_valid_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageIbufBitmapType) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, 0x0005);  // FIL_PAGE_IBUF_BITMAP
    write_be<uint64_t>(page.data(), 0, 0xABCD000000001234ULL);  // non-zero LSN
    EXPECT_TRUE(RawDiskCarver::is_valid_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageFspHeaderType) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, 0x0008);  // FIL_PAGE_FSP_HEADER
    write_be<uint64_t>(page.data(), 0, 0xABCD000000001234ULL);  // non-zero LSN
    EXPECT_TRUE(RawDiskCarver::is_valid_page(page.data(), page.size()));
}

TEST(RawDiskCarverTest, IsValidPageStrictInodeType) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, 0x0003);  // FIL_PAGE_INODE
    write_be<uint32_t>(page.data(), 0, 0x12345678);              // checksum
    write_be<uint32_t>(page.data(), 4, 0x9ABCDEF0);              // LSN low
    write_be<uint32_t>(page.data(), page.size() - 8, 0x12345678); // matching checksum
    write_be<uint32_t>(page.data(), page.size() - 4, 0x9ABCDEF0); // matching LSN low
    EXPECT_TRUE(RawDiskCarver::is_valid_page_strict(page.data(), page.size()));
}

// ==================== P1: SDI schema authority is fail-closed ====================

// A valid SDI ZIP page whose canonical record offsets are corrupted, but which
// contains a valid deflate stream at a non-canonical offset, carries only
// forensic heuristic evidence. extract_schema_from_sdi must not turn that into
// an authoritative TableSchema.
TEST(RawDiskCarverTest, SdiZipHeuristicEvidenceYieldsNoSchema) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_SDI_ZIP);
    constexpr uint8_t SDI_SIG[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};
    std::memcpy(page.data() + 64, SDI_SIG, 8);

    // Corrupt all canonical record offsets used by the strict ZIP adapter.
    for (uint32_t off : {127u, 403u, 679u}) {
        write_be<uint32_t>(page.data(), off + 25, 0xFFFFFFFF);
        write_be<uint32_t>(page.data(), off + 29, 0);
    }

    // Valid raw-deflate JSON stream at a non-canonical offset: the kind of
    // evidence only a heuristic scan could recover.
    const std::string json = R"({"table":"corrupted"})";
    z_stream stream{};
    ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                           Z_DEFAULT_STRATEGY), Z_OK);
    std::vector<uint8_t> compressed(deflateBound(&stream, json.size()));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(json.data()));
    stream.avail_in = static_cast<uInt>(json.size());
    stream.next_out = compressed.data();
    stream.avail_out = static_cast<uInt>(compressed.size());
    ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
    compressed.resize(stream.total_out);
    deflateEnd(&stream);
    std::copy(compressed.begin(), compressed.end(), page.end() - 8 - compressed.size());

    RawDiskCarver carver;
    auto schema = carver.extract_schema_from_sdi(page);
    EXPECT_FALSE(schema.has_value());
}

} // namespace recovery
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}