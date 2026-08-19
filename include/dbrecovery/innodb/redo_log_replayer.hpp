#ifndef DBRECOVERY_INNODB_REDO_LOG_REPLAYER_HPP
#define DBRECOVERY_INNODB_REDO_LOG_REPLAYER_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <memory>
#include "dbrecovery/innodb/redo_log.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Phase 5f: MLOG Whitelist ====================

/// @brief Set of MLOG types considered safe for redo replay
/// Only known, understood MLOG types pass the whitelist gate.
constexpr std::initializer_list<uint8_t> REDO_MLOG_WHITELIST = {
    MLOG_1BYTE,
    MLOG_2BYTES,
    MLOG_4BYTES,
    MLOG_8BYTES,
    MLOG_REC_INSERT,
    MLOG_REC_UPDATE,
    MLOG_REC_DELETE,
    MLOG_PAGE_CREATE,
    MLOG_PAGE_REINIT,
    MLOG_INDEX_ALLOC,
    MLOG_COMP_REC_INSERT,
    MLOG_COMP_REC_UPDATE,
    MLOG_COMP_REC_DELETE,
    MLOG_COMP_PAGE_CREATE,
    MLOG_COMP_PAGE_REINIT,
    MLOG_BLOB_WRITE,
    MLOG_MULTI_REC_END,
    MLOG_CHECKPOINT,
    MLOG_INDEX_LOAD
};

/// @brief Phase 5f: Stable MLOG type name for CLI/report output (header-visible)
inline const char* mlog_type_name_static(uint8_t type) {
    switch (type) {
        case MLOG_1BYTE:                 return "MLOG_1BYTE";
        case MLOG_2BYTES:                return "MLOG_2BYTES";
        case MLOG_4BYTES:                return "MLOG_4BYTES";
        case MLOG_8BYTES:                return "MLOG_8BYTES";
        case MLOG_REC_INSERT:            return "MLOG_REC_INSERT";
        case MLOG_REC_UPDATE:            return "MLOG_REC_UPDATE";
        case MLOG_REC_DELETE:            return "MLOG_REC_DELETE";
        case MLOG_PAGE_CREATE:           return "MLOG_PAGE_CREATE";
        case MLOG_PAGE_REINIT:           return "MLOG_PAGE_REINIT";
        case MLOG_INDEX_ALLOC:           return "MLOG_INDEX_ALLOC";
        case MLOG_COMP_REC_INSERT:       return "MLOG_COMP_REC_INSERT";
        case MLOG_COMP_REC_UPDATE:       return "MLOG_COMP_REC_UPDATE";
        case MLOG_COMP_REC_DELETE:       return "MLOG_COMP_REC_DELETE";
        case MLOG_COMP_PAGE_CREATE:      return "MLOG_COMP_PAGE_CREATE";
        case MLOG_COMP_PAGE_REINIT:      return "MLOG_COMP_PAGE_REINIT";
        case MLOG_BLOB_WRITE:            return "MLOG_BLOB_WRITE";
        case MLOG_MULTI_REC_END:         return "MLOG_MULTI_REC_END";
        case MLOG_CHECKPOINT:            return "MLOG_CHECKPOINT";
        case MLOG_INDEX_LOAD:            return "MLOG_INDEX_LOAD";
        default:                          return "MLOG_UNKNOWN";
    }
}

inline bool is_mlog_whitelisted(uint8_t type) {
    for (auto t : REDO_MLOG_WHITELIST) {
        if (t == type) return true;
    }
    return false;
}

// ==================== Phase 5f: Inventory Types ====================

/// @brief Per-page inventory entry keyed by (space_id, page_no)
struct RedoPageInventoryEntry {
    uint32_t space_id;
    uint32_t page_no;
    uint64_t first_lsn;
    uint64_t last_lsn;
    uint32_t total_records;
    uint32_t whitelisted_records;
    uint32_t non_whitelisted_records;
    std::map<uint8_t, uint32_t> type_distribution;
    bool has_base_page;              // base page was provided for identity check
    bool base_page_identity_match;   // FIL_PAGE_SPACE_ID + FIL_PAGE_OFFSET match
};

/// @brief Full redo log inventory (always produced, no write side effect)
struct RedoLogInventory {
    std::vector<RedoPageInventoryEntry> page_entries;
    uint64_t total_records;
    uint64_t whitelisted_records;
    uint64_t non_whitelisted_records;
    uint64_t start_lsn;
    uint64_t end_lsn;
    uint32_t unique_space_ids;
    uint32_t unique_page_numbers;
    std::map<uint8_t, uint32_t> global_type_distribution;

    bool empty() const { return total_records == 0; }
};

// ==================== Phase 5f: Patch Plan Types ====================

/// @brief A single ordered patch entry (provenance-tracked)
struct RedoPatchPlanEntry {
    uint32_t sequence;           // order in plan (0-based)
    uint32_t space_id;
    uint32_t page_no;
    uint64_t lsn;
    uint8_t  mlog_type;
    std::string mlog_name;
    size_t   data_size;
    bool     whitelisted;
    bool     has_base_page;
    bool     base_page_identity_match;
    std::string provenance;      // e.g. "MLOG_COMP_REC_INSERT @ LSN 12345"
};

