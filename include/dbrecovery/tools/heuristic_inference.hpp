#ifndef DBRECOVERY_TOOLS_HEURISTIC_INFERENCE_H
#define DBRECOVERY_TOOLS_HEURISTIC_INFERENCE_H

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include "page/page_parser.h"

namespace dbrecovery {
namespace tools {

/**
 * @brief Heuristic metadata inference engine
 * @details Automatically infers table schema and metadata from raw IBD files
 *          using pattern analysis and MySQL internal structure knowledge.
 */
class HeuristicInference {
public:
    /**
     * @brief Column type information
     */
    struct ColumnInfo {
        std::string name;
        std::string type;
        bool is_nullable;
        bool is_primary_key;
        bool is_auto_increment;
        uint32_t ordinal_position;
        std::optional<uint32_t> max_length;
        std::optional<uint32_t> precision;
        std::optional<uint32_t> scale;
    };

    /**
     * @brief Table schema information
     */
    struct TableSchema {
        std::string table_name;
        std::string database_name;
        std::vector<ColumnInfo> columns;
        uint64_t estimated_row_count = 0;
        uint64_t data_size = 0;
        uint32_t page_count = 0;
        mysql_recovery::RowFormat row_format;
        mysql_recovery::MySQLVersion version;
        std::string create_ddl;
    };

    /**
     * @brief Index information
     */
    struct IndexInfo {
        std::string name;
        bool is_primary;
        std::vector<std::string> column_names;
        uint32_t page_number;
    };

    /**
     * @brief Constructor
     */
    HeuristicInference();

    /**
     * @brief Destructor
     */
    ~HeuristicInference();

    /**
     * @brief Set page data for analysis
     * @param page_data Pointer to page data
     * @param page_size Size of the page
     */
    void set_page_data(const uint8_t* page_data, size_t page_size);

    /**
     * @brief Set file path for analysis
     * @param file_path Path to IBD file
     */
    void set_file_path(const std::string& file_path);

    /**
     * @brief Analyze and infer table metadata
     * @return Inferred table schema
     */
    std::optional<TableSchema> infer_schema();

    /**
     * @brief Infer column types from record data
     * @param record_data Pointer to record data
     * @param record_len Length of record
     * @param col_meta Column metadata
     * @return Vector of inferred column info
     */
    std::vector<ColumnInfo> infer_columns(
        const uint8_t* record_data,
        size_t record_len,
        const std::vector<uint8_t>& col_meta
    );

    /**
     * @brief Infer column name from position
     * @param column_index Column index (0-based)
     * @return Inferred column name
     */
    static std::string infer_column_name(uint32_t column_index);

    /**
     * @brief Infer column type from MySQL internal type code
     * @param type_code MySQL internal type code
     * @param col_meta Column metadata
     * @param col_index Column index
     * @return SQL type string
     */
    static std::string infer_column_type(uint8_t type_code, const std::vector<uint8_t>& col_meta, uint32_t col_index);

    /**
     * @brief Infer row count from page statistics
     * @param page_count Number of pages
     * @param avg_row_size Average row size in bytes
     * @return Estimated row count
     */
    static uint64_t estimate_row_count(uint32_t page_count, uint32_t avg_row_size);

    /**
     * @brief Infer row format from page data
     * @param page_data Pointer to page data
     * @param page_size Size of the page
     * @return Inferred row format
     */
    static mysql_recovery::RowFormat infer_row_format(const uint8_t* page_data, size_t page_size);

    /**
     * @brief Infer MySQL version from page data
     * @param page_data Pointer to page data
     * @param page_size Size of the page
     * @return Inferred MySQL version
     */
    static mysql_recovery::MySQLVersion infer_version(const uint8_t* page_data, size_t page_size);

    /**
     * @brief Extract column metadata from page
     * @param page_data Pointer to page data
     * @param page_size Size of the page
     * @return Vector of column metadata bytes
     */
    static std::vector<uint8_t> extract_column_metadata(const uint8_t* page_data, size_t page_size);

    /**
     * @brief Parse record header
     * @param record_data Pointer to record data
     * @param format Row format
     * @return Parsed record header
     */
    static mysql_recovery::RowParser::RecordHeader parse_record_header(
        const uint8_t* record_data,
        mysql_recovery::RowFormat format
    );

    /**
     * @brief Generate CREATE TABLE statement
     * @param schema Table schema
     * @return SQL CREATE TABLE statement
     */
    static std::string generate_create_statement(const TableSchema& schema);

    /**
     * @brief Get all inferred indexes
     * @return Vector of index information
     */
    std::vector<IndexInfo> get_indexes() const;

    /**
     * @brief Set maximum columns to infer
     * @param max_columns Maximum number of columns
     */
    void set_max_columns(uint32_t max_columns) { max_columns_ = max_columns; }

    /**
     * @brief Set minimum records for inference
     * @param min_records Minimum number of records required
     */
    void set_min_records(uint32_t min_records) { min_records_ = min_records; }

    /**
     * @brief Reset inference state
     */
    void reset();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    uint32_t max_columns_;
    uint32_t min_records_;
};

/**
 * @brief Simple column type inference based on byte patterns
 */
class SimpleTypeInferer {
public:
    /**
     * @brief Infer type from value string
     * @param value String representation of value
     * @return Inferred type
     */
    static std::string infer_type_from_value(const std::string& value);

    /**
     * @brief Check if value looks like an integer
     * @param value String to check
     * @return true if likely integer
     */
    static bool is_integer(const std::string& value);

    /**
     * @brief Check if value looks like a float
     * @param value String to check
     * @return true if likely float
     */
    static bool is_float(const std::string& value);

    /**
     * @brief Check if value looks like a date
     * @param value String to check
     * @return true if likely date
     */
    static bool is_date(const std::string& value);

    /**
     * @brief Check if value looks like a string
     * @param value String to check
     * @return true if likely string
     */
    static bool is_string(const std::string& value);

    /**
     * @brief Get type priority (higher = more specific)
     * @param type Type name
     * @return Priority score
     */
    static int get_type_priority(const std::string& type);
};

/**
 * @brief Statistics collector for heuristic inference
 */
class InferenceStats {
public:
    struct Stats {
        uint32_t pages_analyzed = 0;
        uint32_t records_found = 0;
        uint32_t records_parsed = 0;
        uint32_t columns_inferred = 0;
        uint32_t indexes_found = 0;
        uint64_t data_bytes_scanned = 0;
        uint64_t inference_time_ms = 0;
    };

    void record_page_analyzed() { stats_.pages_analyzed++; }
    void record_records_found(uint32_t count) { stats_.records_found += count; }
    void record_records_parsed(uint32_t count) { stats_.records_parsed += count; }
    void record_columns_inferred(uint32_t count) { stats_.columns_inferred += count; }
    void record_indexes_found(uint32_t count) { stats_.indexes_found += count; }
    void record_bytes_scanned(uint64_t bytes) { stats_.data_bytes_scanned += bytes; }

    void start_timer();
    void stop_timer();

    Stats get_stats() const { return stats_; }
    void reset() { stats_ = Stats{}; }

private:
    Stats stats_;
    uint64_t start_time_ms_ = 0;
};

}  // namespace tools
}  // namespace dbrecovery

#endif  // DBRECOVERY_TOOLS_HEURISTIC_INFERENCE_H
