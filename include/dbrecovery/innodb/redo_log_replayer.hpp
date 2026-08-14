#ifndef DBRECOVERY_INNODB_REDO_LOG_REPLAYER_HPP
#define DBRECOVERY_INNODB_REDO_LOG_REPLAYER_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>
#include "dbrecovery/innodb/redo_log.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief Redo log replay result
 */
struct RedoReplayResult {
    bool success;
    std::string message;
    uint32_t records_processed;
    uint32_t records_applied;
    uint32_t records_failed;
    std::vector<std::string> errors;
};

/**
 * @brief Page update operation
 */
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
     * @brief Add page data for replay
     */
    bool add_page(uint32_t page_no, std::vector<uint8_t> page_data);

    /**
     * @brief Add redo log records
     */
    void add_redo_records(const std::vector<RedoLogRecord>& records);

    /**
     * @brief Replay redo log on pages
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
