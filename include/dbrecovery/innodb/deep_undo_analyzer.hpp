/**
 * @file deep_undo_analyzer.hpp
 * @brief Deep Undo Analysis Engine — MVCC version chain traversal & flashback recovery
 *
 * v2.0 architecture: trace full version history from DB_ROLL_PTR, reconstruct
 * row versions at any point in time, and generate flashback SQL.
 *
 * Algorithm (trace_version_chain):
 *   1. Parse DB_ROLL_PTR → (undo_page_no, undo_page_offset)
 *   2. Read undo page, parse undo record at offset
 *   3. Extract old field values from undo record
 *   4. Follow prev_undo_ptr → next older version
 *   5. Stop at TRX_UNDO_INSERT (oldest version)
 *   6. Return oldest-first chain
 */

#ifndef DBRECOVERY_INNODB_DEEP_UNDO_ANALYZER_HPP
#define DBRECOVERY_INNODB_DEEP_UNDO_ANALYZER_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>
#include <chrono>
#include <set>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <iomanip>

#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Data Structures ====================

/**
 * @brief Undo record field — old value of a single column
 */
struct UndoFieldValue {
    uint16_t field_no;              // 1-based field number within the table
    std::vector<uint8_t> old_value; // Raw bytes of the old value
    bool is_null   = false;         // Field was NULL
    bool is_extern = false;         // Externally stored (BLOB/off-page)

    std::string to_hex() const {
        if (is_null) return "NULL";
        std::ostringstream oss;
        for (auto b : old_value) oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
        return oss.str();
    }
};

/**
 * @brief Enhanced undo record — full version metadata
 *
 * Maps to MySQL's TRX_UNDO record types:
 *   TRX_UNDO_INSERT=1      — row was inserted (chain end / oldest version)
 *   TRX_UNDO_UPDATE=2      — row was updated (generic)
 *   TRX_UNDO_UPD_EXIST_REC=11 — existing record updated
 *   TRX_UNDO_UPD_DEL_REC=12   — deleted record updated (undelete)
 *   TRX_UNDO_DEL_REC=13       — record deleted (delete-mark → purge)
 *   TRX_UNDO_INS_REC=14       — insert by update (split/merge)
 */
struct UndoRecordV2 {
    // Location of this undo record
    uint32_t undo_page_no   = 0;
    uint16_t undo_page_offset = 0;

    // Transaction metadata
    uint64_t trx_id  = 0;
    uint64_t undo_no = 0;          // Monotonic counter within transaction

    // Record type
    enum Type : uint8_t {
        INSERT      = 1,
        UPDATE      = 2,
        DELETE_MARK = 3,           // TRX_UNDO_DEL_REC
        DELETE      = 4            // purge-completed delete
    };
    Type type = UPDATE;

    // Target record (where this undo applies in the tablespace)
    uint32_t table_id   = 0;
    uint64_t index_id   = 0;
    uint32_t page_no    = 0;
    uint16_t page_offset = 0;

    // Chain pointers
    uint64_t roll_ptr      = 0;    // DB_ROLL_PTR of this version
    uint64_t prev_undo_ptr = 0;    // prev_undo_ptr → older version (0 = chain end)

    // Data extracted from undo record
    std::vector<uint8_t> primary_key_raw;   // Primary key bytes (for grouping)
    std::vector<UndoFieldValue> old_fields; // Old values of modified columns
    uint16_t info_bits = 0;

    // Optional timing info (from trx_sys or redo inference)
    std::optional<uint64_t> commit_lsn;
    std::optional<std::chrono::system_clock::time_point> approx_timestamp;

    bool is_insert()    const { return type == INSERT; }
    bool is_delete()    const { return type == DELETE_MARK || type == DELETE; }
    bool is_update()    const { return type == UPDATE; }

    std::string type_str() const {
        switch (type) {
            case INSERT:      return "INSERT";
            case UPDATE:      return "UPDATE";
            case DELETE_MARK: return "DELETE_MARK";
            case DELETE:      return "DELETE";
            default:          return "UNKNOWN";
        }
    }
};

