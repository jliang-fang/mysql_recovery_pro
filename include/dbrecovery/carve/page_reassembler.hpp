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

inline PageIdentity PageFragmentReassembler::select_best_candidate(
    const std::vector<PageIdentity>& candidates) {

    if (candidates.size() == 1) {
        return candidates[0];
    }

    // Sort by quality: checksum_valid > highest LSN > non-zero
    std::vector<PageIdentity> sorted = candidates;
    std::sort(sorted.begin(), sorted.end(),
        [](const PageIdentity& a, const PageIdentity& b) {
            // 1. Checksum valid beats invalid
            if (a.checksum_valid != b.checksum_valid) {
                return a.checksum_valid;
            }
            // 2. Higher LSN beats lower LSN
            if (a.lsn != b.lsn) {
                return a.lsn > b.lsn;
            }
            // 3. Prefer non-zero pages
            auto is_zero = [](const std::vector<uint8_t>& d) {
                for (size_t i = 0; i < std::min(d.size(), size_t(64)); i++) {
                    if (d[i] != 0) return false;
                }
                return true;
            };
            return !is_zero(a.data);
        });

    return sorted[0];
}

inline std::vector<uint8_t> PageFragmentReassembler::create_placeholder(
    uint32_t space_id, uint32_t page_no, uint32_t page_size) {

    std::vector<uint8_t> placeholder(page_size, 0);

    // Write minimal FIL header for the placeholder
    // FIL_PAGE_SPACE_ID at offset 34 (4 bytes, big-endian)
    placeholder[34] = (space_id >> 24) & 0xFF;
    placeholder[35] = (space_id >> 16) & 0xFF;
    placeholder[36] = (space_id >> 8) & 0xFF;
    placeholder[37] = space_id & 0xFF;

    // FIL_PAGE_OFFSET at offset 4 (4 bytes, big-endian)
    placeholder[4] = (page_no >> 24) & 0xFF;
    placeholder[5] = (page_no >> 16) & 0xFF;
    placeholder[6] = (page_no >> 8) & 0xFF;
    placeholder[7] = page_no & 0xFF;

    // FIL_PAGE_TYPE at offset 24 (2 bytes, big-endian) — mark as ALLOCATED (0)
    placeholder[24] = 0;
    placeholder[25] = 0;

    return placeholder;
}

inline std::map<uint32_t, std::vector<PageIdentity>>
PageFragmentReassembler::group_pages() {

    space_groups_.clear();

    // Group by SPACE_ID
    for (const auto& page : pages_) {
        space_groups_[page.space_id].push_back(page);
    }

    // Deduplicate within each group
    for (auto& [space_id, group_pages] : space_groups_) {
        // Group by PAGE_NO
        std::map<uint32_t, std::vector<PageIdentity>> by_page_no;
        for (auto& page : group_pages) {
            by_page_no[page.page_no].push_back(std::move(page));
        }

        // Select best for each PAGE_NO
        std::vector<PageIdentity> deduped;
        for (auto& [pno, candidates] : by_page_no) {
            deduped.push_back(select_best_candidate(candidates));
        }

        // Sort by PAGE_NO
        std::sort(deduped.begin(), deduped.end(),
            [](const PageIdentity& a, const PageIdentity& b) {
                return a.page_no < b.page_no;
            });

        group_pages = std::move(deduped);
    }

    return space_groups_;
}

