#ifndef DBRECOVERY_INNODB_INDEX_PAGE_PARSER_HPP
#define DBRECOVERY_INNODB_INDEX_PAGE_PARSER_HPP

/**
 * @file index_page_parser.hpp
 * @brief INDEX page parser for COMPACT/DYNAMIC/REDUNDANT row formats
 *
 * This is the core parser that enables >=99.9% recovery rate by correctly
 * parsing InnoDB record headers, null bitmasks, variable-length field sizes,
 * and extracting field data with schema awareness.
 *
 * Key differences from the old heuristic_parser:
 * 1. Correct 5-byte COMPACT/DYNAMIC record header parsing
 * 2. Null bitmask reading (backward from record end)
 * 3. Variable-length field size array (backward from record end)
 * 4. Off-page (overflow) pointer detection
 * 5. PAGE_FREE-based deleted record recovery
 * 6. Page directory-based fast recovery for corrupted pages
 */

#include <cstdint>
#include <algorithm>
#include <vector>
#include <string>
#include <optional>
#include <map>
#include <set>
#include <functional>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <fstream>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Page Constants ====================

/// Off-page flag for DYNAMIC/COMPRESSED row format
constexpr uint32_t OFF_PAGE_FLAG_DYNAMIC = 16404;

/// Off-page flag for COMPACT/REDUNDANT (Antelope) row format
constexpr uint32_t OFF_PAGE_FLAG_ANTLELOPE = 17172;  // 0x4314

/// Alias for compatibility
constexpr uint32_t OFF_PAGE_FLAG_COMPACT = OFF_PAGE_FLAG_ANTLELOPE;

/// System column sizes
constexpr size_t DB_TRX_ID_SIZE = 6;
constexpr size_t DB_ROLL_PTR_SIZE = 7;
constexpr size_t DB_ROW_ID_SIZE = 6;

/// INFIMUM record origin offset (data start) in COMPACT/DYNAMIC format pages
/// The 5-byte COMPACT record header precedes this at offset 94 (99 - 5)
/// The 6-byte REDUNDANT record header precedes offset 101 at offset 95 (101 - 6)
constexpr uint16_t INFIMUM_OFFSET = 99;

/// INFIMUM record origin offset for REDUNDANT format
constexpr uint16_t INFIMUM_OFFSET_OLD = 101;

/// Record header sizes
constexpr size_t REC_HEADER_SIZE_NEW = 5;   // COMPACT/DYNAMIC
constexpr size_t REC_HEADER_SIZE_OLD = 6;   // REDUNDANT

/// Maximum records per page (safety limit)
constexpr uint32_t MAX_RECORDS_PER_PAGE = 32768;

/// Global verbose flag for debug output
extern bool g_verbose;

} // namespace innodb
} // namespace dbrecovery

// JSON binary decoder - standalone function
std::string decode_json_binary(const uint8_t* data, size_t offset, size_t field_size);

namespace dbrecovery {
namespace innodb {

/// Maximum fields per record (safety limit)
constexpr uint32_t MAX_FIELDS_PER_RECORD = 4096;

// ==================== Column Definition (for schema-aware parsing) ====================

/**
 * @brief Column definition for schema-aware record parsing
 */
struct IndexColumnDef {
    std::string name;
    uint8_t col_type = 0;       // InnoDB column type ID
    uint32_t col_length = 0;    // Max byte length of column
    bool is_nullable = false;
    bool is_unsigned = false;
    bool is_binary = false;
    uint32_t char_set_id = 0;   // Character set ID
    uint16_t precision = 0;     // For DECIMAL
    uint16_t scale = 0;         // For DECIMAL
    uint8_t datetime_precision = 0;  // For TIME2, DATETIME2, TIMESTAMP2 (0-6)
    uint32_t mb_minlen = 1;     // Min bytes per character
    uint32_t mb_maxlen = 1;     // Max bytes per character
    bool is_big = false;        // True if col_length > 255 (affects var-size encoding)
    std::vector<std::string> enum_values;  // For ENUM type
    std::vector<std::string> set_values;   // For SET type

    // ✅ 新增：用于正确处理 InnoDB 变长列大小字节的读取顺序
    // InnoDB 按原始 SCHEMA 顺序（ordinal_position）存储大小字节和 NULL 位图
    // 但数据按存储顺序（physical_pos）存储
    uint32_t ordinal_position = 0;  // 原始 SCHEMA 定义顺序（从 1 开始）
    uint32_t physical_pos = 0;      // InnoDB物理存储顺序（从 1 开始）

