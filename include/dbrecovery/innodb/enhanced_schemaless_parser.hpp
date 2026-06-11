/**
 * @file enhanced_schemaless_parser.hpp
 * @brief Enhanced schemaless parser with pattern-based field detection
 */

#ifndef DBRECOVERY_INNODB_ENHANCED_SCHEMALESS_PARSER_HPP
#define DBRECOVERY_INNODB_ENHANCED_SCHEMALESS_PARSER_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <optional>

namespace dbrecovery {
namespace innodb {

/**
 * @brief Detected field type from pattern analysis
 */
enum class DetectedFieldType {
    UNKNOWN,
    STRING,
    EMAIL,
    USERNAME,
    PHONE,
    ORDER_NUMBER,
    SKU,
    DATE,
    DATETIME,
    INTEGER,
    DECIMAL,
    BOOLEAN,
    JSON
};

/**
 * @brief A detected field with type inference
 */
struct DetectedField {
    DetectedFieldType type = DetectedFieldType::UNKNOWN;
    std::vector<uint8_t> raw_bytes;
    std::string str_value;
    int64_t int_value = 0;
    double float_value = 0.0;
    bool is_null = false;
    double confidence = 0.0;

    std::string to_string() const;
};

/**
 * @brief Analyze bytes and detect the most likely field type
 */
DetectedFieldType detect_field_type(const uint8_t* data, size_t size);

/**
 * @brief Parse a detected field value from raw bytes
 */
DetectedField parse_detected_field(const uint8_t* data, size_t size);

/**
 * @brief Enhanced schemaless record parser with better field boundary detection
 */
class EnhancedSchemalessParser {
public:
    struct ParsedRecord {
        uint64_t id = 0;
        std::vector<DetectedField> fields;
        double confidence = 0.0;
        bool is_valid = false;
    };

    /**
     * @brief Parse a record from raw page data with enhanced heuristics
     *
     * @param page_data Raw page data
     * @param page_size Size of page
     * @param rec_offset Record start offset
     * @param rec_end Record end offset
     * @param is_compact True if COMPACT/DYNAMIC format
     * @param is_leaf True if leaf page
     * @return ParsedRecord with detected fields
     */
    ParsedRecord parse_record(
        const uint8_t* page_data, size_t page_size,
        size_t rec_offset, size_t rec_end,
        bool is_compact, bool is_leaf);

    /**
     * @brief Convert parsed record to CSV line
     */
    std::string to_csv_line(const ParsedRecord& record) const;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_ENHANCED_SCHEMALESS_PARSER_HPP
