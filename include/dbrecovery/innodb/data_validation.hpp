/**
 * @file data_validation.hpp
 * @brief Data validation functions for recovered field values
 *
 * Reference: undrop-for-innodb/check_data.c
 * Provides validation functions to detect corrupted or invalid field values
 */

#ifndef DBRECOVERY_INNODB_DATA_VALIDATION_HPP
#define DBRECOVERY_INNODB_DATA_VALIDATION_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <cctype>
#include <algorithm>
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {
namespace validation {

// ==================== DateTime Validation ====================
// Reference: undrop-for-innodb/check_data.c check_datetime()

/**
 * @brief Validate datetime field value (MySQL <5.6 format)
 * @param packed_value The packed datetime value (8 bytes)
 * @return true if datetime components are within valid ranges
 *
 * Valid ranges:
 *   year: 1950-2050
 *   month: 0-12
 *   day: 0-31
 *   hour: 0-23
 *   minute: 0-59
 *   second: 0-59
 */
inline bool validate_datetime_old(uint64_t packed_value) {
    // Clear high bit (sign flag)
    packed_value &= ~(1ULL << 63);

    // Zero datetime is valid (NULL)
    if (packed_value == 0) return true;

    // Extract components (stored as YYMMDDHHMMSS in BCD-like format)
    int second = packed_value % 100; packed_value /= 100;
    if (second < 0 || second > 59) return false;

    int minute = packed_value % 100; packed_value /= 100;
    if (minute < 0 || minute > 59) return false;

    int hour = packed_value % 100; packed_value /= 100;
    if (hour < 0 || hour > 23) return false;

    int day = packed_value % 100; packed_value /= 100;
    if (day < 0 || day > 31) return false;

    int month = packed_value % 100; packed_value /= 100;
    if (month < 0 || month > 12) return false;

    int year = packed_value % 10000;
    if (year < 1950 || year > 2050) return false;

    return true;
}

/**
 * @brief Validate datetime2 field value (MySQL 5.6+ format)
 * @param year Year component
 * @param month Month component
 * @param day Day component
 * @param hour Hour component
 * @param minute Minute component
 * @param second Second component
 * @return true if all components are within valid ranges
 */
inline bool validate_datetime2(int year, int month, int day,
                               int hour, int minute, int second) {
    // Year range: MySQL supports 1000-9999, but we use 1950-2050 for realistic values
    if (year < 1950 || year > 2050) return false;
    // Month 0 indicates uninitialized/zero date (valid for MySQL)
    if (month < 0 || month > 12) return false;
    // Day 0 indicates uninitialized/zero date (valid for MySQL)
    if (day < 0 || day > 31) return false;
    if (hour < 0 || hour > 23) return false;
    if (minute < 0 || minute > 59) return false;
    if (second < 0 || second > 59) return false;

    return true;
}

/**
 * @brief Validate TIME2 field value (MySQL 5.6+ format)
 * @param hour Hour component (-838 to 838 for TIME type)
 * @param minute Minute component (0-59)
 * @param second Second component (0-59)
 * @return true if all components are within valid ranges
 */
inline bool validate_time2(int hour, int minute, int second) {
    // TIME type in MySQL can be negative and range from -838:59:59 to 838:59:59
    if (hour < -838 || hour > 838) return false;
    if (minute < 0 || minute > 59) return false;
    if (second < 0 || second > 59) return false;

    return true;
}

/**
 * @brief Validate DATE field value
 * @param year Year component
 * @param month Month component
 * @param day Day component
 * @return true if date is valid
 */
inline bool validate_date(int year, int month, int day) {
    // MySQL DATE range: 1000-01-01 to 9999-12-31
    if (year < 1000 || year > 9999) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > 31) return false;

    return true;
}

// ==================== String Validation ====================
// Reference: undrop-for-innodb/check_data.c check_char_ascii()

/**
 * @brief Validate ASCII printable characters
 * @param data String data bytes
 * @return true if all bytes are printable ASCII (plus \n, \t, \r)
 */
inline bool validate_ascii_printable(const std::vector<uint8_t>& data) {
    if (data.empty()) return true;

    for (uint8_t c : data) {
        // Allow printable characters plus newline, tab, carriage return
        if (!isprint(c) && c != '\n' && c != '\t' && c != '\r') {
            return false;
        }
    }
    return true;
}

/**
 * @brief Validate digits-only string
 * @param data String data bytes
 * @return true if all bytes are ASCII digits (0-9)
 */
inline bool validate_digits_only(const std::vector<uint8_t>& data) {
    if (data.empty()) return true;

    for (uint8_t c : data) {
        if (!isdigit(c)) return false;
    }
    return true;
}

/**
 * @brief Validate UTF-8 encoded string
 * @param data String data bytes
 * @return true if data appears to be valid UTF-8
 */
inline bool validate_utf8(const std::vector<uint8_t>& data) {
    if (data.empty()) return true;

    size_t i = 0;
    while (i < data.size()) {
        uint8_t c = data[i];

        // ASCII (0x00-0x7F): single byte
        if (c <= 0x7F) {
            i++;
            continue;
        }

        // 2-byte sequence (0xC2-0xDF followed by 0x80-0xBF)
        if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= data.size()) return false;
            if (data[i + 1] < 0x80 || data[i + 1] > 0xBF) return false;
            i += 2;
            continue;
        }

        // 3-byte sequence (0xE0-0xEF followed by continuation bytes)
        if (c >= 0xE0 && c <= 0xEF) {
            if (i + 2 >= data.size()) return false;
            // First continuation byte has stricter rules for 0xE0
            if (c == 0xE0 && (data[i + 1] < 0xA0 || data[i + 1] > 0xBF)) return false;
            if (c != 0xE0 && (data[i + 1] < 0x80 || data[i + 1] > 0xBF)) return false;
            if (data[i + 2] < 0x80 || data[i + 2] > 0xBF) return false;
            i += 3;
            continue;
        }

        // 4-byte sequence (0xF0-0xF4 followed by continuation bytes)
        if (c >= 0xF0 && c <= 0xF4) {
            if (i + 3 >= data.size()) return false;
            // First continuation byte has stricter rules
            if (c == 0xF0 && (data[i + 1] < 0x90 || data[i + 1] > 0xBF)) return false;
            if (c == 0xF4 && (data[i + 1] > 0x8F)) return false;
            if (data[i + 1] < 0x80 || data[i + 1] > 0xBF) return false;
            if (data[i + 2] < 0x80 || data[i + 2] > 0xBF) return false;
            if (data[i + 3] < 0x80 || data[i + 3] > 0xBF) return false;
            i += 4;
            continue;
        }

        // Invalid UTF-8 byte
        return false;
    }
    return true;
}

