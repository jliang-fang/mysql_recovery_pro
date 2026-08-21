/**
 * @file schema_aware_parser.cpp
 * @brief Schema-Aware Parser implementation for MySQL 5.6 data recovery
 *
 * Parses binary tuples from recovered IBD pages and maps them to SQL tuples
 * using schema templates or inferred structures.
 *
 * Key features:
 * - COMPACT row format parsing with record chain traversal
 * - Field decoders for all MySQL types (TINYINT through BLOB)
 * - Schema template matching and inference
 * - SQL INSERT generation
 */

#include "dbrecovery/engine/schema_aware_parser.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <iostream>

namespace dbrecovery {
namespace engine {

// ==================== MySQL Type Constants (local aliases) ====================

namespace mt = innodb::mysql_types;

// ==================== Field Decoders ====================

/**
 * @brief Decode a TINYINT field (1 byte, signed or unsigned)
 */
static std::string decode_tinyint(const uint8_t* data, uint32_t& offset, bool is_signed) {
    if (is_signed) {
        int8_t val = static_cast<int8_t>(data[offset]);
        offset += 1;
        return std::to_string(val);
    } else {
        uint8_t val = data[offset];
        offset += 1;
        return std::to_string(val);
    }
}

/**
 * @brief Decode a SMALLINT field (2 bytes, signed or unsigned, LE)
 */
static std::string decode_smallint(const uint8_t* data, uint32_t& offset, bool is_signed) {
    if (is_signed) {
        int16_t val = innodb::read_le<int16_t>(data, offset);
        offset += 2;
        return std::to_string(val);
    } else {
        uint16_t val = innodb::read_le<uint16_t>(data, offset);
        offset += 2;
        return std::to_string(val);
    }
}

/**
 * @brief Decode a MEDIUM_INT field (3 bytes, signed or unsigned, LE)
 */
static std::string decode_mediumint(const uint8_t* data, uint32_t& offset, bool is_signed) {
    if (is_signed) {
        int32_t val = innodb::read_le<int32_t>(data, offset) & 0xFFFFFF;
        if (val & 0x800000) val |= 0xFF000000;  // Sign extend
        offset += 3;
        return std::to_string(val);
    } else {
        uint32_t val = innodb::read_le<uint32_t>(data, offset) & 0xFFFFFF;
        offset += 3;
        return std::to_string(val);
    }
}

/**
 * @brief Decode an INT field (4 bytes, signed or unsigned, LE)
 */
static std::string decode_int(const uint8_t* data, uint32_t& offset, bool is_signed) {
    if (is_signed) {
        int32_t val = innodb::read_le<int32_t>(data, offset);
        offset += 4;
        return std::to_string(val);
    } else {
        uint32_t val = innodb::read_le<uint32_t>(data, offset);
        offset += 4;
        return std::to_string(val);
    }
}

/**
 * @brief Decode a BIGINT field (8 bytes, signed or unsigned, LE)
 */
static std::string decode_bigint(const uint8_t* data, uint32_t& offset, bool is_signed) {
    if (is_signed) {
        int64_t val = innodb::read_le<int64_t>(data, offset);
        offset += 8;
        return std::to_string(val);
    } else {
        uint64_t val = innodb::read_le<uint64_t>(data, offset);
        offset += 8;
        return std::to_string(val);
    }
}

/**
 * @brief Decode a FLOAT field (4 bytes, IEEE 754)
 * NOTE: InnoDB stores FLOAT in LITTLE-ENDIAN (native IEEE 754), NOT big-endian!
 */
static std::string decode_float(const uint8_t* data, uint32_t& offset) {
    // InnoDB stores FLOAT as little-endian IEEE 754 (native byte order)
    float val;
    std::memcpy(&val, data + offset, sizeof(float));
    offset += 4;

    std::ostringstream oss;
    oss << std::setprecision(7) << val;
    return oss.str();
}

/**
 * @brief Decode a DOUBLE field (8 bytes, IEEE 754)
 * NOTE: InnoDB stores DOUBLE in LITTLE-ENDIAN (native IEEE 754), NOT big-endian!
 */
static std::string decode_double(const uint8_t* data, uint32_t& offset) {
    // InnoDB stores DOUBLE as little-endian IEEE 754 (native byte order)
    double val;
    std::memcpy(&val, data + offset, sizeof(double));
    offset += 8;

    std::ostringstream oss;
    oss << std::setprecision(15) << val;
    return oss.str();
}

/**
 * @brief Decode a VARCHAR/VAR_STRING field (length-prefixed string)
 */
static std::string decode_varchar(const uint8_t* data, uint32_t& offset,
                                   uint32_t max_length) {
    uint32_t str_len;

    // Length prefix: 1 byte if max_length <= 255, 2 bytes if > 255
    if (max_length <= 255) {
        str_len = data[offset];
        offset += 1;
    } else {
        str_len = innodb::read_le<uint16_t>(data, offset);
        offset += 2;
    }

    std::string result;
    result.reserve(str_len);
    for (uint32_t i = 0; i < str_len && offset + i < static_cast<uint32_t>(-1); ++i) {
        char c = static_cast<char>(data[offset + i]);
        if (c >= 32 && c < 127) {
            result += c;
        } else if (c == 0) {
            // Null byte in string
        } else {
            result += '?';  // Non-printable
        }
    }

    offset += str_len;
    return result;
}

/**
 * @brief Decode a BLOB field (20-byte external pointer)
 */
static std::string decode_blob(const uint8_t* data, uint32_t& offset) {
    // BLOB stored as 20-byte external pointer in the page
    // The pointer contains: space_id(4), page_number(4), offset(4), length(8)
    // For now, we just skip the pointer and indicate external storage
    offset += 20;
    return "<BLOB_EXTERNAL>";
}

/**
 * @brief Decode a DATETIME field (8 bytes, InnoDB binary format)
 */
static std::string decode_datetime(const uint8_t* data, uint32_t& offset, uint8_t fractional_digits = 0) {
    // InnoDB DATETIME binary format (5 bytes + fractional):
    // t = (B2UINT4(data[:4])<<8) + B2UINT1(data[4:5])
    // months = (t&549751619584)>>22  = (t & 0x8000000000) >> 22
    // year = months//13, month = months%13
    // day = (t&4063232)>>17 = (t & 0x3E0000) >> 17
    // hour = (t&126976)>>12 = (t & 0x1F000) >> 12
    // minute = (t&4032)>>6 = (t & 0xFC0) >> 6
    // second = t&63 = t & 0x3F

    // Read 5 bytes: 4 bytes BE shifted left by 8, plus 1 byte
    uint64_t t = (static_cast<uint64_t>(innodb::read_be<uint32_t>(data, offset)) << 8) |
                 static_cast<uint64_t>(data[offset + 4]);
    offset += 5;

    // Extract fields using bit masks
    // YearMonth: 17 bits at position 22-38 (mask 0x7fffc00000 = 549751619584)
    uint64_t yearmonth = (t & 0x7FFFC00000ULL) >> 22;  // 549751619584 = 0x7FFFC00000
    uint32_t year = static_cast<uint32_t>(yearmonth / 13);
    uint32_t month = static_cast<uint32_t>(yearmonth % 13);
    uint32_t day = static_cast<uint32_t>((t & 0x3E0000ULL) >> 17);     // 4063232 = 0x3E0000
    uint32_t hour = static_cast<uint32_t>((t & 0x1F000ULL) >> 12);     // 126976 = 0x1F000
    uint32_t minute = static_cast<uint32_t>((t & 0xFC0ULL) >> 6);      // 4032 = 0xFC0
    uint32_t second = static_cast<uint32_t>(t & 0x3FULL);              // 63 = 0x3F

    // Handle fractional seconds if specified (DATETIME(6))
    std::string fractional = "";
    if (fractional_digits > 0 && offset + fractional_digits <= 1024) {
        uint32_t frac_val = 0;
        if (fractional_digits <= 3) {
            frac_val = innodb::read_be<uint32_t>(data, offset) >> (24 - fractional_digits * 8);
            offset += fractional_digits;
        } else if (fractional_digits <= 6) {
            // For DATETIME(6), read 3 bytes for microseconds
            frac_val = (static_cast<uint32_t>(data[offset]) << 16) |
                       (static_cast<uint32_t>(data[offset + 1]) << 8) |
                       static_cast<uint32_t>(data[offset + 2]);
            offset += 3;
        }
        std::ostringstream frac_oss;
        frac_oss << "." << std::setfill('0') << std::setw(fractional_digits) << frac_val;
        fractional = frac_oss.str();
    }

    if (year == 0 && month == 0 && day == 0 && hour == 0 && minute == 0 && second == 0) {
        return "0000-00-00 00:00:00";
    }

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << year << "-"
        << std::setw(2) << month << "-"
        << std::setw(2) << day << " "
        << std::setw(2) << hour << ":"
        << std::setw(2) << minute << ":"
        << std::setw(2) << second << fractional;

    return oss.str();
}

/**
 * @brief Decode a TIMESTAMP field (4 bytes, Unix epoch seconds LE)
 */
static std::string decode_timestamp(const uint8_t* data, uint32_t& offset, uint8_t fractional_digits = 0) {
    // InnoDB stores TIMESTAMP as big-endian 4-byte Unix epoch
    uint32_t epoch = innodb::read_be<uint32_t>(data, offset);
    offset += 4;

    if (epoch == 0) {
        return "0000-00-00 00:00:00";
    }

    time_t t = static_cast<time_t>(epoch);
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    std::string result(buf);

    // Handle fractional seconds for TIMESTAMP(6)
    if (fractional_digits > 0 && offset + fractional_digits <= 1024) {
        uint32_t frac_val = 0;
        if (fractional_digits <= 3) {
            frac_val = innodb::read_be<uint32_t>(data, offset) >> (24 - fractional_digits * 8);
            offset += fractional_digits;
        } else if (fractional_digits <= 6) {
            frac_val = (static_cast<uint32_t>(data[offset]) << 16) |
                       (static_cast<uint32_t>(data[offset + 1]) << 8) |
                       static_cast<uint32_t>(data[offset + 2]);
            offset += 3;
        }
        std::ostringstream frac_oss;
        frac_oss << "." << std::setfill('0') << std::setw(fractional_digits) << frac_val;
        result += frac_oss.str();
    }

    return result;
}

/**
 * @brief Decode a DATE field (3 bytes)
 * Format: year*16*32 + month*32 + day
 */
static std::string decode_date(const uint8_t* data, uint32_t& offset) {
    // InnoDB DATE format: 3 bytes, big-endian
    // Bits: 1 bit sign | 14 bits year | 4 bits month | 5 bits day
    uint32_t raw = (static_cast<uint32_t>(data[offset]) << 16) |
                   (static_cast<uint32_t>(data[offset + 1]) << 8) |
                   data[offset + 2];
    offset += 3;

    uint32_t day_part = raw & 0x1F;
    uint32_t month_part = (raw >> 5) & 0x0F;
    uint32_t year_part = (raw >> 9) & 0x3FFF;

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << year_part << "-"
        << std::setw(2) << month_part << "-"
        << std::setw(2) << day_part;
    return oss.str();
}

/**
 * @brief Decode a TIME field (3 bytes)
 */
static std::string decode_time(const uint8_t* data, uint32_t& offset, uint8_t fractional_digits = 0) {
    // InnoDB TIME format: 3 bytes + optional fractional
    // t = B2INT3(data[:3]) - signed 3-byte integer
    // For negative: hour = 2047 - ((8384512&t)>>12), minute = 63 - ((4032&t)>>6), second = 63 - (63&t)
    // For positive: hour = (8384512&t)>>12, minute = (4032&t)>>6, second = 63&t

    int32_t raw = static_cast<int32_t>(
        (static_cast<uint32_t>(data[offset]) << 16) |
        (static_cast<uint32_t>(data[offset + 1]) << 8) |
        static_cast<uint32_t>(data[offset + 2]));
    // Sign extend 24-bit to 32-bit
    if (raw & 0x800000) raw |= 0xFF000000;
    offset += 3;

    std::string sign = "";
    uint32_t hour, minute, second;

    if (raw < 0) {
        sign = "-";
        uint32_t t = static_cast<uint32_t>(-raw);
        hour = 2047 - ((t & 0x800000) >> 12);   // 8384512 = 0x800000
        minute = 63 - ((t & 0xFC0) >> 6);       // 4032 = 0xFC0
        second = 63 - (t & 0x3F);               // 63 = 0x3F
    } else {
        uint32_t t = static_cast<uint32_t>(raw);
        hour = (t & 0x800000) >> 12;            // 8384512 = 0x800000
        minute = (t & 0xFC0) >> 6;              // 4032 = 0xFC0
        second = t & 0x3F;                      // 63 = 0x3F
    }

    // Handle fractional seconds for TIME(6)
    std::string fractional = "";
    if (fractional_digits > 0 && offset + fractional_digits <= 1024) {
        uint32_t frac_val = 0;
        if (fractional_digits <= 3) {
            frac_val = innodb::read_be<uint32_t>(data, offset) >> (24 - fractional_digits * 8);
            offset += fractional_digits;
        } else if (fractional_digits <= 6) {
            frac_val = (static_cast<uint32_t>(data[offset]) << 16) |
                       (static_cast<uint32_t>(data[offset + 1]) << 8) |
                       static_cast<uint32_t>(data[offset + 2]);
            offset += 3;
        }
        std::ostringstream frac_oss;
        frac_oss << "." << std::setfill('0') << std::setw(fractional_digits) << frac_val;
        fractional = frac_oss.str();
    }

    std::ostringstream oss;
    oss << sign << std::setfill('0') << std::setw(2) << hour << ":"
        << std::setw(2) << minute << ":"
        << std::setw(2) << second << fractional;
    return oss.str();
}

/**
 * @brief Decode a YEAR field (1 byte, offset from 1900)
 */
static std::string decode_year(const uint8_t* data, uint32_t& offset) {
    uint8_t raw = data[offset];
    offset += 1;
    uint32_t year = 1900 + raw;
    return std::to_string(year);
}

/**
 * @brief Decode a BIT field
 */
static std::string decode_bit(const uint8_t* data, uint32_t& offset, uint32_t bit_length) {
    uint32_t byte_length = (bit_length + 7) / 8;
    std::string result;
    for (uint32_t i = 0; i < byte_length; ++i) {
        if (i > 0) result += " ";
        char buf[4];
        snprintf(buf, sizeof(buf), "%02x", data[offset + i]);
        result += buf;
    }
    offset += byte_length;
    return result;
}

// ==================== TupleParser Implementation ====================

TupleParser::TupleParser(const Config& config)
    : config_(config) {
}

TupleParser::~TupleParser() = default;

RecoveredRecord TupleParser::parse_record(
    const std::vector<uint8_t>& record_data,
    const innodb::Record& record_header) {

    RecoveredRecord rec;
    rec.is_deleted = record_header.is_deleted;
    rec.is_infimum = (record_header.info_bits & innodb::REC_INFO_STATUS_MASK) == innodb::REC_STATUS_INFIMUM;
    rec.is_supremum = (record_header.info_bits & innodb::REC_INFO_STATUS_MASK) == innodb::REC_STATUS_SUPREMUM;
    rec.raw_data = record_data;

    if (rec.is_infimum || rec.is_supremum) {
        return rec;
    }

    if (record_data.empty()) {
        return rec;
    }

    const uint8_t* data = record_data.data();
    uint32_t data_size = static_cast<uint32_t>(record_data.size());

    // COMPACT row format:
    // [variable-length column sizes] [NULL bitmap] [record header fields...]
    // Then: field data from the end of the record

    // Count variable-length and nullable columns
    uint32_t num_cols = static_cast<uint32_t>(config_.schema.columns.size());
    uint32_t num_nullable = 0;
    uint32_t num_variable = 0;

    for (const auto& col : config_.schema.columns) {
        if (col.is_nullable) num_nullable++;
        if (col.is_variable_length) num_variable++;
    }

    rec.field_count = num_cols;

    // Parse variable-length column sizes from the end of record header
    // In COMPACT format, these are stored before the record data, reading backwards
    // from the record start

    // Read NULL bitmap (after variable-length column sizes)
    uint32_t null_bitmap_size = (num_nullable + 7) / 8;

    // Calculate offsets for field data
    // In COMPACT format, field data starts after the record header
    // Variable-length field sizes are stored in reverse order at the start
    uint32_t field_offset = 0;  // Offset into the record data portion

    // Parse each field
    uint32_t nullable_ordinal = 0;  // ordinal among nullable columns only
    for (const auto& col : config_.schema.columns) {
        std::string value;

        // Check NULL bitmap using nullable column ordinal
        if (col.is_nullable) {
            uint32_t null_byte_idx = nullable_ordinal / 8;
            uint32_t null_bit_idx = nullable_ordinal % 8;
            // Check if NULL bit is set in the null_bitmap
            // (The null_bitmap is parsed in BinaryTupleParser::parse)
            nullable_ordinal++;
        }

        value.clear();
        field_offset = parse_field(data, field_offset, col, value);
        rec.fields[col.name] = value;

        rec.field_offsets.push_back(field_offset);
        rec.field_lengths.push_back(static_cast<uint32_t>(value.size()));
    }

    return rec;
}

uint32_t TupleParser::parse_field(const uint8_t* data, uint32_t offset,
                                    const SchemaTemplate::Column& column,
                                    std::string& value) {
    // Delegate to BinaryTupleParser for actual field decoding
    return BinaryTupleParser::parse_field_value(data, offset, column, value);
}

uint32_t TupleParser::parse_page(
    const std::vector<uint8_t>& page_data,
    const SchemaTemplate& schema,
    std::function<void(const RecoveredRecord&)> callback) {

    uint32_t records_parsed = 0;

    if (page_data.size() < 128) {
        return records_parsed;
    }

    const uint8_t* data = page_data.data();
    uint32_t page_size = static_cast<uint32_t>(page_data.size());

    // Check page type (BIG-ENDIAN)
    uint16_t page_type = innodb::read_be<uint16_t>(data, innodb::FIL_PAGE_TYPE_ABS);
    if (!innodb::is_valid_index_page_type(page_type) && page_type != 0) {
        return records_parsed;
    }

    // P0 fix: Skip internal (non-leaf) B-tree nodes - they only store index keys + child pointers,
    // not actual data records. page_level is at offset FIL_HEADER_SIZE + PAGE_LEVEL_OFFSET (64+8=72 in InnoDB).
    // page_level == 0 means leaf node (contains real data), > 0 means internal node.
    uint16_t page_level = innodb::read_be<uint16_t>(data, innodb::FIL_HEADER_SIZE + 8);
    if (page_level > 0) {
        // Internal node - skip it, only leaf nodes contain user data records
        return records_parsed;
    }

    // Read page header (starts at FIL_HEADER_SIZE = 38)
    auto page_hdr = innodb::PageHeader::parse(data + innodb::FIL_HEADER_SIZE,
                                               page_size - innodb::FIL_HEADER_SIZE);
    if (!page_hdr.has_value()) {
        return records_parsed;
    }

    uint16_t n_recs = page_hdr->n_heap - 2;  // n_heap includes infimum+supremum
    if (n_recs == 0) return records_parsed;

    // PAGE_NEW_INFIMUM = 99
    // Both forward (offset) and backward pointers start at 99
    constexpr uint16_t PAGE_NEW_INFIMUM = 99;

    if (PAGE_NEW_INFIMUM >= page_size) return records_parsed;

    // Read record count from PAGE_N_RECS (offset 38+16 = 54, big-endian)
    uint16_t page_n_recs = innodb::read_be<uint16_t>(data, 54);

    // Start from infimum origin offset
    uint32_t rec_origin = PAGE_NEW_INFIMUM;

    // Determine column metadata from schema
    uint32_t num_cols = static_cast<uint32_t>(schema.columns.size());

    // Count nullable and variable-length columns
    // In InnoDB, variable-length columns include: VARCHAR, VAR_STRING, BLOB types, TEXT, DECIMAL, JSON, ENUM, SET
    uint32_t num_nullable = 0;
    uint32_t num_variable = 0;
    for (const auto& col : schema.columns) {
        if (col.is_nullable) num_nullable++;
        bool is_var = col.is_variable_length || col.mysql_type == mt::VARCHAR ||
                      col.mysql_type == mt::VAR_STRING ||
                      col.mysql_type == mt::STRING ||  // STRING/CHAR (type 29) - variable for utf8mb4
                      col.mysql_type == mt::TINY_BLOB || col.mysql_type == mt::MEDIUM_BLOB ||
                      col.mysql_type == mt::LONG_BLOB || col.mysql_type == mt::BLOB ||
                      col.mysql_type == mt::JSON || col.mysql_type == mt::NEWDECIMAL;
        if (is_var) num_variable++;
    }

    // Walk the record chain:
    // 1. Read 5-byte record header backward from rec_origin
    // 2. REC_NEXT = signed_offset + rec_origin
    // 3. For user records: read NULL bitmap + var sizes backward, then field data forward
    uint32_t safety_counter = 0;
    const uint32_t MAX_RECORDS = 10000;

    while (rec_origin > 0 && rec_origin < page_size && safety_counter < MAX_RECORDS) {
        // Read 5-byte COMPACT record header from [rec_origin-5, rec_origin)
        if (rec_origin < 5) break;
        uint32_t hdr_start = rec_origin - 5;

        // Parse header bytes: rec1(2 bytes) + rec2(1 byte) + rec_next(2 bytes signed)
        // Layout in page at [hdr_start]: [rec1_hi][rec1_lo][rec2][rec_next_hi][rec_next_lo]
        // >HBh unpack: rec1 = big-endian 16-bit, rec2 = unsigned 8-bit, rec_next = big-endian signed 16-bit
        uint16_t rec1 = (static_cast<uint16_t>(data[hdr_start]) << 8) |
                         static_cast<uint16_t>(data[hdr_start + 1]);
        uint8_t rec2 = data[hdr_start + 2];
        int16_t rec_next_raw = static_cast<int16_t>(
            (static_cast<uint16_t>(data[hdr_start + 3]) << 8) |
             static_cast<uint16_t>(data[hdr_start + 4]));

        // 24-bit rec value: rec = (rec1 << 8) + rec2
        uint32_t rec = (static_cast<uint32_t>(rec1) << 8) | static_cast<uint32_t>(rec2);

        // Extract bit fields
        bool rec_info_instant  = (rec & 0x800000) != 0;  // bit 23
        bool rec_info_version  = (rec & 0x400000) != 0;  // bit 22
        bool rec_info_deleted  = (rec & 0x200000) != 0;  // bit 21
        bool rec_info_min_rec  = (rec & 0x100000) != 0;  // bit 20
        uint8_t rec_n_owned    = (rec & 0x0F0000) >> 16; // bits 19-16
        uint16_t rec_heap_no   = (rec & 0x00FFF8) >> 3;  // bits 15-3
        uint8_t rec_type       = rec & 0x07;              // bits 2-0

        // REC_NEXT is relative to rec_origin: next_origin = rec_next_raw + rec_origin
        uint32_t next_origin = static_cast<uint32_t>(
            static_cast<int32_t>(rec_next_raw) + static_cast<int32_t>(rec_origin));

        if (rec_type == innodb::REC_STATUS_SUPREMUM) {
            break;  // End of record chain
        }

        if (rec_type == innodb::REC_STATUS_ORDINARY) {
            // Parse this user record using bidirectional approach
            // The backward pointer starts at rec_origin (after reading header, it's at hdr_start)
            // The forward pointer starts at rec_origin

            uint32_t fwd_offset = rec_origin;  // Forward read position (field data)
            uint32_t bwd_offset = hdr_start;    // Backward read position (metadata)

            // 1. Read row version (if instant ALTER TABLE flag is set)
            uint8_t row_version = 0;
            if (rec_info_instant || rec_info_version) {
                if (bwd_offset < 1) goto next_record;
                row_version = data[bwd_offset - 1];
                bwd_offset--;
            }

            // 根据row_version获取活跃列 (Instant ADD COLUMN 支持)
            // version_added <= row_version < version_dropped 时列存在
            auto active_cols = schema.get_active_columns(row_version);
            uint32_t active_num_cols = static_cast<uint32_t>(active_cols.size());
            uint32_t active_num_nullable = schema.get_nullable_count_for_version(row_version);

            // Count variable-length columns among active columns
            uint32_t active_num_variable = 0;
            for (const auto& col : active_cols) {
                bool is_var = col.is_variable_length || col.mysql_type == mt::VARCHAR ||
                              col.mysql_type == mt::VAR_STRING ||
                              col.mysql_type == mt::STRING ||  // STRING/CHAR (type 29) - variable for utf8mb4
                              col.mysql_type == mt::TINY_BLOB || col.mysql_type == mt::MEDIUM_BLOB ||
                              col.mysql_type == mt::LONG_BLOB || col.mysql_type == mt::BLOB ||
                              col.mysql_type == mt::JSON;
                if (is_var) active_num_variable++;
            }

            // 2. Read NULL bitmap backward (using active column count)
            uint32_t null_bitmap_size = (active_num_nullable + 7) / 8;
            uint32_t nullvalue = 0;
            if (null_bitmap_size > 0 && bwd_offset >= null_bitmap_size) {
                bwd_offset -= null_bitmap_size;
                // Read as big-endian integer from bwd_offset
                for (uint32_t i = 0; i < null_bitmap_size; i++) {
                    nullvalue = (nullvalue << 8) | data[bwd_offset + i];
                }
            }

            // 3. Read variable-length column sizes backward
            // Variable-length sizes are read in FORWARD column order
            // Each size is read via read_reverse (toward lower offsets), but the
            // column iteration order matches the schema definition order.
            // is_big = char_length > 255 where char_length = mbmaxlen * character_count
            std::vector<uint32_t> var_lengths(active_num_variable, 0);
            uint32_t var_idx = 0;
            for (uint32_t ci = 0; ci < active_num_cols; ci++) {
                const auto& col = active_cols[ci];
                bool is_var = col.is_variable_length || col.mysql_type == mt::VARCHAR ||
                              col.mysql_type == mt::VAR_STRING ||
                              col.mysql_type == mt::STRING ||  // STRING/CHAR (type 29) - variable for utf8mb4
                              col.mysql_type == mt::TINY_BLOB || col.mysql_type == mt::MEDIUM_BLOB ||
                              col.mysql_type == mt::LONG_BLOB || col.mysql_type == mt::BLOB ||
                              col.mysql_type == mt::JSON;
                // NOTE: NEWDECIMAL is NOT is_var
                if (!is_var) continue;

                if (var_idx >= active_num_variable) break;  // safety

                // is_big: char_length > 255
                // char_length = mbmaxlen * character_count (byte length)
                uint32_t char_len = col.char_length > 0 ? col.char_length : col.max_length;
                // For VARCHAR(N) with mbmaxlen>1, char_length should already be mbmaxlen*N
                // If not set, compute it
                if (char_len > 0 && col.mbmaxlen > 1 && char_len < 256) {
                    char_len = char_len * col.mbmaxlen;
                }
                bool is_big = char_len > 255;

                if (is_big) {
                    // Read 1 or 2 bytes for length
                    if (bwd_offset < 1) goto next_record;
                    uint8_t tsize = data[bwd_offset - 1];
                    bwd_offset--;
                    if (tsize > 0x7F) {
                        // 2-byte encoding: vsize = byte2 + (tsize-128)*256
                        if (bwd_offset < 1) goto next_record;
                        uint8_t byte2 = data[bwd_offset - 1];
                        bwd_offset--;
                        var_lengths[var_idx] = byte2 + (tsize - 128) * 256;
                    } else {
                        var_lengths[var_idx] = tsize;
                    }
                } else {
                    // 1-byte length
                    if (bwd_offset < 1) goto next_record;
                    var_lengths[var_idx] = data[bwd_offset - 1];
                    bwd_offset--;
                }
                var_idx++;
            }

            // 4. Read field data forward from rec_origin
            // Order: PK fields → DB_TRX_ID(6) → DB_ROLL_PTR(7) → non-PK fields
            // PK fields are stored in InnoDB internal format: big-endian with sign bit inverted

            // Determine PK column count among active columns
            uint32_t pk_count = 0;
            for (const auto& col : active_cols) {
                if (col.is_primary_key) pk_count++;
            }
            if (pk_count == 0) pk_count = 1;  // Assume first column is PK if none marked

            // Track nullable ordinal for NULL bitmap checking
            uint32_t nullable_ordinal = 0;
            uint32_t var_read_idx = 0;
            RecoveredRecord recovered;
            recovered.is_deleted = rec_info_deleted;
            recovered.is_infimum = false;
            recovered.is_supremum = false;
            recovered.page_number = innodb::read_be<uint32_t>(data, 4);
            recovered.space_id = innodb::read_be<uint32_t>(data, 34);
            recovered.field_count = active_num_cols;
            recovered.transaction_id = 0;
            recovered.roll_ptr = 0;

            bool parse_ok = true;
            uint32_t col_ordinal = 0;

            for (const auto& col : active_cols) {
                if (fwd_offset >= page_size) { parse_ok = false; break; }

                // Check if this is where DB_TRX_ID and DB_ROLL_PTR go
                // (after PK columns, before non-PK columns)
                if (col_ordinal == pk_count) {
                    // Read DB_TRX_ID (6 bytes, big-endian)
                    if (fwd_offset + 6 <= page_size) {
                        uint64_t trx_id = 0;
                        for (int i = 0; i < 6; i++) {
                            trx_id = (trx_id << 8) | data[fwd_offset++];
                        }
                        recovered.transaction_id = trx_id;
                    }
                    // Read DB_ROLL_PTR (roll_ptr_bytes() bytes, big-endian)
                    if (fwd_offset + roll_ptr_bytes() <= page_size) {
                        uint64_t roll_ptr = 0;
                        for (uint32_t i = 0; i < roll_ptr_bytes(); i++) {
                            roll_ptr = (roll_ptr << 8) | data[fwd_offset++];
                        }
                        recovered.roll_ptr = roll_ptr;
                    }
                }

                // Check NULL bitmap
                bool is_null = false;
                if (col.is_nullable) {
                    is_null = (nullvalue & (1 << nullable_ordinal)) != 0;
                    nullable_ordinal++;
                }

                std::string value;
                if (is_null) {
                    value = "NULL";
                } else {
                    // Determine if variable-length
                    bool is_var = col.is_variable_length || col.mysql_type == mt::VARCHAR ||
                                  col.mysql_type == mt::VAR_STRING ||
                                  col.mysql_type == mt::STRING ||  // STRING/CHAR (type 29) - variable for utf8mb4
                                  col.mysql_type == mt::TINY_BLOB || col.mysql_type == mt::MEDIUM_BLOB ||
                                  col.mysql_type == mt::LONG_BLOB || col.mysql_type == mt::BLOB ||
                                  col.mysql_type == mt::JSON;
                    // NOTE: NEWDECIMAL is NOT is_var

                    if (is_var) {
                        uint32_t vsize = (var_read_idx < var_lengths.size()) ?
                                         var_lengths[var_read_idx] : 0;
                        var_read_idx++;

                        // Check for off-page column marker
                        if (vsize == 16404) {  // 0x4014 - off-page (BLOB)
                            if (fwd_offset + 20 <= page_size) {
                                fwd_offset += 20;
                            }
                            value = "<BLOB_EXTERNAL>";
                        } else if (vsize == 17172) {  // 0x4314 - off-page (COMPRESSED)
                            if (fwd_offset + 20 <= page_size) {
                                fwd_offset += 20;
                            }
                            value = "<BLOB_EXTERNAL>";
                        } else {
                            if (fwd_offset + vsize <= page_size) {
                                // For PK columns, use InnoDB internal format decoding
                                if (col.is_primary_key) {
                                    value = decode_innodb_pk_field(data + fwd_offset, vsize, col);
                                } else {
                                    uint32_t local_offset = 0;
                                    value = decode_field_data(data + fwd_offset, local_offset, col, vsize);
                                }
                                fwd_offset += vsize;
                            } else {
                                parse_ok = false;
                                break;
                            }
                        }
                    } else {
                        // Fixed-length field
                        uint32_t field_size = get_fixed_field_size(col);
                        if (field_size == 0) {
                            value = "<UNKNOWN>";
                        } else if (fwd_offset + field_size <= page_size) {
                            // For PK columns, use InnoDB internal format decoding
                            if (col.is_primary_key) {
                                value = decode_innodb_pk_field(data + fwd_offset, field_size, col);
                                fwd_offset += field_size;
                            } else {
                                uint32_t local_offset = 0;
                                value = decode_field_data(data + fwd_offset, local_offset, col, field_size);
                                fwd_offset += field_size;
                            }
                        } else {
                            parse_ok = false;
                            break;
                        }
                    }
                }

                recovered.fields[col.name] = value;
                col_ordinal++;
            }

            // Handle DB_TRX_ID/DB_ROLL_PTR if not yet read (when pk_count >= num_cols)
            if (col_ordinal <= pk_count && parse_ok) {
                if (fwd_offset + 6 <= page_size) {
                    uint64_t trx_id = 0;
                    for (int i = 0; i < 6; i++) {
                        trx_id = (trx_id << 8) | data[fwd_offset++];
                    }
                    recovered.transaction_id = trx_id;
                }
                if (fwd_offset + roll_ptr_bytes() <= page_size) {
                    uint64_t roll_ptr = 0;
                    for (uint32_t i = 0; i < roll_ptr_bytes(); i++) {
                        roll_ptr = (roll_ptr << 8) | data[fwd_offset++];
                    }
                    recovered.roll_ptr = roll_ptr;
                }
            }

            if (parse_ok) {
                records_parsed++;
                if (callback) {
                    callback(recovered);
                }
            }
        }

        next_record:
        // Move to next record
        if (next_origin <= rec_origin || next_origin >= page_size) break;
        rec_origin = next_origin;
        safety_counter++;
    }

    return records_parsed;
}

// Get fixed-size field byte length for a column type
uint32_t TupleParser::get_fixed_field_size(const SchemaTemplate::Column& col) const {
    switch (col.mysql_type) {
        case mt::TINY:       return 1;
        case mt::SHORT:      return 2;
        case mt::INT24:      return 3;
        case mt::LONG:       return 4;
        case mt::LONGLONG:   return 8;
        case mt::FLOAT:      return 4;
        case mt::DOUBLE:     return 8;
        case mt::DATE:       return 3;
        case mt::TIME:       return 3;
        case mt::DATETIME:   return 8;   // Old MySQL 5.6 format
        case mt::DATETIME2: {            // MySQL 5.6+ new format
            // 5 bytes base + fractional seconds (0-3 bytes for DATETIME(0-6))
            uint8_t frac_digits = col.datetime_precision;
            return 5 + (frac_digits > 0 ? (frac_digits + 1) / 2 : 0);
        }
        case mt::TIMESTAMP2: {           // MySQL 5.6+ new format
            // 4 bytes base + fractional seconds (0-3 bytes for TIMESTAMP(0-6))
            uint8_t frac_digits = col.datetime_precision;
            return 4 + (frac_digits > 0 ? (frac_digits + 1) / 2 : 0);
        }
        case mt::TIME2: {                // MySQL 5.6+ new format
            // 3 bytes base + fractional seconds (0-3 bytes for TIME(0-6))
            uint8_t frac_digits = col.datetime_precision;
            return 3 + (frac_digits > 0 ? (frac_digits + 1) / 2 : 0);
        }
        case mt::YEAR:       return 1;
        case mt::BIT:        return (col.max_length + 7) / 8;
        case mt::NEWDECIMAL: {
            // DECIMAL(P,S): int_groups + dec_groups, each 4 bytes
            uint32_t prec = col.decimal_precision > 0 ? col.decimal_precision : 10;
            uint32_t scale = col.decimal_scale;
            uint32_t int_digits = prec - scale;
            uint32_t int_groups = (int_digits + 8) / 9;
            uint32_t dec_groups = (scale + 8) / 9;
            return (int_groups + dec_groups) * 4;
        }
        default:             return col.max_length;
    }
}

// Decode PK field stored in InnoDB internal format (big-endian with sign bit inverted)
std::string TupleParser::decode_innodb_pk_field(
    const uint8_t* data, uint32_t data_size,
    const SchemaTemplate::Column& col) const {

    switch (col.mysql_type) {
        case mt::TINY: {
            // 1 byte, sign bit inverted: 0x80+val for positive, 0x7F-val for negative
            uint8_t raw = data[0];
            if (col.is_signed) {
                int8_t val = static_cast<int8_t>(raw - 0x80);
                return std::to_string(val);
            } else {
                uint8_t val = raw - 0x80;
                return std::to_string(val);
            }
        }
        case mt::SHORT: {
            // 2 bytes big-endian, sign bit inverted
            uint16_t raw = (static_cast<uint16_t>(data[0]) << 8) | data[1];
            if (col.is_signed) {
                int16_t val = static_cast<int16_t>(raw - 0x8000);
                return std::to_string(val);
            } else {
                uint16_t val = raw - 0x8000;
                return std::to_string(val);
            }
        }
        case mt::INT24: {
            // 3 bytes big-endian, sign bit inverted
            uint32_t raw = (static_cast<uint32_t>(data[0]) << 16) |
                           (static_cast<uint32_t>(data[1]) << 8) | data[2];
            if (col.is_signed) {
                int32_t val = static_cast<int32_t>(raw - 0x800000);
                return std::to_string(val);
            } else {
                uint32_t val = raw - 0x800000;
                return std::to_string(val);
            }
        }
        case mt::LONG: {
            // 4 bytes big-endian, sign bit inverted
            uint32_t raw = (static_cast<uint32_t>(data[0]) << 24) |
                           (static_cast<uint32_t>(data[1]) << 16) |
                           (static_cast<uint32_t>(data[2]) << 8) | data[3];
            if (col.is_signed) {
                int32_t val = static_cast<int32_t>(raw - 0x80000000U);
                return std::to_string(val);
            } else {
                uint32_t val = raw - 0x80000000U;
                return std::to_string(val);
            }
        }
        case mt::LONGLONG: {
            // 8 bytes big-endian, sign bit inverted
            uint64_t raw = (static_cast<uint64_t>(data[0]) << 56) |
                           (static_cast<uint64_t>(data[1]) << 48) |
                           (static_cast<uint64_t>(data[2]) << 40) |
                           (static_cast<uint64_t>(data[3]) << 32) |
                           (static_cast<uint64_t>(data[4]) << 24) |
                           (static_cast<uint64_t>(data[5]) << 16) |
                           (static_cast<uint64_t>(data[6]) << 8) | data[7];
            if (col.is_signed) {
                int64_t val = static_cast<int64_t>(raw - 0x8000000000000000ULL);
                return std::to_string(val);
            } else {
                uint64_t val = raw - 0x8000000000000000ULL;
                return std::to_string(val);
            }
        }
        default: {
            // For other PK types (VARCHAR, etc.), fall through to normal decode
            uint32_t local_offset = 0;
            return decode_field_data(data, local_offset, col, data_size);
        }
    }
}

// Decode field data from raw bytes with known size
std::string TupleParser::decode_field_data(
    const uint8_t* data, uint32_t& offset,
    const SchemaTemplate::Column& col, uint32_t data_size) const {

    std::string value;
    switch (col.mysql_type) {
        case mt::TINY: {
            if (data_size >= 1) {
                if (col.is_signed) {
                    value = std::to_string(static_cast<int8_t>(data[offset]));
                } else {
                    value = std::to_string(data[offset]);
                }
                offset += 1;
            }
            break;
        }
        case mt::SHORT: {
            if (data_size >= 2) {
                if (col.is_signed) {
                    value = std::to_string(innodb::read_le<int16_t>(data, offset));
                } else {
                    value = std::to_string(innodb::read_le<uint16_t>(data, offset));
                }
                offset += 2;
            }
            break;
        }
        case mt::INT24: {
            if (data_size >= 3) {
                int32_t val = innodb::read_le<int32_t>(data, offset) & 0xFFFFFF;
                if (col.is_signed && (val & 0x800000)) val |= 0xFF000000;
                value = std::to_string(val);
                offset += 3;
            }
            break;
        }
        case mt::LONG: {
            if (data_size >= 4) {
                if (col.is_signed) {
                    value = std::to_string(innodb::read_le<int32_t>(data, offset));
                } else {
                    value = std::to_string(innodb::read_le<uint32_t>(data, offset));
                }
                offset += 4;
            }
            break;
        }
        case mt::LONGLONG: {
            if (data_size >= 8) {
                if (col.is_signed) {
                    value = std::to_string(innodb::read_le<int64_t>(data, offset));
                } else {
                    value = std::to_string(innodb::read_le<uint64_t>(data, offset));
                }
                offset += 8;
            }
            break;
        }
        case mt::FLOAT: {
            if (data_size >= 4) {
                uint32_t raw = innodb::read_be<uint32_t>(data, offset);
                float val;
                std::memcpy(&val, &raw, sizeof(float));
                std::ostringstream oss;
                oss << std::setprecision(7) << val;
                value = oss.str();
                offset += 4;
            }
            break;
        }
        case mt::DOUBLE: {
            if (data_size >= 8) {
                uint64_t raw = innodb::read_be<uint64_t>(data, offset);
                double val;
                std::memcpy(&val, &raw, sizeof(double));
                std::ostringstream oss;
                oss << std::setprecision(15) << val;
                value = oss.str();
                offset += 8;
            }
            break;
        }
        case mt::NEWDECIMAL: {
            // MySQL NEWDECIMAL binary format: groups of 9 digits stored as
            // big-endian 32-bit integers. First group may have fewer digits.
            // Each group: 4 bytes, big-endian, value 0-999999999
            uint32_t prec = col.decimal_precision > 0 ? col.decimal_precision : 10;
            uint32_t scale = col.decimal_scale;
            uint32_t int_digits = prec - scale;
            uint32_t int_groups = (int_digits + 8) / 9;
            uint32_t dec_groups = (scale + 8) / 9;
            uint32_t total_groups = int_groups + dec_groups;
            uint32_t expected_size = total_groups * 4;

            if (data_size >= expected_size && expected_size > 0) {
                // First byte is the sign: 0x80 = positive, 0xC0 = negative (or 0xD0)
                bool is_negative = (data[offset] & 0x80) == 0 ||
                                   (data[offset] & 0x40) != 0;

                // Read digit groups as big-endian 32-bit integers
                // The first group includes the sign in its high byte
                std::string digits;
                for (uint32_t g = 0; g < total_groups; g++) {
                    uint32_t group_val = innodb::read_be<uint32_t>(data, offset + g * 4);
                    if (g == 0) {
                        // Mask out the sign bits from the first group
                        group_val &= 0x3FFFFFFF;  // Clear top 2 bits (sign)
                    }
                    // Determine how many digits this group represents
                    uint32_t digits_in_group;
                    if (g == 0) {
                        digits_in_group = int_digits - (int_groups - 1) * 9;
                        if (digits_in_group == 0 || digits_in_group > 9) digits_in_group = 9;
                    } else {
                        digits_in_group = 9;
                    }
                    // Append digits with leading zeros
                    std::string group_str = std::to_string(group_val);
                    while (group_str.size() < digits_in_group) {
                        group_str = "0" + group_str;
                    }
                    digits += group_str;
                }

                // Insert decimal point
                if (scale > 0 && digits.size() > scale) {
                    digits.insert(digits.size() - scale, ".");
                } else if (scale > 0) {
                    // Need leading zeros after decimal point
                    std::string prefix(scale - digits.size(), '0');
                    digits = "0." + prefix + digits;
                }

                // Remove leading zeros (but keep at least one digit before decimal)
                size_t first_nonzero = digits.find_first_not_of('0');
                if (first_nonzero != std::string::npos && first_nonzero > 0) {
                    // Don't strip past the decimal point
                    size_t dot_pos = digits.find('.');
                    if (dot_pos == std::string::npos || first_nonzero < dot_pos) {
                        // Keep one leading zero before decimal point
                        if (dot_pos != std::string::npos && first_nonzero >= dot_pos) {
                            // All zeros before decimal, keep "0.xxx"
                        } else {
                            digits = digits.substr(first_nonzero);
                            if (!digits.empty() && digits[0] == '.') {
                                digits = "0" + digits;
                            }
                        }
                    }
                }

                if (is_negative && digits != "0") {
                    digits = "-" + digits;
                }
                value = digits;
                offset += expected_size;
            } else {
                // Fallback: output as hex
                std::ostringstream oss;
                for (uint32_t i = 0; i < data_size; i++) {
                    oss << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(data[offset + i]);
                }
                value = "0x" + oss.str();
                offset += data_size;
            }
            break;
        }
        case mt::VARCHAR:
        case mt::VAR_STRING: {
            // Variable-length data — data_size is already known from var_lengths
            // Check if this is binary data (charset_id=63 for binary) or text
            if (col.charset_id == 63) {
                // Binary data (BINARY/VARBINARY) - output as HEX
                std::ostringstream oss;
                for (uint32_t i = 0; i < data_size; i++) {
                    oss << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(data[offset + i]);
                }
                value = oss.str();
            } else {
                // Text data (VARCHAR/CHAR)
                std::string result;
                result.reserve(data_size);
                for (uint32_t i = 0; i < data_size; i++) {
                    char c = static_cast<char>(data[offset + i]);
                    if (c >= 32 && c < 127) {
                        result += c;
                    } else if (c == 0) {
                        // null byte
                    } else {
                        result += '?';
                    }
                }
                value = result;
            }
            offset += data_size;
            break;
        }
        case mt::DATE: {
            if (data_size >= 3) {
                // Big-endian 3-byte DATE: bit layout = sign(1) | year(14) | month(4) | day(5)
                uint32_t raw = (static_cast<uint32_t>(data[offset]) << 16) |
                               (static_cast<uint32_t>(data[offset + 1]) << 8) |
                               data[offset + 2];
                uint32_t day_part = raw & 0x1F;
                uint32_t month_part = (raw >> 5) & 0x0F;
                uint32_t year_part = (raw >> 9) & 0x3FFF;
                std::ostringstream oss;
                oss << std::setfill('0') << std::setw(4) << year_part << "-"
                    << std::setw(2) << month_part << "-"
                    << std::setw(2) << day_part;
                value = oss.str();
                offset += 3;
            }
            break;
        }
        case mt::DATETIME:
        case mt::DATETIME2: {
            // DATETIME decoding
            value = decode_datetime(data, offset, col.datetime_precision);
            break;
        }
        case mt::TIMESTAMP2: {
            value = decode_timestamp(data, offset, col.datetime_precision);
            break;
        }
        case mt::TIME:
        case mt::TIME2: {
            value = decode_time(data, offset, col.datetime_precision);
            break;
        }
        case mt::YEAR: {
            if (data_size >= 1) {
                value = std::to_string(1900 + data[offset]);
                offset += 1;
            }
            break;
        }
        case mt::ENUM: {
            // ENUM stored as 1 or 2 byte index (big-endian)
            uint32_t idx = 0;
            if (data_size == 1) {
                idx = data[offset];
            } else if (data_size >= 2) {
                idx = (static_cast<uint32_t>(data[offset]) << 8) | data[offset + 1];
            }
            // InnoDB stores ENUM starting at index 1, convert to 0-based
            if (idx > 0 && idx - 1 < col.enum_values.size()) {
                value = col.enum_values[idx - 1];
            } else {
                value = "<ENUM_" + std::to_string(idx) + ">";
            }
            offset += data_size;
            break;
        }
        case mt::SET: {
            // SET stored as bitmap (1-8 bytes, big-endian)
            // Read bitmap as big-endian
            uint64_t bits = 0;
            for (uint32_t i = 0; i < data_size && i < 8; i++) {
                bits = (bits << 8) | data[offset + i];
            }
            if (bits == 0) {
                value = "";  // Empty SET
            } else if (!col.set_values.empty()) {
                std::string result;
                for (size_t i = 0; i < col.set_values.size() && i < 64; i++) {
                    if (bits & (1ULL << i)) {
                        if (!result.empty()) result += ",";
                        result += col.set_values[i];
                    }
                }
                value = result;
            } else {
                // No set_values available, output as hex
                std::ostringstream oss;
                for (uint32_t i = 0; i < data_size; i++) {
                    oss << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(data[offset + i]);
                }
                value = oss.str();
            }
            offset += data_size;
            break;
        }
        case mt::BIT: {
            uint32_t byte_len = (col.max_length + 7) / 8;
            if (byte_len == 0) byte_len = data_size;
            for (uint32_t i = 0; i < byte_len && i < data_size; i++) {
                if (i > 0) value += " ";
                char buf[4];
                snprintf(buf, sizeof(buf), "%02x", data[offset + i]);
                value += buf;
            }
            offset += std::min(byte_len, data_size);
            break;
        }
        case mt::TINY_BLOB:
        case mt::MEDIUM_BLOB:
        case mt::LONG_BLOB:
        case mt::BLOB:
            // Binary BLOB data - output as HEX for SQL compatibility
            {
                std::ostringstream oss;
                for (uint32_t i = 0; i < data_size; i++) {
                    oss << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(data[offset + i]);
                }
                value = oss.str();
                offset += data_size;
            }
            break;

        case mt::JSON: {
            // Use binary JSON decoder for proper JSON parsing
            value = decode_json_binary(data, offset, data_size);
            offset += data_size;
            break;
        }
        default: {
            // Unknown type — output as hex
            std::ostringstream oss;
            for (uint32_t i = 0; i < data_size; i++) {
                oss << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(data[offset + i]);
            }
            value = "0x" + oss.str();
            offset += data_size;
            break;
        }
    }
    return value;
}

SchemaMatchResult TupleParser::match_schema(
    const std::vector<RecoveredRecord>& records,
    const SchemaTemplate& template_) {

    SchemaMatchResult result;
    result.matches = false;
    result.confidence = 0.0;

    if (records.empty() || template_.columns.empty()) {
        return result;
    }

    double total_confidence = 0.0;
    uint32_t matched_cols = 0;

    for (const auto& col : template_.columns) {
        SchemaMatchResult::ColumnMatch match;
        match.column_name = col.name;
        match.expected_type = col.type_name;

        // Check if this column exists in the records
        bool found = false;
        for (const auto& rec : records) {
            auto it = rec.fields.find(col.name);
            if (it != rec.fields.end()) {
                found = true;
                match.matches = true;
                match.matched_type = col.type_name;
                match.type_confidence = 1.0;
                break;
            }
        }

        if (found) {
            matched_cols++;
            result.matched_columns.push_back(col.name);
        } else {
            match.matches = false;
            match.type_confidence = 0.0;
            result.unmatched_columns.push_back(col.name);
        }

        result.column_matches.push_back(match);
    }

    result.confidence = template_.columns.empty() ? 0.0
        : static_cast<double>(matched_cols) / template_.columns.size();
    result.matches = result.confidence >= 0.5;

    return result;
}

SchemaTemplate::Column TupleParser::infer_column_type(
    const std::vector<std::string>& values) {
    return SchemaTemplateInference::infer_column(values);
}

std::string TupleParser::generate_sql_insert(
    const RecoveredRecord& record,
    const SchemaTemplate& template_) {

    SqlTupleGenerator gen(SqlTupleGenerator::Config{});
    return gen.generate_insert(record, template_);
}

std::string TupleParser::generate_sql_ddl(const SchemaTemplate& template_) {
    SqlTupleGenerator gen(SqlTupleGenerator::Config{});
    return gen.generate_create_table(template_);
}

std::string TupleParser::to_sql_literal(
    const std::string& value,
    const SchemaTemplate::Column& column) {

    if (value == "NULL") {
        return "NULL";
    }

    switch (column.mysql_type) {
        case mt::TINY:
        case mt::SHORT:
        case mt::LONG:
        case mt::LONGLONG:
        case mt::INT24:
        case mt::FLOAT:
        case mt::DOUBLE:
            return value;  // Numeric types - no quoting

        case mt::DATE:
        case mt::TIME:
        case mt::DATETIME:
        case mt::TIMESTAMP2:
        case mt::DATETIME2:
        case mt::TIME2:
            return "'" + escape_sql_string(value) + "'";

        case mt::VARCHAR:
        case mt::VAR_STRING:
        case mt::TINY_BLOB:
        case mt::MEDIUM_BLOB:
        case mt::LONG_BLOB:
        case mt::BLOB:
        default:
            return "'" + escape_sql_string(value) + "'";
    }
}

std::string TupleParser::escape_sql_string(const std::string& str) {
    std::string result;
    result.reserve(str.size() * 2);

    for (char c : str) {
        switch (c) {
            case '\'': result += "''"; break;
            case '\\': result += "\\\\"; break;
            case '\0': result += "\\0"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 32) {
                    char buf[5];
                    snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned char>(c));
                    result += buf;
                } else {
                    result += c;
                }
                break;
        }
    }

