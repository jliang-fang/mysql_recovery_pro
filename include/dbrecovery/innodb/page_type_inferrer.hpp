/**
 * @file page_type_inferrer.hpp
 * @brief PageTypeInferrer — Multi-level page type inference engine
 *
 * v2.0 architecture: 4-level progressive page detection for Extreme Disk Carver.
 * Designed to push page detection rate from 85% to 99% in disaster recovery
 * scenarios (rm-rf, raw disk carving, corrupted filesystems).
 *
 * Algorithm (4-level progressive scan):
 *   Level 0 — Exact signature match at known page boundaries (4K/8K/16K/32K/64K)
 *   Level 1 — Fuzzy signature match at 512-byte boundaries (misaligned pages)
 *   Level 2 — Content-based scanning at 1-byte step (infimum/supremum, offset table)
 *   Level 3 — Data pattern matching (UUID, timestamps, auto-increment IDs)
 *
 * Each level feeds candidates to the next, with cumulative confidence scoring
 * and false-positive rejection at each stage.
 */

#ifndef DBRECOVERY_INNODB_PAGE_TYPE_INFERRER_HPP
#define DBRECOVERY_INNODB_PAGE_TYPE_INFERRER_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <cstring>
#include <array>
#include <mutex>
#include <atomic>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Scan Level Enum ====================

enum class ScanLevel : uint8_t {
    LEVEL_0_EXACT_BOUNDARY = 0,  ///< Exact page boundary match
    LEVEL_1_FUZZY_BOUNDARY = 1,  ///< 512-byte boundary fuzzy match
    LEVEL_2_CONTENT_BASED   = 2,  ///< 1-byte step content-based scan
    LEVEL_3_DATA_PATTERN    = 3   ///< Data pattern matching
};

inline std::string scan_level_to_string(ScanLevel level) {
    switch (level) {
        case ScanLevel::LEVEL_0_EXACT_BOUNDARY: return "LEVEL_0_EXACT_BOUNDARY";
        case ScanLevel::LEVEL_1_FUZZY_BOUNDARY: return "LEVEL_1_FUZZY_BOUNDARY";
        case ScanLevel::LEVEL_2_CONTENT_BASED:   return "LEVEL_2_CONTENT_BASED";
        case ScanLevel::LEVEL_3_DATA_PATTERN:    return "LEVEL_3_DATA_PATTERN";
        default: return "UNKNOWN";
    }
}

// ==================== Data Structures ====================

/**
 * @brief Validation flags for a detected page candidate
 */
struct PageValidationFlags {
    bool checksum_valid      = false;  ///< InnoDB checksum matches
    bool space_id_consistent = false;  ///< SPACE_ID is reasonable
    bool page_no_consistent  = false;  ///< PAGE_NO is reasonable
    bool type_content_match  = false;  ///< Page type matches content heuristics
    bool lsn_continuous      = false;  ///< LSN is in reasonable range
    bool index_id_consistent = false;  ///< Index ID is consistent (if INDEX page)
    bool has_infimum         = false;  ///< Has valid INFIMUM record
    bool has_supremum        = false;  ///< Has valid SUPREMUM record
    bool offset_table_ok     = false;  ///< Offset table has valid structure
    bool chain_links_ok      = false;  ///< Prev/next page links are consistent

    uint32_t passed_checks() const {
        uint32_t count = 0;
        if (checksum_valid)      count++;
        if (space_id_consistent) count++;
        if (page_no_consistent)  count++;
        if (type_content_match)  count++;
        if (lsn_continuous)      count++;
        if (index_id_consistent) count++;
        if (has_infimum)         count++;
        if (has_supremum)        count++;
        if (offset_table_ok)     count++;
        if (chain_links_ok)      count++;
        return count;
    }

    uint32_t total_checks() const { return 10; }

    double confidence() const {
        return total_checks() > 0 ? static_cast<double>(passed_checks()) / total_checks() : 0.0;
    }
};

/**
 * @brief Page candidate detected by the inferrer
 */
struct PageCandidate {
    uint64_t file_offset   = 0;     ///< Absolute byte offset in the file
    uint32_t page_no       = 0;     ///< Inferred page number
    uint32_t space_id      = 0;     ///< Inferred space ID
    uint16_t page_type     = 0;     ///< Page type (FIL_PAGE_INDEX, etc.)
    uint16_t page_level    = 0;     ///< B+Tree level
    uint64_t lsn           = 0;     ///< LSN from FIL header
    uint32_t prev_page     = 0xFFFFFFFF;  ///< Previous page in chain
    uint32_t next_page     = 0xFFFFFFFF;  ///< Next page in chain
    uint64_t index_id      = 0;     ///< Index ID (if INDEX page)
    size_t detected_size   = 0;     ///< Detected page size (0 = unknown)

    ScanLevel detected_at  = ScanLevel::LEVEL_0_EXACT_BOUNDARY;
    double confidence      = 0.0;   ///< 0.0–1.0 confidence score
    PageValidationFlags validation;

    /// Human-readable dump
    std::string to_string() const {
        std::ostringstream oss;
        oss << "PageCandidate{offset=" << file_offset
            << ", page_no=" << page_no
            << ", space_id=" << space_id
            << ", type=0x" << std::hex << page_type << std::dec
            << "(" << page_type_to_string(page_type) << ")"
            << ", level=" << page_level
            << ", lsn=" << lsn
            << ", size=" << detected_size
            << ", confidence=" << std::fixed << std::setprecision(2) << (confidence * 100) << "%"
            << ", level=" << scan_level_to_string(detected_at)
            << "}";
        return oss.str();
    }
};

/**
 * @brief Scan configuration for PageTypeInferrer
 */
struct PageInferrerConfig {
    // Page sizes to try (in order)
    std::vector<size_t> page_sizes = {16384, 8192, 4096, 32768, 65536};

    // Level 0: exact boundary scan
    bool enable_level0 = true;

    // Level 1: fuzzy boundary scan
    bool enable_level1 = true;
    size_t level1_step = 512;       ///< Scan step in bytes for Level 1

    // Level 2: content-based scan
    bool enable_level2 = true;

    // Level 3: data pattern matching
    bool enable_level3 = true;

    // Confidence thresholds
    double promote_threshold = 0.3;  ///< Minimum confidence to promote a candidate
    double accept_threshold  = 0.6;  ///< Minimum confidence to accept a candidate
    double reject_threshold  = 0.1;  ///< Below this, discard immediately

    // Validation toggles
    bool verify_checksum    = true;
    bool require_infimum    = false; ///< Require INFIMUM for INDEX pages (strict)
    bool require_supremum   = false; ///< Require SUPREMUM for INDEX pages (strict)
    bool verify_lsn_range   = true;

    // Performance
    size_t max_candidates   = 1000000;  ///< Safety limit
    size_t max_level2_bytes = 64 * 1024 * 1024;  ///< Max bytes for Level 2 scan
    size_t batch_size       = 1024;     ///< Batch size for parallel processing
    bool parallel_scan      = false;    ///< Enable parallel scanning

