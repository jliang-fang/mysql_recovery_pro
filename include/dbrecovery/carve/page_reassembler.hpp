#ifndef DBRECOVERY_CARVE_PAGE_REASSEMBLER_HPP
#define DBRECOVERY_CARVE_PAGE_REASSEMBLER_HPP

/**
 * @file page_reassembler.hpp
 * @brief InnoDB page fragment reassembly engine
 *
 * When filesystem metadata is lost (rm -rf, disk format), InnoDB pages
 * may be scattered across raw disk sectors. This engine identifies pages
 * by their FIL headers, groups them by SPACE_ID, sorts by PAGE_NO,
 * detects gaps, and composes logically complete tablespace files.
 */

#include <cstdint>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <optional>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <cstring>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace carve {

// ==================== Page Identity ====================

/**
 * @brief Identified InnoDB page with metadata
 */
struct PageIdentity {
    uint32_t space_id;          // Tablespace ID (FIL header offset 34)
    uint32_t page_no;           // Page number (FIL header offset 4)
    uint16_t page_type;         // FIL_PAGE_TYPE (offset 24, big-endian)
    uint64_t lsn;               // Log sequence number (offset 16)
    uint32_t checksum;          // Page checksum (offset 0)
    bool checksum_valid;        // Whether checksum verification passed
    bool is_compressed;         // Whether page is compressed
    bool is_encrypted;          // Whether page is TDE-encrypted
    uint64_t disk_offset;       // Physical offset on disk
    std::vector<uint8_t> data;  // Full page data (16384 bytes typically)
    uint32_t page_size;         // Actual page size (4096/8192/16384/32768/65536)
    bool is_synthetic = false;  // True for generated gap placeholders

    std::string page_type_name() const {
        switch (page_type) {
            case 17855: return "INDEX";      // 0x45BF
            case 17842: return "UNDO_LOG";   // 0x45B2
            case 17854: return "SDI";        // 0x45BE
            case 17853: return "SDI_ZIP/BLOB"; // 0x45BD
            case 17845: return "COMPRESSED"; // 0x45B5
            case 0:     return "ALLOCATED";
            default:    return "UNKNOWN(" + std::to_string(page_type) + ")";
        }
    }

    std::string to_string() const {
        std::ostringstream oss;
        oss << "Page{space=" << space_id << ", pno=" << page_no
            << ", type=" << page_type_name()
            << ", lsn=" << lsn
            << ", cksum=" << (checksum_valid ? "OK" : "FAIL")
            << ", provenance=" << (is_synthetic ? "synthetic" : "physical")
            << ", disk_off=0x" << std::hex << disk_offset << std::dec
            << ", size=" << page_size << "}";
        return oss.str();
    }
};

// ==================== Gap Analysis ====================

/**
 * @brief Information about a gap in the page sequence
 */
struct PageGap {
    uint32_t space_id;
    uint32_t start_page_no;
    uint32_t end_page_no;       // Exclusive
    uint32_t missing_count;     // Number of missing pages

    enum Severity {
        CRITICAL,   // Missing Page 0 (FSP_HDR) — no space flags, page size
        HIGH,       // Missing Page 2 (INODE) — no file segment info
        MEDIUM,     // Missing Page 3/4 (SDI) — no metadata (MySQL 8.0+)
        LOW         // Missing data pages — data incomplete
    };

    Severity severity;
    std::string description;

    std::string severity_name() const {
        switch (severity) {
            case CRITICAL: return "CRITICAL";
            case HIGH:     return "HIGH";
            case MEDIUM:   return "MEDIUM";
            case LOW:      return "LOW";
            default:       return "UNKNOWN";
        }
    }
};

// ==================== Reassembly Statistics ====================

struct ReassemblyStats {
    uint32_t total_pages_found = 0;
    uint32_t total_pages_used = 0;
    uint32_t synthetic_pages_written = 0;
    uint32_t total_gaps = 0;
    uint32_t critical_gaps = 0;
    uint32_t spaces_reconstructed = 0;
    uint64_t total_bytes_output = 0;
    double completeness_pct = 0.0;
    double time_seconds = 0.0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "ReassemblyStats{pages=" << total_pages_found
            << ", used=" << total_pages_used
            << ", gaps=" << total_gaps
            << ", critical=" << critical_gaps
            << ", spaces=" << spaces_reconstructed
            << ", completeness=" << std::fixed << std::setprecision(1)
            << completeness_pct << "%"
            << ", time=" << time_seconds << "s}";
        return oss.str();
    }
};

// ==================== Page Fragment Reassembler ====================

