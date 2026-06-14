#ifndef DBRECOVERY_INNODB_CHARSET_MAPPING_HPP
#define DBRECOVERY_INNODB_CHARSET_MAPPING_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dbrecovery {
namespace innodb {

/**
 * @brief MySQL character set and collation information
 *
 * Reference: ibd2sql/utils/collations.py
 * Complete mapping of MySQL character set IDs to names
 */

struct CharsetInfo {
    uint8_t id;                 // Character set ID
    std::string name;           // Character set name (e.g., "utf8mb4")
    std::string collation;      // Default collation (e.g., "utf8mb4_general_ci")
    uint8_t max_len;            // Maximum bytes per character
    std::string comment;        // Description
};

struct CollationInfo {
    uint16_t id;                // Collation ID
    std::string name;           // Collation name (e.g., "utf8mb4_0900_ai_ci")
    std::string charset;        // Character set name
    uint8_t max_len;            // Maximum bytes per character
    bool is_default;            // Is default collation for charset
};

/**
 * @brief Complete MySQL character set mapping
 *
 * Data from MySQL 8.0 information_schema.CHARACTER_SETS
 */
inline const std::unordered_map<uint8_t, CharsetInfo>& get_charset_map() {
    static const std::unordered_map<uint8_t, CharsetInfo> charsets = {
        // Most common character sets
        {1,   {1,   "big5",      "big5_chinese_ci",       2, "Big5 Traditional Chinese"}},
        {8,   {8,   "latin1",    "latin1_swedish_ci",     1, "cp1252 West European"}},
        {11,  {11,  "ascii",     "ascii_general_ci",      1, "US ASCII"}},
        {12,  {12,  "cp1251",    "cp1251_general_ci",     1, "Windows Cyrillic"}},
        {14,  {14,  "cp1256",    "cp1256_general_ci",     1, "Windows Arabic"}},
        {16,  {16,  "binary",    "binary",                1, "Binary pseudo charset"}},
        {18,  {18,  "tis620",    "tis620_thai_ci",        1, "TIS620 Thai"}},
        {19,  {19,  "euckr",     "euckr_korean_ci",       2, "EUC-KR Korean"}},
        {24,  {24,  "gb2312",    "gb2312_chinese_ci",     2, "GB2312 Simplified Chinese"}},
        {28,  {28,  "gbk",       "gbk_chinese_ci",        2, "GBK Simplified Chinese"}},
        {30,  {30,  "utf8",      "utf8_general_ci",       3, "UTF-8 Unicode (3-byte)"}},  // MySQL's utf8
        {33,  {33,  "utf8mb4",   "utf8mb4_general_ci",    4, "UTF-8 Unicode (4-byte)"}},  // Full UTF-8
        {35,  {35,  "utf16",     "utf16_general_ci",      4, "UTF-16 Unicode"}},
        {36,  {36,  "utf16le",   "utf16le_general_ci",    4, "UTF-16LE Unicode"}},
        {39,  {39,  "utf32",     "utf32_general_ci",      4, "UTF-32 Unicode"}},
        {45,  {45,  "utf8mb3",   "utf8mb3_general_ci",    3, "UTF-8 Unicode (3-byte)"}},  // Renamed from utf8

        // European character sets
        {2,   {2,   "latin2",    "latin2_general_ci",     1, "ISO 8859-2 Central European"}},
        {3,   {3,   "dec8",      "dec8_swedish_ci",       1, "DEC West European"}},
        {4,   {4,   "cp850",     "cp850_general_ci",      1, "DOS West European"}},
        {5,   {5,   "latin1",    "latin1_german1_ci",     1, "cp1252 West European"}},
        {6,   {6,   "hp8",       "hp8_english_ci",        1, "HP West European"}},
        {7,   {7,   "koi8r",     "koi8r_general_ci",      1, "KOI8-R Relcom Russian"}},
        {9,   {9,   "latin2",    "latin2_hungarian_ci",   1, "ISO 8859-2 Central European"}},
        {10,  {10,  "latin1",    "latin1_danish_ci",      1, "cp1252 West European"}},

        // Asian character sets
        {13,  {13,  "hebrew",    "hebrew_general_ci",     1, "ISO 8859-8 Hebrew"}},
        {15,  {15,  "greek",     "greek_general_ci",      1, "ISO 8859-7 Greek"}},
        {17,  {17,  "latin7",    "latin7_general_ci",     1, "ISO 8859-13 Baltic"}},
        {20,  {20,  "koi8u",     "koi8u_general_ci",      1, "KOI8-U Ukrainian"}},
        {21,  {21,  "gb18030",   "gb18030_chinese_ci",    4, "GB18030 Chinese"}},
        {22,  {22,  "armscii8",  "armscii8_general_ci",   1, "ARMSCII-8 Armenian"}},
        {23,  {23,  "geostd8",   "geostd8_general_ci",    1, "GEOSTD8 Georgian"}},
        {25,  {25,  "cp1250",    "cp1250_general_ci",     1, "Windows Central European"}},
        {26,  {26,  "greek",     "greek_bin",             1, "ISO 8859-7 Greek"}},
        {27,  {27,  "latin7",    "latin7_estonian_cs",    1, "ISO 8859-13 Baltic"}},
        {29,  {29,  "cp1257",    "cp1257_lithuanian_ci",  1, "Windows Baltic"}},

        // UTF variants
        {31,  {31,  "ucs2",      "ucs2_general_ci",       2, "UCS-2 Unicode"}},
        {32,  {32,  "cp866",     "cp866_general_ci",      1, "DOS Russian"}},
        {34,  {34,  "cp1250",    "cp1250_croatian_ci",    1, "Windows Central European"}},
        {37,  {37,  "macroman",  "macroman_general_ci",   1, "Mac West European"}},
        {38,  {38,  "macce",     "macce_general_ci",      1, "Mac Central European"}},
        {40,  {40,  "armscii8",  "armscii8_bin",          1, "ARMSCII-8 Armenian"}},
        {41,  {41,  "ascii",     "ascii_bin",             1, "US ASCII"}},
        {42,  {42,  "cp1250",    "cp1250_bin",            1, "Windows Central European"}},
        {43,  {43,  "cp1256",    "cp1256_bin",            1, "Windows Arabic"}},
        {44,  {44,  "cp866",     "cp866_bin",             1, "DOS Russian"}},
        {46,  {46,  "utf8mb3",   "utf8mb3_bin",           3, "UTF-8 Unicode (3-byte)"}},
        {47,  {47,  "utf8mb4",   "utf8mb4_bin",           4, "UTF-8 Unicode (4-byte)"}},
        {48,  {48,  "utf8mb4",   "utf8mb4_unicode_ci",    4, "UTF-8 Unicode (4-byte)"}},
        {49,  {49,  "utf8mb4",   "utf8mb4_unicode_520_ci", 4, "UTF-8 Unicode (4-byte)"}},
        {255, {255, "utf8mb4",   "utf8mb4_0900_ai_ci",    4, "UTF-8 Unicode (4-byte)"}},
    };
    return charsets;
}

/**
 * @brief Complete MySQL collation mapping
 *
 * Data from MySQL 8.0 information_schema.COLLATIONS
 */
inline const std::unordered_map<uint16_t, CollationInfo>& get_collation_map() {
    static const std::unordered_map<uint16_t, CollationInfo> collations = {
        // UTF-8 collations (most commonly used)
        {11,   {11,   "ascii_general_ci",     "ascii",    1, true}},
        {14,   {14,   "cp1256_general_ci",    "cp1256",   1, true}},
        {15,   {15,   "binary",               "binary",   1, true}},
        {33,   {33,   "utf8_general_ci",      "utf8",     3, true}},
        {34,   {34,   "utf8_bin",             "utf8",     3, false}},
        {35,   {35,   "utf8mb4_general_ci",   "utf8mb4",  4, true}},
        {44,   {44,   "utf8mb4_bin",          "utf8mb4",  4, false}},
        {45,   {45,   "utf8mb4_unicode_ci",   "utf8mb4",  4, false}},
        {46,   {46,   "utf8mb4_unicode_520_ci", "utf8mb4", 4, false}},
        {57,   {57,   "utf8mb3_general_ci",   "utf8mb3",  3, true}},
        {83,   {83,   "utf8mb3_bin",          "utf8mb3",  3, false}},
        {255,  {255,  "utf8mb4_0900_ai_ci",   "utf8mb4",  4, true}},  // MySQL 8.0 default

        // Latin1 collations
        {1,    {1,    "big5_chinese_ci",      "big5",     2, true}},
        {8,    {8,    "latin1_swedish_ci",    "latin1",   1, true}},
        {9,    {9,    "latin1_german1_ci",    "latin1",   1, false}},
        {10,   {10,   "latin1_danish_ci",     "latin1",   1, false}},
        {47,   {47,   "latin1_bin",           "latin1",   1, false}},
        {48,   {48,   "latin1_general_ci",    "latin1",   1, false}},
        {49,   {49,   "latin1_general_cs",    "latin1",   1, false}},

        // Chinese character sets
        {24,   {24,   "gb2312_chinese_ci",    "gb2312",   2, true}},
        {28,   {28,   "gbk_chinese_ci",       "gbk",      2, true}},
        {63,   {63,   "gbk_bin",              "gbk",      2, false}},
        {213,  {213,  "gb18030_chinese_ci",   "gb18030",  4, true}},
        {214,  {214,  "gb18030_bin",          "gb18030",  4, false}},
    };
    return collations;
}

/**
 * @brief Get charset name by ID
 */
inline std::string get_charset_name(uint8_t charset_id) {
    const auto& charsets = get_charset_map();
    auto it = charsets.find(charset_id);
    if (it != charsets.end()) {
        return it->second.name;
    }
    return "unknown";
}

/**
 * @brief Get charset max length by ID
 */
inline uint8_t get_charset_max_len(uint8_t charset_id) {
    const auto& charsets = get_charset_map();
    auto it = charsets.find(charset_id);
    if (it != charsets.end()) {
        return it->second.max_len;
    }
    return 1;  // Default to 1 byte per character
}

/**
 * @brief Check if charset is multi-byte
 */
inline bool is_multibyte_charset(uint8_t charset_id) {
    return get_charset_max_len(charset_id) > 1;
}

/**
 * @brief Get collation name by ID
 */
inline std::string get_collation_name(uint16_t collation_id) {
    const auto& collations = get_collation_map();
    auto it = collations.find(collation_id);
    if (it != collations.end()) {
        return it->second.name;
    }
    return "unknown";
}

/**
 * @brief Get charset from collation ID
 */
inline std::string get_charset_from_collation(uint16_t collation_id) {
    const auto& collations = get_collation_map();
    auto it = collations.find(collation_id);
    if (it != collations.end()) {
        return it->second.charset;
    }
    return "unknown";
}

/**
 * @brief MySQL column type definitions
 *
 * Reference: ibd2sql/innodb_page/column_type.py
 * MySQL 9.0 adds VECTOR type (type code varies by version)
 */
enum class MySQLColumnType : uint8_t {
    DECIMAL = 0,
    TINY = 1,
    SHORT = 2,
    LONG = 3,
    FLOAT = 4,
    DOUBLE = 5,
    NULL_TYPE = 6,
    TIMESTAMP = 7,
    LONGLONG = 8,
    INT24 = 9,
    DATE = 10,
    TIME = 11,
    DATETIME = 12,
    YEAR = 13,
    NEWDATE = 14,
    VARCHAR = 15,
    BIT = 16,
    // MySQL 5.7+
    JSON = 245,
    NEWDECIMAL = 246,
    ENUM = 247,
    SET = 248,
    TINY_BLOB = 249,
    MEDIUM_BLOB = 250,
    LONG_BLOB = 251,
    BLOB = 252,
    VAR_STRING = 253,
    STRING = 254,
    GEOMETRY = 255,
    // MySQL 9.0+
    VECTOR = 241,  // MySQL 9.0 VECTOR type for AI/ML embeddings
};

/**
 * @brief Get column type name
 */
inline std::string get_column_type_name(MySQLColumnType type) {
    switch (type) {
        case MySQLColumnType::DECIMAL:     return "DECIMAL";
        case MySQLColumnType::TINY:        return "TINYINT";
        case MySQLColumnType::SHORT:       return "SMALLINT";
        case MySQLColumnType::LONG:        return "INT";
        case MySQLColumnType::FLOAT:       return "FLOAT";
        case MySQLColumnType::DOUBLE:      return "DOUBLE";
        case MySQLColumnType::NULL_TYPE:   return "NULL";
        case MySQLColumnType::TIMESTAMP:   return "TIMESTAMP";
        case MySQLColumnType::LONGLONG:    return "BIGINT";
        case MySQLColumnType::INT24:       return "MEDIUMINT";
        case MySQLColumnType::DATE:        return "DATE";
        case MySQLColumnType::TIME:        return "TIME";
        case MySQLColumnType::DATETIME:    return "DATETIME";
        case MySQLColumnType::YEAR:        return "YEAR";
        case MySQLColumnType::VARCHAR:     return "VARCHAR";
        case MySQLColumnType::BIT:         return "BIT";
        case MySQLColumnType::JSON:        return "JSON";
        case MySQLColumnType::NEWDECIMAL:  return "DECIMAL";
        case MySQLColumnType::ENUM:        return "ENUM";
        case MySQLColumnType::SET:         return "SET";
        case MySQLColumnType::TINY_BLOB:   return "TINYBLOB";
        case MySQLColumnType::MEDIUM_BLOB: return "MEDIUMBLOB";
        case MySQLColumnType::LONG_BLOB:   return "LONGBLOB";
        case MySQLColumnType::BLOB:        return "BLOB";
        case MySQLColumnType::VAR_STRING:  return "VARCHAR";
        case MySQLColumnType::STRING:      return "CHAR";
        case MySQLColumnType::GEOMETRY:    return "GEOMETRY";
        case MySQLColumnType::VECTOR:      return "VECTOR";
        default:                           return "UNKNOWN";
    }
}

/**
 * @brief Check if column type is variable length
 */
inline bool is_variable_length_type(MySQLColumnType type) {
    switch (type) {
        case MySQLColumnType::VARCHAR:
        case MySQLColumnType::VAR_STRING:
        case MySQLColumnType::STRING:  // CHAR can be variable with utf8
        case MySQLColumnType::BLOB:
        case MySQLColumnType::TINY_BLOB:
        case MySQLColumnType::MEDIUM_BLOB:
        case MySQLColumnType::LONG_BLOB:
        case MySQLColumnType::JSON:
        case MySQLColumnType::GEOMETRY:
        case MySQLColumnType::VECTOR:  // VECTOR is variable-length embedding
            return true;
        default:
            return false;
    }
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_CHARSET_MAPPING_HPP