    /**
     * @brief Check if this is a system column (DB_TRX_ID, DB_ROLL_PTR, DB_ROW_ID)
     *
     * CRITICAL: In DYNAMIC format, columns are stored as:
     * - VARIABLE columns FIRST (size stored in record): VARCHAR, BLOB, TEXT, JSON, CHAR(utf8mb4)
     * - FIXED columns AFTER (pre-computed size): INT, DATE, DATETIME, DECIMAL, ENUM, SET, BINARY
     *
     * DECIMAL, ENUM, SET have variable capacity but FIXED storage size for a given schema,
     * so they are stored with fixed columns in DYNAMIC format!
     *
     * CRITICAL FIX: Type 29 (STRING) is used for both CHAR and BINARY!
     * - CHAR with multi-byte charset (utf8mb4): variable-length (is_binary=false)
     * - BINARY: fixed-length (is_binary=true)
     */
    bool is_variable_length() const {
        // TRUE variable-length types (size stored in record):
        if (col_type == 16 ||   // VARCHAR/VARBINARY (InnoDB type 16)
            col_type == 24 ||   // TINY_BLOB (InnoDB type 24)
            col_type == 25 ||   // MEDIUM_BLOB (InnoDB type 25)
            col_type == 26 ||   // LONG_BLOB (InnoDB type 26)
            col_type == 27 ||   // BLOB/TEXT (InnoDB type 27)
            col_type == 28 ||   // VAR_STRING (InnoDB type 28)
            col_type == 30 ||   // GEOMETRY (InnoDB type 30)
            col_type == 31) {   // JSON (InnoDB type 31)
            return true;
        }

        // Type 29 (STRING) is used for both CHAR and BINARY
        // - BINARY (is_binary=true): always fixed-length
        // - CHAR with multi-byte charset (mb_maxlen > 1, e.g. utf8/utf8mb4): variable-length
        // - CHAR with single-byte charset (mb_maxlen == 1, e.g. latin1): fixed-length
        if (col_type == 29) {
            if (is_binary) return false;     // BINARY: fixed-length
            return mb_maxlen > 1;            // CHAR: variable only for multi-byte charset
        }

        return false;

        // NOTE: The following are FIXED-SIZE in DYNAMIC format (stored with fixed columns):
        // - BIT (17): fixed size based on bit count
        // - NEWDECIMAL (21): fixed size based on precision/scale
        // - ENUM (22): fixed size (1-2 bytes) based on value count
        // - SET (23): fixed size (1-8 bytes) based on value count
        // - BINARY (29 with is_binary=true): fixed size (always col_length bytes)
    }

    /**
     * @brief Get the fixed storage size for this column type
     * Returns 0 for variable-length columns (including CHAR for multi-byte charsets)
     * Uses InnoDB type IDs
     */
    uint32_t fixed_size() const {
        switch (col_type) {
            case 2:  return 1;   // TINY (InnoDB type 2)
            case 3:  return 2;   // SHORT (InnoDB type 3)
            case 4:  return 4;   // LONG (InnoDB type 4)
            case 5:  return 4;   // FLOAT (InnoDB type 5)
            case 6:  return 8;   // DOUBLE (InnoDB type 6)
            case 7:  return 4;   // TIMESTAMP (old, InnoDB type 7)
            case 9:  return 8;   // LONGLONG (InnoDB type 9)
            case 10: return 3;   // INT24 (InnoDB type 10)
            case 11: return 3;   // DATE (old, InnoDB type 11)
            case 12: return 3;   // TIME (old, InnoDB type 12)
            case 13: return 8;   // DATETIME (old, InnoDB type 13)
            case 14: return 1;   // YEAR (InnoDB type 14)
            case 15: return 3;   // NEWDATE/DATE (InnoDB type 15) - 3 bytes for DATE
            case 17: return (col_length + 7) / 8;  // BIT (InnoDB type 17)
            case 18: return 4;   // TIMESTAMP2 (InnoDB type 18)
            case 19: return 5;   // DATETIME2 (InnoDB type 19)
            case 20: return 3;   // TIME2 (InnoDB type 20)
            case 21: return decimal_storage_size(); // NEWDECIMAL (InnoDB type 21)
            case 22: return enum_storage_size();    // ENUM (InnoDB type 22)
            case 23: return set_storage_size();     // SET (InnoDB type 23)
            case 29:
                // Type 29 is used for both CHAR and BINARY
                // - BINARY (is_binary=true): fixed-length = col_length bytes
                // - CHAR single-byte (mb_maxlen == 1, e.g. latin1): fixed-length = col_length
                // - CHAR multi-byte (mb_maxlen > 1, e.g. utf8mb4): variable-length
                if (is_binary) {
                    return col_length > 0 ? col_length : 1;
                }
                if (mb_maxlen <= 1) {
                    return col_length > 0 ? col_length : 1;  // Single-byte CHAR is fixed-length
                }
                return 0;  // Multi-byte CHAR is variable-length
            default: return 0;   // Variable-length
        }
    }

    /**
     * @brief Calculate storage size for ENUM type
     * ENUM is stored as an integer index (1-based)
     * Size depends on number of enum values
     */
    uint32_t enum_storage_size() const {
        size_t num_values = enum_values.size();
        if (num_values <= 255) return 1;
        if (num_values <= 65535) return 2;
        return 3;  // Very large ENUM
    }

    /**
     * @brief Calculate storage size for SET type
     * SET is stored as a bitmask
     * Size depends on number of set members
     */
    uint32_t set_storage_size() const {
        size_t num_values = set_values.size();
        if (num_values <= 8) return 1;
        if (num_values <= 16) return 2;
        if (num_values <= 24) return 3;
        if (num_values <= 32) return 4;
        if (num_values <= 64) return 8;
        return 8;  // Max 64 members in MySQL SET
    }

    /**
     * @brief Calculate storage size for DECIMAL type
     * For DECIMAL(M,D): storage = ceil(M/9) * 4 bytes
     * But more precisely: (M-D)/9*4 + digits_to_bytes[(M-D)%9] + D/9*4 + digits_to_bytes[D%9]
     */
    uint32_t decimal_storage_size() const {
        // Digits to bytes mapping: [0, 1, 1, 2, 2, 3, 3, 4, 4, 4]
        static constexpr uint8_t digits_to_bytes[10] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 4};