/// @brief Ordered patch plan with pre-condition results
struct RedoPatchPlan {
    std::vector<RedoPatchPlanEntry> entries;
    bool records_sorted_by_lsn;
    bool all_whitelisted;
    bool all_pages_have_identity_match;
    uint64_t lsn_gap_count;      // number of LSN gaps detected
    uint32_t unknown_mlog_count; // MLOG types not in whitelist
    std::vector<std::string> warnings;

    bool can_apply() const {
        return records_sorted_by_lsn && all_whitelisted && all_pages_have_identity_match;
    }
};

// ==================== Phase 5f: Pre-condition Checks ====================

/// @brief Individual pre-condition check results
struct RedoPreConditionResult {
    bool log_format_valid;           // records can be parsed
    bool lsn_monotonic;              // records are in non-decreasing LSN order
    bool has_checkpoint;             // at least one MLOG_CHECKPOINT present
    bool all_whitelisted;            // all MLOG types are in whitelist
    bool base_pages_identity_match;  // all base pages have matching FIL headers
    bool checksum_available;         // base pages have valid checksums (if verify_checksums=true)
    bool pages_available;            // base pages were loaded for identity check

    bool all_passed() const {
        return log_format_valid && lsn_monotonic && all_whitelisted &&
               base_pages_identity_match && pages_available &&
               has_checkpoint && checksum_available;
    }

    std::string to_string() const;
};

// ==================== RedoReplayResult (extended) ====================

/// @brief Redo log replay result (extended with Phase 5f fields)
struct RedoReplayResult {
    bool success;
    std::string message;
    uint32_t records_processed;
    uint32_t records_applied;
    uint32_t records_failed;
    std::vector<std::string> errors;
    // Phase 5f: inventory and plan attached to result
    RedoLogInventory inventory;
    RedoPatchPlan patch_plan;
    RedoPreConditionResult pre_conditions;
};

// ==================== PageUpdate (unchanged) ====================

/// @brief Page update operation
struct PageUpdate {
    uint32_t page_no;
    uint32_t offset;
    std::vector<uint8_t> data;
    uint8_t type;  // MLOG_REC_INSERT, MLOG_REC_UPDATE, etc.
};

/**
 * @brief Redo Log Replayer - Replays redo log for recovery
 */
class RedoLogReplayer {
public:
    RedoLogReplayer();
    ~RedoLogReplayer();

    /**
     * @brief Set page size
     */
    void set_page_size(uint32_t page_size);

    /**
     * @brief Enable/disable base page checksum verification during identity checks
     */
    void set_verify_checksums(bool verify);

    /**
     * @brief Add page data for replay
     */
    bool add_page(uint32_t page_no, std::vector<uint8_t> page_data);

    /**
     * @brief Add redo log records
     */
    void add_redo_records(const std::vector<RedoLogRecord>& records);

    /**
     * @brief Get the loaded base pages (for identity/status queries)
     */
    const std::map<uint32_t, std::vector<uint8_t>>& get_base_pages() const;

    /**
     * @brief Phase 5f: Build a read-only inventory of the redo log
     *
     * No base pages required. Always succeeds (unless no records).
     */
    RedoLogInventory generate_inventory() const;

    /**
     * @brief Phase 5f: Build an ordered patch plan from redo records
     *
     * Sorts by (space_id, page_no) then LSN. Optionally verifies base page
     * identity when an IBD is loaded. This is read-only — nothing is applied.
     */
    RedoPatchPlan generate_patch_plan(bool require_base_pages = true) const;

    /**
     * @brief Phase 5f: Evaluate all pre-conditions for safe write replay
     */
    RedoPreConditionResult validate_conditions() const;

    /**
     * @brief Phase 5f: Replay only if all pre-conditions pass
     *
     * If pre-conditions fail, returns failure result WITHOUT modifying any page.
     * Callers must check result.success before trusting updated pages.
     */
    RedoReplayResult replay_safe();

    /**
     * @brief Replay redo log on pages
     *
     * @deprecated Phase 5f: prefer replay_safe() which gates on pre-conditions.
     * This method applies as-is and does NOT verify log format/LSN/whitelist/
     * base-page identity before writing pages.
     */
    RedoReplayResult replay();

    /**
     * @brief Apply single redo record to page
     */
    bool apply_redo_record(uint32_t page_no, const RedoLogRecord& record);

    /**
     * @brief Get updated pages
     */
    const std::map<uint32_t, std::vector<uint8_t>>& get_updated_pages() const;

    /**
     * @brief Get replay statistics
     */
    struct ReplayStats {
        uint32_t total_records;
        uint32_t records_processed;
        uint32_t records_applied;
        uint32_t records_failed;
        uint32_t pages_modified;
    };
    ReplayStats get_stats() const;

    /**
     * @brief Clear all data
     */
    void clear();

private:
    uint32_t page_size_;
    std::map<uint32_t, std::vector<uint8_t>> pages_;
    std::vector<RedoLogRecord> redo_records_;
    ReplayStats stats_;
    std::vector<std::string> errors_;
    bool verify_checksums_ = true;
    std::set<uint32_t> applied_pages_;