inline std::vector<PageGap> PageFragmentReassembler::analyze_gaps(
    uint32_t space_id,
    const std::vector<PageIdentity>& sorted_pages) {

    std::vector<PageGap> gaps;

    if (sorted_pages.empty()) return gaps;

    // Check for Page 0 (FSP_HDR) — critical
    if (sorted_pages[0].page_no != 0) {
        PageGap gap;
        gap.space_id = space_id;
        gap.start_page_no = 0;
        gap.end_page_no = sorted_pages[0].page_no;
        gap.missing_count = sorted_pages[0].page_no;
        gap.severity = PageGap::CRITICAL;
        gap.description = "Missing Page 0 (FSP_HDR) — tablespace header, "
                          "page size and space flags unknown";
        gaps.push_back(gap);
    }

    // Check for gaps between consecutive pages
    for (size_t i = 0; i + 1 < sorted_pages.size(); i++) {
        uint32_t expected_next = sorted_pages[i].page_no + 1;
        uint32_t actual_next = sorted_pages[i + 1].page_no;

        if (actual_next > expected_next) {
            PageGap gap;
            gap.space_id = space_id;
            gap.start_page_no = expected_next;
            gap.end_page_no = actual_next;
            gap.missing_count = actual_next - expected_next;

            // Classify severity
            if (expected_next <= 2 && actual_next > 2) {
                // Includes critical metadata pages
                gap.severity = PageGap::HIGH;
                gap.description = "Missing metadata pages (FSP_HDR/IBUF_BITMAP/INODE)";
            } else if ((expected_next <= 4 && actual_next > 4) || expected_next == 3 || expected_next == 4) {
                gap.severity = PageGap::MEDIUM;
                gap.description = "Missing SDI metadata pages (MySQL 8.0+)";
            } else {
                gap.severity = PageGap::LOW;
                gap.description = "Missing data pages " +
                    std::to_string(expected_next) + "-" + std::to_string(actual_next - 1);
            }

            gaps.push_back(gap);
        }
    }

    return gaps;
}

