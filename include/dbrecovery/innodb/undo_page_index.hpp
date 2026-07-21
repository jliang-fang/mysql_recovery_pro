/**
 * @file undo_page_index.hpp
 * @brief UndoPageIndex — O(1) undo page lookup for InnoDB recovery
 *
 * Architecture v2.0 Phase 5: eliminates the O(n) full-table scan of ibdata1
 * on every call to scan_undo_pages(). The index is built once and reused
 * across all undo-based operations (trace_version_chain, flashback_deletes,
 * rebuild_all_histories, heuristic_reconstruct_chains).
 *
 * Algorithm:
 *   1. Build: scan ibdata1 pages sequentially, check FIL_PAGE_TYPE == UNDO_LOG
 *   2. Lookup: O(1) via std::set<uint32_t>
 *   3. Iteration: O(m) via std::vector<uint32_t> (m = number of undo pages)
 */

#ifndef DBRECOVERY_INNODB_UNDO_PAGE_INDEX_HPP
#define DBRECOVERY_INNODB_UNDO_PAGE_INDEX_HPP

#include <cstdint>
#include <vector>
#include <set>
#include <string>
#include <chrono>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <limits>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief O(1) undo page index for InnoDB recovery
 *
 * Builds an index of all UNDO_LOG pages in the system tablespace (ibdata1)
 * by scanning once. Subsequent lookups and iteration are O(1) and O(m)
 * respectively, where m is the number of undo pages (typically < 0.1% of
 * total pages).
 *
 * Thread safety: not thread-safe for concurrent build(). Read-only methods
 * (is_undo_page, undo_pages) are safe if called after build() completes.
 */
class UndoPageIndex {
public:
    UndoPageIndex() = default;
    ~UndoPageIndex() = default;

    // ==================== Build ====================

    /**
     * @brief Build the index by scanning all pages in the data file
     *
     * Opens the file, reads each page header, and records page numbers
     * where FIL_PAGE_TYPE == FIL_PAGE_UNDO_LOG (0x45B2).
     *
     * @param data_file_path  Path to ibdata1 or undo tablespace file
     * @param page_size       Page size in bytes (default 16384)
     * @param progress_interval  Log progress every N pages (0 = silent)
     * @return true if the index was built successfully (even if 0 undo pages found)
     */
    bool build(const std::string& data_file_path,
               size_t page_size = 16384,
               uint32_t progress_interval = 0) {
        auto start = std::chrono::high_resolution_clock::now();
        reset();

        // Open the data file
        std::ifstream file(data_file_path, std::ios::binary | std::ios::ate);
        if (!file) {
            std::cerr << "[UndoPageIndex] Cannot open: " << data_file_path << std::endl;
            return false;
        }

        // Calculate total pages
        const auto end_pos = file.tellg();
        if (end_pos < 0) {
            file.close();
            return false;
        }

        total_pages_ = static_cast<uint64_t>(end_pos) / page_size;
        if (total_pages_ == 0) {
            file.close();
            return false;
        }

        // Buffer for reading page headers (only need first 26 bytes for FIL header)
        // FIL_PAGE_TYPE is at offset 24, 2 bytes big-endian
        constexpr size_t HEADER_SIZE = 26;
        std::vector<uint8_t> header(HEADER_SIZE);

        file.seekg(0, std::ios::beg);

        // Scan all pages
        for (uint64_t pg = 0; pg < total_pages_; pg++) {
            // Read page header
            file.read(reinterpret_cast<char*>(header.data()),
                      static_cast<std::streamsize>(HEADER_SIZE));
            if (file.gcount() != static_cast<std::streamsize>(HEADER_SIZE)) {
                break;
            }

            // Check page type
            uint16_t page_type = read_be<uint16_t>(header.data(), 24);
            if (page_type == FIL_PAGE_UNDO_LOG) {
                if (pg <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
                    uint32_t page_no = static_cast<uint32_t>(pg);
                    undo_page_set_.insert(page_no);
                    undo_pages_.push_back(page_no);
                }
            }

            // Skip to next page
            if (pg + 1 < total_pages_) {
                // Skip the rest of the page after the header we already read
                if (page_size > HEADER_SIZE) {
                    file.seekg(static_cast<std::streamoff>(page_size - HEADER_SIZE),
                               std::ios::cur);
                }
            }

            // Progress logging
            if (progress_interval > 0 && (pg + 1) % progress_interval == 0) {
                std::cerr << "\r[UndoPageIndex] Scanning: " << (pg + 1) << "/" << total_pages_
                          << " pages (" << undo_pages_.size() << " undo found)"
                          << std::flush;
            }
        }

        file.close();

        if (progress_interval > 0) {
            std::cerr << std::endl;
        }

        // Record stats
        stats_.total_pages_scanned = static_cast<uint64_t>(total_pages_);
        stats_.undo_pages_found = undo_pages_.size();

        auto end = std::chrono::high_resolution_clock::now();
        stats_.build_time_ms = std::chrono::duration<double, std::milli>(
            end - start).count();

        return true;
    }

    // ==================== Lookup ====================

    /**
     * @brief O(1) check if a page is an undo log page
     */
    bool is_undo_page(uint32_t page_no) const {
        return undo_page_set_.find(page_no) != undo_page_set_.end();
    }

    /**
     * @brief Get all undo page numbers (ordered by page_no)
     *
     * Pages are naturally sorted since they're added in ascending order
     * during the sequential scan.
     */
    const std::vector<uint32_t>& undo_pages() const {
        return undo_pages_;
    }

    /**
     * @brief Number of undo pages in the index
     */
    size_t size() const {
        return undo_pages_.size();
    }

    /**
     * @brief Check if the index has been built
     */
    bool is_built() const {
        return total_pages_ > 0 || !undo_pages_.empty();
    }

    /**
     * @brief Total pages in the data file
     */
    uint64_t total_pages() const {
        return total_pages_;
    }

    // ==================== Management ====================

    /**
     * @brief Reset the index, clearing all data
     */
    void reset() {
        undo_page_set_.clear();
        undo_pages_.clear();
        total_pages_ = 0;
        stats_ = BuildStats{};
    }

    // ==================== Statistics ====================

    struct BuildStats {
        uint64_t total_pages_scanned = 0;
        uint64_t undo_pages_found = 0;
        double build_time_ms = 0.0;
    };

    const BuildStats& stats() const {
        return stats_;
    }

    /**
     * @brief Log a summary of the index build
     */
    void log_summary() const {
        std::cerr << "[UndoPageIndex] " << undo_pages_.size()
                  << " undo pages found in " << total_pages_
                  << " total pages (" << stats_.build_time_ms << " ms)"
                  << std::endl;
    }

private:
    std::set<uint32_t> undo_page_set_;      // O(1) membership
    std::vector<uint32_t> undo_pages_;      // Ordered iteration
    uint64_t total_pages_ = 0;              // Total pages in data file
    BuildStats stats_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_UNDO_PAGE_INDEX_HPP