// ==================== Integer Validation ====================

/**
 * @brief Validate signed integer within specified range
 * @param value Integer value
 * @param min_val Minimum allowed value
 * @param max_val Maximum allowed value
 * @return true if value is within range
 */
inline bool validate_int_range(int64_t value, int64_t min_val, int64_t max_val) {
    return value >= min_val && value <= max_val;
}

/**
 * @brief Validate unsigned integer within specified range
 * @param value Unsigned integer value
 * @param min_val Minimum allowed value
 * @param max_val Maximum allowed value
 * @return true if value is within range
 */
inline bool validate_uint_range(uint64_t value, uint64_t min_val, uint64_t max_val) {
    return value >= min_val && value <= max_val;
}

// ==================== ENUM/SET Validation ====================

/**
 * @brief Validate ENUM index value
 * @param enum_index Index value (1-based)
 * @param enum_count Number of valid enum values
 * @return true if index is valid (1 <= index <= enum_count)
 */
inline bool validate_enum_index(uint32_t enum_index, uint32_t enum_count) {
    // ENUM values are 1-based in MySQL
    if (enum_index == 0) return false;  // 0 indicates error or empty string
    return enum_index <= enum_count;
}

/**
 * @brief Validate SET bitmask value
 * @param set_bits SET bitmask value
 * @param set_count Number of valid set values
 * @return true if all set bits are within valid range
 */
inline bool validate_set_bits(uint64_t set_bits, uint32_t set_count) {
    // SET uses bitmask, each value corresponds to a bit position
    // Valid bits are 0 to (set_count - 1)
    uint64_t max_bits = (1ULL << set_count) - 1;
    return (set_bits & ~max_bits) == 0;
}

// ==================== Field Limits Structure ====================
// Reference: undrop-for-innodb/tables_dict.h field_limits_t

/**
 * @brief Field validation limits
 */
struct FieldLimits {
    bool can_be_null = true;
    int64_t int_min_val = INT64_MIN;
    int64_t int_max_val = INT64_MAX;
    uint64_t uint_min_val = 0;
    uint64_t uint_max_val = UINT64_MAX;
    int64_t char_min_len = 0;
    int64_t char_max_len = INT64_MAX;
    bool char_ascii_only = false;
    bool char_digits_only = false;
    bool validate_datetime = false;
    std::vector<std::string> enum_values;
    std::vector<std::string> set_values;