/**
 * @brief Complete version history for a single row
 *
 * Tracks all versions of a row from INSERT through UPDATEs to DELETE.
 * undo_chain is ordered oldest-first (INSERT first, newest undo last).
 */
struct RowVersionHistory {
    // Current version from the B+Tree (most recent committed state)
    std::vector<uint8_t> current_data;
    bool current_is_deleted = false;

    // Historical versions — ordered oldest-first (INSERT → ... → newest)
    std::vector<UndoRecordV2> undo_chain;

    // Metadata
    uint64_t index_id    = 0;
    std::vector<uint8_t> primary_key;
    uint32_t n_versions  = 0;       // Total versions including current
    bool truncated       = false;   // Chain was truncated at 1000 versions

    /**
     * @brief Get the row state at a given point in time
     * @return Raw bytes of the row at that time, or nullopt if not found
     */
    std::optional<std::vector<uint8_t>> get_version_at(
        std::chrono::system_clock::time_point point) const;

    /**
     * @brief Generate INSERT SQL for the latest non-deleted version
     */
    std::string to_insert_sql(const catalog::TableSchema& schema) const;

    /**
     * @brief Number of versions in the undo chain
     */
    size_t chain_length() const { return undo_chain.size(); }
};

/**
 * @brief Flashback SQL result
 */
struct FlashbackResult {
    std::string reverse_sql;                    // The generated SQL statement
    uint64_t trx_id         = 0;                // Transaction that made the change
    uint64_t undo_no        = 0;                // Undo number within transaction
    std::optional<std::chrono::system_clock::time_point> timestamp;
    double confidence       = 0.0;              // 0.0–1.0 data completeness
    bool is_complete        = false;            // All columns recovered
};

/**
 * @brief Filter for flashback operations
 */
struct FlashbackFilter {
    std::optional<uint64_t> start_lsn;
    std::optional<uint64_t> end_lsn;
    std::optional<uint64_t> trx_id;
    std::optional<std::chrono::system_clock::time_point> start_time;
    std::optional<std::chrono::system_clock::time_point> end_time;
    uint32_t max_records = 10000;               // Safety limit
    bool include_incomplete = false;            // Include rows with missing columns
};

// ==================== Helper Classes ====================

/**
 * @brief Undo page reader — reads pages from ibdata1 or undo tablespace
 */
class UndoPageManager {
public:
    explicit UndoPageManager(size_t page_size = 16384)
        : page_size_(page_size) {}

    /**
     * @brief Set the data file path (ibdata1 or undo tablespace)
     */
    void set_data_file(const std::string& path) {
        data_file_path_ = path;
    }

    /**
     * @brief Read a single page from the data file
     * @param page_no Page number to read
     * @return Page buffer, or empty on error
     */
    std::vector<uint8_t> read_undo_page(uint32_t page_no);

    /**
     * @brief Check if the data file is accessible
     */
    bool is_open() const;

    /**
     * @brief Get total number of pages in the data file
     */
    uint64_t get_page_count() const;

    size_t page_size() const { return page_size_; }

private:
    std::string data_file_path_;
    size_t page_size_;
    std::ifstream file_;
    bool opened_ = false;
    uint64_t page_count_ = 0;

    bool ensure_open();
};

/**
 * @brief Undo record V2 parser — reads an UndoRecordV2 from raw page data
 */
class UndoRecordV2Parser {
public:
    /**
     * @brief Parse an undo record at a given offset within an undo page
     *
     * @param page_data  Raw page buffer
     * @param page_size  Page size
     * @param offset     Byte offset within the page where the undo record starts
     * @param table_schema  Optional table schema for field decoding
     * @return Parsed UndoRecordV2, or nullopt if parsing fails
     */
    std::optional<UndoRecordV2> parse(
        const uint8_t* page_data,
        size_t page_size,
        size_t offset,
        const catalog::TableSchema* table_schema = nullptr,
        size_t* consumed_size = nullptr);