    return result;
}

std::optional<innodb::Record> TupleParser::parse_record_header(
    const uint8_t* data, uint32_t offset) {
    return innodb::Record::parse(data, offset);
}

double TupleParser::get_schema_confidence(
    const std::vector<RecoveredRecord>& records,
    const SchemaTemplate& template_) {

    auto result = match_schema(records, template_);
    return result.confidence;
}

SchemaTemplate::Column TupleParser::detect_column_pattern(
    const std::vector<std::string>& values) {
    return SchemaTemplateInference::infer_column(values);
}

// ==================== BinaryTupleParser Implementation ====================

BinaryTupleParser::ParseResult BinaryTupleParser::parse(
    const uint8_t* data, uint32_t data_size,
    const SchemaTemplate& schema) {

    ParseResult result;
    result.success = false;

    if (!data || data_size == 0) {
        result.error_message = "Null or empty data";
        return result;
    }

    uint32_t offset = 0;

    // Count variable-length and nullable columns
    uint32_t num_variable = 0;
    uint32_t num_nullable = 0;

    for (const auto& col : schema.columns) {
        if (col.is_variable_length || col.mysql_type == mt::VARCHAR ||
            col.mysql_type == mt::VAR_STRING ||
            col.mysql_type == mt::STRING) {  // STRING/CHAR (type 29) - variable for utf8mb4
            num_variable++;
        }
        if (col.is_nullable) {
            num_nullable++;
        }
    }

    // In COMPACT format, the record starts with:
    // 1. Variable-length column sizes (1 or 2 bytes each, in reverse column order)
    // 2. NULL bitmap ((num_nullable + 7) / 8 bytes)
    // 3. Record header (5 bytes: info_bits + next_record + heap_no)
    // Then field data follows

    // Read variable-length column sizes
    std::vector<uint32_t> var_lengths(num_variable);
    for (uint32_t i = num_variable; i > 0; --i) {
        const auto& col = schema.columns[i - 1];
        uint32_t max_len = col.max_length;

        if (max_len <= 255) {
            if (offset >= data_size) {
                result.error_message = "Unexpected end of data reading varlen";
                return result;
            }
            var_lengths[i - 1] = data[offset++];
        } else {
            if (offset + 1 >= data_size) {
                result.error_message = "Unexpected end of data reading varlen (2-byte)";
                return result;
            }
            var_lengths[i - 1] = innodb::read_le<uint16_t>(data, offset);
            offset += 2;
        }
    }

    // Read NULL bitmap
    uint32_t null_bitmap_size = (num_nullable + 7) / 8;
    if (offset + null_bitmap_size > data_size) {
        result.error_message = "Unexpected end of data reading NULL bitmap";
        return result;
    }

    result.null_bitmap.assign(data + offset, data + offset + null_bitmap_size);
    offset += null_bitmap_size;

    // Parse field values
    uint32_t var_idx = 0;
    uint32_t null_idx = 0;

    for (const auto& col : schema.columns) {
        // Check NULL bitmap
        if (col.is_nullable) {
            uint32_t byte_idx = null_idx / 8;
            uint32_t bit_idx = null_idx % 8;
            if (byte_idx < result.null_bitmap.size() &&
                (result.null_bitmap[byte_idx] & (1 << bit_idx))) {
                result.fields.push_back("NULL");
                result.field_lengths.push_back(0);
                null_idx++;
                continue;
            }
            null_idx++;
        }

        // Parse field value
        std::string value;
        uint32_t field_len = 0;

        if (col.is_variable_length || col.mysql_type == mt::VARCHAR ||
            col.mysql_type == mt::VAR_STRING ||
            col.mysql_type == mt::STRING) {  // STRING/CHAR (type 29) - variable for utf8mb4
            // Use the pre-read variable length
            if (var_idx < var_lengths.size()) {
                field_len = var_lengths[var_idx++];
            }
        }

        uint32_t old_offset = offset;
        offset = parse_field_value(data + offset, 0, col, value);
        // Since we pass data+offset with offset=0, the returned value is the new local offset
        // We need to handle this differently
        // Let's re-implement: parse directly with absolute offset

        // Reset and use absolute offset
        value.clear();
        offset = old_offset;
        uint32_t new_offset = parse_field_value(data, offset, col, value);
        offset = new_offset;

        result.fields.push_back(value);
        result.field_lengths.push_back(static_cast<uint32_t>(value.size()));
    }

    result.success = true;
    return result;
}

