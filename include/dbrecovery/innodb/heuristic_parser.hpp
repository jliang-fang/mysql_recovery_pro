#ifndef DBRECOVERY_INNODB_HEURISTIC_PARSER_HPP
#define DBRECOVERY_INNODB_HEURISTIC_PARSER_HPP

/**
 * @file heuristic_parser.hpp
 * @brief Schemaless record parser for extreme corruption scenarios
 *
 * This parser works without requiring SDI metadata and uses heuristics
 * to detect field boundaries in corrupted IBD files.
 *
 * Key features:
 * - 1-byte/2-byte length prefix detection (0xFF marker for long fields)
 * - NULL marker (0x80) detection
 * - Automatic field count inference
 * - CSV/JSON/SQL export without schema
 */

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <iostream>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Heuristic Field Types ====================
/**
 * @brief HeuristicFieldType: Field type detected by heuristics
 */
enum class HeuristicFieldType {
    UNKNOWN = 0,
    TINY_INT = 1,             // 1 byte
    SHORT_INT = 2,            // 2 bytes
    LONG_INT = 4,             // 4 bytes
    LONGLONG = 8,             // 8 bytes
    VARCHAR_SHORT = 16,       // 1-byte length prefix (0-255 bytes)
    VARCHAR_LONG = 17,        // 2-byte length prefix (256-65535 bytes)
    VARCHAR_VERY_LONG = 18,   // 3-byte length prefix (65536+ bytes)
    NULL_VALUE = 0x80,        // NULL marker
    BLOB_POINTER = 20,        // BLOB pointer (20 bytes)
};

/**
 * @brief HeuristicField: Detected field with inferred type
 */
struct HeuristicField {
    HeuristicFieldType type;
    std::vector<uint8_t> data;
    bool is_null;
    bool is_valid;
    
    HeuristicField()
        : type(HeuristicFieldType::UNKNOWN), is_null(false), is_valid(false) {}
    
    std::string to_string() const {
        if (!is_valid) {
            return "<INVALID>";
        }
        if (is_null) {
            return "NULL";
        }
        
        switch (type) {
            case HeuristicFieldType::TINY_INT:
                if (data.size() == 1) {
                    return std::to_string(static_cast<int8_t>(data[0]));
                }
                break;
                
            case HeuristicFieldType::SHORT_INT:
                if (data.size() == 2) {
                    return std::to_string(read_le<int16_t>(data.data()));
                }
                break;
                
            case HeuristicFieldType::LONG_INT:
                if (data.size() == 4) {
                    return std::to_string(read_le<int32_t>(data.data()));
                }
                break;
                
            case HeuristicFieldType::LONGLONG:
                if (data.size() == 8) {
                    return std::to_string(read_le<int64_t>(data.data()));
                }
                break;
                
            case HeuristicFieldType::VARCHAR_SHORT:
            case HeuristicFieldType::VARCHAR_LONG:
            case HeuristicFieldType::VARCHAR_VERY_LONG:
                // Try to decode as UTF-8 or Latin1
                if (!data.empty()) {
                    std::string result;
                    result.reserve(data.size());
                    for (uint8_t b : data) {
                        if (b >= 32 && b < 127) {
                            result += static_cast<char>(b);
                        } else if (b == 0) {
                            result += ' ';
                        } else {
                            result += '?';
                        }
                    }
                    return result;
                }
                break;
                
            default:
                break;
        }
        
        // Fallback: hex dump
        std::string hex;
        hex.reserve(data.size() * 2);
        for (uint8_t b : data) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", b);
            hex += buf;
        }
        return hex;
    }
};

/**
 * @brief HeuristicRecord: A record parsed with heuristics
 */
struct HeuristicRecord {
    uint16_t offset;
    uint16_t length;
    std::vector<HeuristicField> fields;
    bool is_infimum;
    bool is_supremum;
    bool is_deleted;
    
    HeuristicRecord()
        : offset(0), length(0), is_infimum(false), is_supremum(false), is_deleted(false) {}
    
    std::string to_csv() const {
        if (is_infimum || is_supremum) {
            return "";
        }
        
        std::string result;
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) {
                result += ",";
            }
            result += fields[i].to_string();
        }
        return result;
    }
    
    std::string to_json() const {
        if (is_infimum || is_supremum) {
            return "";
        }
        
        std::string result = "{";
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) {
                result += ", ";
            }
            result += "\"field" + std::to_string(i) + "\": \"" + fields[i].to_string() + "\"";
        }
        result += "}";
        return result;
    }
};

/**
 * @brief HeuristicRecordHeader: Parsed record header with heuristic info
 */
struct HeuristicRecordHeader {
    uint16_t offset;
    uint16_t next_offset;
    uint8_t info_bits;
    uint16_t heap_no;
    bool is_deleted;
    bool is_infimum;
    bool is_supremum;
    uint16_t n_fields;  // Inferred field count
    
    HeuristicRecordHeader()
        : offset(0), next_offset(0), info_bits(0), heap_no(0),
          is_deleted(false), is_infimum(false), is_supremum(false), n_fields(0) {}
};