    /**
     * @brief Extract prev_undo_ptr from the undo record's field data
     *
     * In MySQL undo logs, the old DB_ROLL_PTR value is stored as one of
     * the field values. This helper extracts it from the raw field bytes.
     *
     * @param field_data  The raw field data bytes
     * @return Decoded prev_undo_ptr (0 = no previous version)
     */
    static uint64_t extract_prev_ptr_from_field(const std::vector<uint8_t>& field_data);

private:
    /**
     * @brief Read a single field from the undo record at the given position
     * @return (field_data, bytes_consumed) or empty if error
     */
    std::optional<std::pair<std::vector<uint8_t>, size_t>> read_field(
        const uint8_t* data, size_t data_size, size_t offset);
};

// ==================== Core Engine ====================

/**
 * @brief DeepUndoAnalyzer — MVCC version chain traversal & flashback engine
 *
 * Capabilities:
 *   1. trace_version_chain()  — Follow DB_ROLL_PTR through full undo chain
 *   2. rebuild_all_histories() — Scan all undo logs for a table's history
 *   3. flashback_deletes()    — Generate reverse INSERT for DELETE operations
 *   4. heuristic_reconstruct_chains() — Reassemble broken/corrupted chains
 *
 * Design:
 *   - Uses UndoPageManager for page I/O abstraction
 *   - Uses UndoRecordV2Parser for undo record parsing
 *   - All methods are thread-safe (no shared mutable state in public methods)
 */
class DeepUndoAnalyzer {
public:
    DeepUndoAnalyzer();
    ~DeepUndoAnalyzer() = default;

    // ==================== Configuration ====================

    /**
     * @brief Set the ibdata1 path
     */
    void set_ibdata_path(const std::string& path) {
        ibdata_path_ = path;
        page_manager_.set_data_file(path);
    }

    /**
     * @brief Set page size (default 16384)
     */
    void set_page_size(size_t page_size) {
        page_size_ = page_size;
    }

    /**
     * @brief Set max chain traversal depth (default 1000)
     */
    void set_max_chain_depth(uint32_t depth) {
        max_chain_depth_ = depth;
    }

    // ==================== Core API ====================

    /**
     * @brief Trace full undo version chain for a single row
     *
     * Algorithm (44-step, from architecture v2.0 blueprint):
     *   1.  Parse DB_ROLL_PTR → (undo_page_no, undo_page_offset)
     *   2.  Read undo page, parse undo record at offset
     *   3.  Extract old field values from undo record
     *   4.  Follow prev_undo_ptr → next older version
     *   5.  Stop at TRX_UNDO_INSERT (oldest version) or prev_undo_ptr=0
     *   6.  Return oldest-first chain
     *
     * @param current_record  Current B+Tree record raw bytes
     * @param roll_ptr        DB_ROLL_PTR value from the current record
     * @param schema          Table schema (for field decoding)
     * @param index_id        Clustered index ID
     * @return Complete version history
     */
    RowVersionHistory trace_version_chain(
        const std::vector<uint8_t>& current_record,
        uint64_t roll_ptr,
        const catalog::TableSchema& schema,
        uint64_t index_id);

    /**
     * @brief Rebuild all version histories for a target table
     *
     * Scans ibdata1 and undo tablespaces for undo records belonging to
     * the target space_id, groups by primary key, and builds full
     * version chains for every row.
     *
     * @param space_id  Target tablespace ID
     * @param schema    Table schema
     * @param index_id  Clustered index ID
     * @param lsn_range Optional LSN range filter
     * @return Map of PK hex → RowVersionHistory
     */
    std::map<std::string, RowVersionHistory> rebuild_all_histories(
        uint32_t space_id,
        const catalog::TableSchema& schema,
        uint64_t index_id,
        std::optional<std::pair<uint64_t, uint64_t>> lsn_range = std::nullopt);

    /**
     * @brief Flashback DELETE operations — generate reverse INSERT SQL
     *
     * Algorithm:
     *   1. Scan all undo logs for TRX_UNDO_DEL_REC records
     *   2. For each DELETE record:
     *      a. Parse old field values from undo record
     *      b. Follow update undo chain to reconstruct full row
     *      c. Generate INSERT INTO ... VALUES (...)
     *   3. Order by transaction ID and timestamp
     *
     * @param space_id  Target tablespace ID
     * @param schema    Table schema
     * @param filter    Time/TRX_ID filter
     * @return Flashback SQL results
     */
    std::vector<FlashbackResult> flashback_deletes(
        uint32_t space_id,
        const catalog::TableSchema& schema,
        const FlashbackFilter& filter = {});