        uint32_t p = precision > 0 ? precision : col_length;
        uint32_t s = scale;

        // Integer part digits
        uint32_t int_digits = p - s;
        uint32_t int_bytes = (int_digits / 9) * 4 + digits_to_bytes[int_digits % 9];

        // Fractional part digits
        uint32_t frac_bytes = (s / 9) * 4 + digits_to_bytes[s % 9];

        return int_bytes + frac_bytes;
    }

    /**
     * @brief Get total storage size including fractional part for temporal types
     * IMPORTANT: System columns (DB_TRX_ID, DB_ROLL_PTR, DB_ROW_ID) have FIXED sizes
     * regardless of their type IDs in SDI (which may be incorrect).
     */
    uint32_t storage_size() const {
        // CRITICAL: System columns have fixed sizes by NAME, not by type!
        // SDI may store wrong types for system columns.
        if (name == "DB_TRX_ID") return 6;
        if (name == "DB_ROLL_PTR") return 7;
        if (name == "DB_ROW_ID") return 6;

        uint32_t base = fixed_size();
        // Add fractional seconds storage for new temporal types
        // InnoDB types: 18=TIMESTAMP2, 19=DATETIME2, 20=TIME2
        // Fractional seconds: (precision+1)/2 bytes
        if (col_type == 18 || col_type == 19 || col_type == 20) {
            uint8_t prec = datetime_precision;  // Use datetime_precision, NOT scale
            if (prec > 0 && prec <= 2) base += 1;
            else if (prec >= 3 && prec <= 4) base += 2;
            else if (prec >= 5 && prec <= 6) base += 3;
        }
        return base;
    }

    /**
     * @brief Check if this is a system column (DB_TRX_ID, DB_ROLL_PTR, DB_ROW_ID)
     * System columns are NOT included in NULL bitmap and variable size calculations.
     */
    bool is_system_column() const {
        return name == "DB_TRX_ID" || name == "DB_ROLL_PTR" || name == "DB_ROW_ID";
    }
};

// ==================== Recovered Field Value ====================

/**
 * @brief A recovered field value from an InnoDB record
 */
struct IndexFieldValue {
    enum class Type {
        NULL_VAL,
        INT_VAL,
        UINT_VAL,
        FLOAT_VAL,
        DOUBLE_VAL,
        STRING_VAL,
        BINARY_VAL,
        DECIMAL_VAL,
        TIMESTAMP_VAL,
        DATETIME_VAL,
        DATE_VAL,
        TIME_VAL,
        OVERFLOW_PTR,   // Off-page pointer (20 bytes)
        UNKNOWN_VAL
    };

    Type type = Type::UNKNOWN_VAL;
    std::vector<uint8_t> raw_bytes;
    std::string str_value;          // Decoded string representation
    int64_t int_value = 0;
    uint64_t uint_value = 0;
    double float_value = 0.0;
    bool is_null = false;
    bool is_deleted = false;        // From deleted record
    bool is_overflow = false;       // Data is off-page (overflow pointer)
    bool is_unparsable_json = false; // Raw JSON evidence; never structured-exportable

    // Overflow pointer details
    uint32_t overflow_space_id = 0;
    uint32_t overflow_page_no = 0;
    uint32_t overflow_blob_header = 0;
    uint64_t overflow_real_size = 0;

    std::string to_string() const {
        if (is_null) return "NULL";
        switch (type) {
            case Type::NULL_VAL: return "NULL";
            case Type::INT_VAL: return std::to_string(int_value);
            case Type::UINT_VAL: return std::to_string(uint_value);
            case Type::FLOAT_VAL: return std::to_string(float_value);
            case Type::DOUBLE_VAL: return std::to_string(float_value);
            case Type::DECIMAL_VAL: return str_value;
            case Type::STRING_VAL: return str_value;
            case Type::DATE_VAL: return str_value;
            case Type::DATETIME_VAL: return str_value;
            case Type::TIMESTAMP_VAL: return str_value;
            case Type::BINARY_VAL: {
                if (!str_value.empty()) return str_value;
                // If raw_bytes is used instead, convert to hex
                std::string hex;
                for (uint8_t b : raw_bytes) {
                    char buf[3];
                    snprintf(buf, sizeof(buf), "%02x", b);
                    hex += buf;
                }
                return hex;
            }
            case Type::OVERFLOW_PTR:
                return "<OFFPAGE space=" + std::to_string(overflow_space_id) +
                       " page=" + std::to_string(overflow_page_no) +
                       " size=" + std::to_string(overflow_real_size) + ">";
            default: return "<UNKNOWN>";
        }
    }
};

// ==================== Recovered Index Record ====================

/**
 * @brief A fully recovered record from an InnoDB INDEX page
 */
struct IndexRecord {
    uint32_t page_no = 0;
    uint16_t record_offset = 0;    // Offset within the page
    uint16_t heap_no = 0;
    bool is_deleted = false;
    bool is_infimum = false;
    bool is_supremum = false;
    bool is_min_rec = false;
    uint8_t n_owned = 0;
    uint8_t rec_type = 0;
    int16_t next_record_offset = 0;

    // System columns
    uint64_t trx_id = 0;
    uint64_t roll_ptr = 0;
    uint64_t row_id = 0;

    // User column values (in table column order, excluding system columns)
    std::vector<IndexFieldValue> fields;