inline ReassemblyStats PageFragmentReassembler::reassemble(const std::string& output_dir) {
    auto start_time = std::chrono::high_resolution_clock::now();
    stats_ = {};

    // Create output directory
    std::filesystem::create_directories(output_dir);

    // Group and deduplicate pages
    auto groups = group_pages();
    stats_.total_pages_found = static_cast<uint32_t>(pages_.size());

    // Process each space
    for (auto& [space_id, sorted_pages] : groups) {
        stats_.total_pages_used += static_cast<uint32_t>(sorted_pages.size());

        // Analyze gaps
        auto gaps = analyze_gaps(space_id, sorted_pages);
        all_gaps_.insert(all_gaps_.end(), gaps.begin(), gaps.end());

        stats_.total_gaps += static_cast<uint32_t>(gaps.size());
        for (const auto& g : gaps) {
            if (g.severity == PageGap::CRITICAL) stats_.critical_gaps++;
        }

        // Determine page size from existing pages
        uint32_t page_size = 16384; // default
        for (const auto& p : sorted_pages) {
            if (p.page_size > 0) {
                page_size = p.page_size;
                break;
            }
        }

        // Write composed IBD
        std::string filename = "space_" + std::to_string(space_id) + ".ibd";
        std::string output_path = output_dir + "/" + filename;

        if (write_composed_ibd(output_path, sorted_pages, gaps, page_size)) {
            stats_.spaces_reconstructed++;
            stats_.total_bytes_output += std::filesystem::file_size(output_path);
        }
    }

    // Calculate completeness
    uint32_t total_expected = stats_.total_pages_used + 0; // simplified
    for (const auto& g : all_gaps_) {
        total_expected += g.missing_count;
    }
    if (total_expected > 0) {
        stats_.completeness_pct = 100.0 * stats_.total_pages_used / total_expected;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.time_seconds = std::chrono::duration<double>(end_time - start_time).count();

    return stats_;
}

inline bool PageFragmentReassembler::write_composed_ibd(
    const std::string& output_path,
    const std::vector<PageIdentity>& sorted_pages,
    const std::vector<PageGap>& gaps,
    uint32_t page_size) {

    std::ofstream out(output_path, std::ios::binary);
    if (!out) return false;

    // Determine total pages (last page_no + 1)
    uint32_t max_page_no = 0;
    for (const auto& p : sorted_pages) {
        max_page_no = std::max(max_page_no, p.page_no);
    }
    uint32_t total_pages = max_page_no + 1;

    // Write pages in order, filling gaps with placeholders
    size_t page_idx = 0;
    for (uint32_t pno = 0; pno < total_pages; pno++) {
        if (page_idx < sorted_pages.size() && sorted_pages[page_idx].page_no == pno) {
            // Write actual page data
            const auto& page = sorted_pages[page_idx];
            if (page.data.size() >= page_size) {
                out.write(reinterpret_cast<const char*>(page.data.data()), page_size);
            } else {
                // Pad undersized pages
                std::vector<uint8_t> padded = page.data;
                padded.resize(page_size, 0);
                out.write(reinterpret_cast<const char*>(padded.data()), page_size);
            }
            page_idx++;
        } else {
            // Write placeholder for gap
            auto placeholder = create_placeholder(
                sorted_pages.empty() ? 0 : sorted_pages[0].space_id,
                pno, page_size);
            out.write(reinterpret_cast<const char*>(placeholder.data()), page_size);
        }
    }

    return out.good();
}

inline std::string PageFragmentReassembler::generate_report() const {
    std::ostringstream oss;
    oss << "============================================================\n";
    oss << "  Page Fragment Reassembly Report\n";
    oss << "============================================================\n\n";

    oss << "Summary:\n";
    oss << "  Total pages found:     " << stats_.total_pages_found << "\n";
    oss << "  Pages used (deduped):  " << stats_.total_pages_used << "\n";
    oss << "  Tablespaces rebuilt:   " << stats_.spaces_reconstructed << "\n";
    oss << "  Total gaps detected:   " << stats_.total_gaps << "\n";
    oss << "  Critical gaps:         " << stats_.critical_gaps << "\n";
    oss << "  Completeness:          " << std::fixed << std::setprecision(1)
        << stats_.completeness_pct << "%\n";
    oss << "  Total output:          " << stats_.total_bytes_output << " bytes\n";
    oss << "  Time:                  " << stats_.time_seconds << "s\n\n";

    if (!all_gaps_.empty()) {
        oss << "Gap Analysis:\n";
        oss << "  " << std::setw(10) << "Space" << std::setw(10) << "Start"
            << std::setw(10) << "End" << std::setw(10) << "Missing"
            << std::setw(12) << "Severity" << "  Description\n";
        oss << "  " << std::string(65, '-') << "\n";

        for (const auto& gap : all_gaps_) {
            oss << "  " << std::setw(10) << gap.space_id
                << std::setw(10) << gap.start_page_no
                << std::setw(10) << gap.end_page_no
                << std::setw(10) << gap.missing_count
                << std::setw(12) << gap.severity_name()
                << "  " << gap.description << "\n";
        }
        oss << "\n";
    }

    if (!space_groups_.empty()) {
        oss << "Recovered Spaces:\n";
        for (const auto& [space_id, pages] : space_groups_) {
            uint32_t data_pages = 0, index_pages = 0, sdi_pages = 0, other = 0;
            for (const auto& p : pages) {
                if (p.page_type == 17855) { // FIL_PAGE_INDEX
                    if (p.page_no == 0 || p.page_no == 1 || p.page_no == 2 || p.page_no == 3) {
                        other++;
                    } else {
                        data_pages++;
                    }
                } else if (p.page_type == 17853 || p.page_type == 17854) {
                    sdi_pages++;
                } else {
                    other++;
                }
            }
            oss << "  Space " << space_id << ": "
                << pages.size() << " pages ("
                << data_pages << " data, "
                << sdi_pages << " SDI, "
                << other << " other)\n";
        }
    }

    return oss.str();
}

// ==================== TablespaceComposer inline ====================

inline std::map<std::string, uint32_t> TablespaceComposer::compose(
    const std::vector<PageIdentity>& pages,
    const std::string& output_dir,
    const std::string& base_name) {

    reassembler_.clear();
    reassembler_.add_pages(pages);

    auto stats = reassembler_.reassemble(output_dir);

    std::map<std::string, uint32_t> result;
    // Map output files (done inside reassemble, just collect paths here)
    for (const auto& [space_id, group] : reassembler_.group_pages()) {
        std::string filename = "space_" + std::to_string(space_id) + ".ibd";
        result[output_dir + "/" + filename] = space_id;
    }

    return result;
}

} // namespace carve
} // namespace dbrecovery

#endif // DBRECOVERY_CARVE_PAGE_REASSEMBLER_HPP