    /**
     * @brief Validate a field value against limits
     * @param data Field data bytes
     * @param is_null Whether field is NULL
     * @return true if field value is valid
     */
    bool validate(const std::vector<uint8_t>& data, bool is_null) const {
        if (is_null) {
            return can_be_null;
        }

        // Character field validation
        if (char_ascii_only && !validate_ascii_printable(data)) {
            return false;
        }

        if (char_digits_only && !validate_digits_only(data)) {
            return false;
        }

        // Length validation
        int64_t len = static_cast<int64_t>(data.size());
        if (len < char_min_len || len > char_max_len) {
            return false;
        }

        return true;
    }
};

// ==================== Page-Level Validation ====================

/**
 * @brief Validate infimum record marker
 * @param page_data Page data bytes
 * @param infimum_offset Offset of infimum record (typically 99 for COMPACT)
 * @return true if infimum marker is valid
 */
inline bool validate_infimum(const std::vector<uint8_t>& page_data, size_t infimum_offset) {
    // Infimum should start with REC_STATUS_INFIMUM (0x02) and contain "infimum"
    if (infimum_offset + 10 > page_data.size()) return false;

    // Check for "infimum" marker at offset +5 (after header)
    const char* infimum_str = "infimum";
    for (int i = 0; infimum_str[i]; i++) {
        if (page_data[infimum_offset + 5 + i] != static_cast<uint8_t>(infimum_str[i])) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Validate supremum record marker
 * @param page_data Page data bytes
 * @param supremum_offset Offset of supremum record
 * @return true if supremum marker is valid
 */
inline bool validate_supremum(const std::vector<uint8_t>& page_data, size_t supremum_offset) {
    // Supremum should start with REC_STATUS_SUPREMUM (0x03) and contain "supremum"
    if (supremum_offset + 10 > page_data.size()) return false;

    // Check for "supremum" marker at offset +5 (after header)
    const char* supremum_str = "supremum";
    for (int i = 0; supremum_str[i]; i++) {
        if (page_data[supremum_offset + 5 + i] != static_cast<uint8_t>(supremum_str[i])) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Validate page has valid infimum/supremum markers
 * @param page_data Page data bytes
 * @param page_size Page size (16KB, 8KB, etc.)
 * @return true if page has valid system records
 */
inline bool validate_page_system_records(const std::vector<uint8_t>& page_data, uint32_t page_size) {
    // COMPACT format: infimum at offset 99, supremum at 112 (for 16KB pages)
    // These positions are fixed relative to the page start

    // Get page header to determine actual positions
    if (page_data.size() < FIL_HEADER_SIZE + 20) return false;

    // Check n_heap format bit (COMPACT vs REDUNDANT)
    uint16_t n_heap = (static_cast<uint16_t>(page_data[42]) << 8) | page_data[43];
    bool is_compact = (n_heap & 0x8000) != 0;

    if (is_compact) {
        // COMPACT format: infimum at PAGE_NEW_INFIMUM (99)
        // Record header at offset 99, "infimum" string starts at offset 99+5 = 104
        // Note: The actual infimum marker position depends on header parsing
        // In COMPACT, the infimum record starts at offset 99 with:
        // - info_bits (1 byte): 0x02 for INFIMUM
        // - next_offset (2 bytes)
        // - heap_no bytes (2 bytes)
        // Then "infimum\0" string

        size_t infimum_offset = 99;
        // Check for REC_STATUS_INFIMUM in info_bits position
        // But actually, the "infimum" string is at different offset based on record format
        // Let's check the string position directly
        size_t string_offset = infimum_offset + 5;
        if (string_offset + 7 > page_data.size()) return false;

        const char* infimum_str = "infimum";
        bool has_infimum = true;
        for (int i = 0; infimum_str[i]; i++) {
            if (page_data[string_offset + i] != static_cast<uint8_t>(infimum_str[i])) {
                has_infimum = false;
                break;
            }
        }
        if (has_infimum) return true;

        // Supremum is typically at infimum + 13
        size_t supremum_offset = infimum_offset + 13;
        string_offset = supremum_offset + 5;
        if (string_offset + 8 > page_data.size()) return false;

        const char* supremum_str = "supremum";
        bool has_supremum = true;
        for (int i = 0; supremum_str[i]; i++) {
            if (page_data[string_offset + i] != static_cast<uint8_t>(supremum_str[i])) {
                has_supremum = false;
                break;
            }
        }
        return has_supremum;
    } else {
        // REDUNDANT format: infimum at different offset
        // For REDUNDANT, infimum is at PAGE_OLD_INFIMUM
        size_t infimum_offset = 100;
        if (!validate_infimum(page_data, infimum_offset)) {
            infimum_offset = 50;
            if (!validate_infimum(page_data, infimum_offset)) return false;
        }
    }

    return true;
}

} // namespace validation
} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_DATA_VALIDATION_HPP