    // Raw record data (for debugging)
    std::vector<uint8_t> raw_data;

    // Confidence score for this record (0.0-1.0)
    double confidence = 1.0;

    bool has_unparsable_json_evidence() const {
        return std::any_of(fields.begin(), fields.end(), [](const IndexFieldValue& field) {
            return field.is_unparsable_json;
        });
    }

    /// True when any column still holds an off-page LOB pointer instead of the
    /// payload itself.  Such a record must never be rendered: the pointer bytes
    /// would be exported as if they were the column value.
    bool has_unreconstructed_overflow() const {
        return std::any_of(fields.begin(), fields.end(), [](const IndexFieldValue& field) {
            return field.is_overflow;
        });
    }

    std::string to_csv_line() const {
        if (has_unparsable_json_evidence()) return {};
        if (has_unreconstructed_overflow()) return {};
        if (has_unparsable_json_evidence()) return {};
        // Pre-calculate approximate size to reduce allocations
        size_t estimated_size = fields.size() * 16;  // rough estimate
        for (const auto& f : fields) {
            if (!f.is_null && (f.type == IndexFieldValue::Type::STRING_VAL ||
                               f.type == IndexFieldValue::Type::BINARY_VAL)) {
                estimated_size += f.str_value.size();
            }
        }

        std::string result;
        result.reserve(estimated_size);

        for (size_t i = 0; i < fields.size(); i++) {
            if (i > 0) result += ',';
            const auto& f = fields[i];
            if (f.is_null) {
                result += "NULL";
            } else if (f.type == IndexFieldValue::Type::STRING_VAL ||
                       f.type == IndexFieldValue::Type::BINARY_VAL) {
                const std::string& escaped = f.str_value;
                // Check if quoting needed
                bool needs_quote = escaped.find(',') != std::string::npos ||
                                   escaped.find('"') != std::string::npos ||
                                   escaped.find('\n') != std::string::npos;
                if (needs_quote) {
                    result += '"';
                    for (char c : escaped) {
                        if (c == '"') {
                            result += "\"\"";
                        } else {
                            result += c;
                        }
                    }
                    result += '"';
                } else {
                    result += escaped;
                }
            } else {
                result += f.to_string();
            }
        }
        return result;
    }

    std::string to_sql_values(const std::string& table_name = "recovered_table") const {
        if (has_unparsable_json_evidence()) return {};
        if (has_unreconstructed_overflow()) return {};
        // Pre-calculate size
        size_t estimated_size = 50 + table_name.size() + fields.size() * 20;
        for (const auto& f : fields) {
            if (!f.is_null) {
                estimated_size += f.str_value.size() * 2 + f.raw_bytes.size() * 2;
            }
        }

        std::string result;
        result.reserve(estimated_size);

        result += "INSERT INTO ";
        result += table_name;
        result += " VALUES (";

        for (size_t i = 0; i < fields.size(); i++) {
            if (i > 0) result += ", ";
            const auto& f = fields[i];
            if (f.is_null) {
                result += "NULL";
            } else if (f.type == IndexFieldValue::Type::STRING_VAL ||
                       f.type == IndexFieldValue::Type::DATETIME_VAL ||
                       f.type == IndexFieldValue::Type::DATE_VAL ||
                       f.type == IndexFieldValue::Type::TIMESTAMP_VAL ||
                       f.type == IndexFieldValue::Type::TIME_VAL) {
                result += '\'';
                // Escape single quotes and backslashes inline
                for (char c : f.str_value) {
                    if (c == '\'') {
                        result += "\\'";
                    } else if (c == '\\') {
                        result += "\\\\";
                    } else {
                        result += c;
                    }
                }
                result += '\'';
            } else if (f.type == IndexFieldValue::Type::BINARY_VAL) {
                // Output as hex string for binary data
                result += "0x";
                for (uint8_t b : f.raw_bytes) {
                    const char hex_chars[] = "0123456789abcdef";
                    result += hex_chars[b >> 4];
                    result += hex_chars[b & 0xf];
                }
            } else if (f.type == IndexFieldValue::Type::UNKNOWN_VAL ||
                       f.type == IndexFieldValue::Type::OVERFLOW_PTR) {
                if (!f.str_value.empty() && f.str_value != "<UNKNOWN>") {
                    result += '\'';
                    for (char c : f.str_value) {
                        if (c == '\'') {
                            result += "\\'";
                        } else {
                            result += c;
                        }
                    }
                    result += '\'';
                } else if (!f.raw_bytes.empty()) {
                    result += "0x";
                    for (uint8_t b : f.raw_bytes) {
                        const char hex_chars[] = "0123456789abcdef";
                        result += hex_chars[b >> 4];
                        result += hex_chars[b & 0xf];
                    }
                } else {
                    result += "NULL";
                }
            } else {
                result += f.to_string();
            }
        }
        result += ");";
        return result;
    }
};

// ==================== Record Header ====================

/**
 * @brief Parsed COMPACT/DYNAMIC record header
 */
struct NewRecordHeader {
    // Bit fields from 3-byte header (24 bits, big-endian)
    bool instant_flag;       // bit 23
    bool version_flag;       // bit 22
    bool deleted;            // bit 21
    bool min_rec;            // bit 20
    uint8_t n_owned;         // bits 19-16 (4 bits)
    uint16_t heap_no;        // bits 15-3 (13 bits)
    uint8_t rec_type;        // bits 2-0 (3 bits)