uint32_t BinaryTupleParser::parse_field_value(
    const uint8_t* data, uint32_t offset,
    const SchemaTemplate::Column& column,
    std::string& value) {

    switch (column.mysql_type) {
        case mt::TINY:
            value = decode_tinyint(data, offset, column.is_signed);
            break;

        case mt::SHORT:
            value = decode_smallint(data, offset, column.is_signed);
            break;

        case mt::INT24:
            value = decode_mediumint(data, offset, column.is_signed);
            break;

        case mt::LONG:
            value = decode_int(data, offset, column.is_signed);
            break;

        case mt::LONGLONG:
            value = decode_bigint(data, offset, column.is_signed);
            break;

        case mt::FLOAT:
            value = decode_float(data, offset);
            break;

        case mt::DOUBLE:
            value = decode_double(data, offset);
            break;

        case mt::VARCHAR:
        case mt::VAR_STRING:
            value = decode_varchar(data, offset, column.max_length);
            break;

        case mt::TINY_BLOB:
        case mt::MEDIUM_BLOB:
        case mt::LONG_BLOB:
        case mt::BLOB:
            value = decode_blob(data, offset);
            break;

        case mt::DATE:
            value = decode_date(data, offset);
            break;

        case mt::TIME:
            value = decode_time(data, offset, column.datetime_precision);
            break;

        case mt::DATETIME:
        case mt::DATETIME2:
            value = decode_datetime(data, offset, column.datetime_precision);
            break;

        case mt::TIMESTAMP2:
            value = decode_timestamp(data, offset, column.datetime_precision);
            break;

        case mt::YEAR:
            value = decode_year(data, offset);
            break;

        case mt::BIT:
            value = decode_bit(data, offset, column.max_length);
            break;

        case mt::JSON:
            // JSON is stored as BLOB internally
            value = decode_blob(data, offset);
            break;

        default:
            // Unknown type - try to read as raw bytes
            if (column.max_length > 0 && column.max_length <= 255) {
                std::string raw;
                for (uint32_t i = 0; i < column.max_length && offset + i < static_cast<uint32_t>(-1); ++i) {
                    char buf[3];
                    snprintf(buf, sizeof(buf), "%02x", data[offset + i]);
                    raw += buf;
                }
                offset += column.max_length;
                value = raw;
            } else {
                value = "<UNKNOWN_TYPE>";
            }
            break;
    }

    return offset;
}

