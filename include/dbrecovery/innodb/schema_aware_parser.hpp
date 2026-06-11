/**
 * @file schema_aware_parser.hpp
 * @brief Schema-Aware Parser for MySQL 5.6 COMPACT row format
 *
 * Implements:
 * - MySQL 5.6 COMPACT row format parsing
 * - Template matching with known schema
 * - Deleted record extraction from free list
 * - Confidence scoring for recovered records
 *
 * @version 2.2.0
 */

#ifndef DBRECOVERY_INNODB_SCHEMA_AWARE_PARSER_HPP
#define DBRECOVERY_INNODB_SCHEMA_AWARE_PARSER_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>
#include <map>
#include <unordered_map>

#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/engine/advanced_signature_scraper.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Record Header Constants ====================

/// @brief Record header size for COMPACT/DYNAMIC format (5 bytes)
constexpr uint32_t COMPACT_RECORD_HEADER_SIZE = 5;

/// @brief Record header size for REDUNDANT format (6 bytes)
constexpr uint32_t REDUNDANT_RECORD_HEADER_SIZE = 6;

// Use REC_STATUS_* from page_defs.hpp instead of redefining here

/// @brief Record info bits (from page_defs.hpp)
constexpr uint32_t REC_INFO_DELETED = 0x200000;  // Bit for deleted flag
constexpr uint32_t REC_INFO_MIN_REC = 0x100000;  // Minimum record flag
constexpr uint32_t REC_INFO_INSTANT = 0x800000;  // Instant ADD column

// ==================== Parsed Record Header ====================

/**
 * @brief Parsed COMPACT record header
 */
struct CompactRecordHeader {
    uint8_t info_bits;              ///< Info bits (1 byte)
    uint16_t next_offset;           ///< Next record offset (2 bytes)
    uint16_t heap_no;               ///< Heap number (2 bytes, includes info bits)
    bool is_deleted;                ///< Deleted flag
    bool is_min_rec;                ///< Minimum record flag
    bool is_infimum;                ///< Is INFIMUM record
    bool is_supremum;               ///< Is SUPREMUM record
    uint8_t n_owned;                ///< Number of records owned (for page directory)

    /**
     * @brief Parse from raw data
     */
    static std::optional<CompactRecordHeader> parse(const uint8_t* data, size_t size);
};

// ==================== Parsed Field ====================

/**
 * @brief Parsed field value
 */
struct ParsedField {
    std::string name;               ///< Field name (from schema)
    int mysql_type;                 ///< MySQL type ID
    std::vector<uint8_t> raw_data;  ///< Raw bytes
    std::string value;              ///< String representation
    bool is_null;                   ///< Is NULL
    bool is_valid;                  ///< Parsing was successful

    /**
     * @brief Convert to SQL value string
     */
    std::string to_sql_value() const;
};

// ==================== Parsed Record ====================

/**
 * @brief Parsed record from page
 */
struct ParsedRecord {
    uint16_t offset;                ///< Record offset in page
    uint16_t next_offset;           ///< Next record offset
    bool is_deleted;                ///< Is deleted record
    bool is_infimum;                ///< Is INFIMUM
    bool is_supremum;               ///< Is SUPREMUM
    std::vector<ParsedField> fields; ///< Parsed fields
    double confidence;              ///< Confidence score

    /**
     * @brief Convert to SQL INSERT statement
     */
    std::string to_sql_insert(const std::string& table_name) const;

    /**
     * @brief Convert to CSV row
     */
    std::string to_csv() const;

    /**
     * @brief Convert to JSON
     */
    std::string to_json() const;
};

// ==================== MySQL 5.6 COMPACT Parser ====================

/**
 * @brief Parser for MySQL 5.6 COMPACT row format
 *
 * COMPACT format structure:
 * - Record header: 5 bytes (info_bits + next_offset + heap_no)
 * - NULL bitmap: ceil(n_nullable / 8) bytes
 * - Variable-length field sizes: 1 or 2 bytes per field
 * - Fixed-length fields: stored directly
 * - Variable-length fields: stored after fixed fields
 */
class MySQL56CompactParser {
public:
    /**
     * @brief Construct with schema
     */
    explicit MySQL56CompactParser(const catalog::TableSchema& schema);

    /**
     * @brief Parse a single record at offset
     *
     * @param page_data Page data
     * @param page_size Page size
     * @param offset Record offset
     * @return Parsed record or nullopt
     */
    std::optional<ParsedRecord> parse_record(
        const uint8_t* page_data,
        uint32_t page_size,
        uint16_t offset);

