/**
 * @file schema_agnostic_scanner.hpp
 * @brief Schema-Agnostic InnoDB Record Scanner
 *
 * A universal record scanner that works without prior knowledge of table schema.
 *
 * Design Principles:
 * 1. Parse Page Header to locate User Records Area
 * 2. Use Page Directory (slot array) for precise record boundary detection
 * 3. Dynamically parse Record Header and Null Bitmap
 * 4. Robust variable-length field parsing
 * 5. Fault-tolerant: skip corrupted records via page directory
 */

#ifndef DBRECOVERY_INNODB_SCHEMA_AGNOSTIC_SCANNER_HPP
#define DBRECOVERY_INNODB_SCHEMA_AGNOSTIC_SCANNER_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <set>
#include <functional>

namespace dbrecovery {
namespace innodb {

// ==================== Page Directory Slot ====================

/**
 * @brief Represents a slot in the Page Directory
 *
 * Page Directory is located at the end of the page.
 * Each slot is 2 bytes, containing an offset to a record (relative to page start).
 * Slots are stored in reverse order (slot[0] is at highest address).
 */
struct PageDirSlot {
    uint16_t offset;      // Record offset (relative to page start)
    uint8_t  n_owned;     // Number of records owned by this slot (from record header)

    static std::optional<PageDirSlot> parse(const uint8_t* page_data, size_t page_size,
                                            size_t slot_index, uint16_t n_dir_slots);
};

// ==================== Dynamic Record Header ====================

/**
 * @brief Parsed COMPACT record header (5 bytes)
 *
 * Layout (5 bytes, big-endian interpretation):
 * - Bytes 0-2: info_and_heap (24 bits)
 *   - bit 23: instant_flag (MySQL 8.0+)
 *   - bit 22: version_flag
 *   - bit 21: deleted_flag
 *   - bit 20: min_rec_flag
 *   - bits 19-16: n_owned (4 bits)
 *   - bits 15-3: heap_no (13 bits)
 *   - bits 2-0: rec_type (3 bits: 0=ordinary, 1=node_ptr, 2=infimum, 3=supremum)
 * - Bytes 3-4: next_record_offset (signed 16-bit, relative to data start)
 */
struct DynamicRecordHeader {
    uint8_t  rec_type;      // 0=ordinary, 1=node_ptr, 2=infimum, 3=supremum
    bool     is_deleted;
    bool     is_min_rec;
    uint16_t heap_no;
    uint8_t  n_owned;
    int16_t  next_offset;   // Relative to record data start (after header)

    // Header size in bytes (5 for COMPACT, 6 for REDUNDANT)
    size_t   header_size;

    bool is_system_record() const {
        return rec_type == 2 || rec_type == 3;  // infimum or supremum
    }

    static std::optional<DynamicRecordHeader> parse_compact(const uint8_t* data, size_t max_size);
    static std::optional<DynamicRecordHeader> parse_redundant(const uint8_t* data, size_t max_size);
};

// ==================== Null Bitmap Analysis ====================

/**
 * @brief Result of Null Bitmap analysis
 */
struct NullBitmapInfo {
    size_t   num_null_bytes;    // Number of null bitmap bytes
    size_t   num_nullable_cols; // Estimated number of nullable columns
    std::vector<bool> null_flags; // Actual null flags (if determinable)

    /**
     * @brief Calculate null bitmap size from estimated nullable columns
     */
    static size_t calc_null_bytes(size_t n_nullable) {
        return (n_nullable + 7) / 8;
    }
};

// ==================== Variable-Length Field Size Array ====================

/**
 * @brief Information about variable-length field size array
 */
struct VarSizeArrayInfo {
    std::vector<uint32_t> sizes;     // Field sizes
    size_t bytes_read;               // Bytes consumed by size array
    bool has_overflow;               // True if any field is off-page
    int confidence_score;            // How confident we are in this parsing

