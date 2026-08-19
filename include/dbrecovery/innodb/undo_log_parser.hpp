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
#include "dbrecovery/innodb/roll_ptr_decoder.hpp"

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
     * @brief Parse undo logs from a standalone undo tablespace (MySQL 8.0+)
     * @param undo_tablespace_path Path to undo_001, undo_002, etc.
     * @return All undo log entries found
     */
    std::vector<UndoLogEntry> parse_undo_tablespace(
        const std::string& undo_tablespace_path);

    /**
     * @brief Scan and parse all undo tablespaces in a directory
     * @param data_dir MySQL data directory containing undo_* files
     * @return All undo log entries from all undo tablespaces
     */
    std::vector<UndoLogEntry> parse_all_undo_tablespaces(
        const std::string& data_dir);

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
        uint32_t total_undo_pages = 0;
        uint32_t total_undo_records = 0;
        uint32_t recoverable_records = 0;
        uint32_t transactions_found = 0;
        uint32_t flashback_statements = 0;
    };

    const UndoLogStats& get_stats() const { return stats_; }

    /**
     * @brief v2.0: Deep undo chain traversal — follow DB_ROLL_PTR through all undo records
     *
     * Given a starting undo record (from DB_ROLL_PTR), traverses the full undo chain
     * to reconstruct all historical versions of a row.
     *
     * Chain traversal:
     *   Current row → DB_ROLL_PTR → undo_record#1 (newest)
     *     → prev_undo_ptr → undo_record#2
     *     → ... → TRX_UNDO_INSERT (chain end / oldest version)
     *
     * The roll pointer storage width is pinned by the resolved UndoFormatProfile:
     *   LEGACY_6BYTE (5.6/5.7): 6 bytes big-endian
     *   MYSQL8_7BYTE (8.0+):    7 bytes big-endian
     * UNRESOLVED rejects the entire chain — no implicit width guessing.
     *
     * @param ibdata_path Path to ibdata1 or undo tablespace
     * @param start_page_no Undo page number from DB_ROLL_PTR
     * @param start_offset Offset within undo page from DB_ROLL_PTR
     * @param profile Resolved undo format profile (default: UNRESOLVED)
     * @return Ordered undo entries (oldest first) or empty if chain broken or profile unresolved
     */
    std::vector<UndoLogEntry> traverse_undo_chain(
        const std::string& ibdata_path,
        uint32_t start_page_no,
        uint32_t start_offset,
        UndoFormatProfile profile = UndoFormatProfile::UNRESOLVED);

    /**
     * @brief v2.0: DELETE flashback — generate reverse INSERT from undo logs
     *
     * Scans all undo logs for TRX_UNDO_DEL_MARK records, extracts the old row
     * values, and generates SQL INSERT statements to reverse the deletes.
     *
     * @param ibdata_path Path to ibdata1 or undo tablespace
     * @param table_name Target table name for INSERT generation
     * @param start_time Only flashback deletes after this time (empty = all)
     * @param end_time Only flashback deletes before this time (empty = all)
     * @return Vector of SQL INSERT strings
     */
    std::vector<std::string> flashback_delete(
        const std::string& ibdata_path,
        const std::string& table_name = "",
        const std::string& start_time = "",
        const std::string& end_time = "");

    /**
     * @brief v2.0: Reconstruct complete row from undo log fields
     *
     * Merges old values from undo log with default values for unchanged columns.
     *
     * @param entries Undo entries from a single undo chain (oldest first)
     * @param column_count Expected column count
     * @return Reconstructed row values as strings
     */
    std::vector<std::string> reconstruct_row_from_undo(
        const std::vector<UndoLogEntry>& entries, uint32_t column_count);

    /**
     * @brief v2.0: Heuristic reassembly of broken undo chains
     *
     * When undo pages are partially damaged, chains may break at corruption points.
     * This method heuristically reconnects fragments by:
     * 1. Extracting table_id from each undo record header
     * 2. Matching records by table_id + primary_key prefix
     * 3. Ordering by undo_no within each transaction
     * 4. Using LSN to resolve ambiguous ordering
     *
     * Each reassembled chain gets a confidence score:
     *   - 1.0: perfect chain (no gaps in undo_no sequence)
     *   - 0.5-0.9: matched by PK but gaps exist
     *   - 0.0-0.5: heuristic match with low confidence
     *
     * @param ibdata_path Path to ibdata1 or undo tablespace
     * @return Vector of reassembled chains with confidence scores
     */
    struct ReassembledChain {
        std::vector<UndoLogEntry> entries;  // ordered oldest-first
        double confidence = 0.0;
        uint64_t trx_id = 0;
        uint32_t table_id = 0;
        std::string pk_hex;                 // hex-encoded primary key
        std::vector<uint32_t> gap_positions; // positions where chain breaks occurred
    };

    std::vector<ReassembledChain> heuristic_reassemble_undo_chains(
        const std::string& ibdata_path,
        uint32_t target_table_id = 0);

private:
    UndoLogStats stats_{0, 0, 0, 0};
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_UNDO_LOG_PARSER_HPP