// ==================== Heuristic Parser ====================
/**
 * @brief HeuristicParser: Schemaless record parser using heuristics
 */
class HeuristicParser {
public:
    HeuristicParser();
    
    /**
     * @brief Parse a single record at the given offset
     * @param data Page data buffer
     * @param size Page data size
     * @param offset Record offset within page
     * @return Parsed record or nullopt if parsing failed
     */
    std::optional<HeuristicRecord> parse_record(
        const uint8_t* data, size_t size, size_t offset);
    
    /**
     * @brief Parse all records in a page
     * @param data Page data buffer
     * @param size Page data size
     * @param heap_top Heap top position
     * @return Vector of parsed records
     */
    std::vector<HeuristicRecord> parse_all_records(
        const uint8_t* data, size_t size, uint16_t heap_top);
    
    /**
     * @brief Detect field types based on data patterns
     * @param data Field data to analyze
     * @return Inferred field type
     */
    static HeuristicFieldType detect_field_type(const std::vector<uint8_t>& data);
    
    /**
     * @brief Read length-prefixed value
     * @param data Buffer
     * @param size Buffer size
     * @param offset Start offset
     * @param read_size Returns bytes read
     * @return Value bytes
     */
    static std::vector<uint8_t> read_length_value(
        const uint8_t* data, size_t size, size_t offset, size_t& read_size);
    
    /**
     * @brief Check if value is NULL marker
     */
    static bool is_null_marker(uint8_t byte);
    
    /**
     * @brief Get field count heuristic (based on record pattern analysis)
     */
    static uint16_t infer_field_count(const uint8_t* data, size_t size, size_t offset);

    /**
     * @brief Parse records in custom format (for extreme corruption)
     * @details This parser looks for "user<N>@test.com" patterns in the data
     */
    static std::vector<HeuristicRecord> parse_custom_format(const uint8_t* data, size_t size);

private:
    /**
     * @brief Parse record header (5 bytes + varlen)
     */
    std::optional<HeuristicRecordHeader> parse_header(
        const uint8_t* data, size_t size, size_t offset);

    /**
     * @brief Parse record body (field values)
     */
    std::vector<HeuristicField> parse_fields(
        const uint8_t* data, size_t size, size_t offset,
        uint16_t n_fields, size_t& total_read);

    /**
     * @brief Read 1-byte or 2-byte length prefix
     */
    uint16_t read_var_len(const uint8_t* data, size_t offset,
                          size_t& read_size) const;

    /**
     * @brief Try to decode as numeric value
     */
    static std::optional<int64_t> try_parse_numeric(const std::vector<uint8_t>& data);

    /**
     * @brief Check if bytes look like ASCII text
     */
    static bool looks_like_ascii(const std::vector<uint8_t>& data);
};

// ==================== Exporter Functions ====================
/**
 * @brief Export records to CSV
 */
std::string export_records_to_csv(
    const std::vector<HeuristicRecord>& records,
    const std::string& table_name = "recovered_table");

/**
 * @brief Export records to JSON
 */
std::string export_records_to_json(
    const std::vector<HeuristicRecord>& records,
    const std::string& table_name = "recovered_table");

/**
 * @brief Export records to SQL INSERT statements
 */
std::string export_records_to_sql(
    const std::vector<HeuristicRecord>& records,
    const std::string& table_name = "recovered_table");

// ==================== Scanner ====================
/**
 * @brief HeuristicScanner: Scan entire IBD file for records
 */
class HeuristicScanner {
public:
    explicit HeuristicScanner(uint32_t page_size = 16384);
    
    /**
     * @brief Scan entire IBD file
     * @param file_path Path to .ibd file
     * @return Vector of all recovered records
     */
    std::vector<HeuristicRecord> scan_file(const std::string& file_path);
    
    /**
     * @brief Scan a specific page
     */
    std::vector<HeuristicRecord> scan_page(const uint8_t* page_data, size_t page_size, uint32_t page_no);
    
    /**
     * @brief Set page range to scan
     */
    void set_page_range(uint32_t start_page, uint32_t end_page);
    
    /**
     * @brief Get statistics
     */
    struct ScanStats {
        uint32_t pages_scanned;
        uint32_t pages_with_records;
        uint32_t total_records;
        uint32_t null_markers_found;
        uint32_t invalid_records;
    };
    
    ScanStats get_stats() const { return stats_; }
    
private:
    uint32_t page_size_;
    uint32_t start_page_;
    uint32_t end_page_;
    ScanStats stats_;
    HeuristicParser parser_;
};

// ==================== Helper Functions ====================
/**
 * @brief Detect if page type indicates valid index page
 */
inline bool is_valid_index_page(uint16_t page_type) {
    constexpr uint16_t FIL_PAGE_INDEX = dbrecovery::innodb::FIL_PAGE_INDEX;
    // Also accept 0x0000 as corrupted but potentially recoverable
    return page_type == FIL_PAGE_INDEX || page_type == 0x0000;
}

/**
 * @brief Try to detect record start after supremum marker
 */
size_t detect_record_start_after_supremum(const uint8_t* data, size_t size, size_t supremum_offset);

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_HEURISTIC_PARSER_HPP
