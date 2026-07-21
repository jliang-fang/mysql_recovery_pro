/**
 * @file trx_sys_timestamp_index.hpp
 * @brief TRX_SYS timestamp index — O(1) trx_id → commit_lsn lookup
 *
 * Algorithm (TRX_SYS page scan):
 *   1. Read ibdata1 page 5 (TRX_SYS page)
 *   2. Iterate 128 rseg slots at offset 108 (8 bytes each: LE space_id + page_no)
 *   3. Read each rseg header page, locate the undo segment header
 *   4. Extract trx_id (6 bytes LE at TRX_UNDO_SEG_HDR+12) and
 *      commit_lsn (8 bytes LE at TRX_UNDO_SEG_HDR+4)
 *   5. Store in std::map<uint64_t, TrxTimestampEntry>
 *
 * MySQL version note:
 *   MySQL 5.6/5.7 have swapped offsets (trx_id at +4, commit_lsn at +12).
 *   This implementation uses the 8.0+ layout by default. If a lookup fails
 *   (trx_id == 0), it falls back to the 5.6/5.7 layout.
 */

#ifndef DBRECOVERY_INNODB_TRX_SYS_TIMESTAMP_INDEX_HPP
#define DBRECOVERY_INNODB_TRX_SYS_TIMESTAMP_INDEX_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <string>
#include <chrono>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <limits>
#include <optional>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief Timestamp entry for a single transaction
 */
struct TrxTimestampEntry {
    uint64_t trx_id = 0;           // Transaction ID (6 bytes)
    uint64_t commit_lsn = 0;       // Commit LSN (8 bytes)
    uint32_t rseg_id = 0;          // Rollback segment ID
    uint32_t undo_page_no = 0;     // Undo segment header page number
    bool is_valid = false;         // Entry was successfully read
};

/**
 * @brief TrxSysTimestampIndex — O(1) trx_id → commit_lsn lookup
 *
 * Builds a mapping from transaction ID to commit LSN by reading the
 * TRX_SYS page and walking rollback segment headers. This enables
 * timestamp inference for undo records in the DeepUndoAnalyzer.
 *
 * Thread safety: not thread-safe for concurrent build(). Read-only
 * methods (lookup, lookup_bulk) are safe if called after build().
 */
class TrxSysTimestampIndex {
public:
    TrxSysTimestampIndex() = default;
    ~TrxSysTimestampIndex() = default;

    // ==================== Build ====================

    /**
     * @brief Build the timestamp index by scanning TRX_SYS and rseg headers
     *
     * Algorithm:
     *   1. Open ibdata1, read page 5 (TRX_SYS)
     *   2. Iterate 128 rseg slots at offset 108
     *   3. For each active slot, read the rseg header page
     *   4. Extract trx_id and commit_lsn from the undo segment header
     *   5. Store in lookup map
     *
     * @param data_file_path  Path to ibdata1
     * @param page_size       Page size in bytes (default 16384)
     * @return true if the index was built successfully
     */
    bool build(const std::string& data_file_path, size_t page_size = 16384);

    /**
     * @brief Build the index from a pre-read TRX_SYS page buffer
     *
     * Allows callers who already have the TRX_SYS page in memory
     * to avoid re-reading it.
     *
     * @param trx_sys_page  Buffer containing the TRX_SYS page data
     * @param data_file_path  Path to ibdata1 (for reading rseg pages)
     * @param page_size       Page size in bytes
     * @return true if the index was built successfully
     */
    bool build_from_buffer(const std::vector<uint8_t>& trx_sys_page,
                           const std::string& data_file_path,
                           size_t page_size = 16384);

    // ==================== Lookup ====================

    /**
     * @brief O(log n) lookup of a single transaction
     * @return TrxTimestampEntry, or nullopt if not found
     */
    std::optional<TrxTimestampEntry> lookup(uint64_t trx_id) const;

    /**
     * @brief Bulk lookup for multiple transaction IDs
     * @param trx_ids  Vector of transaction IDs to look up
     * @return Map of trx_id → TrxTimestampEntry (only found entries)
     */
    std::map<uint64_t, TrxTimestampEntry> lookup_bulk(
        const std::vector<uint64_t>& trx_ids) const;

    // ==================== Management ====================

    /**
     * @brief Number of entries in the index
     */
    size_t size() const { return entries_.size(); }

    /**
     * @brief Check if the index has been built
     */
    bool is_built() const { return built_; }

    /**
     * @brief Reset the index, clearing all data
     */
    void reset();

    /**
     * @brief Get all entries (for iteration)
     */
    const std::map<uint64_t, TrxTimestampEntry>& entries() const {
        return entries_;
    }

    // ==================== Statistics ====================

    struct BuildStats {
        uint64_t total_pages_scanned = 0;
        uint64_t rseg_slots_found = 0;
        uint64_t entries_found = 0;
        double build_time_ms = 0.0;
    };

    const BuildStats& stats() const { return stats_; }

    /**
     * @brief Log a summary of the index build
     */
    void log_summary() const;

private:
    std::map<uint64_t, TrxTimestampEntry> entries_;
    bool built_ = false;
    BuildStats stats_;

    /**
     * @brief Read an rseg header page and extract trx_id/commit_lsn
     *
     * The undo segment header is at TRX_UNDO_SEG_HDR (offset 32) on the
     * first page of each undo segment. Layout:
     *   +0:  TRX_UNDO_STATE (2 bytes)
     *   +4:  TRX_UNDO_LAST_COMMIT (8 bytes, LE) — MySQL 8.0+
     *   +12: TRX_UNDO_TRX_ID (6 bytes, LE) — MySQL 8.0+
     *
     * MySQL 5.6/5.7 swap +4 and +12 (trx_id at +4, commit_lsn at +12).
     * We detect this by checking if trx_id at +12 is 0 and retrying at +4.
     *
     * @param page_data  The rseg header page data
     * @param page_size  Page size
     * @param rseg_id    Rollback segment ID (for stats)
     * @return TrxTimestampEntry if valid, nullopt if no active transaction
     */
    static std::optional<TrxTimestampEntry> read_rseg_undo_seg_header(
        const uint8_t* page_data, size_t page_size, uint32_t rseg_id);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_TRX_SYS_TIMESTAMP_INDEX_HPP