    /**
     * @brief Heuristically reassemble broken undo chains
     *
     * When undo pages are partially damaged, chains may break.
     * This method:
     *   1. Collects all undo records
     *   2. Groups by table_id + primary_key prefix
     *   3. Sorts by undo_no within each group
     *   4. Calculates confidence scores
     *
     * @param space_id  Target tablespace ID (0 = all)
     * @param schema    Table schema
     * @return Reassembled version histories
     */
    std::vector<RowVersionHistory> heuristic_reconstruct_chains(
        uint32_t space_id,
        const catalog::TableSchema& schema);

    // ==================== Statistics ====================

    struct AnalyzerStats {
        uint64_t chains_traced         = 0;
        uint64_t total_undo_records    = 0;
        uint64_t broken_chains         = 0;
        uint64_t flashback_statements  = 0;
        uint64_t pages_scanned         = 0;
        double   analysis_time_ms      = 0.0;
    };

    const AnalyzerStats& get_stats() const { return stats_; }
    void reset_stats() { stats_ = AnalyzerStats{}; }

    /**
     * @brief Scan all undo pages in ibdata1
     * @return Vector of (page_data, page_no) for all UNDO_LOG pages
     */
    std::vector<std::pair<std::vector<uint8_t>, uint32_t>> scan_undo_pages();

    /**
     * @brief Extract table_id from undo record fields
     */
    static uint32_t extract_table_id(const std::vector<UndoFieldValue>& fields);

private:
    // Configuration
    std::string ibdata_path_;
    size_t page_size_ = 16384;
    uint32_t max_chain_depth_ = 1000;

    // Helpers
    UndoPageManager page_manager_;
    UndoRecordV2Parser record_parser_;
    AnalyzerStats stats_;

    // Internal helpers

    /**
     * @brief Parse DB_ROLL_PTR into (page_no, offset)
     *
     * MySQL 5.6/5.7: 6-byte DB_ROLL_PTR stored as [4B page_no, 2B offset] (LE)
     * MySQL 8.0+: 7-byte DB_ROLL_PTR stored as [4B page_no, 2B offset, 1B flags] (LE)
     * When loaded as uint64_t in LE: bits [31:0] = page_no, bits [47:32] = offset
     * The 7th byte (flags in 8.0+) occupies bits [55:48] and is masked out.
     *
     * @param roll_ptr  The DB_ROLL_PTR value (6 or 7 bytes loaded as uint64_t)
     * @return (page_no, offset_within_page)
     */
    static std::pair<uint32_t, uint32_t> parse_roll_ptr(uint64_t roll_ptr) {
        // MySQL 5.6/5.7: 6-byte DB_ROLL_PTR = [4B page_no, 2B offset] (LE)
        // MySQL 8.0+: 7-byte DB_ROLL_PTR = [4B page_no, 2B offset, 1B flags] (LE)
        // roll_ptr is stored as: lower 32 bits = page_no, bits 32-47 = offset
        uint32_t page_no = static_cast<uint32_t>(roll_ptr & 0xFFFFFFFF);
        uint32_t offset  = static_cast<uint32_t>((roll_ptr >> 32) & 0xFFFF);
        return {page_no, offset};
    }

    /**
     * @brief Read a 6-byte prev_undo_ptr from page data at offset
     *
     * Format: [4 bytes page_no, 2 bytes offset]
     * Returns as uint64_t: ((uint64_t)page_no << 32) | offset
     */
    static uint64_t read_prev_undo_ptr(const uint8_t* page_data, size_t offset) {
        uint32_t page_no = read_le<uint32_t>(page_data, offset);
        uint32_t off     = read_le<uint16_t>(page_data, offset + 4);
        if (page_no == 0 || page_no == FIL_NULL) return 0;
        return static_cast<uint64_t>(page_no) | (static_cast<uint64_t>(off) << 32);
    }

    /**
     * @brief Encode primary key as hex string
     */
    static std::string pk_to_hex(const std::vector<uint8_t>& pk_raw);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_DEEP_UNDO_ANALYZER_HPP