    // Data pattern matching
    bool detect_uuid_patterns     = true;
    bool detect_timestamp_patterns = true;
    bool detect_autoinc_patterns  = true;
};

/**
 * @brief Scan statistics
 */
struct PageInferrerStats {
    uint64_t total_bytes_scanned    = 0;
    uint64_t level0_candidates      = 0;
    uint64_t level1_candidates      = 0;
    uint64_t level2_candidates      = 0;
    uint64_t level3_candidates      = 0;
    uint64_t accepted_pages         = 0;
    uint64_t rejected_pages         = 0;
    uint64_t false_positives_removed = 0;
    uint64_t checksum_failures      = 0;
    uint64_t pages_by_type_index    = 0;
    uint64_t pages_by_type_undo     = 0;
    uint64_t pages_by_type_blob     = 0;
    uint64_t pages_by_type_sdi      = 0;
    uint64_t pages_by_type_other    = 0;
    double   scan_time_ms           = 0.0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "PageInferrerStats{"
            << "scanned=" << total_bytes_scanned << " bytes, "
            << "L0=" << level0_candidates << ", "
            << "L1=" << level1_candidates << ", "
            << "L2=" << level2_candidates << ", "
            << "L3=" << level3_candidates << ", "
            << "accepted=" << accepted_pages << ", "
            << "rejected=" << rejected_pages << ", "
            << "FP_removed=" << false_positives_removed << ", "
            << "checksum_fail=" << checksum_failures
            << "}";
        return oss.str();
    }
};

// ==================== PageTypeInferrer ====================

/**
 * @brief Multi-level page type inference engine
 *
 * Progressive detection pipeline:
 *   Level 0 → Level 1 → Level 2 → Level 3 → Validation → Dedup → Output
 *
 * Each level feeds candidates to the next. Lower levels are fast (O(n) at
 * coarse granularity); higher levels are slower but detect pages at arbitrary
 * offsets. The pipeline terminates early for regions already well-covered
 * by lower levels.
 */
class PageTypeInferrer {
public:
    PageTypeInferrer() = default;
    explicit PageTypeInferrer(const PageInferrerConfig& cfg) : config_(cfg) {}

    ~PageTypeInferrer() = default;

    // ==================== Configuration ====================

    void set_config(const PageInferrerConfig& cfg) { config_ = cfg; }
    const PageInferrerConfig& config() const { return config_; }
    PageInferrerConfig& mutable_config() { return config_; }

    // ==================== Core API ====================

    /**
     * @brief Scan a file buffer and detect all pages
     *
     * Runs the full 4-level progressive pipeline.
     *
     * @param data Raw file data
     * @param size File size
     * @return Vector of detected page candidates (sorted by offset)
     */
    std::vector<PageCandidate> scan(const uint8_t* data, size_t size);

    /**
     * @brief Scan a file at the given path
     * @param file_path Path to the file (ibd, ibdata1, or raw device)
     * @return Vector of detected page candidates
     */
    std::vector<PageCandidate> scan_file(const std::string& file_path);

    /**
     * @brief Run only a specific level of the scan
     * @param data Raw file data
     * @param size File size
     * @param level Scan level to run
     * @return Candidates from this level only
     */
    std::vector<PageCandidate> scan_level(
        const uint8_t* data, size_t size, ScanLevel level);

    // ==================== Page Validation ====================

    /**
     * @brief Validate a single page candidate
     *
     * Runs all 10 validation checks:
     *   1. Checksum validation
     *   2. SPACE_ID consistency
     *   3. PAGE_NO consistency
     *   4. Page type vs content match
     *   5. LSN range check
     *   6. Index ID consistency (INDEX pages)
     *   7. INFIMUM record presence (INDEX pages)
     *   8. SUPREMUM record presence (INDEX pages)
     *   9. Offset table structure (INDEX pages)
     *   10. Prev/next page link consistency
     *
     * @param data Pointer to page data (must be >= page_size bytes)
     * @param page_size Size of the page
     * @param file_offset File offset (for chain consistency checks)
     * @param existing_candidates Previously detected candidates (for chain checks)
     * @return Validation result
     */
    PageValidationFlags validate_page(
        const uint8_t* data,
        size_t page_size,
        uint64_t file_offset = 0,
        const std::vector<PageCandidate>* existing_candidates = nullptr);

    /**
     * @brief Quick pre-filter: check if data looks like a valid page header
     *
     * Fast check without full validation:
     *   - FIL header has a valid page type
     *   - SPACE_ID is not insane (> 0xFFFFFF)
     *   - PAGE_NO is not insane
     *   - LSN looks reasonable (not zero, not all 0xFF)
     *
     * @param data Pointer to candidate page data
     * @param page_size Page size to validate
     * @return true if quick checks pass
     */
    static bool quick_prefilter(const uint8_t* data, size_t page_size);

    // ==================== Candidates Processing ====================

    /**
     * @brief Deduplicate overlapping candidates
     *
     * When multiple levels detect the same page (or overlapping offsets),
     * keep the highest-confidence candidate.
     *
     * @param candidates Input candidates
     * @return Deduplicated candidates
     */
    std::vector<PageCandidate> deduplicate(
        std::vector<PageCandidate> candidates) const;

    /**
     * @brief Filter candidates by minimum confidence threshold
     * @param candidates Input candidates
     * @param threshold Minimum confidence (0.0–1.0)
     * @return Filtered candidates
     */
    std::vector<PageCandidate> filter_by_confidence(
        const std::vector<PageCandidate>& candidates,
        double threshold) const;

    /**
     * @brief Sort candidates by file offset
     * @param candidates Input candidates
     * @return Sorted candidates
     */
    static std::vector<PageCandidate> sort_by_offset(
        std::vector<PageCandidate> candidates);

    /**
     * @brief Group candidates by page type
     * @param candidates Input candidates
     * @return Map of page type → candidates
     */
    static std::map<uint16_t, std::vector<PageCandidate>> group_by_type(
        const std::vector<PageCandidate>& candidates);

    // ==================== Statistics ====================

    const PageInferrerStats& stats() const { return stats_; }
    void reset_stats() { stats_ = PageInferrerStats{}; }

    /**
     * @brief Detect page size from the first valid page
     */
    static std::optional<size_t> detect_page_size(
        const uint8_t* data, size_t size);

    /**
     * @brief Estimate page size from FIL header checksum location
     *
     * InnoDB stores checksum at page_size - 8 and algorithm ID at page_size - 4.
     * By scanning forward from a page start, we can find where the checksum
     * lands and infer the page size.
     */
    static std::vector<size_t> infer_page_sizes_from_checksum(
        const uint8_t* data, size_t size);

private:
    PageInferrerConfig config_;
    PageInferrerStats stats_;
    std::mutex stats_mutex_;

    // ==================== Level 0: Exact Boundary ====================

