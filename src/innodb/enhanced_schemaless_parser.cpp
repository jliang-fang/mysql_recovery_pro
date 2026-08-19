/**
 * @file enhanced_schemaless_parser.cpp
 * @brief Enhanced schemaless parser with pattern-based field detection
 *
 * This module provides improved field detection for schemaless recovery:
 * 1. Pattern-based string detection (email, phone, username patterns)
 * 2. Type inference from data patterns
 * 3. Better handling of small files with fewer records
 */

#include "dbrecovery/innodb/enhanced_schemaless_parser.hpp"
#include <regex>
#include <algorithm>
#include <cstring>
#include <ctime>

namespace dbrecovery {
namespace innodb {

// Common patterns for field type detection
static const std::regex EMAIL_PATTERN(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})");
static const std::regex PHONE_PATTERN(R"(\+?[0-9]{10,15})");
static const std::regex USERNAME_PATTERN(R"(user_[0-9]+_[a-zA-Z0-9]+)");
static const std::regex DATE_PATTERN(R"([0-9]{4}-[0-9]{2}-[0-9]{2})");
static const std::regex DATETIME_PATTERN(R"([0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2})");
static const std::regex ORDER_NO_PATTERN(R"(ORD[0-9]{9})");
static const std::regex SKU_PATTERN(R"(SKU[0-9]{8})");

std::string DetectedField::to_string() const {
    if (is_null) return "NULL";
    switch (type) {
        case DetectedFieldType::INTEGER:
            return std::to_string(int_value);
        case DetectedFieldType::DECIMAL:
            return std::to_string(float_value);
        case DetectedFieldType::BOOLEAN:
            return int_value ? "true" : "false";
        case DetectedFieldType::STRING:
        case DetectedFieldType::EMAIL:
        case DetectedFieldType::USERNAME:
        case DetectedFieldType::PHONE:
        case DetectedFieldType::ORDER_NUMBER:
        case DetectedFieldType::SKU:
        case DetectedFieldType::DATE:
        case DetectedFieldType::DATETIME:
        case DetectedFieldType::JSON:
            return str_value;
        default:
            // Return hex for unknown binary
            if (!raw_bytes.empty()) {
                std::string hex = "0x";
                for (uint8_t b : raw_bytes) {
                    char buf[3];
                    snprintf(buf, sizeof(buf), "%02x", b);
                    hex += buf;
                }
                return hex;
            }
            return "<UNKNOWN>";
    }
}

DetectedFieldType detect_field_type(const uint8_t* data, size_t size) {
    if (!data || size == 0) return DetectedFieldType::UNKNOWN;

    // Check for null
    bool all_zero = true;
    for (size_t i = 0; i < size; i++) {
        if (data[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) return DetectedFieldType::UNKNOWN;

    // Check for DECIMAL binary format (4-16 bytes, with sign bit in first byte)
    // DECIMAL: first byte has sign (0x80=positive, 0x00-0x7F=negative)
    // Common sizes: 4, 8, 12, 16 bytes
    if (size >= 4 && size <= 16 && (size % 4 == 0)) {
        uint8_t first_byte = data[0];
        // InnoDB DECIMAL: first byte 0x80-0xFF means positive, 0x00-0x7F means negative
        // Also check if remaining bytes look like valid decimal data
        bool likely_decimal = (first_byte & 0x80) != 0;
        // Additional validation: values should not be all 0xFF (invalid)
        bool valid_decimal_data = true;
        for (size_t i = 1; i < size; i++) {
            if (data[i] == 0xFF && data[i-1] == 0xFF) {
                valid_decimal_data = false;
                break;
            }
        }
        if (likely_decimal && valid_decimal_data) {
            return DetectedFieldType::DECIMAL;
        }
    }

    // Check for 5-byte DATETIME (MySQL 5.6+ format)
    if (size == 5) {
        // MySQL DATETIME stored as packed integer YYYYMMDDHHMMSS in 5 bytes
        // First byte usually 0x99 (year 2020-ish) or similar
        uint64_t packed = 0;
        for (size_t i = 0; i < 5; i++) {
            packed = (packed << 8) | data[i];
        }
        // Valid range: 1000-9999 year, reasonable month/day/hour/minute/second
        // Packed format: roughly 14 bits for year, then 4+5+5+6+6 bits
        uint64_t year_part = packed >> 22;
        if (year_part >= 1000 && year_part <= 9999) {
            // Likely a valid DATETIME
            return DetectedFieldType::DATETIME;
        }
    }

    // Check if it's a printable string
    size_t printable = 0;
    for (size_t i = 0; i < size; i++) {
        uint8_t c = data[i];
        if ((c >= 32 && c < 127) || c == '\n' || c == '\r' || c == '\t') {
            printable++;
        }
    }

    double printable_ratio = static_cast<double>(printable) / size;
    std::string str(reinterpret_cast<const char*>(data), size);

    // Pattern-based detection
    if (std::regex_search(str, EMAIL_PATTERN)) {
        return DetectedFieldType::EMAIL;
    }
    if (std::regex_search(str, USERNAME_PATTERN)) {
        return DetectedFieldType::USERNAME;
    }
    if (std::regex_search(str, ORDER_NO_PATTERN)) {
        return DetectedFieldType::ORDER_NUMBER;
    }
    if (std::regex_search(str, SKU_PATTERN)) {
        return DetectedFieldType::SKU;
    }
    if (std::regex_search(str, DATETIME_PATTERN)) {
        return DetectedFieldType::DATETIME;
    }
    if (std::regex_search(str, DATE_PATTERN)) {
        return DetectedFieldType::DATE;
    }

    // JSON detection
    if (size >= 2 && (str.front() == '{' || str.front() == '[')) {
        if (str.back() == '}' || str.back() == ']') {
            return DetectedFieldType::JSON;
        }
    }

    // Mostly printable = string
    if (printable_ratio > 0.7) {
        return DetectedFieldType::STRING;
    }

    // Check for integer patterns
    if (size == 1) {
        return DetectedFieldType::BOOLEAN;
    }
    if (size == 4 || size == 8) {
        // Could be integer or float
        return DetectedFieldType::INTEGER;
    }

    return DetectedFieldType::UNKNOWN;
}

DetectedField parse_detected_field(const uint8_t* data, size_t size) {
    DetectedField field;
    field.raw_bytes.assign(data, data + size);

    if (!data || size == 0) {
        field.is_null = true;
        return field;
    }

    field.type = detect_field_type(data, size);
    field.str_value = std::string(reinterpret_cast<const char*>(data), size);

    // Try to extract numeric value
    if (size == 1) {
        field.int_value = data[0];
        field.confidence = 0.8;
    } else if (size == 2) {
        // Big-endian 16-bit
        field.int_value = (static_cast<int64_t>(data[0]) << 8) | data[1];
        field.confidence = 0.7;
    } else if (size == 4) {
        // Big-endian 32-bit
        uint32_t v = (static_cast<uint32_t>(data[0]) << 24) |
                     (static_cast<uint32_t>(data[1]) << 16) |
                     (static_cast<uint32_t>(data[2]) << 8) |
                     data[3];
        field.int_value = static_cast<int64_t>(v);

        // Check if it looks like a timestamp
        if (v >= 946684800 && v <= 4102444800) {
            field.type = DetectedFieldType::DATE;
            time_t t = static_cast<time_t>(v);
            char buf[32];
            struct tm tm_buf;
            gmtime_r(&t, &tm_buf);
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
            field.str_value = buf;
            field.confidence = 0.9;
        } else if (field.type == DetectedFieldType::DECIMAL) {
            // Parse DECIMAL stored in 4 bytes
            bool is_negative = (data[0] & 0x80) == 0;
            uint32_t group_val = ((static_cast<uint32_t>(data[0]) & 0x7F) << 24) |
                                 (static_cast<uint32_t>(data[1]) << 16) |
                                 (static_cast<uint32_t>(data[2]) << 8) |
                                 static_cast<uint32_t>(data[3]);
            field.float_value = static_cast<double>(group_val) / 100.0;  // Assume scale=2
            if (is_negative) field.float_value = -field.float_value;
            field.str_value = std::to_string(field.float_value);
            field.confidence = 0.8;
        } else {
            field.confidence = 0.6;
        }
    } else if (size == 8) {
        // Big-endian 64-bit with signed conversion
        uint64_t v = (static_cast<uint64_t>(data[0]) << 56) |
                     (static_cast<uint64_t>(data[1]) << 48) |
                     (static_cast<uint64_t>(data[2]) << 40) |
                     (static_cast<uint64_t>(data[3]) << 32) |
                     (static_cast<uint64_t>(data[4]) << 24) |
                     (static_cast<uint64_t>(data[5]) << 16) |
                     (static_cast<uint64_t>(data[6]) << 8) |
                     data[7];
        v ^= 0x8000000000000000ULL;  // Convert from InnoDB signed format
        field.int_value = static_cast<int64_t>(v);
        field.confidence = 0.7;
    } else if (field.type == DetectedFieldType::DECIMAL && size >= 4 && size <= 16) {
        // Parse multi-byte DECIMAL
        bool is_negative = (data[0] & 0x80) == 0;
        std::string result;

        size_t pos = 0;
        // Integer part (first group with sign masked)
        if (pos + 4 <= size) {
            uint32_t group_val = ((static_cast<uint32_t>(data[pos]) & 0x7F) << 24) |
                                 (static_cast<uint32_t>(data[pos + 1]) << 16) |
                                 (static_cast<uint32_t>(data[pos + 2]) << 8) |
                                 static_cast<uint32_t>(data[pos + 3]);
            result = std::to_string(group_val);
            pos += 4;
        }

        // Additional integer groups (if any)
        while (pos + 4 <= size) {
            uint32_t group_val = (static_cast<uint32_t>(data[pos]) << 24) |
                                 (static_cast<uint32_t>(data[pos + 1]) << 16) |
                                 (static_cast<uint32_t>(data[pos + 2]) << 8) |
                                 static_cast<uint32_t>(data[pos + 3]);
            if (is_negative) {
                group_val = 1000000000 - group_val;
            }
            result += std::to_string(group_val);
            pos += 4;
        }

        // Format as decimal with assumed scale=2
        if (result.length() > 2) {
            std::string int_part = result.substr(0, result.length() - 2);
            std::string frac_part = result.substr(result.length() - 2);
            result = int_part + "." + frac_part;
        }

        // Remove leading zeros
        while (result.length() > 1 && result[0] == '0' && result[1] != '.') {
            result = result.substr(1);
        }

        if (is_negative && result != "0") {
            result = "-" + result;
        }

        field.str_value = result;
        field.float_value = std::stod(result);
        field.confidence = 0.85;
    } else if (field.type == DetectedFieldType::DATETIME && size == 5) {
        // Parse 5-byte DATETIME
        uint64_t packed = 0;
        for (size_t i = 0; i < 5; i++) {
            packed = (packed << 8) | data[i];
        }
        // Extract components (approximate - actual encoding varies)
        uint16_t year = static_cast<uint16_t>(packed >> 22);
        uint8_t month = static_cast<uint8_t>((packed >> 18) & 0x0F);
        uint8_t day = static_cast<uint8_t>((packed >> 13) & 0x1F);
        uint8_t hour = static_cast<uint8_t>((packed >> 8) & 0x1F);
        uint8_t minute = static_cast<uint8_t>((packed >> 2) & 0x3F);
        uint8_t second = static_cast<uint8_t>(packed & 0x3F);

        char buf[32];
        snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", year, month, day, hour, minute, second);
        field.str_value = buf;
        field.confidence = 0.9;
    }

    return field;
}

EnhancedSchemalessParser::ParsedRecord EnhancedSchemalessParser::parse_record(
    const uint8_t* page_data, size_t page_size,
    size_t rec_offset, size_t rec_end,
    bool is_compact, bool is_leaf) {

    ParsedRecord result;

    size_t header_size = is_compact ? 5 : 6;
    size_t data_start = rec_offset + header_size;

    if (data_start >= rec_end || rec_end > page_size) {
        return result;
    }

    size_t pos = data_start;
    size_t data_len = rec_end - data_start;

    if (data_len < 8) {
        return result;
    }

    // Parse primary key (first 8 bytes)
    uint64_t raw_id = 0;
    for (int i = 0; i < 8 && pos + i < rec_end; i++) {
        raw_id = (raw_id << 8) | page_data[pos + i];
    }
    raw_id ^= 0x8000000000000000ULL;
    result.id = raw_id;
    pos += 8;

    // For leaf pages, skip TRX_ID (6 bytes) + ROLL_PTR (roll_ptr width) = system_cols_bytes()
    if (is_leaf && pos + system_cols_bytes() <= rec_end) {
        pos += system_cols_bytes();
    }

    size_t remaining = rec_end - pos;
    if (remaining == 0) {
        result.is_valid = true;
        return result;
    }

    // Heuristic field boundary detection using pattern analysis
    while (pos < rec_end) {
        remaining = rec_end - pos;

        // Skip null bytes
        if (page_data[pos] == 0 && remaining > 0) {
            DetectedField null_field;
            null_field.is_null = true;
            null_field.type = DetectedFieldType::UNKNOWN;
            result.fields.push_back(null_field);
            pos += 1;
            continue;
        }

        // Try to detect string fields
        size_t str_len = 0;
        size_t printable_start = 0;

        // Find printable sequence
        for (size_t i = 0; i < remaining && i < 256; i++) {
            uint8_t c = page_data[pos + i];
            if (c >= 32 && c < 127) {
                if (str_len == 0) printable_start = i;
                str_len++;
            } else if (str_len > 3) {
                // End of printable sequence
                break;
            } else {
                str_len = 0;
            }
        }

        if (str_len >= 4) {
            // Found a string field
            DetectedField str_field = parse_detected_field(
                page_data + pos + printable_start, str_len);
            str_field.confidence = 0.8;
            result.fields.push_back(str_field);
            pos += printable_start + str_len;
            continue;
        }

        // Try fixed-size numeric fields
        if (remaining >= 8) {
            // Check for timestamp pattern
            uint32_t v4 = (static_cast<uint32_t>(page_data[pos]) << 24) |
                          (static_cast<uint32_t>(page_data[pos+1]) << 16) |
                          (static_cast<uint32_t>(page_data[pos+2]) << 8) |
                          page_data[pos+3];

            if (v4 >= 946684800 && v4 <= 4102444800) {
                // Looks like Unix timestamp
                DetectedField ts_field = parse_detected_field(page_data + pos, 4);
                ts_field.confidence = 0.9;
                result.fields.push_back(ts_field);
                pos += 4;
                continue;
            }

            // Try as 64-bit integer
            DetectedField int_field = parse_detected_field(page_data + pos, 8);
            int_field.confidence = 0.6;
            result.fields.push_back(int_field);
            pos += 8;
            continue;
        }

        if (remaining >= 4) {
            DetectedField field = parse_detected_field(page_data + pos, 4);
            field.confidence = 0.5;
            result.fields.push_back(field);
            pos += 4;
            continue;
        }

        if (remaining >= 1) {
            DetectedField field = parse_detected_field(page_data + pos, 1);
            field.confidence = 0.4;
            result.fields.push_back(field);
            pos += 1;
        }
    }

    result.is_valid = true;
    result.confidence = result.fields.empty() ? 0.0 : 0.7;

    return result;
}

std::string EnhancedSchemalessParser::to_csv_line(const ParsedRecord& record) const {
    std::string result = std::to_string(record.id);
    for (const auto& field : record.fields) {
        result += ",";
        result += field.to_string();
    }
    return result;
}

} // namespace innodb
} // namespace dbrecovery