    /**
     * @brief Parse all records in page
     */
    std::vector<ParsedRecord> parse_all_records(
        const uint8_t* page_data,
        uint32_t page_size);

    /**
     * @brief Parse deleted records (from free list)
     */
    std::vector<ParsedRecord> parse_deleted_records(
        const uint8_t* page_data,
        uint32_t page_size);

    // ==================== Field Parsing Helpers ====================

    /**
     * @brief Read variable-length field
     *
     * @param data Data buffer
     * @param offset Current offset
     * @param[out] read_size Bytes read
     * @return Field data
     */
    static std::vector<uint8_t> read_var_len_field(
        const uint8_t* data,
        size_t offset,
        size_t& read_size);

    /**
     * @brief Read fixed-length field
     */
    static std::vector<uint8_t> read_fixed_field(
        const uint8_t* data,
        size_t offset,
        size_t field_size);

    /**
     * @brief Check if field is NULL from bitmap
     */
    bool is_field_null(const uint8_t* null_bitmap, uint8_t field_index) const;

    /**
     * @brief Read NULL bitmap
     */
    std::vector<uint8_t> read_null_bitmap(
        const uint8_t* data,
        size_t offset,
        uint8_t n_nullable);

    /**
     * @brief Read variable-length size array
     */
    std::vector<uint16_t> read_var_len_sizes(
        const uint8_t* data,
        size_t offset,
        uint8_t n_var_fields);

    // ==================== Value Conversion ====================

    /**
     * @brief Convert raw bytes to string value
     */
    static std::string bytes_to_value(
        const std::vector<uint8_t>& bytes,
        int mysql_type);

    /**
     * @brief Convert integer bytes to string
     */
    static std::string int_to_string(const std::vector<uint8_t>& bytes);

    /**
     * @brief Convert VARCHAR bytes to string
     */
    static std::string varchar_to_string(const std::vector<uint8_t>& bytes);

    /**
     * @brief Convert DATETIME bytes to string
     */
    static std::string datetime_to_string(const std::vector<uint8_t>& bytes);

    /**
     * @brief Convert TIMESTAMP bytes to string
     */
    static std::string timestamp_to_string(const std::vector<uint8_t>& bytes);

private:
    catalog::TableSchema schema_;
    uint8_t n_nullable_;
    uint8_t n_fixed_;
    uint8_t n_var_;
    size_t null_bitmap_size_;

    /**
     * @brief Calculate field counts from schema
     */
    void calculate_field_counts();

    /**
     * @brief Parse record body (after header)
     */
    std::vector<ParsedField> parse_record_body(
        const uint8_t* page_data,
        uint32_t page_size,
        uint16_t offset,
        size_t header_size);
};

// ==================== Schema-Aware Parser ====================

/**
 * @brief Parser that uses schema template for validation
 */
class SchemaAwareParser {
public:
    /**
     * @brief Construct with schema
     */
    explicit SchemaAwareParser(const catalog::TableSchema& schema);

    /**
     * @brief Parse page with template matching
     *
     * @param page_data Page data
     * @param page_size Page size
     * @return Parsed records
     */
    std::vector<ParsedRecord> parse_with_template(
        const uint8_t* page_data,
        uint32_t page_size);

    /**
     * @brief Parse carved page
     */
    std::vector<ParsedRecord> parse_carved_page(
        const engine::CarvedPage& page);

    /**
     * @brief Calculate confidence for record
     *
     * Validates record against schema:
     * - Field type matching
     * - Field count matching
     * - Value range validation
     */
    double calculate_confidence(const ParsedRecord& record) const;

    /**
     * @brief Validate field against schema
     */
    bool validate_field(const ParsedField& field, const catalog::ColumnInfo& col_info) const;

    /**
     * @brief Filter records by confidence threshold
     */
    std::vector<ParsedRecord> filter_by_confidence(
        const std::vector<ParsedRecord>& records,
        double min_confidence) const;

    /**
     * @brief Deduplicate records
     */
    std::vector<ParsedRecord> deduplicate(
        const std::vector<ParsedRecord>& records) const;

private:
    catalog::TableSchema schema_;
    MySQL56CompactParser compact_parser_;

    /**
     * @brief Hash record for deduplication
     */
    std::string hash_record(const ParsedRecord& record) const;
};

