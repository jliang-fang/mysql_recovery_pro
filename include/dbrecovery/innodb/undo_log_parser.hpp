/**
 * @file undo_log_parser.hpp
 * @brief Undo log parser for recovering TRUNCATE/DROP/DELETE operations
 */

#ifndef DBRECOVERY_INNODB_UNDO_LOG_PARSER_HPP
#define DBRECOVERY_INNODB_UNDO_LOG_PARSER_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include "dbrecovery/innodb/heuristic_parser.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Undo Log Structures ====================

/**
 * @brief Undo log field - contains partial record data from undo log
 */
struct UndoLogField {
    uint8_t type;           // Field type flag
    uint16_t size;          // Field data size
    std::vector<uint8_t> data;  // Field data

    size_t total_size() const { return 1 + (size > 127 ? 2 : 1) + size; }
};

/**
 * @brief Undo log entry - represents a single undo log record
 */
struct UndoLogEntry {
    uint8_t undo_type;      // TRX_UNDO_INSERT/UPDATE/DELETE
    uint16_t page_no;       // Page number of original record
    uint16_t page_offset;   // Offset within page
    uint8_t field_count;    // Number of fields
    std::vector<UndoLogField> fields;

    size_t total_size() const {
        size_t total = 2;  // type + field_count
        for (const auto& f : fields) {
            total += f.total_size();
        }
        return total;
    }
};

/**
 * @brief UndoLogParser - Parser for InnoDB undo logs
 * 
 * This class parses undo log pages from rollback segments to recover
 * records that were deleted by TRUNCATE, DROP, or DELETE operations.
 * 
 * Key features:
 * - Parse UNDO log pages (FIL_PAGE_UNDO_LOG type)
 * - Extract previous versions of updated/deleted records
 * - Reconstruct records from undo log data
 * - Support multiple rollback segments
 */
class UndoLogParser {
public:
    UndoLogParser() = default;
    ~UndoLogParser() = default;

    /**
     * @brief Parse a single undo log page
     * @param page_data Page buffer
     * @param page_size Page size (typically 16384)
     * @param page_no Page number
     * @return Vector of undo log entries
     */
    std::vector<UndoLogEntry> parse_undo_log_page(
        const uint8_t* page_data, size_t page_size, uint32_t page_no);

    /**
     * @brief Parse all undo logs from a rollback segment
     * @param ibdata_path Path to ibdata1 file
     * @param rseg_id Rollback segment ID (0-127)
     * @return All undo log entries from this segment
     */
    std::vector<UndoLogEntry> parse_rollback_segment(
        const std::string& ibdata_path, uint32_t rseg_id);

    /**
     * @brief Recover records from undo logs for a specific table
     * @param ibdata_path Path to ibdata1
     * @param table_space_id Target table space ID
     * @param table_index_id Target table index ID
     * @return Recovered records (marked as deleted)
     */
    std::vector<HeuristicRecord> recover_from_undo_logs(
        const std::string& ibdata_path,
        const std::string& table_space_id,
        uint64_t table_index_id);

    /**
     * @brief Find transaction info by transaction ID
     * @param ibdata_path Path to ibdata1
     * @param target_trx_id Target transaction ID
     * @return Transaction info or nullopt
     */
    // TransactionInfo struct defined in .cpp file for internal use
    std::optional<std::tuple<uint64_t, uint32_t, uint32_t>> find_transaction_by_trx_id(
        const std::string& ibdata_path, uint64_t target_trx_id);

    /**
     * @brief Scan all rollback segments and return total undo log count
     * @param ibdata_path Path to ibdata1
     * @return Total number of undo log entries across all segments
     */
    size_t scan_all_rollback_segments(const std::string& ibdata_path);

    /**
     * @brief Build complete transaction chain from undo logs
     * 
     * Follows the undo log page chain to reconstruct all changes made by a transaction.
     * Essential for point-in-time recovery and transaction rollback.
     * 
     * @param ibdata_path Path to ibdata1
     * @param start_page First undo page of the transaction
     * @param rseg_id Rollback segment ID
     * @return Complete transaction undo log entries in order
     */
    std::vector<UndoLogEntry> build_transaction_chain(
        const std::string& ibdata_path,
        uint32_t start_page,
        uint32_t rseg_id);

    /**
     * @brief Get statistics about undo log analysis
     */
    struct UndoLogStats {
        uint32_t total_undo_pages;     // Total undo log pages scanned
        uint32_t total_undo_records;   // Total undo log records
        uint32_t recoverable_records;  // Records that can be recovered
        uint32_t transactions_found;   // Number of distinct transactions found
    };

    const UndoLogStats& get_stats() const { return stats_; }

private:
    UndoLogStats stats_{0, 0, 0, 0};
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_UNDO_LOG_PARSER_HPP