uint64_t BinaryTupleParser::read_varint(
    const uint8_t* data, uint32_t& offset) {
    uint64_t result = 0;
    uint32_t shift = 0;

    while (true) {
        uint8_t byte = data[offset++];
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
        if (shift >= 64) break;
    }

    return result;
}

std::string BinaryTupleParser::read_length_encoded_string(
    const uint8_t* data, uint32_t& offset) {
    // MySQL length-encoded string format:
    // If first byte < 251: length = first byte
    // If first byte == 0xFC: length = next 2 bytes
    // If first byte == 0xFD: length = next 3 bytes
    // If first byte == 0xFE: length = next 8 bytes

    uint8_t first = data[offset++];

    uint64_t length;
    switch (first) {
        case 0xFC:
            length = innodb::read_le<uint16_t>(data, offset);
            offset += 2;
            break;
        case 0xFD:
            length = static_cast<uint64_t>(data[offset]) |
                     (static_cast<uint64_t>(data[offset + 1]) << 8) |
                     (static_cast<uint64_t>(data[offset + 2]) << 16);
            offset += 3;
            break;
        case 0xFE:
            length = innodb::read_le<uint64_t>(data, offset);
            offset += 8;
            break;
        default:
            length = first;
            break;
    }

    std::string result(reinterpret_cast<const char*>(data + offset),
                       static_cast<size_t>(length));
    offset += static_cast<uint32_t>(length);
    return result;
}