    // Next record offset (signed 16-bit, relative to START of this record header)
    int16_t next_offset;

    // The raw 3 header bytes
    uint8_t raw_byte0, raw_byte1, raw_byte2;
};

/**
 * @brief Parsed REDUNDANT record header
 */
struct OldRecordHeader {
    bool instant_flag;       // bit 31
    bool version_flag;       // bit 30
    bool deleted;            // bit 29
    bool min_rec;            // bit 28
    uint8_t n_owned;         // bits 27-24 (4 bits)
    uint16_t heap_no;        // bits 23-11 (13 bits)
    uint16_t n_fields;       // bits 10-1 (10 bits)
    bool short_flag;         // bit 0

    // Next record offset (unsigned 16-bit)
    uint16_t next_offset;
};

// ==================== Page Header ====================

/**
 * @brief InnoDB page header parsed from bytes 38+ of a page
 * All fields are stored BIG-ENDIAN in InnoDB
 */
struct InnoDBPageHeader {
    uint16_t n_dir_slots;    // PAGE_N_DIR_SLOTS (offset 38)
    uint16_t heap_top;       // PAGE_HEAP_TOP (offset 40)
    uint16_t n_heap;         // PAGE_N_HEAP (offset 42), bit 15 = format flag
    uint16_t page_free;      // PAGE_FREE (offset 44)
    uint16_t garbage;        // PAGE_GARBAGE (offset 46)
    uint16_t last_insert;    // PAGE_LAST_INSERT (offset 48)
    uint16_t direction;      // PAGE_DIRECTION (offset 50)
    uint16_t n_direction;    // PAGE_N_DIRECTION (offset 52)
    uint16_t n_recs;         // PAGE_N_RECS (offset 54)
    uint64_t max_trx_id;     // PAGE_MAX_TRX_ID (offset 56)
    uint16_t level;          // PAGE_LEVEL (offset 64)
    uint64_t index_id;       // PAGE_INDEX_ID (offset 66)

    // Derived values
    uint16_t n_heap_raw;     // Raw n_heap value (including format bit)
    bool is_compact_format;  // True if n_heap bit 15 is set (COMPACT/DYNAMIC)

    // Deleted record count: (n_heap & 0x7FFF) - n_recs - 2
    uint16_t deleted_count() const {
        uint16_t heap_records = n_heap_raw & 0x7FFF;
        if (heap_records < n_recs + 2) return 0;
        return heap_records - n_recs - 2;
    }

    std::string to_string() const {
        std::ostringstream oss;
        oss << "PageHeader{dir_slots=" << n_dir_slots
            << ", heap_top=" << heap_top
            << ", n_heap=" << n_heap
            << ", free=" << page_free
            << ", garbage=" << garbage
            << ", n_recs=" << n_recs
            << ", level=" << level
            << ", index_id=" << index_id
            << ", compact=" << is_compact_format
            << ", deleted_count=" << deleted_count()
            << "}";
        return oss.str();
    }
};

// ==================== InnoDBIndexParser ====================

/**
 * @brief INDEX page parser
 *
 * This is the core class that correctly parses InnoDB INDEX pages
 * to extract records with proper COMPACT/DYNAMIC/REDUNDANT handling.
 *
 * Usage:
 *   InnoDBIndexParser parser;
 *   parser.set_columns(columns);  // Optional: set schema for type-aware parsing
 *   auto records = parser.parse_page(page_data, page_size, page_no);
 */
class InnoDBIndexParser {
public:
    InnoDBIndexParser();
    ~InnoDBIndexParser();

    // ---- Configuration ----

    /**
     * @brief Set column definitions for schema-aware parsing
     * @param columns Column definitions in table order (PK columns first, then others)
     * @param has_row_id True if table has no explicit PK (InnoDB generates DB_ROW_ID)
     * @param n_pk_cols Number of primary key columns
     */
    void set_columns(const std::vector<IndexColumnDef>& columns,
                     bool has_row_id = false,
                     uint32_t n_pk_cols = 1);

    /**
     * @brief Set row format hint (COMPACT, DYNAMIC, REDUNDANT)
     * If not set, auto-detect from page header
     */
    void set_row_format(uint8_t format);

    /**
     * @brief Enable/disable deleted record recovery
     * When enabled, records from PAGE_FREE chain and delete-marked records are included
     */
    void set_recover_deleted(bool enable) { recover_deleted_ = enable; }

    /**
     * @brief Enable strict decoding for schema-aware recovery
     *
     * When strict decoding is enabled the parser only accepts records found by
     * walking the on-page record chain. The brute-force page scans
     * (parse_page_fast / parse_page_try) fabricate plausible-looking rows from
     * unrelated bytes, which is acceptable for evidence but never acceptable
     * for exportable structured recovery.
     */
    void set_strict(bool enable) { strict_ = enable; }

    /**
     * @brief Set bad page handling mode
     * 0 = skip bad pages (default)
     * 1 = fast mode (page directory traversal)
     * 2 = try mode (brute-force byte scan)
     */
    void set_bad_page_mode(int mode) { bad_page_mode_ = mode; }

    /**
     * @brief Set off-page flag based on row format
     */
    void set_off_page_flag(uint32_t flag) { off_page_flag_ = flag; }

    /**
     * @brief Set MySQL version for format-specific behavior
     */
    void set_mysql_version(uint32_t version) { mysql_version_ = version; }