    /**
     * @brief Total data size (excluding overflow pointers)
     */
    size_t total_data_size() const;
};

// ==================== Schemaless Field Value ====================

/**
 * @brief A parsed field value without schema knowledge
 */
struct SchemalessField {
    enum class Type : uint8_t {
        NULL_VAL = 0,
        INT_VAL,
        UINT_VAL,
        FLOAT_VAL,
        DOUBLE_VAL,
        STRING_VAL,
        BINARY_VAL,
        TIMESTAMP_VAL,
        DATETIME_VAL,
        DATE_VAL,
        OVERFLOW_PTR,   // 20-byte off-page pointer
        UNKNOWN
    };

    Type type = Type::UNKNOWN;
    std::vector<uint8_t> raw_bytes;
    std::string str_value;
    int64_t int_value = 0;
    double float_value = 0.0;
    bool is_null = false;
    double confidence = 0.0;

    // For overflow pointers
    uint32_t overflow_space_id = 0;
    uint32_t overflow_page_no = 0;
    uint64_t overflow_size = 0;

    std::string to_string() const;
    std::string to_csv() const;
};

// ==================== Schemaless Record ====================

/**
 * @brief A complete record parsed without schema
 */
struct SchemalessRecord {
    uint64_t record_id = 0;        // Primary key (if detectable)
    uint32_t page_no = 0;
    uint16_t record_offset = 0;    // Offset of record header
    uint16_t data_offset = 0;      // Offset of record data (after header)
    uint16_t data_length = 0;      // Length of record data

    std::vector<SchemalessField> fields;

    bool is_deleted = false;
    bool is_valid = false;
    double confidence = 0.0;

    // Raw data for debugging
    std::vector<uint8_t> raw_header;
    std::vector<uint8_t> raw_data;

    std::string to_csv() const;
};

// ==================== Page Analysis Result ====================

/**
 * @brief Result of analyzing a single page
 */
struct PageAnalysisResult {
    uint32_t page_no = 0;
    bool is_valid = false;
    bool is_leaf = false;
    bool is_compact = true;
    uint64_t index_id = 0;
    uint16_t level = 0;

    // Page header info
    uint16_t n_dir_slots = 0;
    uint16_t heap_top = 0;
    uint16_t n_heap = 0;
    uint16_t n_recs = 0;

    // Parsed records
    std::vector<SchemalessRecord> records;

    // Errors encountered
    std::vector<std::string> errors;
};

// ==================== Main Scanner Class ====================

/**
 * @brief Schema-Agnostic InnoDB Record Scanner
 *
 * Key Features:
 * 1. No schema dependency - works with any table structure
 * 2. Page Directory based record location
 * 3. Dynamic Null Bitmap parsing
 * 4. Robust variable-length field detection
 * 5. Fault-tolerant record chain traversal
 */
class SchemaAgnosticScanner {
public:
    // Configuration
    struct Config {
        bool recover_deleted = true;    // Also recover deleted records
        bool parse_overflow = false;    // Follow overflow pointers
        bool verbose = false;           // Debug output
        size_t max_fields_per_record = 256;  // Safety limit
        size_t min_string_length = 2;   // Minimum string length to detect
        /// Physical page size. 0 = derive it from the file size.
        uint32_t page_size = 0;

        Config() = default;
    };

    explicit SchemaAgnosticScanner(const Config& config);

    /**
     * @brief Parse all records from a page
     */
    PageAnalysisResult parse_page(const uint8_t* page_data, size_t page_size, uint32_t page_no);

    /**
     * @brief Parse all records from an IBD file
     */
    std::vector<SchemalessRecord> parse_ibd(const std::string& ibd_path);

    /**
     * @brief Set callback for progress reporting
     */
    void set_progress_callback(std::function<void(uint32_t page_no, size_t records)> cb) {
        progress_cb_ = std::move(cb);
    }

private:
    // ==================== Phase 1: Page Structure Analysis ====================

    /**
     * @brief Validate page and parse header
     * @return true if valid INDEX page
     */
    bool analyze_page_header(const uint8_t* page_data, size_t page_size,
                            PageAnalysisResult& result);