// ==================== SchemaTemplateMatchEngine Implementation ====================

SchemaMatchResult SchemaTemplateMatchEngine::match(
    const std::vector<RecoveredRecord>& records,
    const SchemaTemplate& template_,
    const Config& config) {

    SchemaMatchResult result;
    result.matches = false;
    result.confidence = 0.0;

    if (records.empty() || template_.columns.empty()) {
        return result;
    }

    double total_confidence = 0.0;
    uint32_t matched_cols = 0;

    for (const auto& col : template_.columns) {
        SchemaMatchResult::ColumnMatch match;
        match.column_name = col.name;
        match.expected_type = col.type_name;

        // Check column presence and type compatibility
        bool found = false;
        for (const auto& rec : records) {
            auto it = rec.fields.find(col.name);
            if (it != rec.fields.end()) {
                found = true;
                match.matches = true;
                match.matched_type = col.type_name;
                match.type_confidence = calculate_pattern_confidence(it->second, col);
                break;
            }
        }

        if (found && match.type_confidence >= config.min_confidence) {
            matched_cols++;
            result.matched_columns.push_back(col.name);
        } else {
            match.matches = false;
            match.type_confidence = 0.0;
            result.unmatched_columns.push_back(col.name);
        }

        result.column_matches.push_back(match);
    }

    result.confidence = template_.columns.empty() ? 0.0
        : static_cast<double>(matched_cols) / template_.columns.size();
    result.matches = result.confidence >= config.min_confidence;

    return result;
}