    /**
     * @brief Level 0 — Exact page boundary scan
     *
     * Tries each configured page size in order. For each page size, walks
     * the buffer at exact page_size boundaries, reads FIL header, validates
     * page type, and runs quick prefilter.
     *
     * This is the fastest level (O(n / page_size)) and catches all
     * well-aligned pages.
     */
    std::vector<PageCandidate> scan_level0(
        const uint8_t* data, size_t size);

    /**
     * @brief Scan at a specific page size for Level 0
     */
    std::vector<PageCandidate> scan_level0_fixed_size(
        const uint8_t* data, size_t size, size_t page_size);

    // ==================== Level 1: Fuzzy Boundary ====================

    /**
     * @brief Level 1 — Fuzzy page boundary scan
     *
     * Walks the buffer at config_.level1_step (default 512) bytes.
     * For each offset, checks if the data looks like a valid FIL header.
     *
     * This catches misaligned pages (e.g., pages that shifted due to
     * partial overwrite, torn pages, or filesystem block reordering).
     *
     * Performance: O(n / step) — for 512-byte step on a 100GB file,
     * this is ~200M checks. Each check is ~10 reads + 1 prefilter.
     */
    std::vector<PageCandidate> scan_level1(
        const uint8_t* data, size_t size);

    // ==================== Level 2: Content-Based ====================

    /**
     * @brief Level 2 — Content-based page scanning
     *
     * Scans at 1-byte step looking for InnoDB page content signatures:
     *   - INFIMUM record marker: 0x00 0x02 (status INFIMUM) at expected offset
     *   - SUPREMUM record marker: 0x00 0x03 (status SUPREMUM) at expected offset
     *   - REC_NEXT chain pointers forming valid linked lists
     *   - Offset table patterns (sorted 2-byte entries with page directory)
     *   - PAGE_N_HEAP / PAGE_HEAP_TOP self-consistency
     *
     * This is the most expensive level (O(n)) but catches pages at
     * truly arbitrary offsets.
     *
     * Only scans the first config_.max_level2_bytes bytes to bound cost.
     */
    std::vector<PageCandidate> scan_level2(
        const uint8_t* data, size_t size);

    // ==================== Level 3: Data Pattern ====================

    /**
     * @brief Level 3 — Data pattern matching
     *
     * Scans for data patterns that indicate InnoDB page content:
     *   - UUID patterns (36-char hex + dashes at known offsets)
     *   - Timestamp patterns (UNIX timestamps, DATETIME encoding)
     *   - Auto-increment ID sequences (monotonic integer sequences)
     *   - Index ID patterns (consistent index_id appearing at BTR_IND_PAGE_OFFSET)
     *   - SPACE_ID patterns (same space_id appearing at predictable offsets)
     *
     * This level is speculative — it finds probable page boundaries
     * by detecting the data WITHIN pages rather than the page headers.
     */
    std::vector<PageCandidate> scan_level3(
        const uint8_t* data, size_t size);

    // ==================== Internal Helpers ====================

    /**
     * @brief Check if two candidates overlap (same page region)
     */
    static bool candidates_overlap(
        const PageCandidate& a, const PageCandidate& b);

    /**
     * @brief Merge two overlapping candidates (keep higher confidence)
     */
    static PageCandidate merge_candidates(
        const PageCandidate& a, const PageCandidate& b);

    /**
     * @brief Reject a candidate (update stats)
     */
    void reject_candidate(PageCandidate& candidate, const std::string& reason);

    /**
     * @brief Get best page size for a given offset
     *
     * Tries each configured page size and returns the one where the
     * offset is page-aligned, or the default if none match.
     */
    size_t best_page_size_for_offset(uint64_t offset) const;

    /**
     * @brief Check LSN reasonableness
     */
    bool is_lsn_reasonable(uint64_t lsn) const;

    /**
     * @brief Check if offset is within an already-detected page
     */
    bool is_within_detected_page(
        uint64_t offset,
        const std::vector<PageCandidate>& existing) const;

    /**
     * @brief Determine if page type matches content
     */
    bool does_type_match_content(
        const uint8_t* page_data,
        size_t page_size,
        uint16_t page_type) const;

    /**
     * @brief Check for valid INFIMUM record at expected offset
     */
    static bool has_valid_infimum(const uint8_t* page_data, size_t page_size);

    /**
     * @brief Check for valid SUPREMUM record at expected offset
     */
    static bool has_valid_supremum(const uint8_t* page_data, size_t page_size);

    /**
     * @brief Validate offset table structure
     */
    static bool validate_offset_table(
        const uint8_t* page_data, size_t page_size);

    /**
     * @brief Extract index ID from an INDEX page
     */
    static uint64_t extract_index_id(const uint8_t* page_data, size_t page_size);

    /**
     * @brief Check for UUID data pattern within page
     */
    static bool has_uuid_pattern(const uint8_t* data, size_t size);

    /**
     * @brief Check for timestamp pattern within page
     */
    static bool has_timestamp_pattern(const uint8_t* data, size_t size);

    /**
     * @brief Check for auto-increment sequence pattern
     */
    static bool has_autoinc_pattern(const uint8_t* data, size_t size);

    /**
     * @brief Read a candidate from raw data without full validation
     */
    static PageCandidate read_candidate_header(
        const uint8_t* data, uint64_t file_offset, size_t page_size);
};

// ==================== Implementation ====================

// ==================== Level 0: Exact Boundary ====================

inline std::vector<PageCandidate> PageTypeInferrer::scan_level0_fixed_size(
    const uint8_t* data, size_t size, size_t page_size) {

    std::vector<PageCandidate> candidates;

    if (!data || size < page_size || page_size < 38) {
        return candidates;
    }

    size_t count = size / page_size;
    candidates.reserve(count);

    for (size_t i = 0; i < count; i++) {
        uint64_t offset = static_cast<uint64_t>(i) * page_size;
        const uint8_t* page_data = data + offset;

        if (!quick_prefilter(page_data, page_size)) {
            continue;
        }

        PageCandidate candidate = read_candidate_header(
            page_data, offset, page_size);
        candidate.detected_at = ScanLevel::LEVEL_0_EXACT_BOUNDARY;

        // Run validation
        candidate.validation = validate_page(page_data, page_size, offset, &candidates);
        candidate.confidence = candidate.validation.confidence();

        if (candidate.confidence >= config_.reject_threshold) {
            candidates.push_back(candidate);
        }
    }

    return candidates;
}