// ==================== Deleted Record Extractor ====================

/**
 * @brief Extract deleted records from page free list
 */
class DeletedRecordExtractor {
public:
    /**
     * @brief Construct with schema
     */
    explicit DeletedRecordExtractor(const catalog::TableSchema& schema);

    /**
     * @brief Extract deleted records from page
     *
     * Deleted records are stored in the free list.
     * Free list head is at PAGE_HEADER + 6.
     *
     * @param page_data Page data
     * @param page_size Page size
     * @return Deleted records
     */
    std::vector<ParsedRecord> extract_deleted(
        const uint8_t* page_data,
        uint32_t page_size);

    /**
     * @brief Get free list head offset
     */
    static uint16_t get_free_list_head(const uint8_t* page_data, uint32_t page_size);

    /**
     * @brief Follow free list chain
     */
    static std::vector<uint16_t> get_free_list_offsets(
        const uint8_t* page_data,
        uint32_t page_size,
        uint16_t head);

    /**
     * @brief Parse deleted record header
     */
    static std::optional<CompactRecordHeader> parse_deleted_header(
        const uint8_t* page_data,
        uint32_t page_size,
        uint16_t offset);

private:
    catalog::TableSchema schema_;
    MySQL56CompactParser parser_;
};

// ==================== Record Chain Parser ====================

/**
 * @brief Parse record chain from INFIMUM to SUPREMUM
 */
class RecordChainParser {
public:
    /**
     * @brief Parse record chain in order
     *
     * Records are linked from INFIMUM (offset 99) through
     * the chain to SUPREMUM.
     *
     * @param page_data Page data
     * @param page_size Page size
     * @param parser Record parser
     * @return Records in chain order
     */
    static std::vector<ParsedRecord> parse_chain(
        const uint8_t* page_data,
        uint32_t page_size,
        MySQL56CompactParser& parser);

    /**
     * @brief Get INFIMUM offset (always 99)
     */
    static constexpr uint16_t get_infimum_offset() { return 99; }

    /**
     * @brief Get SUPREMUM offset (follows INFIMUM)
     */
    static uint16_t get_supremum_offset(const uint8_t* page_data, uint32_t page_size);

    /**
     * @brief Follow record chain
     */
    static std::vector<uint16_t> get_record_offsets(
        const uint8_t* page_data,
        uint32_t page_size);
};

// ==================== Multi-Format Parser ====================

/**
 * @brief Parser that handles multiple row formats
 */
class MultiFormatParser {
public:
    /**
     * @brief Row format type
     */
    enum class RowFormat {
        REDUNDANT,
        COMPACT,
        DYNAMIC,
        COMPRESSED
    };

    /**
     * @brief Construct with schema and format
     */
    MultiFormatParser(const catalog::TableSchema& schema, RowFormat format);

    /**
     * @brief Parse records with detected format
     */
    std::vector<ParsedRecord> parse(
        const uint8_t* page_data,
        uint32_t page_size);

    /**
     * @brief Detect row format from page
     */
    static RowFormat detect_format(const uint8_t* page_data, uint32_t page_size);

    /**
     * @brief Get format name
     */
    static std::string format_to_string(RowFormat format);

private:
    catalog::TableSchema schema_;
    RowFormat format_;
    std::unique_ptr<MySQL56CompactParser> compact_parser_;

    /**
     * @brief Parse REDUNDANT format
     */
    std::vector<ParsedRecord> parse_redundant(
        const uint8_t* page_data,
        uint32_t page_size);

    /**
     * @brief Parse DYNAMIC format (similar to COMPACT)
     */
    std::vector<ParsedRecord> parse_dynamic(
        const uint8_t* page_data,
        uint32_t page_size);
};

// ==================== Export Functions ====================

/**
 * @brief Export records to SQL INSERT statements
 */
std::string export_to_sql(
    const std::vector<ParsedRecord>& records,
    const std::string& table_name);

/**
 * @brief Export records to CSV
 */
std::string export_to_csv(
    const std::vector<ParsedRecord>& records,
    const std::string& table_name);

/**
 * @brief Export records to JSON
 */
std::string export_to_json(
    const std::vector<ParsedRecord>& records,
    const std::string& table_name);

/**
 * @brief Export records with deduplication
 */
std::vector<ParsedRecord> export_with_dedup(
    const std::vector<ParsedRecord>& records);

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SCHEMA_AWARE_PARSER_HPP