/**
 * @brief Reassembles scattered InnoDB pages into logical tablespaces
 *
 * Algorithm:
 * 1. Accept identified pages (from RawDiskCarver or other scanners)
 * 2. Group pages by SPACE_ID
 * 3. Within each group, deduplicate by PAGE_NO (select best candidate)
 * 4. Sort by PAGE_NO
 * 5. Detect gaps and classify severity
 * 6. Generate placeholder pages for gaps
 * 7. Write composed .ibd file for each space
 */
class PageFragmentReassembler {
public:
    PageFragmentReassembler() = default;

    /**
     * @brief Add an identified page to the reassembly pool
     */
    void add_page(const PageIdentity& page) {
        pages_.push_back(page);
    }

    /**
     * @brief Add multiple pages at once
     */
    void add_pages(const std::vector<PageIdentity>& pages) {
        pages_.insert(pages_.end(), pages.begin(), pages.end());
    }

    /**
     * @brief Execute the full reassembly pipeline
     * @param output_dir Directory to write composed .ibd files
     * @return Reassembly statistics
     */
    ReassemblyStats reassemble(const std::string& output_dir);

    /**
     * @brief Group pages by SPACE_ID without writing files
     * @return Map of space_id → sorted, deduplicated pages
     */
    std::map<uint32_t, std::vector<PageIdentity>> group_pages();

    /**
     * @brief Analyze gaps for a specific space
     */
    std::vector<PageGap> analyze_gaps(
        uint32_t space_id,
        const std::vector<PageIdentity>& sorted_pages);

    /**
     * @brief Get the reassembly report
     */
    std::string generate_report() const;

    /**
     * @brief Get reassembly statistics
     */
    const ReassemblyStats& get_stats() const { return stats_; }

    /**
     * @brief Clear all accumulated pages
     */
    void clear() { pages_.clear(); stats_ = {}; space_groups_.clear(); }

private:
    std::vector<PageIdentity> pages_;
    std::map<uint32_t, std::vector<PageIdentity>> space_groups_;
    std::vector<PageGap> all_gaps_;
    ReassemblyStats stats_;

    /**
     * @brief Select the best candidate when multiple pages claim same PAGE_NO
     *
     * Priority:
     * 1. checksum_valid == true
     * 2. Highest LSN (most recent data)
     * 3. Not all-zero
     */
    static PageIdentity select_best_candidate(
        const std::vector<PageIdentity>& candidates);

    /**
     * @brief Create a zero-filled placeholder page
     */
    static std::vector<uint8_t> create_placeholder(
        uint32_t space_id, uint32_t page_no, uint32_t page_size);

    /**
     * @brief Synthesize a Page 0 FSP_HDR page when the original is missing
     *
     * Creates a minimal FSP_HDR with FIL header, FSP header (space_flags,
     * free_limit, free list), and XDES section. The synthesized page allows
     * InnoDB recovery tools to recognize the tablespace.
     */
    static std::vector<uint8_t> synthesize_page0(
        uint32_t space_id, uint32_t page_size);

    /**
     * @brief Write a composed .ibd file from sorted pages
     */
    bool write_composed_ibd(
        const std::string& output_path,
        const std::vector<PageIdentity>& sorted_pages,
        const std::vector<PageGap>& gaps,
        uint32_t page_size);
};

// ==================== Tablespace Composer ====================

/**
 * @brief Higher-level composer that creates recovery-ready .ibd files
 *
 * Handles the full pipeline:
 * 1. Page validation and deduplication
 * 2. Critical metadata reconstruction (Page 0 FSP_HDR synthesis if missing)
 * 3. Gap filling with zero pages
 * 4. SDI page preservation (MySQL 8.0+)
 * 5. Output file generation with proper naming
 */
class TablespaceComposer {
public:
    TablespaceComposer() = default;

    /**
     * @brief Compose tablespace files from identified pages
     *
     * @param pages All identified pages from disk carving
     * @param output_dir Output directory for composed files
     * @param base_name Base name for output files (e.g., "recovered")
     * @return Map of output file paths → space_id
     */
    std::map<std::string, uint32_t> compose(
        const std::vector<PageIdentity>& pages,
        const std::string& output_dir,
        const std::string& base_name = "recovered_space");

    /**
     * @brief Get composition statistics
     */
    const ReassemblyStats& get_stats() const { return reassembler_.get_stats(); }

private:
    PageFragmentReassembler reassembler_;
};

// ==================== Inline Implementations ====================

// (implementations moved to src/carve/page_reassembler.cpp)

} // namespace carve
} // namespace dbrecovery

#endif // DBRECOVERY_CARVE_PAGE_REASSEMBLER_HPP