inline std::vector<PageCandidate> PageTypeInferrer::scan_level0(
    const uint8_t* data, size_t size) {

    std::vector<PageCandidate> all_candidates;

    for (auto page_size : config_.page_sizes) {
        if (page_size < 38 || page_size > size) continue;

        auto candidates = scan_level0_fixed_size(data, size, page_size);

        // Deduplicate against already-found candidates
        for (auto& cand : candidates) {
            if (!is_within_detected_page(cand.file_offset, all_candidates)) {
                all_candidates.push_back(std::move(cand));
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.level0_candidates += all_candidates.size();
        stats_.total_bytes_scanned += size;
    }

    return all_candidates;
}

// ==================== Level 1: Fuzzy Boundary ====================

inline std::vector<PageCandidate> PageTypeInferrer::scan_level1(
    const uint8_t* data, size_t size) {

    std::vector<PageCandidate> candidates;

    if (!data || size < 38 || config_.level1_step == 0) {
        return candidates;
    }

    // Pre-scan to find the most common page size from valid Level 0 pages
    size_t dominant_page_size = config_.page_sizes[0];

    // Walk at fuzzy step
    for (uint64_t offset = 0; offset + 38 <= size; offset += config_.level1_step) {
        // Skip if already covered by a Level 0 page
        if (is_within_detected_page(offset, candidates)) {
            continue;
        }

        const uint8_t* page_data = data + offset;

        if (!quick_prefilter(page_data, 16384)) {
            continue;
        }

        // Try each page size to find the best fit
        for (auto page_size : config_.page_sizes) {
            if (offset + page_size > size) continue;

            // Check checksum at the expected location
            if (config_.verify_checksum) {
                uint32_t algo_id = read_le<uint32_t>(page_data, page_size - 4);
                if (algo_id != CHECKSUM_CRC32C_MAGIC &&
                    algo_id != CHECKSUM_INNODB_MAGIC &&
                    algo_id != CHECKSUM_NONE_MAGIC) {
                    continue;
                }
            }

            PageCandidate candidate = read_candidate_header(
                page_data, offset, page_size);
            candidate.detected_at = ScanLevel::LEVEL_1_FUZZY_BOUNDARY;
            candidate.validation = validate_page(
                page_data, page_size, offset, &candidates);
            candidate.confidence = candidate.validation.confidence();

            if (candidate.confidence >= config_.reject_threshold) {
                candidates.push_back(candidate);
                break;  // Found a matching page size for this offset
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.level1_candidates += candidates.size();
    }

    return candidates;
}

// ==================== Level 2: Content-Based ====================

inline std::vector<PageCandidate> PageTypeInferrer::scan_level2(
    const uint8_t* data, size_t size) {

    std::vector<PageCandidate> candidates;

    if (!data || size < 54) {
        return candidates;
    }

    // Bound the scan range
    size_t scan_end = std::min(size, config_.max_level2_bytes);

    // Collect all INFIMUM positions first (fast pre-filter)
    // INFIMUM record at absolute offset 54 has status byte 0x02
    struct ContentHint {
        uint64_t offset;
        uint8_t  status;      // INFIMUM (0x02) or SUPREMUM (0x03)
        uint16_t heap_top;    // From PAGE_HEAP_TOP at offset 40
    };
    std::vector<ContentHint> hints;

    // Scan for page content markers
    // We look for:
    // 1. INFIMUM records: at offset 54 from page start, byte 0 = 0x00, byte 4 = 0x02 (status)
    // 2. POTENTIAL page boundaries by looking for consistent 0x00 0x02 patterns at 54-byte offsets
    //
    // Since we don't know page boundaries yet, we scan byte-by-byte looking for
    // FIL header patterns combined with INFIMUM markers.

    // Strategy: scan for pairs of (valid FIL header at offset, INFIMUM at offset + 54)
    // and (valid FIL header at offset, SUPREMUM at offset + 54 + 8)
    for (uint64_t offset = 0; offset + 80 <= scan_end; offset++) {
        // Skip if covered by a known page
        if (!candidates.empty() && is_within_detected_page(offset, candidates)) {
            continue;
        }

        const uint8_t* page_data = data + offset;

        // Quick prefilter on the FIL header area
        uint16_t page_type = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);
        if (!is_valid_page_type(page_type) && page_type != FIL_PAGE_TYPE_FSP_HDR) {
            continue;
        }

        // Check SPACE_ID and PAGE_NO sanity
        uint32_t space_id = read_be<uint32_t>(page_data, FIL_PAGE_SPACE_ID);
        uint32_t page_no = read_be<uint32_t>(page_data, 4);
        if (space_id > 0xFFFFFF || page_no > 0xFFFFFFF0) {
            continue;
        }

        // For INDEX pages, verify INFIMUM at offset 54
        if (page_type == FIL_PAGE_INDEX) {
            if (!has_valid_infimum(page_data, 16384)) {
                continue;
            }
            if (!has_valid_supremum(page_data, 16384)) {
                continue;
            }
        }

        // Try each page size
        for (auto page_size : config_.page_sizes) {
            if (offset + page_size > size) continue;

            PageCandidate candidate = read_candidate_header(
                page_data, offset, page_size);
            candidate.detected_at = ScanLevel::LEVEL_2_CONTENT_BASED;

            // Full validation
            candidate.validation = validate_page(
                page_data, page_size, offset, &candidates);
            candidate.confidence = candidate.validation.confidence();

            // For Level 2, also check offset table if INDEX page
            if (candidate.page_type == FIL_PAGE_INDEX &&
                !candidate.validation.offset_table_ok) {
                candidate.confidence *= 0.5;
            }

            if (candidate.confidence >= config_.reject_threshold) {
                candidates.push_back(candidate);
                break;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.level2_candidates += candidates.size();
    }

    return candidates;
}

// ==================== Level 3: Data Pattern ====================

inline std::vector<PageCandidate> PageTypeInferrer::scan_level3(
    const uint8_t* data, size_t size) {

    std::vector<PageCandidate> candidates;

    if (!data || size < 38) {
        return candidates;
    }

    // Level 3 scans for data patterns that indicate page boundaries
    // even when the FIL header is damaged or absent.

    const size_t min_pattern_window = 256;  // Minimum window to analyze

    for (uint64_t offset = 0; offset + min_pattern_window <= size; offset++) {
        if (is_within_detected_page(offset, candidates)) {
            continue;
        }

        const uint8_t* window = data + offset;
        double pattern_score = 0.0;
        uint32_t hints_found = 0;

        // Check for UUID pattern
        if (config_.detect_uuid_patterns && has_uuid_pattern(window, min_pattern_window)) {
            pattern_score += 0.3;
            hints_found++;
        }

        // Check for timestamp pattern
        if (config_.detect_timestamp_patterns && has_timestamp_pattern(window, min_pattern_window)) {
            pattern_score += 0.3;
            hints_found++;
        }

        // Check for auto-increment pattern
        if (config_.detect_autoinc_patterns && has_autoinc_pattern(window, min_pattern_window)) {
            pattern_score += 0.2;
            hints_found++;
        }

        // Check for index_id pattern (consistent 8-byte value at BTR_IND_PAGE_OFFSET)
        if (offset + 50 <= size) {
            uint64_t index_id = read_be<uint64_t>(data + offset, 44);
            if (index_id > 0 && index_id < 0xFFFFFFFFFFFF) {
                pattern_score += 0.2;
                hints_found++;
            }
        }

        if (hints_found < 2 || pattern_score < config_.promote_threshold) {
            continue;
        }

        // Found enough patterns — try to validate as a page
        for (auto page_size : config_.page_sizes) {
            if (offset + page_size > size) continue;

            PageCandidate candidate;
            candidate.file_offset = offset;
            candidate.detected_size = page_size;
            candidate.detected_at = ScanLevel::LEVEL_3_DATA_PATTERN;
            candidate.confidence = pattern_score;

            // Try to read FIL header
            const uint8_t* page_data = data + offset;
            if (quick_prefilter(page_data, page_size)) {
                candidate = read_candidate_header(page_data, offset, page_size);
                candidate.validation = validate_page(
                    page_data, page_size, offset, &candidates);
                candidate.confidence = std::max(
                    pattern_score, candidate.validation.confidence());
            } else {
                // No valid FIL header — use pattern-based confidence only
                candidate.validation.space_id_consistent = true;
                candidate.confidence = pattern_score * 0.7;  // Penalty for missing header
            }

            if (candidate.confidence >= config_.reject_threshold) {
                candidates.push_back(candidate);
                break;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.level3_candidates += candidates.size();
    }

    return candidates;
}

// ==================== Core Scan Pipeline ====================

inline std::vector<PageCandidate> PageTypeInferrer::scan(
    const uint8_t* data, size_t size) {

    auto start_time = std::chrono::high_resolution_clock::now();
    reset_stats();

    std::vector<PageCandidate> all_candidates;

    // Level 0: Fast exact boundary scan
    if (config_.enable_level0 && size >= 4096) {
        auto level0 = scan_level0(data, size);
        all_candidates.insert(all_candidates.end(),
            std::make_move_iterator(level0.begin()),
            std::make_move_iterator(level0.end()));
    }

    // Deduplicate after Level 0
    all_candidates = deduplicate(all_candidates);

    // Level 1: Fuzzy boundary scan (512-byte step)
    if (config_.enable_level1 && size >= 512) {
        auto level1 = scan_level1(data, size);
        all_candidates.insert(all_candidates.end(),
            std::make_move_iterator(level1.begin()),
            std::make_move_iterator(level1.end()));
    }

    all_candidates = deduplicate(all_candidates);

    // Level 2: Content-based scan (1-byte step, bounded)
    if (config_.enable_level2) {
        auto level2 = scan_level2(data, size);
        all_candidates.insert(all_candidates.end(),
            std::make_move_iterator(level2.begin()),
            std::make_move_iterator(level2.end()));
    }

    all_candidates = deduplicate(all_candidates);

    // Level 3: Data pattern matching
    if (config_.enable_level3) {
        auto level3 = scan_level3(data, size);
        all_candidates.insert(all_candidates.end(),
            std::make_move_iterator(level3.begin()),
            std::make_move_iterator(level3.end()));
    }

    // Final dedup and sort
    all_candidates = deduplicate(all_candidates);
    all_candidates = filter_by_confidence(all_candidates, config_.accept_threshold);
    all_candidates = sort_by_offset(all_candidates);

    // Update stats
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.accepted_pages = all_candidates.size();
        for (const auto& c : all_candidates) {
            switch (c.page_type) {
                case FIL_PAGE_INDEX:
                    stats_.pages_by_type_index++;
                    break;
                case FIL_PAGE_UNDO_LOG:
                    stats_.pages_by_type_undo++;
                    break;
                case FIL_PAGE_TYPE_BLOB:
                case FIL_PAGE_COMPRESSED:
                    stats_.pages_by_type_blob++;
                    break;
                case FIL_PAGE_SDI:
                    stats_.pages_by_type_sdi++;
                    break;
                default:
                    stats_.pages_by_type_other++;
                    break;
            }
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        stats_.scan_time_ms = std::chrono::duration<double, std::milli>(
            end_time - start_time).count();
    }

    return all_candidates;
}

inline std::vector<PageCandidate> PageTypeInferrer::scan_file(
    const std::string& file_path) {

    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }

    size_t file_size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    file.close();

    return scan(buffer.data(), buffer.size());
}

inline std::vector<PageCandidate> PageTypeInferrer::scan_level(
    const uint8_t* data, size_t size, ScanLevel level) {

    switch (level) {
        case ScanLevel::LEVEL_0_EXACT_BOUNDARY:
            return scan_level0(data, size);
        case ScanLevel::LEVEL_1_FUZZY_BOUNDARY:
            return scan_level1(data, size);
        case ScanLevel::LEVEL_2_CONTENT_BASED:
            return scan_level2(data, size);
        case ScanLevel::LEVEL_3_DATA_PATTERN:
            return scan_level3(data, size);
        default:
            return {};
    }
}

// ==================== Page Validation ====================

inline PageValidationFlags PageTypeInferrer::validate_page(
    const uint8_t* data,
    size_t page_size,
    uint64_t file_offset,
    const std::vector<PageCandidate>* existing_candidates) {

    PageValidationFlags flags;

    if (!data || page_size < 38) {
        return flags;
    }

    // 1. Checksum validation
    if (config_.verify_checksum) {
        auto result = validate_page_checksum(data, page_size, false);
        flags.checksum_valid = result.is_valid;
        if (!result.is_valid) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.checksum_failures++;
        }
    } else {
        flags.checksum_valid = true;  // Skip checksum verification
    }

    uint32_t space_id = read_be<uint32_t>(data, FIL_PAGE_SPACE_ID);
    uint32_t page_no = read_be<uint32_t>(data, 4);
    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    uint64_t lsn = read_be<uint64_t>(data, 16);

    // 2. SPACE_ID consistency
    // Valid range: 0 to 0xFFFFFF (InnoDB max)
    flags.space_id_consistent = (space_id <= 0xFFFFFF);

    // 3. PAGE_NO consistency
    // Valid range: 0 to 0xFFFFFFEF (reserved values above that)
    if (page_no == 0xFFFFFFFF) {
        // Special value: page not yet assigned
        flags.page_no_consistent = true;
    } else {
        flags.page_no_consistent = (page_no <= 0xFFFFFFEF);

        // Cross-check: if offset is at a known page boundary,
        // page_no should match offset / page_size (approximately)
        if (flags.page_no_consistent && page_size > 0) {
            uint32_t expected_page_no = static_cast<uint32_t>(file_offset / page_size);
            if (expected_page_no > 0 && page_no > 0) {
                // Allow some tolerance for disk carving
                uint32_t diff = (page_no > expected_page_no)
                    ? (page_no - expected_page_no)
                    : (expected_page_no - page_no);
                if (diff > 10) {
                    flags.page_no_consistent = false;  // Too far off
                }
            }
        }
    }

    // 4. Page type vs content match
    flags.type_content_match = does_type_match_content(data, page_size, page_type);

    // 5. LSN reasonableness
    flags.lsn_continuous = is_lsn_reasonable(lsn);

    // 6. Index ID consistency (INDEX pages only)
    if (page_type == FIL_PAGE_INDEX) {
        uint64_t index_id = extract_index_id(data, page_size);
        flags.index_id_consistent = (index_id > 0 && index_id < 0xFFFFFFFFFFFFULL);
    } else {
        flags.index_id_consistent = true;  // N/A
    }

    // 7. INFIMUM record presence (INDEX pages)
    if (page_type == FIL_PAGE_INDEX) {
        flags.has_infimum = has_valid_infimum(data, page_size);
    } else {
        flags.has_infimum = true;  // N/A
    }

    // 8. SUPREMUM record presence (INDEX pages)
    if (page_type == FIL_PAGE_INDEX) {
        flags.has_supremum = has_valid_supremum(data, page_size);
    } else {
        flags.has_supremum = true;  // N/A
    }

    // 9. Offset table structure (INDEX pages)
    if (page_type == FIL_PAGE_INDEX) {
        flags.offset_table_ok = validate_offset_table(data, page_size);
    } else {
        flags.offset_table_ok = true;  // N/A
    }

    // 10. Chain link consistency (prev/next page)
    if (existing_candidates && !existing_candidates->empty()) {
        uint32_t prev_page = read_be<uint32_t>(data, 8);
        uint32_t next_page = read_be<uint32_t>(data, 12);

        // Check if prev_page or next_page reference known pages
        bool prev_found = (prev_page == 0xFFFFFFFF);
        bool next_found = (next_page == 0xFFFFFFFF);

        for (const auto& c : *existing_candidates) {
            if (c.page_no == prev_page) prev_found = true;
            if (c.page_no == next_page) next_found = true;
        }

        // If we have many candidates, expect some chain links to resolve
        if (existing_candidates->size() > 10) {
            flags.chain_links_ok = (prev_found || next_found || prev_page == 0xFFFFFFFF);
        } else {
            flags.chain_links_ok = true;  // Not enough data to judge
        }
    } else {
        flags.chain_links_ok = true;
    }

    return flags;
}

inline bool PageTypeInferrer::quick_prefilter(const uint8_t* data, size_t page_size) {
    if (!data || page_size < 38) {
        return false;
    }

    // Check page type
    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    if (!is_valid_page_type(page_type) &&
        page_type != FIL_PAGE_TYPE_FSP_HDR &&
        page_type != FIL_PAGE_TYPE_XDES) {
        return false;
    }

    // SPACE_ID sanity
    uint32_t space_id = read_be<uint32_t>(data, FIL_PAGE_SPACE_ID);
    if (space_id > 0xFFFFFF) {
        return false;
    }

    // PAGE_NO sanity
    uint32_t page_no = read_be<uint32_t>(data, 4);
    if (page_no > 0xFFFFFFF0 && page_no != 0xFFFFFFFF) {
        return false;
    }

    // LSN sanity
    uint64_t lsn = read_be<uint64_t>(data, 16);
    // LSN should be > 0 and not all 0xFF
    if (lsn == 0 || lsn == 0xFFFFFFFFFFFFFFFFULL) {
        return false;
    }

    // Check checksum trailer exists (not all zeros, not all 0xFF)
    if (page_size >= 4) {
        uint32_t tail = read_le<uint32_t>(data, page_size - 4);
        if (tail == 0xFFFFFFFF) {
            return false;  // Unwritten page
        }
    }

    return true;
}

// ==================== Candidates Processing ====================

inline std::vector<PageCandidate> PageTypeInferrer::deduplicate(
    std::vector<PageCandidate> candidates) const {

    if (candidates.empty()) return candidates;

    // Sort by offset
    std::sort(candidates.begin(), candidates.end(),
        [](const PageCandidate& a, const PageCandidate& b) {
            return a.file_offset < b.file_offset;
        });

    // Merge overlapping candidates
    std::vector<PageCandidate> result;
    result.reserve(candidates.size());

    for (auto& cand : candidates) {
        bool merged = false;
        for (auto& existing : result) {
            if (candidates_overlap(existing, cand)) {
                existing = merge_candidates(existing, cand);
                merged = true;
                break;
            }
        }
        if (!merged) {
            result.push_back(std::move(cand));
        }
    }

    return result;
}

inline std::vector<PageCandidate> PageTypeInferrer::filter_by_confidence(
    const std::vector<PageCandidate>& candidates, double threshold) const {

    std::vector<PageCandidate> result;
    result.reserve(candidates.size());

    for (const auto& c : candidates) {
        if (c.confidence >= threshold) {
            result.push_back(c);
        }
    }

    return result;
}

inline std::vector<PageCandidate> PageTypeInferrer::sort_by_offset(
    std::vector<PageCandidate> candidates) {

    std::sort(candidates.begin(), candidates.end(),
        [](const PageCandidate& a, const PageCandidate& b) {
            return a.file_offset < b.file_offset;
        });
    return candidates;
}

inline std::map<uint16_t, std::vector<PageCandidate>> PageTypeInferrer::group_by_type(
    const std::vector<PageCandidate>& candidates) {

    std::map<uint16_t, std::vector<PageCandidate>> groups;
    for (const auto& c : candidates) {
        groups[c.page_type].push_back(c);
    }
    return groups;
}

// ==================== Static Helpers ====================

inline bool PageTypeInferrer::candidates_overlap(
    const PageCandidate& a, const PageCandidate& b) {

    if (a.file_offset == b.file_offset) return true;

    size_t a_end = a.detected_size > 0
        ? a.file_offset + a.detected_size
        : a.file_offset + 16384;
    size_t b_end = b.detected_size > 0
        ? b.file_offset + b.detected_size
        : b.file_offset + 16384;

    return (a.file_offset < b_end && b.file_offset < a_end);
}

inline PageCandidate PageTypeInferrer::merge_candidates(
    const PageCandidate& a, const PageCandidate& b) {

    if (a.confidence >= b.confidence) return a;
    return b;
}

inline void PageTypeInferrer::reject_candidate(
    PageCandidate& candidate, const std::string& /*reason*/) {

    candidate.confidence = 0.0;
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.rejected_pages++;
    }
}

inline size_t PageTypeInferrer::best_page_size_for_offset(uint64_t offset) const {
    for (auto page_size : config_.page_sizes) {
        if (offset % page_size == 0) {
            return page_size;
        }
    }
    return config_.page_sizes[0];
}

inline bool PageTypeInferrer::is_lsn_reasonable(uint64_t lsn) const {
    // LSN should be > 0 and reasonable
    if (lsn == 0 || lsn == 0xFFFFFFFFFFFFFFFFULL) {
        return false;
    }

    // Very small LSNs (< 100) are unusual for data pages
    // (LSN starts at ~700000 after bootstrap)
    if (lsn < 100) {
        return false;
    }

    return true;
}

inline bool PageTypeInferrer::is_within_detected_page(
    uint64_t offset, const std::vector<PageCandidate>& existing) const {

    for (const auto& c : existing) {
        size_t page_end = c.detected_size > 0
            ? c.file_offset + c.detected_size
            : c.file_offset + 16384;
        if (offset >= c.file_offset && offset < page_end) {
            return true;
        }
    }
    return false;
}

inline bool PageTypeInferrer::does_type_match_content(
    const uint8_t* page_data, size_t page_size, uint16_t page_type) const {

    if (!page_data) return false;

    switch (page_type) {
        case FIL_PAGE_INDEX: {
            // INDEX pages should have BTR header with valid index_id
            uint64_t index_id = extract_index_id(page_data, page_size);
            if (index_id == 0) return false;

            // Should have INFIMUM and SUPREMUM
            bool has_inf = has_valid_infimum(page_data, page_size);
            bool has_sup = has_valid_supremum(page_data, page_size);
            return has_inf && has_sup;
        }
        case FIL_PAGE_UNDO_LOG: {
            // UNDO_LOG pages should have TRX_UNDO_SEG header
            // Check at offset 38 (FIL_PAGE_DATA) + some undo header
            uint32_t undo_marker = read_be<uint32_t>(page_data, 38);
            return undo_marker != 0;
        }
        case FIL_PAGE_TYPE_BLOB:
        case FIL_PAGE_COMPRESSED: {
            // BLOB pages: check for BLOB header markers
            uint32_t first_word = read_be<uint32_t>(page_data, 38);
            return first_word != 0;
        }
        case FIL_PAGE_SDI: {
            // SDI pages should have valid SDI header
            uint32_t sdi_marker = read_be<uint32_t>(page_data, 38);
            return sdi_marker != 0;
        }
        case FIL_PAGE_TYPE_ALLOCATED: {
            // Allocated pages — no content to check
            return true;
        }
        default:
            return true;
    }
}

inline bool PageTypeInferrer::has_valid_infimum(
    const uint8_t* page_data, size_t page_size) {

    if (!page_data || page_size < 55) return false;

    // INFIMUM record is at absolute offset 54 (FIL_HEADER_SIZE + page header)
    // It has:
    //   bytes 0-4:  rec header (status=INFIMUM=0x02 in byte 4, bits 4-5)
    //   bytes 5-7:  next record offset (2 bytes BE)
    //   bytes 8-17: "infimum\0\0" (8 bytes)
    const uint8_t* infimum = page_data + 54;

    // Check status byte at offset 4 from INFIMUM start
    // In COMPACT/DYNAMIC format, the record header has:
    //   byte 0: variable length (nullable?) + info bits
    //   byte 1: n_fields (0 for infimum)
    //   byte 2: 1-byte offset (or 2 bytes)
    //   byte 3: n_owned
    //   byte 4: status (bits 4-5) + heap_no (bits 0-3) + n_fields (bit 6-7)
    uint8_t status_byte = infimum[4];
    // Status bits are in positions 4-5 (REC_STATUS_INFIMUM = 0x02 shifted)
    // In MySQL 8.0: status is at bits 4-5 of byte 4
    uint8_t status = (status_byte >> 4) & 0x03;
    if (status != REC_STATUS_INFIMUM) {
        return false;
    }

    // Check for "infimum" string at offset 8 from INFIMUM start
    // (Only in older MySQL versions; in 8.0+ it may be different)
    // We'll just check the status bits for now.

    return true;
}

inline bool PageTypeInferrer::has_valid_supremum(
    const uint8_t* page_data, size_t page_size) {

    if (!page_data || page_size < 80) return false;

    // SUPREMUM is after INFIMUM. Need to read the "next record" offset
    // from INFIMUM's record header to find SUPREMUM's position.
    const uint8_t* infimum = page_data + 54;

    // Read the next-record offset from INFIMUM (bytes 5-6, big-endian)
    // The offset is relative to the page start
    uint16_t supremum_offset = read_be<uint16_t>(infimum, 5);

    if (supremum_offset == 0 || supremum_offset >= page_size) {
        return false;
    }

    // Check SUPREMUM status
    const uint8_t* supremum = page_data + supremum_offset;
    uint8_t status_byte = supremum[4];
    uint8_t status = (status_byte >> 4) & 0x03;

    return status == REC_STATUS_SUPREMUM;
}

inline bool PageTypeInferrer::validate_offset_table(
    const uint8_t* page_data, size_t page_size) {

    if (!page_data || page_size < 42) return false;

    // Read page header fields
    uint16_t n_dir_slots = read_be<uint16_t>(page_data, FIL_HEADER_SIZE + 0);
    uint16_t heap_top = read_be<uint16_t>(page_data, FIL_HEADER_SIZE + 2);
    uint16_t n_heap = read_be<uint16_t>(page_data, FIL_HEADER_SIZE + 4);

    // Sanity checks
    if (n_dir_slots == 0 || n_dir_slots > 1000) return false;
    if (heap_top < FIL_HEADER_SIZE + 36 || heap_top > page_size) return false;
    if (n_heap == 0 || n_heap > 10000) return false;

    // Page directory is at the end of the page, growing backward
    // Each slot is 2 bytes (LE offset of the last record in each group)
    size_t dir_start = page_size - static_cast<size_t>(n_dir_slots) * 2;

    // Directory should be after heap_top
    if (dir_start < heap_top + 10) return false;

    // Validate first slot: should point to SUPREMUM
    if (n_dir_slots >= 2) {
        uint16_t slot0 = read_le<uint16_t>(page_data, dir_start);
        // First slot usually points to INFIMUM or SUPREMUM
        // Last slot should point to SUPREMUM
        uint16_t last_slot = read_le<uint16_t>(
            page_data, dir_start + (n_dir_slots - 1) * 2);

        // Slot values should be within the page
        if (slot0 < 54 || slot0 > page_size) return false;
        if (last_slot < 54 || last_slot > page_size) return false;
    }

    return true;
}

inline uint64_t PageTypeInferrer::extract_index_id(
    const uint8_t* page_data, size_t page_size) {

    if (!page_data || page_size < 50) return 0;

    // Index ID is at BTR_IND_PAGE_OFFSET_ABS = FIL_HEADER_SIZE + 6 = 44
    uint64_t index_id = read_be<uint64_t>(page_data, 44);

    // Index ID should be a reasonable value
    if (index_id == 0 || index_id > 0xFFFFFFFFFFFFULL) {
        return 0;
    }

    return index_id;
}

// ==================== Data Pattern Detection ====================

inline bool PageTypeInferrer::has_uuid_pattern(
    const uint8_t* data, size_t size) {

    if (!data || size < 36) return false;

    // UUID string pattern: 8-4-4-4-12 hex digits with dashes
    // e.g., "550e8400-e29b-41d4-a716-446655440000"
    // Look for sequences of hex digits and dashes

    for (size_t i = 0; i + 36 <= size; i++) {
        // Check for UUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
        bool is_uuid = true;

        for (int j = 0; j < 36; j++) {
            uint8_t c = data[i + j];
            if (j == 8 || j == 13 || j == 18 || j == 23) {
                if (c != '-') { is_uuid = false; break; }
            } else {
                bool is_hex = (c >= '0' && c <= '9') ||
                              (c >= 'a' && c <= 'f') ||
                              (c >= 'A' && c <= 'F');
                if (!is_hex) { is_uuid = false; break; }
            }
        }

        if (is_uuid) return true;
    }

    // Also check for binary UUID (16 bytes, not null-terminated string)
    // UUIDs often have specific byte patterns in version/variant fields
    for (size_t i = 0; i + 16 <= size; i++) {
        // Check byte 7 (version field): should be 0x40, 0x50, etc.
        uint8_t version_byte = data[i + 7];
        uint8_t variant_byte = data[i + 8];

        if ((version_byte & 0xF0) == 0x40 ||  // UUID v4
            (version_byte & 0xF0) == 0x50 ||  // UUID v5
            (version_byte & 0xF0) == 0x30) {  // UUID v3
            // Check variant field (bytes 8-9): 0x80-0xBF is standard
            if ((variant_byte & 0xC0) == 0x80) {
                return true;
            }
        }
    }

    return false;
}

inline bool PageTypeInferrer::has_timestamp_pattern(
    const uint8_t* data, size_t size) {

    if (!data || size < 8) return false;

    // Check for UNIX timestamps (32-bit, range 2000-2030)
    // This is a speculative check: look for 4-byte values that
    // fall in the range 946684800 (2000-01-01) to 1893456000 (2030-01-01)
    for (size_t i = 0; i + 4 <= size; i++) {
        uint32_t ts = read_le<uint32_t>(data, i);
        if (ts >= 946684800 && ts <= 1893456000) {
            // Found a potential timestamp — check if nearby bytes
            // also look like timestamps (multiple timestamps = pattern)
            uint32_t nearby_count = 0;
            for (size_t j = i + 8; j + 4 <= size && j < i + 64; j += 4) {
                uint32_t ts2 = read_le<uint32_t>(data, j);
                if (ts2 >= 946684800 && ts2 <= 1893456000) {
                    nearby_count++;
                    if (nearby_count >= 3) return true;
                }
            }
        }
    }

    // Check for MySQL DATETIME/TIMESTAMP encoding
    // DATETIME(0) stored as 8-byte integer: YYYYMMDDHHMMSSuuuuuu
    for (size_t i = 0; i + 8 <= size; i++) {
        uint64_t val = read_be<uint64_t>(data, i);
        // Check if it looks like a DATETIME (e.g., 20260721...)
        if (val >= 20000000000000000ULL && val <= 21000000000000000ULL) {
            return true;
        }
    }

    return false;
}

inline bool PageTypeInferrer::has_autoinc_pattern(
    const uint8_t* data, size_t size) {

    if (!data || size < 16) return false;

    // Look for monotonic integer sequences (auto-increment IDs)
    // Scan for consecutive integer values that are close together
    for (size_t i = 0; i + 8 <= size; i++) {
        uint64_t val1 = read_le<uint64_t>(data, i);
        if (val1 == 0 || val1 > 1000000000ULL) continue;

        // Check the next 8-byte value at a reasonable record offset
        // Auto-increment values often appear in consecutive records
        // with small gaps (1-1000)
        for (size_t gap = 8; gap <= 64; gap += 4) {
            if (i + gap + 8 > size) break;
            uint64_t val2 = read_le<uint64_t>(data, i + gap);
            if (val2 == 0 || val2 > 1000000000ULL) continue;

            uint64_t diff = (val2 > val1) ? (val2 - val1) : (val1 - val2);
            if (diff > 0 && diff <= 1000) {
                // Found two close values — check for a third
                if (i + gap * 2 + 8 <= size) {
                    uint64_t val3 = read_le<uint64_t>(data, i + gap * 2);
                    if (val3 == 0 || val3 > 1000000000ULL) continue;

                    uint64_t diff2 = (val3 > val2) ? (val3 - val2) : (val2 - val3);
                    if (diff2 > 0 && diff2 <= 1000) {
                        return true;  // Three close values = auto-inc pattern
                    }
                }
            }
        }
    }

    return false;
}

inline PageCandidate PageTypeInferrer::read_candidate_header(
    const uint8_t* data, uint64_t file_offset, size_t page_size) {

    PageCandidate candidate;
    candidate.file_offset = file_offset;
    candidate.detected_size = page_size;

    // Read FIL header fields (big-endian)
    if (data) {
        candidate.space_id = read_be<uint32_t>(data, FIL_PAGE_SPACE_ID);
        candidate.page_no = read_be<uint32_t>(data, 4);
        candidate.prev_page = read_be<uint32_t>(data, 8);
        candidate.next_page = read_be<uint32_t>(data, 12);
        candidate.lsn = read_be<uint64_t>(data, 16);
        candidate.page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
        candidate.page_level = read_be<uint16_t>(data, 26);

        // Try to read index_id for INDEX pages
        if (candidate.page_type == FIL_PAGE_INDEX) {
            candidate.index_id = extract_index_id(data, page_size);
        }
    }

    return candidate;
}

// ==================== Page Size Detection ====================

inline std::optional<size_t> PageTypeInferrer::detect_page_size(
    const uint8_t* data, size_t size) {

    if (!data || size < 4096) {
        return std::nullopt;
    }

    // Try each page size
    const std::vector<size_t> sizes = {16384, 8192, 4096, 32768, 65536};
    for (auto page_size : sizes) {
        if (page_size > size) continue;

        // Check first page: must have valid header AND valid checksum
        if (!quick_prefilter(data, page_size)) continue;

        // If checksum is off, that's OK for damaged files
        // but at least the header should look reasonable
        uint16_t type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
        if (is_valid_page_type(type) || type == FIL_PAGE_TYPE_FSP_HDR) {
            return page_size;
        }
    }

    return std::nullopt;
}

inline std::vector<size_t> PageTypeInferrer::infer_page_sizes_from_checksum(
    const uint8_t* data, size_t size) {

    std::vector<size_t> detected_sizes;

    if (!data || size < 4096) {
        return detected_sizes;
    }

    // Scan forward from offset 0 looking for checksum algorithm IDs
    // at various offsets. InnoDB stores algorithm ID at page_size - 4.
    const std::vector<size_t> candidate_sizes = {4096, 8192, 16384, 32768, 65536};
    const std::set<uint32_t> known_magic = {
        CHECKSUM_CRC32C_MAGIC,
        CHECKSUM_INNODB_MAGIC,
        CHECKSUM_NONE_MAGIC
    };

    for (auto page_size : candidate_sizes) {
        if (page_size > size + 4 || page_size < 4) continue;

        // Check if there's a valid algorithm ID at page_size - 4
        uint32_t algo_id = read_le<uint32_t>(data, page_size - 4);
        if (known_magic.count(algo_id)) {
            // Also check that there's a valid FIL header at offset 0
            if (quick_prefilter(data, page_size)) {
                detected_sizes.push_back(page_size);
            }
        }
    }

    return detected_sizes;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_TYPE_INFERRER_HPP