    /**
     * @brief Get current MySQL version
     */
    uint32_t get_mysql_version() const { return mysql_version_; }

    /**
     * @brief Pin the physical page size before parsing a file
     *
     * InnoDB pages carry no page-size field, so a hint taken from the server
     * configuration (or from a correctly detected first page) is the only
     * authoritative source. Without it `parse_ibd_force` falls back to layout
     * detection, which must not guess 16 KiB for an 8 KiB tablespace.
     */
    void set_page_size_hint(uint32_t size) { page_size_hint_ = size; }

    /**
     * @brief Enable/disable debug output
     */
    void set_verbose(bool verbose) { verbose_ = verbose; }

    /**
     * @brief Set target INDEX_ID to filter pages (for force mode)
     * 0 = accept all INDEX pages
     */
    void set_target_index_id(uint64_t index_id) { target_index_id_ = index_id; }

    /**
     * @brief Records dropped by the clustered-leaf key-order guard
     *
     * In strict schema-aware mode with a fixed-size integer primary key, a
     * leaf record whose stored key bytes violate the chain's non-decreasing
     * order is proven corrupted (InnoDB could not have written it there) and
     * is dropped instead of exported. Returns how many rows the guard removed.
     */
    uint64_t key_order_violations_dropped() const { return key_order_violations_dropped_; }

    /**
     * @brief Records dropped by the off-page LOB pointer fail-closed guard
     *
     * When a column's variable-length prefix resolves to an off-page pointer,
     * the record holds only the 20-byte pointer and no payload; this path
     * never reassembles the external BLOB chain, so exporting the row would
     * fabricate the column from pointer bytes. The whole record is dropped
     * and counted here instead.
     */
    void set_retain_overflow_records(bool enable) { retain_overflow_records_ = enable; }

    uint64_t overflow_pointer_records_dropped() const {
        return overflow_pointer_records_dropped_;
    }

    // ---- Page Parsing ----

    /**
     * @brief Parse page header from raw page data
     * @param page_data Pointer to page data (must be at least page_size bytes)
     * @param page_size Page size in bytes
     * @return Parsed page header, or nullopt if invalid
     */
    std::optional<InnoDBPageHeader> parse_page_header(const uint8_t* page_data, size_t page_size) const;

    /**
     * @brief Parse all records from an INDEX page
     * @param page_data Pointer to page data
     * @param page_size Page size in bytes
     * @param page_no Page number (for metadata)
     * @return Vector of recovered records
     */
    std::vector<IndexRecord> parse_page(const uint8_t* page_data, size_t page_size, uint32_t page_no);

    /**
     * @brief Parse records using page directory (fast mode for corrupted pages)
     */
    std::vector<IndexRecord> parse_page_fast(const uint8_t* page_data, size_t page_size, uint32_t page_no);

    /**
     * @brief Parse records by brute-force byte scan (try mode for severely corrupted pages)
     */
    std::vector<IndexRecord> parse_page_try(const uint8_t* page_data, size_t page_size, uint32_t page_no);

    // ---- Record Header Parsing ----

    /**
     * @brief Parse COMPACT/DYNAMIC record header (5 bytes)
     */
    static std::optional<NewRecordHeader> parse_new_record_header(const uint8_t* data, size_t offset, size_t max_size);

    /**
     * @brief Parse REDUNDANT record header (6 bytes)
     */
    static std::optional<OldRecordHeader> parse_old_record_header(const uint8_t* data, size_t offset, size_t max_size);

    // ---- Schema-less Record Parsing ----

    /**
     * @brief Parse a record without schema (heuristic field boundary detection)
     * Uses null bitmap and variable-length size array from the record itself
     */
    std::vector<IndexFieldValue> parse_record_schemaless(
        const uint8_t* page_data, size_t page_size,
        size_t rec_offset, size_t rec_end,
        bool is_compact, bool is_leaf);

    // ---- Full File Processing ----

    /**
     * @brief Parse all pages from an IBD file sequentially (force mode)
     * Scans every page in the file
     * @param ibd_path Path to .ibd file
     * @return All recovered records from all leaf INDEX pages
     */
    std::vector<IndexRecord> parse_ibd_force(const std::string& ibd_path);

    /**
     * @brief Parse IBD file and export records directly to CSV (streaming, low memory)
     * @param ibd_path Path to .ibd file
     * @param csv_output Path to output CSV file
     * @param columns Column definitions for header row (optional)
     * @return Number of records exported
     */
    uint64_t parse_ibd_force_streaming(const std::string& ibd_path,
                                        const std::string& csv_output,
                                        const std::vector<IndexColumnDef>& columns = {});

    /**
     * @brief Parse IBD file following B+Tree leaf page chain
     * @param ibd_path Path to .ibd file
     * @param root_page_no Root page number of the primary key index
     * @return All recovered records from leaf pages
     */
    std::vector<IndexRecord> parse_ibd_btree(const std::string& ibd_path, uint32_t root_page_no);

    /**
     * @brief Find leaf pages from root page using B+Tree traversal (file-based)
     */
    std::vector<uint32_t> find_leaf_pages_from_file(
        std::ifstream& file, uint32_t page_size, uint32_t total_pages,
        uint32_t root_page_no);

    // ---- Utility ----

    /**
     * @brief Check if a page is a valid INDEX leaf page
     */
    bool is_index_leaf_page(const uint8_t* page_data, size_t page_size) const;