    /**
     * @brief Parse redo record data
     */
    bool parse_redo_data(const RedoLogRecord& record, PageUpdate& update);

    /**
     * @brief Apply insert record
     */
    bool apply_insert(uint32_t page_no, const PageUpdate& update);

    /**
     * @brief Apply update record
     */
    bool apply_update(uint32_t page_no, const PageUpdate& update);

    /**
     * @brief Apply delete record
     */
    bool apply_delete(uint32_t page_no, const PageUpdate& update);

    /**
     * @brief Read record from page
     */
    std::optional<std::vector<uint8_t>> read_record(uint32_t page_no, uint16_t offset);

    /**
     * @brief Phase 5f: Check base page identity — FIL_PAGE_SPACE_ID and FIL_PAGE_OFFSET
     * @return true if page_no matches FIL_PAGE_OFFSET and space_id is non-zero
     */
    bool check_base_page_identity(uint32_t page_no, const std::vector<uint8_t>& page_data,
                                   uint32_t& out_space_id, uint32_t& out_fil_page_no) const;

    /**
     * @brief Phase 5f: Replay records with ordering (internal, no pre-condition gate)
     */
    RedoReplayResult replay_for(const std::vector<RedoLogRecord>& records);
};

/**
 * @brief Redo Log Chain Builder - Builds redo log chains
 */
class RedoLogChainBuilder {
public:
    RedoLogChainBuilder();
    ~RedoLogChainBuilder();

    /**
     * @brief Add redo log record
     */
    void add_record(const RedoLogRecord& record);

    /**
     * @brief Build chains by transaction
     */
    bool build_chains();

    /**
     * @brief Get chains by LSN range
     */
    std::vector<RedoLogRecord> get_records(uint64_t start_lsn, uint64_t end_lsn);

    /**
     * @brief Get all records
     */
    const std::vector<RedoLogRecord>& get_records() const;

    /**
     * @brief Get statistics
     */
    struct ChainStats {
        uint32_t total_records;
        uint32_t total_chains;
        uint64_t start_lsn;
        uint64_t end_lsn;
    };
    ChainStats get_stats() const;

    /**
     * @brief Clear all data
     */
    void clear();

private:
    std::vector<RedoLogRecord> records_;
    std::map<uint64_t, std::vector<RedoLogRecord>> chains_;
    ChainStats stats_;
};

/**
 * @brief Redo Log Filter - Filters redo log records
 */
class RedoLogFilter {
public:
    RedoLogFilter();
    ~RedoLogFilter();

    /**
     * @brief Set table space ID filter
     */
    void set_space_id(uint32_t space_id);

    /**
     * @brief Set page number filter
     */
    void set_page_no(uint32_t page_no);

    /**
     * @brief Set record type filter
     */
    void set_record_type(uint8_t type);

    /**
     * @brief Set LSN range filter
     */
    void set_lsn_range(uint64_t start_lsn, uint64_t end_lsn);

    /**
     * @brief Filter redo log records
     */
    std::vector<RedoLogRecord> filter(const std::vector<RedoLogRecord>& records);

    /**
     * @brief Check if record passes filter
     */
    bool passes_filter(const RedoLogRecord& record);

    /**
     * @brief Clear all filters
     */
    void clear_filters();

private:
    std::optional<uint32_t> space_id_;
    std::optional<uint32_t> page_no_;
    std::optional<uint8_t> record_type_;
    std::optional<uint64_t> start_lsn_;
    std::optional<uint64_t> end_lsn_;
};

/**
 * @brief Redo Log Analyzer - Analyzes redo log for recovery planning
 */
class RedoLogAnalyzer {
public:
    RedoLogAnalyzer();
    ~RedoLogAnalyzer();

    /**
     * @brief Analyze redo log file
     */
    bool analyze_file(const std::string& path);

    /**
     * @brief Get analysis results
     */
    struct AnalysisResult {
        uint32_t total_records;
        uint32_t insert_records;
        uint32_t update_records;
        uint32_t delete_records;
        uint32_t page_create_records;
        uint32_t page_reinit_records;
        uint32_t blob_write_records;
        uint32_t unique_space_ids;
        uint32_t unique_page_numbers;
        uint64_t total_data_size;
        uint64_t start_lsn;
        uint64_t end_lsn;
    };
    AnalysisResult get_analysis() const;

    /**
     * @brief Get record types distribution
     */
    std::map<uint8_t, uint32_t> get_type_distribution() const;

    /**
     * @brief Get pages with most updates
     */
    std::vector<std::pair<uint32_t, uint32_t>> get_top_pages(uint32_t count = 10) const;

    /**
     * @brief Clear all data
     */
    void clear();

private:
    AnalysisResult analysis_;
    std::map<uint8_t, uint32_t> type_distribution_;
    std::map<uint32_t, uint32_t> page_update_count_;

    /**
     * @brief Analyze single record
     */
    void analyze_record(const RedoLogRecord& record);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_REDO_LOG_REPLAYER_HPP
