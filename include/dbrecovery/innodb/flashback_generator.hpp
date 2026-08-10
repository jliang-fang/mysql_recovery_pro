/**
 * @file flashback_generator.hpp
 * @brief FlashbackGenerator — schema-aware flashback SQL/CSV/JSON generation from undo logs
 *
 * Wraps DeepUndoAnalyzer with:
 * - Correct field_no mapping (skip DB_ROLL_PTR, map field_no 2+ to columns)
 * - Schema-aware human-readable value decoding
 * - Multiple output formats (SQL, CSV, JSON)
 * - Multiple flashback modes (DELETE, UPDATE, POINT_IN_TIME, TRANSACTION)
 * - Confidence scoring and completeness tracking
 */

#ifndef DBRECOVERY_INNODB_FLASHBACK_GENERATOR_HPP
#define DBRECOVERY_INNODB_FLASHBACK_GENERATOR_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <functional>

#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/deep_undo_analyzer.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Flashback Mode ====================

enum class FlashbackMode {
    DELETE,        // Reverse DELETE operations -> INSERT
    UPDATE,        // Reverse UPDATE operations -> original values
    POINT_IN_TIME, // Row state at a specific point in time
    TRANSACTION,   // All changes by a specific transaction
    FULL_CHAIN     // Full version chain for a row
};

// ==================== Output Format ====================

enum class FlashbackOutputFormat {
    SQL,  // SQL statements
    CSV,  // Comma-separated values
    JSON  // JSON objects
};

// ==================== Configuration ====================

struct FlashbackConfig {
    FlashbackMode mode = FlashbackMode::DELETE;
    FlashbackOutputFormat output_format = FlashbackOutputFormat::SQL;

    // Time range filter
    std::optional<std::chrono::system_clock::time_point> start_time;
    std::optional<std::chrono::system_clock::time_point> end_time;

    // LSN range filter
    std::optional<uint64_t> start_lsn;
    std::optional<uint64_t> end_lsn;

    // Transaction filter
    std::optional<uint64_t> trx_id;

    // Safety limits
    uint32_t max_records = 10000;
    uint32_t max_chain_depth = 1000;

    // Behavior flags
    bool include_incomplete = false;
    bool human_readable = true;
    bool pretty_json = true;
    bool include_metadata = true;
};

// ==================== Decoded Field ====================

struct DecodedField {
    std::string column_name;
    std::string human_value;
    std::string hex_value;
    bool is_null = false;
    bool is_decoded = false;
    double confidence = 1.0;
};

// ==================== Flashback Record ====================

struct FlashbackRecord {
    enum OpType { INSERT, UPDATE, DELETE_OP };
    OpType operation = INSERT;

    std::vector<DecodedField> fields;
    std::map<std::string, std::string> field_map;

    uint64_t trx_id = 0;
    uint64_t undo_no = 0;
    std::optional<std::chrono::system_clock::time_point> timestamp;
    std::optional<uint64_t> lsn;

    double completeness = 0.0;
    uint32_t n_columns_total = 0;
    uint32_t n_columns_found = 0;
    bool is_complete = false;

    std::string source_type;

    std::string to_sql(const catalog::TableSchema& schema) const;
    std::string to_csv(const catalog::TableSchema& schema, char delimiter = ',') const;
    std::string to_json(bool pretty = true) const;

    static std::string to_sql_helper(
        const std::vector<FlashbackRecord>& records,
        const catalog::TableSchema& schema);
};

// ==================== FlashbackGenerator ====================

class FlashbackGenerator {
public:
    FlashbackGenerator();
    ~FlashbackGenerator() = default;

    // Configuration
    void set_ibdata_path(const std::string& path);
    void set_page_size(size_t page_size);
    void set_config(const FlashbackConfig& config);
    void set_undo_format_profile(UndoFormatProfile profile);
    void set_mysql_version(const std::string& version);
    FlashbackConfig& config() { return config_; }
    const FlashbackConfig& config() const { return config_; }

    // Core API
    std::vector<FlashbackRecord> flashback_deletes(
        const catalog::TableSchema& schema);

    std::vector<FlashbackRecord> flashback_updates(
        const catalog::TableSchema& schema);

    std::vector<FlashbackRecord> flashback_transaction(
        uint64_t trx_id,
        const catalog::TableSchema& schema);

    std::vector<FlashbackRecord> flashback_point_in_time(
        std::chrono::system_clock::time_point point,
        const catalog::TableSchema& schema);

    std::vector<FlashbackRecord> flashback_full_chain(
        const std::vector<uint8_t>& current_record,
        uint64_t roll_ptr,
        const catalog::TableSchema& schema,
        uint64_t index_id);

    // Output generation
    std::string generate_output(
        const std::vector<FlashbackRecord>& records,
        const catalog::TableSchema& schema);

    // Statistics
    struct Stats {
        uint64_t total_records = 0;
        uint64_t complete_records = 0;
        uint64_t partial_records = 0;
        double avg_completeness = 0.0;
        double execution_time_ms = 0.0;
    };
    const Stats& get_stats() const { return stats_; }
    void reset_stats() { stats_ = Stats{}; }

    // Static helpers
    static std::string decode_field_value(
        const std::vector<uint8_t>& raw_bytes,
        const catalog::ColumnInfo& col_info,
        bool human_readable = true);

    static std::string bytes_to_hex(const std::vector<uint8_t>& data);

    static std::optional<uint32_t> field_no_to_ordinal(uint16_t field_no, bool is_delete);

private:
    std::string ibdata_path_;
    size_t page_size_ = 16384;
    FlashbackConfig config_;
    Stats stats_;

    std::unique_ptr<DeepUndoAnalyzer> analyzer_;

    FlashbackRecord decode_single_record(
        const UndoRecordV2& rec,
        const catalog::TableSchema& schema);

    std::vector<catalog::ColumnInfo> get_ordered_columns(
        const catalog::TableSchema& schema) const;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_FLASHBACK_GENERATOR_HPP