    /**
     * @brief Get the number of records recovered so far
     */
    uint64_t get_total_records() const { return total_records_; }

    /**
     * @brief Get the number of pages parsed so far
     */
    uint64_t get_total_pages() const { return total_pages_; }

    /**
     * @brief Get statistics
     */
    struct Stats {
        uint64_t pages_scanned = 0;
        uint64_t pages_valid = 0;
        uint64_t pages_index_leaf = 0;
        uint64_t pages_index_nonleaf = 0;
        uint64_t records_recovered = 0;
        uint64_t records_deleted = 0;
        uint64_t records_skipped = 0;
        uint64_t pages_skipped_corrupt = 0;
        uint64_t dedup_removed = 0;
        /// Active clustered-leaf rows rejected because their verified integer
        /// PRIMARY-key bytes violate the B-tree record-chain order.
        uint64_t key_order_violations_dropped = 0;
        /// Rows rejected because a LOB column still points off-page and the
        /// external chain was never reassembled (fail-closed, no CSV row).
        uint64_t overflow_pointer_records_dropped = 0;
    };
    const Stats& get_stats() const { return stats_; }

    /**
     * @brief Get the column definitions (after set_columns was called)
     * @return Vector of column definitions with names and types
     */
    const std::vector<IndexColumnDef>& get_column_defs() const { return columns_; }

private:
    // ---- Internal Parsing ----

    /**
     * @brief Follow the record chain starting from infimum
     * Returns all user records in page order
     *
     * IMPORTANT: In InnoDB COMPACT format, the record chain works as follows:
     * - Each record has an "origin" (data start) and a header that precedes it
     * - INFIMUM origin is at offset 99 (COMPACT) or 101 (REDUNDANT)
     * - The header is read BACKWARD from the origin (5 bytes for COMPACT, 6 for REDUNDANT)
     * - REC_NEXT is a signed 16-bit offset RELATIVE to the current record's origin
     * - The next record's absolute offset = current_origin + REC_NEXT
     */
    std::vector<IndexRecord> follow_record_chain(
        const uint8_t* page_data, size_t page_size,
        const InnoDBPageHeader& page_hdr, uint32_t page_no,
        bool is_compact);

    /**
     * @brief Follow the PAGE_FREE chain for deleted records
     */
    std::vector<IndexRecord> follow_free_chain(
        const uint8_t* page_data, size_t page_size,
        const InnoDBPageHeader& page_hdr, uint32_t page_no,
        bool is_compact);

    /**
     * @brief Parse a single user record at the given offset
     * Handles both COMPACT/DYNAMIC and REDUNDANT formats
     */
    std::optional<IndexRecord> parse_user_record(
        const uint8_t* page_data, size_t page_size,
        size_t rec_offset, uint32_t page_no,
        bool is_compact, bool is_leaf);

    /**
     * @brief Read null bitmask and variable-length field sizes from a COMPACT/DYNAMIC record
     * This reads BACKWARD from the record end.
     *
     * @param page_data Full page data
     * @param rec_start Offset of record header (5 bytes before record data start)
     * @param rec_end Offset of record data end (= start of next record or heap_top)
     * @param n_nullable Number of nullable columns
     * @param n_variable Number of variable-length columns
     * @param columns Column definitions
     * @return Pair of (null_bitmask_bytes, variable_sizes)
     */
    struct FieldMetadata {
        std::vector<uint8_t> null_bitmask;
        std::vector<uint32_t> variable_sizes;
        size_t null_bytes_read;
        size_t var_size_bytes_read;
        size_t data_start_offset;  // Position after reading sizes backward (not used for forward data)
    };
    FieldMetadata read_null_and_varsizes(
        const uint8_t* page_data, size_t page_size,
        size_t rec_start, size_t rec_end,
        uint32_t n_nullable, uint32_t n_variable,
        const std::vector<IndexColumnDef>& columns,
        uint8_t row_format = 2);  // Default to DYNAMIC (2)

    /**
     * @brief Read null bitmask and field offsets for REDUNDANT format
     * REDUNDANT uses cumulative offsets instead of separate var-size array
     */
    struct RedundantFieldMetadata {
        std::vector<uint16_t> field_offsets;  // Cumulative byte offsets for each field
        std::vector<bool> null_flags;          // Null flag per field
        bool short_flag;                       // 1-byte or 2-byte offset entries
    };
    RedundantFieldMetadata read_redundant_field_offsets(
        const uint8_t* page_data, size_t page_size,
        size_t rec_start, size_t rec_end,
        uint32_t n_fields);

    /**
     * @brief Extract field values from record data using schema
     */
    std::vector<IndexFieldValue> extract_fields_with_schema(
        const uint8_t* page_data, size_t page_size,
        size_t data_start, size_t data_end,
        const std::vector<IndexColumnDef>& columns,
        const std::vector<uint8_t>& null_bitmask,
        const std::vector<uint32_t>& variable_sizes,
        bool is_leaf);

    /**
     * @brief Extract field values from record data without schema (schemaless)
     * Uses the null bitmask and variable sizes from the record itself
     */
    std::vector<IndexFieldValue> extract_fields_schemaless(
        const uint8_t* page_data, size_t page_size,
        size_t data_start, size_t data_end,
        const std::vector<uint8_t>& null_bitmask,
        const std::vector<uint32_t>& variable_sizes,
        bool is_compact, bool is_leaf);