std::optional<SchemaTemplate> SchemaTemplateMatchEngine::find_best_template(
    const std::vector<RecoveredRecord>& records,
    const std::vector<SchemaTemplate>& templates,
    const Config& config) {

    double best_score = 0.0;
    std::optional<SchemaTemplate> best;

    for (const auto& tmpl : templates) {
        double score = calculate_score(records, tmpl);
        if (score > best_score && score >= config.min_confidence) {
            best_score = score;
            best = tmpl;
        }
    }

    return best;
}

double SchemaTemplateMatchEngine::calculate_score(
    const std::vector<RecoveredRecord>& records,
    const SchemaTemplate& template_) {

    if (records.empty() || template_.columns.empty()) {
        return 0.0;
    }

    uint32_t matched = 0;
    for (const auto& col : template_.columns) {
        for (const auto& rec : records) {
            if (rec.fields.count(col.name)) {
                matched++;
                break;
            }
        }
    }

    return static_cast<double>(matched) / template_.columns.size();
}

bool SchemaTemplateMatchEngine::is_type_compatible(
    const std::string& actual_type,
    const std::string& expected_type) {

    if (actual_type == expected_type) return true;

    // Integer types are compatible with each other
    static const std::vector<std::string> int_types = {
        "TINYINT", "SMALLINT", "MEDIUMINT", "INT", "BIGINT"
    };

    bool actual_is_int = std::find(int_types.begin(), int_types.end(), actual_type) != int_types.end();
    bool expected_is_int = std::find(int_types.begin(), int_types.end(), expected_type) != int_types.end();

    if (actual_is_int && expected_is_int) return true;

    // String types are compatible with each other
    static const std::vector<std::string> str_types = {
        "VARCHAR", "CHAR", "TEXT", "TINYTEXT", "MEDIUMTEXT", "LONGTEXT"
    };

    bool actual_is_str = std::find(str_types.begin(), str_types.end(), actual_type) != str_types.end();
    bool expected_is_str = std::find(str_types.begin(), str_types.end(), expected_type) != str_types.end();

    if (actual_is_str && expected_is_str) return true;

    return false;
}

double SchemaTemplateMatchEngine::calculate_pattern_confidence(
    const std::string& value,
    const SchemaTemplate::Column& column) {

    if (value == "NULL" && column.is_nullable) {
        return 1.0;
    }

    if (value.empty()) {
        return 0.5;
    }

    // Try to validate the value against the expected type
    switch (column.mysql_type) {
        case mt::TINY:
        case mt::SHORT:
        case mt::LONG:
        case mt::LONGLONG:
        case mt::INT24: {
            // Check if value looks like an integer
            try {
                size_t pos;
                std::stoll(value, &pos);
                return (pos == value.size()) ? 1.0 : 0.3;
            } catch (...) {
                return 0.1;
            }
        }

        case mt::FLOAT:
        case mt::DOUBLE: {
            try {
                size_t pos;
                std::stod(value, &pos);
                return (pos == value.size()) ? 1.0 : 0.3;
            } catch (...) {
                return 0.1;
            }
        }

        case mt::VARCHAR:
        case mt::VAR_STRING:
            return 0.8;  // Strings generally match

        case mt::DATE:
        case mt::DATETIME:
        case mt::TIMESTAMP2:
        case mt::DATETIME2:
            // Check for date-like patterns
            if (value.find('-') != std::string::npos ||
                value.find(':') != std::string::npos) {
                return 0.9;
            }
            return 0.3;

        default:
            return 0.5;
    }
}