    /**
     * @brief Parse Page Directory slots
     * @return Vector of record offsets (sorted ascending)
     */
    std::vector<uint16_t> parse_page_directory(const uint8_t* page_data, size_t page_size,
                                               uint16_t n_dir_slots);

    // ==================== Phase 2: Record Chain Traversal ====================

    /**
     * @brief Find record boundaries using page directory
     * @return Vector of (record_start, record_end) pairs
     */
    std::vector<std::pair<size_t, size_t>> find_record_boundaries(
        const uint8_t* page_data, size_t page_size,
        const std::vector<uint16_t>& dir_slots,
        uint16_t heap_top);

    /**
     * @brief Follow record chain from infimum to supremum
     */
    std::vector<size_t> follow_record_chain(const uint8_t* page_data, size_t page_size,
                                            bool is_compact);

    // ==================== Phase 3: Single Record Parsing ====================

    /**
     * @brief Parse a single record at given offset
     */
    std::optional<SchemalessRecord> parse_record(
        const uint8_t* page_data, size_t page_size,
        size_t rec_offset, size_t rec_end,
        bool is_compact, bool is_leaf,
        uint32_t page_no);

    /**
     * @brief Parse record header (COMPACT format)
     */
    std::optional<DynamicRecordHeader> parse_record_header(
        const uint8_t* data, size_t offset, size_t max_size, bool is_compact);

    // ==================== Phase 4: Field Extraction ====================

    /**
     * @brief Analyze Null Bitmap from record data
     *
     * Uses heuristic: null bitmap starts after TRX_ID/ROLL_PTR for leaf pages
     */
    NullBitmapInfo analyze_null_bitmap(const uint8_t* data, size_t data_len, bool is_leaf);

    /**
     * @brief Parse variable-length field size array (backward from record end)
     *
     * Key insight: sizes are stored at the END of record, read backward
     * Each size is 1 or 2 bytes depending on magnitude:
     * - 0x00-0x7F: 1 byte, size = value
     * - 0x80-0xFF: 2 bytes, size = ((high & 0x7F) << 8) | low
     */
    VarSizeArrayInfo parse_var_size_array(const uint8_t* data, size_t data_len,
                                          size_t var_start_offset);

    /**
     * @brief Extract fields from record data
     */
    std::vector<SchemalessField> extract_fields(
        const uint8_t* data, size_t data_len,
        const NullBitmapInfo& null_info,
        const VarSizeArrayInfo& var_info,
        bool is_leaf);

    /**
     * @brief Detect field type from raw bytes
     */
    SchemalessField::Type detect_field_type(const uint8_t* data, size_t size);

    /**
     * @brief Parse a field value
     */
    SchemalessField parse_field_value(const uint8_t* data, size_t size);

    // ==================== Phase 5: Heuristic Validation ====================

    /**
     * @brief Score the quality of a parsed record
     */
    double score_record_quality(const SchemalessRecord& record);

    /**
     * @brief Validate record boundaries make sense
     */
    bool validate_record_bounds(size_t start, size_t end, size_t heap_top);

    /**
     * @brief Set MySQL version for version-dependent layout decisions
     *
     * Controls DB_ROLL_PTR width when skipping system columns:
     * 6 bytes for 5.6/5.7, 7 bytes for 8.0+.
     * Default (0 / unknown) uses 7 bytes as conservative choice.
     *
     * @param version MySQL version number (e.g. 50600, 50700, 80000)
     */
    void set_mysql_version(uint32_t version) { mysql_version_ = version; }

private:
    Config config_;
    std::function<void(uint32_t, size_t)> progress_cb_;
    uint32_t mysql_version_ = 0;  // Not set → conservative 7-byte DB_ROLL_PTR

    // Statistics
    size_t total_pages_scanned_ = 0;
    size_t total_records_parsed_ = 0;
    size_t corrupted_records_skipped_ = 0;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SCHEMA_AGNOSTIC_SCANNER_HPP