    /**
     * @brief Decode a specific column type from raw bytes
     */
    IndexFieldValue decode_column(const uint8_t* data, size_t offset, size_t max_size,
                                   const IndexColumnDef& col, size_t field_size);

    /**
     * @brief Read system columns (DB_TRX_ID, DB_ROLL_PTR, DB_ROW_ID) from record
     */
    void read_system_columns(const uint8_t* page_data, size_t offset,
                              IndexRecord& record, bool has_row_id);

    /**
     * @brief Check if a page is an INDEX page
     */
    bool is_index_page(const uint8_t* page_data, size_t page_size) const;

    /**
     * @brief Validate INFIMUM and SUPREMUM markers in page
     */
    bool validate_infimum_supremum(const uint8_t* page_data, size_t page_size, bool is_compact) const;

    /**
     * @brief Validate page checksum (both old and new style)
     */
    bool validate_page_checksum(const uint8_t* page_data, size_t page_size) const;

    /**
     * @brief Deduplicate records by (page_no, heap_no) pair
     */
    void deduplicate_records(std::vector<IndexRecord>& records);

    // ---- Member Variables ----
    std::vector<IndexColumnDef> columns_;
    bool has_row_id_ = false;
    uint32_t n_pk_cols_ = 1;
    uint32_t n_nullable_ = 0;
    uint32_t n_variable_ = 0;
    bool verbose_ = false;
    bool recover_deleted_ = true;
    int bad_page_mode_ = 0;
    bool strict_ = false;             // true = chain-only decoding for schema-aware export
    uint32_t off_page_flag_ = OFF_PAGE_FLAG_DYNAMIC;
    uint32_t mysql_version_ = 0;      // 0 = not set; callers must set explicitly (e.g. 50600/50700/80000/80400/90000)
    uint64_t target_index_id_ = 0;     // 0 = accept all
    uint8_t row_format_hint_ = 0;      // 0 = auto-detect
    uint32_t page_size_hint_ = 0;      // 0 = derive from the file layout
    /// Set when a field length could not be bounded by the record buffer.
    /// The whole record is then rejected instead of exporting a shifted row.
    bool field_size_guard_overflow_ = false;

    /// Records dropped by the clustered-leaf key-order guard (strict schema
    /// mode, integer pk): their stored key bytes cannot be part of an
    /// InnoDB-written chain, so they are corruptions rather than real rows.
    uint64_t key_order_violations_dropped_ = 0;

    /// Physical positions whose variable-length prefix resolved to an off-page
    /// BLOB pointer (record carries the 20-byte pointer, not the payload).
    /// Refilled for every record by read_null_and_varsizes().
    std::vector<uint32_t> offpage_physical_pos_;
    std::map<uint32_t, uint32_t> offpage_inline_prefix_len_;
    /// Records dropped by the off-page pointer fail-closed guard.
    uint64_t overflow_pointer_records_dropped_ = 0;
    /// When true, records with off-page LOB pointers are returned to caller
    /// for external chain reassembly instead of dropped immediately.
    bool retain_overflow_records_ = false;

    uint64_t total_records_ = 0;
    uint64_t total_pages_ = 0;
    Stats stats_;

    // Deduplication tracking
    std::set<std::pair<uint32_t, uint16_t>> seen_records_;
};

// ==================== Utility Functions ====================

/**
 * @brief Validate FSP metadata using the supplied bounded page buffer.
 * @note ZIP_SSIZE describes a physical compressed-page size, not the logical page size.
 */
uint32_t detect_page_size_from_fsp(const uint8_t* page_data, size_t available_bytes);

/**
 * @brief Detect page size from file size when header is corrupted
 * Returns most likely page size based on file alignment
 */
uint32_t detect_page_size_from_file_size(size_t file_size);

/**
 * @brief Smart page size detection with corruption resilience
 * Tries multiple methods and validates results
 */
uint32_t smart_detect_page_size(const uint8_t* page_data, size_t available_bytes,
                                size_t file_size);

/**
 * @brief Detect row format from FSP space flags
 * Returns: 0=REDUNDANT, 1=COMPACT, 2=DYNAMIC, 3=COMPRESSED
 */
uint8_t detect_row_format_from_fsp(const uint8_t* page_data, size_t available_bytes);

/**
 * @brief Check if FSP flags indicate Antelope (pre-5.7) format
 */
bool is_antelope_format(const uint8_t* page_data, size_t available_bytes);

/**
 * @brief Read FSP space flags from the first page
 */
uint32_t read_fsp_space_flags(const uint8_t* page_data, size_t available_bytes);

/**
 * @brief Get root page number from INODE page (page 2 or 3)
 */
uint32_t get_root_page_from_inode(const uint8_t* ibd_data, size_t ibd_size, uint32_t page_size);

/**
 * @brief Find leaf pages starting from root page (B+Tree traversal)
 */
std::vector<uint32_t> find_leaf_pages(const uint8_t* ibd_data, size_t ibd_size,
                                       uint32_t page_size, uint32_t root_page_no);

/**
 * @brief CRC32C checksum validation for InnoDB pages
 */
bool validate_innodb_checksum(const uint8_t* page_data, size_t page_size);

/**
 * @brief Legacy InnoDB checksum validation
 */
bool validate_innodb_checksum_legacy(const uint8_t* page_data, size_t page_size);

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_INDEX_PAGE_PARSER_HPP