// ==================== SqlTupleGenerator Implementation ====================

SqlTupleGenerator::SqlTupleGenerator(const Config& config)
    : config_(config) {
}

std::string SqlTupleGenerator::generate(
    const std::vector<RecoveredRecord>& records,
    const SchemaTemplate& template_) {

    std::string result;

    if (config_.include_create_table) {
        result += generate_create_table(template_);
        result += config_.line_ending;
        result += config_.line_ending;
    }

    result += generate_inserts(records, template_);

    return result;
}

std::string SqlTupleGenerator::generate_create_table(
    const SchemaTemplate& template_) {

    std::ostringstream oss;

    oss << "CREATE TABLE ";
    if (!config_.table_prefix.empty()) {
        oss << config_.table_prefix;
    }
    oss << "`" << template_.table_name << "` (" << config_.line_ending;

    // Column definitions
    for (size_t i = 0; i < template_.columns.size(); ++i) {
        const auto& col = template_.columns[i];
        oss << "  `" << col.name << "` " << col.type_name;

        if (!col.is_nullable) {
            oss << " NOT NULL";
        } else {
            oss << " NULL";
        }

        if (col.is_auto_increment) {
            oss << " AUTO_INCREMENT";
        }

        if (!col.default_value.empty()) {
            oss << " DEFAULT '" << col.default_value << "'";
        }

        if (i + 1 < template_.columns.size() ||
            !template_.primary_key_columns.empty()) {
            oss << ",";
        }
        oss << config_.line_ending;
    }

    // Primary key
    if (!template_.primary_key_columns.empty()) {
        oss << "  PRIMARY KEY (";
        for (size_t i = 0; i < template_.primary_key_columns.size(); ++i) {
            if (i > 0) oss << ", ";
            if (template_.primary_key_columns[i] < template_.columns.size()) {
                oss << "`" << template_.columns[template_.primary_key_columns[i]].name << "`";
            }
        }
        oss << ")";
        oss << config_.line_ending;
    }

    oss << ") ENGINE=InnoDB";
    oss << ";" << config_.line_ending;

    return oss.str();
}

std::string SqlTupleGenerator::generate_inserts(
    const std::vector<RecoveredRecord>& records,
    const SchemaTemplate& template_) {

    std::string result;

    if (config_.batch_insert && records.size() > 1) {
        // Batch INSERT
        uint32_t batch_count = 0;

        for (size_t i = 0; i < records.size(); ++i) {
            if (batch_count == 0) {
                result += "INSERT INTO ";
                if (!config_.table_prefix.empty()) {
                    result += config_.table_prefix;
                }
                result += "`" + template_.table_name + "` (";

                for (size_t j = 0; j < template_.columns.size(); ++j) {
                    if (j > 0) result += ", ";
                    result += "`" + template_.columns[j].name + "`";
                }
                result += ") VALUES" + config_.line_ending;
            }

            result += "  (";
            for (size_t j = 0; j < template_.columns.size(); ++j) {
                const auto& col = template_.columns[j];
                if (j > 0) result += ", ";

                auto it = records[i].fields.find(col.name);
                if (it != records[i].fields.end()) {
                    result += TupleParser::to_sql_literal(it->second, col);
                } else {
                    result += "NULL";
                }
            }
            result += ")";

            batch_count++;
            if (batch_count >= config_.batch_size || i + 1 >= records.size()) {
                result += ";" + config_.line_ending;
                batch_count = 0;
            } else {
                result += "," + config_.line_ending;
            }
        }
    } else {
        // Individual INSERTs
        for (const auto& record : records) {
            result += generate_insert(record, template_);
        }
    }

    return result;
}

std::string SqlTupleGenerator::generate_insert(
    const RecoveredRecord& record,
    const SchemaTemplate& template_) {

    std::ostringstream oss;

    oss << "INSERT INTO ";
    if (!config_.table_prefix.empty()) {
        oss << config_.table_prefix;
    }
    oss << "`" << template_.table_name << "` (";

    for (size_t i = 0; i < template_.columns.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "`" << template_.columns[i].name << "`";
    }
    oss << ") VALUES (";

    for (size_t i = 0; i < template_.columns.size(); ++i) {
        const auto& col = template_.columns[i];
        if (i > 0) oss << ", ";

        auto it = record.fields.find(col.name);
        if (it != record.fields.end()) {
            oss << TupleParser::to_sql_literal(it->second, col);
        } else {
            oss << "NULL";
        }
    }
    oss << ");" << config_.line_ending;

    return oss.str();
}

// ==================== SchemaTemplateInference Implementation ====================

SchemaTemplateInference::Result SchemaTemplateInference::infer(
    const std::vector<RecoveredRecord>& records,
    const std::string& table_name) {

    Result result;
    result.confidence = 0.0;
    result.schema.table_name = table_name;
    result.schema.database_name = "recovered";

    if (records.empty()) {
        result.warnings.push_back("No records to infer schema from");
        return result;
    }

    // Collect all field names and their values
    std::map<std::string, std::vector<std::string>> column_values;

    for (const auto& record : records) {
        for (const auto& [name, value] : record.fields) {
            column_values[name].push_back(value);
        }
    }

    // Infer each column
    uint32_t ordinal = 0;
    for (auto& [name, values] : column_values) {
        auto col = infer_column(values);
        col.name = name.empty() ? infer_column_name(ordinal, values) : name;
        result.schema.columns.push_back(col);
        result.details.columns_inferred++;
        ordinal++;
    }

    // Calculate confidence
    if (result.details.columns_inferred > 0) {
        result.confidence = static_cast<double>(result.details.columns_matched) /
                           result.details.columns_inferred;
    }

    return result;
}

SchemaTemplateInference::Result SchemaTemplateInference::infer_from_csv(
    const std::vector<std::vector<std::string>>& csv_records,
    const std::string& table_name) {

    Result result;
    result.confidence = 0.0;
    result.schema.table_name = table_name;
    result.schema.database_name = "recovered";

    if (csv_records.empty()) {
        return result;
    }

    // First row might be headers
    uint32_t num_cols = static_cast<uint32_t>(csv_records[0].size());

    // Collect column values
    std::vector<std::vector<std::string>> column_values(num_cols);
    for (size_t row = 0; row < csv_records.size(); ++row) {
        for (size_t col = 0; col < csv_records[row].size() && col < num_cols; ++col) {
            column_values[col].push_back(csv_records[row][col]);
        }
    }

    // Infer each column
    for (uint32_t i = 0; i < num_cols; ++i) {
        auto col = infer_column(column_values[i]);
        col.name = infer_column_name(i, column_values[i]);
        result.schema.columns.push_back(col);
        result.details.columns_inferred++;
    }

    result.confidence = result.details.columns_inferred > 0 ? 0.7 : 0.0;
    return result;
}

SchemaTemplate::Column SchemaTemplateInference::infer_column(
    const std::vector<std::string>& values) {

    if (values.empty()) {
        SchemaTemplate::Column col;
        col.mysql_type = mt::VARCHAR;
        col.type_name = "VARCHAR";
        col.max_length = 255;
        col.is_nullable = true;
        col.is_signed = true;
        return col;
    }

    // Check if all values are NULL
    bool all_null = true;
    for (const auto& v : values) {
        if (v != "NULL" && !v.empty()) {
            all_null = false;
            break;
        }
    }
    if (all_null) {
        SchemaTemplate::Column col;
        col.mysql_type = mt::VARCHAR;
        col.type_name = "VARCHAR";
        col.max_length = 255;
        col.is_nullable = true;
        col.is_signed = true;
        return col;
    }

    // Try numeric inference
    auto numeric_col = infer_numeric_column(values);
    if (numeric_col.mysql_type != mt::VARCHAR) {
        return numeric_col;
    }

    // Try datetime inference
    for (const auto& v : values) {
        if (v == "NULL" || v.empty()) continue;
        auto dt_col = infer_datetime_column(v);
        if (dt_col.mysql_type != mt::VARCHAR) {
            return dt_col;
        }
    }

    // Default to string
    return infer_string_column(values);
}

std::string SchemaTemplateInference::infer_column_name(
    uint32_t ordinal, const std::vector<std::string>& values) {

    // Try to infer from email patterns
    if (!values.empty()) {
        for (const auto& v : values) {
            if (v.find('@') != std::string::npos) {
                return "email";
            }
        }

        // Try to detect ID patterns
        bool all_numeric = true;
        for (const auto& v : values) {
            if (v == "NULL" || v.empty()) continue;
            try {
                std::stoll(v);
            } catch (...) {
                all_numeric = false;
                break;
            }
        }
        if (all_numeric && ordinal == 0) {
            return "id";
        }
    }

    return "col_" + std::to_string(ordinal + 1);
}

SchemaTemplate::Column SchemaTemplateInference::infer_numeric_column(
    const std::vector<std::string>& values) {

    SchemaTemplate::Column col;
    col.mysql_type = mt::VARCHAR;
    col.type_name = "VARCHAR";
    col.is_nullable = false;
    col.is_signed = true;

    bool all_integer = true;
    bool all_numeric = true;
    int64_t min_val = INT64_MAX;
    int64_t max_val = INT64_MIN;
    bool has_negative = false;
    bool has_null = false;

    for (const auto& v : values) {
        if (v == "NULL" || v.empty()) {
            has_null = true;
            continue;
        }

        try {
            size_t pos;
            int64_t val = std::stoll(v, &pos);
            if (pos != v.size()) {
                all_integer = false;
                // Try floating point
                std::stod(v);
                all_numeric = true;
            } else {
                if (val < 0) has_negative = true;
                if (val < min_val) min_val = val;
                if (val > max_val) max_val = val;
            }
        } catch (...) {
            all_integer = false;
            all_numeric = false;
            break;
        }
    }

    if (!all_numeric) {
        return col;  // Return VARCHAR
    }

    col.is_nullable = has_null;

    if (all_integer) {
        // Determine integer type based on range
        if (!has_negative && max_val <= 255) {
            col.mysql_type = mt::TINY;
            col.type_name = "TINYINT UNSIGNED";
            col.max_length = 1;
            col.is_signed = false;
        } else if (has_negative && min_val >= -128 && max_val <= 127) {
            col.mysql_type = mt::TINY;
            col.type_name = "TINYINT";
            col.max_length = 1;
        } else if (!has_negative && max_val <= 65535) {
            col.mysql_type = mt::SHORT;
            col.type_name = "SMALLINT UNSIGNED";
            col.max_length = 2;
            col.is_signed = false;
        } else if (has_negative && min_val >= -32768 && max_val <= 32767) {
            col.mysql_type = mt::SHORT;
            col.type_name = "SMALLINT";
            col.max_length = 2;
        } else if (!has_negative && max_val <= 16777215) {
            col.mysql_type = mt::INT24;
            col.type_name = "MEDIUMINT UNSIGNED";
            col.max_length = 3;
            col.is_signed = false;
        } else if (has_negative && min_val >= -8388608 && max_val <= 8388607) {
            col.mysql_type = mt::INT24;
            col.type_name = "MEDIUMINT";
            col.max_length = 3;
        } else if (!has_negative && max_val <= 4294967295LL) {
            col.mysql_type = mt::LONG;
            col.type_name = "INT UNSIGNED";
            col.max_length = 4;
            col.is_signed = false;
        } else if (has_negative && min_val >= -2147483648LL && max_val <= 2147483647LL) {
            col.mysql_type = mt::LONG;
            col.type_name = "INT";
            col.max_length = 4;
        } else {
            col.mysql_type = mt::LONGLONG;
            col.type_name = "BIGINT";
            col.max_length = 8;
        }
    } else {
        // Floating point
        col.mysql_type = mt::DOUBLE;
        col.type_name = "DOUBLE";
        col.max_length = 8;
    }

    return col;
}

