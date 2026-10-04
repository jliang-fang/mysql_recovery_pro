#pragma once

#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "dbrecovery/innodb/index_page_parser.hpp"

namespace dbrecovery {
namespace exporter {

// Shared rendering rules for one recovered field in a text artifact.
//
// Binary columns are always hex-encoded: a BLOB/BINARY value can contain NUL
// bytes and commas, which would otherwise corrupt the CSV row layout and make
// the artifact impossible to re-import.

inline std::string hex_encode_bytes(const std::vector<uint8_t>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2 + 2);
    encoded += "0x";
    for (uint8_t byte : bytes) {
        encoded.push_back(digits[byte >> 4]);
        encoded.push_back(digits[byte & 0x0f]);
    }
    return encoded;
}

inline std::string csv_escape_text(const std::string& value) {
    const bool needs_quotes = value.find_first_of(",\"\r\n") != std::string::npos;
    if (!needs_quotes) return value;
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char character : value) {
        if (character == '"') escaped.push_back('"');
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

// Render one decoded field. Control bytes are blanked so a single row can never
// span lines, and binary columns become 0x<hex> which csv_to_canonical.py and
// UNHEX() both understand.
inline std::string csv_column_value(const innodb::IndexFieldValue& field,
                                    bool is_binary_column) {
    using FieldType = innodb::IndexFieldValue::Type;
    if (!field.raw_bytes.empty() &&
        (is_binary_column || field.type == FieldType::BINARY_VAL)) {
        return hex_encode_bytes(field.raw_bytes);
    }
    switch (field.type) {
        case FieldType::INT_VAL:
            return std::to_string(field.int_value);
        case FieldType::UINT_VAL:
            return std::to_string(field.uint_value);
        case FieldType::FLOAT_VAL:
        case FieldType::DOUBLE_VAL: {
            std::ostringstream out;
            out << field.float_value;
            return out.str();
        }
        case FieldType::NULL_VAL:
            return std::string{};
        default:
            break;
    }
    if (!field.str_value.empty()) {
        std::string sanitized;
        sanitized.reserve(field.str_value.size());
        for (unsigned char character : field.str_value) {
            if (character < 0x20 && character != '\t') sanitized.push_back(' ');
            else sanitized.push_back(static_cast<char>(character));
        }
        return csv_escape_text(sanitized);
    }
    if (!field.raw_bytes.empty()) return hex_encode_bytes(field.raw_bytes);
    return std::string{};
}

// Convert a stored record value (already a string) into a safe CSV cell.
inline std::string csv_cell_from_string(const std::string& value, bool is_binary) {
    if (value.empty()) return std::string{};
    if (is_binary && (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)) {
        return value;
    }
    std::string sanitized;
    sanitized.reserve(value.size());
    for (unsigned char character : value) {
        if (character < 0x20 && character != '\t') sanitized.push_back(' ');
        else sanitized.push_back(static_cast<char>(character));
    }
    return csv_escape_text(sanitized);
}

} // namespace exporter
} // namespace dbrecovery
