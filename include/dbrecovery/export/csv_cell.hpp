#pragma once

#include <iomanip>
#include <limits>
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

// Emit one control byte as a backslash escape so the rendered text stays on a
// single physical line AND is reversible. \t \n \r \\ keep their conventional
// two-letter forms; every other byte < 0x20 becomes \u00XX.
inline void escape_control_byte(std::string& out, unsigned char character) {
    static constexpr char hex_digits[] = "0123456789abcdef";
    switch (character) {
        case '\t': out += "\\t"; return;
        case '\n': out += "\\n"; return;
        case '\r': out += "\\r"; return;
        case '\\': out += "\\\\"; return;
        default:
            out += "\\u00";
            out.push_back(hex_digits[character >> 4]);
            out.push_back(hex_digits[character & 0x0f]);
            return;
    }
}

// Render a recovered text cell without losing bytes: control characters become
// backslash escapes (parsed back by _parse_csv_cell), then normal CSV quoting
// rules apply for comma/quote/newline-containing content.
inline std::string csv_escape_text_preserving_controls(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char character : value) {
        if (character < 0x20 || character == '\\') {
            escape_control_byte(escaped, character);
        } else {
            escaped.push_back(static_cast<char>(character));
        }
    }
    return csv_escape_text(escaped);
}

// Render one decoded field. Control bytes are escaped (not blanked) so the
// exported text round-trips exactly; binary columns become 0x<hex> which
// csv_to_canonical.py and UNHEX() both understand.
inline std::string csv_column_value(const innodb::IndexFieldValue& field,
                                    bool is_binary_column) {
    using FieldType = innodb::IndexFieldValue::Type;
    if (field.type == FieldType::NULL_VAL) {
        // Row writers emit the literal NULL themselves; this path only ever
        // renders values.
        return std::string{};
    }
    if (is_binary_column || field.type == FieldType::BINARY_VAL) {
        // Always 0x<hex>, including the empty value: an empty binary cell has
        // to stay distinguishable from NULL, and 0x is what UNHEX()/the CSV
        // adapter already decode back to zero bytes.
        return hex_encode_bytes(field.raw_bytes);
    }
    switch (field.type) {
        case FieldType::INT_VAL:
            return std::to_string(field.int_value);
        case FieldType::UINT_VAL:
            return std::to_string(field.uint_value);
        case FieldType::FLOAT_VAL: {
            std::ostringstream out;
            out << std::setprecision(7) << static_cast<float>(field.float_value);
            return out.str();
        }
        case FieldType::DOUBLE_VAL: {
            std::ostringstream out;
            out << std::setprecision(std::numeric_limits<double>::max_digits10) << field.float_value;
            return out.str();
        }
        case FieldType::NULL_VAL:
            return std::string{};
        default:
            break;
    }
    if (!field.str_value.empty()) {
        return csv_escape_text_preserving_controls(field.str_value);
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
    return csv_escape_text_preserving_controls(value);
}

} // namespace exporter
} // namespace dbrecovery