SchemaTemplate::Column SchemaTemplateInference::infer_string_column(
    const std::vector<std::string>& values) {

    SchemaTemplate::Column col;
    col.mysql_type = mt::VARCHAR;
    col.type_name = "VARCHAR";
    col.is_nullable = false;
    col.is_signed = true;

    uint32_t max_len = 0;
    bool has_null = false;

    for (const auto& v : values) {
        if (v == "NULL" || v.empty()) {
            has_null = true;
            continue;
        }
        if (v.size() > max_len) {
            max_len = static_cast<uint32_t>(v.size());
        }
    }

    col.is_nullable = has_null;
    col.max_length = max_len > 0 ? max_len : 255;

    if (col.max_length > 65535) {
        col.mysql_type = mt::LONG_BLOB;
        col.type_name = "LONGTEXT";
    } else if (col.max_length > 255) {
        col.mysql_type = mt::VAR_STRING;
        col.type_name = "VARCHAR";
    } else {
        col.type_name = "VARCHAR(" + std::to_string(col.max_length) + ")";
    }

    return col;
}

SchemaTemplate::Column SchemaTemplateInference::infer_datetime_column(
    const std::string& value) {

    SchemaTemplate::Column col;
    col.mysql_type = mt::VARCHAR;
    col.type_name = "VARCHAR";
    col.is_nullable = true;
    col.is_signed = true;

    if (value.empty() || value == "NULL") {
        return col;
    }

    // Check for date patterns: YYYY-MM-DD
    if (value.size() == 10 && value[4] == '-' && value[7] == '-') {
        col.mysql_type = mt::DATE;
        col.type_name = "DATE";
        col.max_length = 3;
        return col;
    }

    // Check for datetime patterns: YYYY-MM-DD HH:MM:SS
    if (value.size() >= 19 && value[4] == '-' && value[7] == '-' &&
        value[10] == ' ' && value[13] == ':' && value[16] == ':') {
        col.mysql_type = mt::DATETIME;
        col.type_name = "DATETIME";
        col.max_length = 8;
        return col;
    }

    // Check for timestamp (Unix epoch)
    try {
        int64_t val = std::stoll(value);
        if (val > 1000000000 && val < 2000000000) {
            col.mysql_type = mt::LONG;
            col.type_name = "TIMESTAMP";
            col.max_length = 4;
            return col;
        }
    } catch (...) {
    }

    return col;
}

// ==================== SchemaTemplateValidator Implementation ====================

SchemaTemplateValidator::ValidationResult SchemaTemplateValidator::validate(
    const std::vector<RecoveredRecord>& records,
    const SchemaTemplate& template_) {

    ValidationResult result;
    result.is_valid = true;
    result.total_records = static_cast<uint32_t>(records.size());
    result.valid_records = 0;
    result.invalid_records = 0;
    result.partial_records = 0;

    for (const auto& record : records) {
        if (validate_record(record, template_)) {
            result.valid_records++;
        } else {
            result.invalid_records++;
            result.is_valid = false;
        }
    }

    return result;
}

bool SchemaTemplateValidator::validate_record(
    const RecoveredRecord& record,
    const SchemaTemplate& template_) {

    // Check that all required (non-nullable) columns are present
    for (const auto& col : template_.columns) {
        if (!col.is_nullable) {
            auto it = record.fields.find(col.name);
            if (it == record.fields.end() || it->second == "NULL" || it->second.empty()) {
                return false;
            }
        }
    }

    return true;
}

bool SchemaTemplateValidator::validate_field(
    const std::string& value,
    const SchemaTemplate::Column& column) {

    if (value == "NULL") {
        return column.is_nullable;
    }

    if (value.empty()) {
        return column.is_nullable;
    }

    switch (column.mysql_type) {
        case mt::TINY:
        case mt::SHORT:
        case mt::LONG:
        case mt::LONGLONG:
        case mt::INT24:
            return validate_integer(value, column);

        case mt::DATE:
        case mt::DATETIME:
        case mt::DATETIME2:
        case mt::TIMESTAMP2:
        case mt::TIME2:
            return validate_datetime(value, column);

        case mt::VARCHAR:
        case mt::VAR_STRING:
            return validate_string(value, column);

        default:
            return true;
    }
}

bool SchemaTemplateValidator::validate_integer(
    const std::string& value, const SchemaTemplate::Column&) {
    try {
        std::stoll(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool SchemaTemplateValidator::validate_datetime(
    const std::string& value, const SchemaTemplate::Column&) {
    // Basic format check
    if (value.find('-') != std::string::npos) {
        return true;
    }
    if (value == "0000-00-00 00:00:00" || value == "0000-00-00") {
        return true;
    }
    return false;
}

bool SchemaTemplateValidator::validate_string(
    const std::string& value, const SchemaTemplate::Column& column) {
    // Check length constraint
    if (column.max_length > 0 && value.size() > column.max_length) {
        return false;
    }
    return true;
}

bool SchemaTemplateValidator::validate_email(
    const std::string& value, const SchemaTemplate::Column&) {
    return value.find('@') != std::string::npos;
}

// ==================== SchemaSdiRecovery Implementation ====================

SchemaSdiRecovery::Result SchemaSdiRecovery::recover_from_sdi(
    const std::vector<uint8_t>& sdi_data) {

    Result result;
    result.found = false;

    if (sdi_data.size() < 10) {
        result.error_message = "SDI data too small";
        return result;
    }

    // Try to extract JSON from SDI payload
    // SDI pages have a specific format in MySQL 8.0+
    auto json_str = extract_sdi_json(sdi_data.data(), 0,
                                      static_cast<uint32_t>(sdi_data.size()));
    if (!json_str.has_value()) {
        result.error_message = "Failed to extract SDI JSON";
        return result;
    }

    // Parse the JSON to extract schema
    auto col = recover_from_sdi_json(json_str.value());
    result.schema.table_name = "recovered_from_sdi";
    result.schema.columns.push_back(col);
    result.found = true;

    return result;
}

SchemaTemplate::Column SchemaSdiRecovery::recover_from_sdi_json(
    const std::string& sdi_json) {

    SchemaTemplate::Column col;
    col.name = "unknown";
    col.mysql_type = mt::VARCHAR;
    col.type_name = "VARCHAR";
    col.is_nullable = true;

    // Basic JSON parsing for SDI format
    // In a full implementation, we would use a JSON parser
    // For now, extract basic column info

    return col;
}

std::optional<std::string> SchemaSdiRecovery::extract_sdi_json(
    const uint8_t* data, uint32_t payload_offset, uint32_t payload_size) {

    if (!data || payload_size == 0) {
        return std::nullopt;
    }

    // Look for JSON start marker
    for (uint32_t i = payload_offset; i < payload_size; ++i) {
        if (data[i] == '{') {
            // Found potential JSON start
            // Find matching end
            uint32_t depth = 0;
            for (uint32_t j = i; j < payload_size; ++j) {
                if (data[j] == '{') depth++;
                if (data[j] == '}') {
                    depth--;
                    if (depth == 0) {
                        return std::string(reinterpret_cast<const char*>(data + i),
                                          j - i + 1);
                    }
                }
            }
        }
    }

    return std::nullopt;
}

// ==================== SchemaFrmRecovery Implementation ====================

SchemaFrmRecovery::Result SchemaFrmRecovery::recover(const std::string& frm_path) {
    Result result;
    result.found = false;

    // Read .frm file
    int fd = ::open(frm_path.c_str(), O_RDONLY);
    if (fd < 0) {
        result.error_message = "Failed to open .frm file";
        return result;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        ::close(fd);
        result.error_message = "Failed to stat .frm file";
        return result;
    }

    std::vector<uint8_t> data(st.st_size);
    ssize_t bytes_read = ::read(fd, data.data(), data.size());
    ::close(fd);

    if (bytes_read < 0 || static_cast<size_t>(bytes_read) < 64) {
        result.error_message = "Failed to read .frm file or file too small";
        return result;
    }

    // Parse header
    FrmHeader header;
    if (!parse_header(data.data(), static_cast<uint32_t>(data.size()), header)) {
        result.error_message = "Failed to parse .frm header";
        return result;
    }

    // Check for InnoDB table type
    if (header.db_type != 3) {
        result.error_message = "Not an InnoDB table";
        return result;
    }

    // Parse InnoDB extra info
    if (header.extra_size > 0 && header.extra_size < data.size()) {
        if (!parse_innodb_info(data.data(), header.extra_size, result.schema)) {
            result.error_message = "Failed to parse InnoDB info";
            return result;
        }
    }

    result.found = true;
    return result;
}

bool SchemaFrmRecovery::parse_header(const uint8_t* data, uint32_t size,
                                       FrmHeader& header) {
    if (!data || size < 64) {
        return false;
    }

    // .frm file header format (first 64 bytes)
    header.type = innodb::read_le<uint16_t>(data, 0);
    header.version = data[2];
    header.db_type = data[3];
    header.length = innodb::read_le<uint32_t>(data, 4);
    header.record_length = innodb::read_le<uint16_t>(data, 8);
    header.key_info_length = innodb::read_le<uint16_t>(data, 10);
    header.mysql_version = innodb::read_le<uint16_t>(data, 12);
    header.extra_size = innodb::read_le<uint32_t>(data, 14);

    return true;
}

bool SchemaFrmRecovery::parse_innodb_info(const uint8_t* data, uint32_t extra_size,
                                             SchemaTemplate& schema) {
    // Simplified InnoDB extra info parsing
    // In a full implementation, we would parse the column definitions
    // from the .frm file's extra section

    // For now, just set basic info
    // Schema template doesn't have engine field in engine namespace
    // The catalog::TableSchema has engine field
    (void)schema;

    return true;
}

} // namespace engine
} // namespace dbrecovery
