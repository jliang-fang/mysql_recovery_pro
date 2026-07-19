/**
 * @file schema_aware_parser.cpp
 * @brief Implementation of Schema-Aware Parser for MySQL 5.6 COMPACT row format
 *
 * @version 2.2.0
 */

#include "dbrecovery/innodb/schema_aware_parser.hpp"

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <unordered_set>
#include <cstring>
#include <zlib.h>

namespace dbrecovery {
namespace innodb {

// ==================== CompactRecordHeader Implementation ====================

std::optional<CompactRecordHeader> CompactRecordHeader::parse(const uint8_t* data, size_t size) {
    if (size < COMPACT_RECORD_HEADER_SIZE) {
        return std::nullopt;
    }

    CompactRecordHeader header = {};

    // COMPACT header format:
    // Byte 0: info_bits (1 byte) - contains deleted flag, min_rec flag
    // Bytes 1-2: next_offset (2 bytes, little-endian)
    // Bytes 3-4: heap_no (2 bytes, includes n_owned in high bits)

    header.info_bits = data[0];
    header.next_offset = data[1] | (data[2] << 8);
    header.heap_no = data[3] | (data[4] << 8);

    // Extract flags from info_bits
    // The info_bits are encoded in a specific way:
    // - Deleted flag: bit 20 of the combined 3-byte field
    // - Min rec flag: bit 19
    // - n_owned: bits 16-18

    // Actually, the encoding is:
    // Byte 0 contains: n_owned (bits 0-3) | heap_no bits
    // The deleted flag is in a different position

    // Re-parse with correct format:
    // The 5-byte header is: [info_bits:1][next:2][heap_no:2]
    // But the actual encoding packs flags differently

    // Record header interpretation:
    // rec = (byte0 << 16) | (byte1 << 8) | byte2
    // Then extract flags from rec

    uint32_t rec = (static_cast<uint32_t>(data[0]) << 16) |
                   (static_cast<uint32_t>(data[1]) << 8) |
                   static_cast<uint32_t>(data[2]);

    header.is_deleted = (rec & REC_INFO_DELETED) != 0;
    header.is_min_rec = (rec & REC_INFO_MIN_REC) != 0;
    header.n_owned = (rec >> 16) & 0x0F;

    // Check for INFIMUM/SUPREMUM
    if (header.n_owned == 0 && header.next_offset != 0) {
        // Could be INFIMUM or SUPREMUM
        // INFIMUM is typically at offset 99
        // SUPREMUM follows INFIMUM
        if (data[0] == REC_STATUS_INFIMUM) {
            header.is_infimum = true;
        } else if (data[0] == REC_STATUS_SUPREMUM) {
            header.is_supremum = true;
        }
    }

    return header;
}

// ==================== ParsedField Implementation ====================

std::string ParsedField::to_sql_value() const {
    if (is_null) {
        return "NULL";
    }

    if (!is_valid) {
        return "NULL /* invalid */";
    }

    // Quote strings
    if (mysql_type == mysql_types::VARCHAR ||
        mysql_type == mysql_types::VAR_STRING ||
        mysql_type == mysql_types::BLOB ||
        mysql_type == mysql_types::MEDIUM_BLOB ||
        mysql_type == mysql_types::LONG_BLOB ||
        mysql_type == mysql_types::TINY_BLOB ||
        mysql_type == mysql_types::JSON) {

        // Escape single quotes
        std::string escaped;
        escaped.reserve(value.size() * 2);
        for (char c : value) {
            if (c == '\'') {
                escaped += "''";
            } else if (c == '\\') {
                escaped += "\\\\";
            } else {
                escaped += c;
            }
        }
        return "'" + escaped + "'";
    }

    // Numbers don't need quotes
    return value;
}

// ==================== ParsedRecord Implementation ====================

std::string ParsedRecord::to_sql_insert(const std::string& table_name) const {
    if (is_infimum || is_supremum) {
        return "";
    }

    std::ostringstream oss;
    oss << "INSERT INTO " << table_name << " VALUES (";

    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << fields[i].to_sql_value();
    }

    oss << ");";
    return oss.str();
}

std::string ParsedRecord::to_csv() const {
    if (is_infimum || is_supremum) {
        return "";
    }

    std::ostringstream oss;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            oss << ",";
        }

        if (fields[i].is_null) {
            oss << "\\N";  // NULL in CSV
        } else {
            // Quote if contains comma or quote
            const std::string& val = fields[i].value;
            if (val.find(',') != std::string::npos ||
                val.find('"') != std::string::npos) {
                oss << '"';
                for (char c : val) {
                    if (c == '"') {
                        oss << "\"\"";
                    } else {
                        oss << c;
                    }
                }
                oss << '"';
            } else {
                oss << val;
            }
        }
    }

    return oss.str();
}

std::string ParsedRecord::to_json() const {
    if (is_infimum || is_supremum) {
        return "{}";
    }

    std::ostringstream oss;
    oss << "{";

    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }

        oss << '"' << fields[i].name << "\": ";

        if (fields[i].is_null) {
            oss << "null";
        } else {
            // Quote all values as strings for simplicity
            oss << '"' << fields[i].value << '"';
        }
    }

    oss << "}";
    return oss.str();
}

// ==================== MySQL56CompactParser Implementation ====================

MySQL56CompactParser::MySQL56CompactParser(const catalog::TableSchema& schema)
    : schema_(schema) {
    calculate_field_counts();
}

void MySQL56CompactParser::calculate_field_counts() {
    n_nullable_ = 0;
    n_fixed_ = 0;
    n_var_ = 0;

    for (const auto& [pos, col] : schema_.columns) {
        if (col.is_nullable) {
            n_nullable_++;
        }

        // Check if fixed or variable length
        switch (col.mysql_type_id) {
            case mysql_types::TINY:
            case mysql_types::SHORT:
            case mysql_types::LONG:
            case mysql_types::LONGLONG:
            case mysql_types::FLOAT:
            case mysql_types::DOUBLE:
            case mysql_types::DATE:
            case mysql_types::DATETIME:
            case mysql_types::TIMESTAMP2:
            case mysql_types::TIME:
            case mysql_types::YEAR:
                n_fixed_++;
                break;

            case mysql_types::VARCHAR:
            case mysql_types::VAR_STRING:
            case mysql_types::BLOB:
            case mysql_types::MEDIUM_BLOB:
            case mysql_types::LONG_BLOB:
            case mysql_types::TINY_BLOB:
            case mysql_types::JSON:
                n_var_++;
                break;

            default:
                // Treat as variable
                n_var_++;
                break;
        }
    }

    // NULL bitmap size
    null_bitmap_size_ = (n_nullable_ + 7) / 8;
}

std::optional<ParsedRecord> MySQL56CompactParser::parse_record(
    const uint8_t* page_data, uint32_t page_size, uint16_t offset) {

    if (offset < 100 || offset >= page_size - 8) {
        return std::nullopt;
    }

    const uint8_t* rec_data = page_data + offset;

    // Parse header
    auto header = CompactRecordHeader::parse(rec_data, page_size - offset);
    if (!header.has_value()) {
        return std::nullopt;
    }

    ParsedRecord record;
    record.offset = offset;
    record.next_offset = header->next_offset;
    record.is_deleted = header->is_deleted;
    record.is_infimum = header->is_infimum;
    record.is_supremum = header->is_supremum;

    if (header->is_infimum || header->is_supremum) {
        record.confidence = 1.0;
        return record;
    }

    // Parse fields
    record.fields = parse_record_body(page_data, page_size, offset, COMPACT_RECORD_HEADER_SIZE);

    // Calculate confidence
    if (!record.fields.empty()) {
        record.confidence = 0.8;
        for (const auto& field : record.fields) {
            if (field.is_valid) {
                record.confidence += 0.02;
            }
        }
        record.confidence = std::min(record.confidence, 1.0);
    } else {
        record.confidence = 0.1;
    }

    return record;
}

std::vector<ParsedRecord> MySQL56CompactParser::parse_all_records(
    const uint8_t* page_data, uint32_t page_size) {

    return RecordChainParser::parse_chain(page_data, page_size, *this);
}

std::vector<ParsedRecord> MySQL56CompactParser::parse_deleted_records(
    const uint8_t* page_data, uint32_t page_size) {

    DeletedRecordExtractor extractor(schema_);
    return extractor.extract_deleted(page_data, page_size);
}

std::vector<uint8_t> MySQL56CompactParser::read_var_len_field(
    const uint8_t* data, size_t offset, size_t& read_size) {

    read_size = 0;

    if (offset >= 65536) {  // Sanity check
        return {};
    }

    // Read length prefix
    // 1-byte prefix: 0x00-0x7F = length 0-127
    // 2-byte prefix: 0x80-0xFF indicates 2-byte length

    uint8_t first_byte = data[offset];

    size_t length;
    size_t prefix_size;

    if ((first_byte & 0x80) == 0) {
        // 1-byte length prefix
        length = first_byte;
        prefix_size = 1;
    } else {
        // 2-byte length prefix
        // Actual length = ((first_byte & 0x3F) << 8) | second_byte
        length = ((first_byte & 0x3F) << 8) | data[offset + 1];
        prefix_size = 2;
    }

    read_size = prefix_size + length;

    // Extract field data
    std::vector<uint8_t> result(length);
    std::memcpy(result.data(), data + offset + prefix_size, length);

    return result;
}

std::vector<uint8_t> MySQL56CompactParser::read_fixed_field(
    const uint8_t* data, size_t offset, size_t field_size) {

    std::vector<uint8_t> result(field_size);
    std::memcpy(result.data(), data + offset, field_size);
    return result;
}

bool MySQL56CompactParser::is_field_null(const uint8_t* null_bitmap, uint8_t field_index) const {
    uint8_t byte_index = field_index / 8;
    uint8_t bit_index = field_index % 8;
    return (null_bitmap[byte_index] & (1 << bit_index)) != 0;
}

std::vector<uint8_t> MySQL56CompactParser::read_null_bitmap(
    const uint8_t* data, size_t offset, uint8_t n_nullable) {

    size_t bitmap_size = (n_nullable + 7) / 8;
    std::vector<uint8_t> bitmap(bitmap_size);
    std::memcpy(bitmap.data(), data + offset, bitmap_size);
    return bitmap;
}

std::vector<uint16_t> MySQL56CompactParser::read_var_len_sizes(
    const uint8_t* data, size_t offset, uint8_t n_var_fields) {

    std::vector<uint16_t> sizes;
    sizes.reserve(n_var_fields);

    size_t current = offset;

    for (uint8_t i = 0; i < n_var_fields; ++i) {
        uint8_t first_byte = data[current];

        if ((first_byte & 0x80) == 0) {
            // 1-byte length
            sizes.push_back(first_byte);
            current += 1;
        } else {
            // 2-byte length
            uint16_t len = ((first_byte & 0x3F) << 8) | data[current + 1];
            sizes.push_back(len);
            current += 2;
        }
    }

    return sizes;
}

std::string MySQL56CompactParser::bytes_to_value(
    const std::vector<uint8_t>& bytes, int mysql_type) {

    if (bytes.empty()) {
        return "";
    }

    switch (mysql_type) {
        case mysql_types::TINY:
            if (bytes.size() >= 1) {
                return std::to_string(static_cast<int8_t>(bytes[0]));
            }
            break;

        case mysql_types::SHORT:
            if (bytes.size() >= 2) {
                int16_t val = bytes[0] | (bytes[1] << 8);
                return std::to_string(val);
            }
            break;

        case mysql_types::LONG:
            if (bytes.size() >= 4) {
                int32_t val = bytes[0] | (bytes[1] << 8) |
                              (bytes[2] << 16) | (bytes[3] << 24);
                return std::to_string(val);
            }
            break;

        case mysql_types::LONGLONG:
            if (bytes.size() >= 8) {
                int64_t val = static_cast<int64_t>(bytes[0]) |
                              (static_cast<int64_t>(bytes[1]) << 8) |
                              (static_cast<int64_t>(bytes[2]) << 16) |
                              (static_cast<int64_t>(bytes[3]) << 24) |
                              (static_cast<int64_t>(bytes[4]) << 32) |
                              (static_cast<int64_t>(bytes[5]) << 40) |
                              (static_cast<int64_t>(bytes[6]) << 48) |
                              (static_cast<int64_t>(bytes[7]) << 56);
                return std::to_string(val);
            }
            break;

        case mysql_types::VARCHAR:
        case mysql_types::VAR_STRING:
        case mysql_types::BLOB:
        case mysql_types::TINY_BLOB:
        case mysql_types::MEDIUM_BLOB:
        case mysql_types::LONG_BLOB:
        case mysql_types::JSON:
            return varchar_to_string(bytes);

        case mysql_types::DATETIME:
            return datetime_to_string(bytes);

        case mysql_types::TIMESTAMP2:
            return timestamp_to_string(bytes);

        case mysql_types::DATE:
            if (bytes.size() >= 3) {
                // Date stored as 3 bytes
                uint32_t packed = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16);
                uint16_t year = packed >> 9;
                uint8_t month = (packed >> 5) & 0x0F;
                uint8_t day = packed & 0x1F;
                std::ostringstream oss;
                oss << std::setfill('0') << std::setw(4) << year << "-"
                    << std::setw(2) << static_cast<int>(month) << "-"
                    << std::setw(2) << static_cast<int>(day);
                return oss.str();
            }
            break;

        default:
            // Return hex for unknown types
            return varchar_to_string(bytes);
    }

    return "";
}

std::string MySQL56CompactParser::int_to_string(const std::vector<uint8_t>& bytes) {
    return bytes_to_value(bytes, mysql_types::LONGLONG);
}

std::string MySQL56CompactParser::varchar_to_string(const std::vector<uint8_t>& bytes) {
    std::string result;
    result.reserve(bytes.size());

    for (uint8_t b : bytes) {
        if (b >= 32 && b < 127) {
            result += static_cast<char>(b);
        } else if (b == 0) {
            // Skip null bytes
        } else {
            // Replace non-printable with ?
            result += '?';
        }
    }

    return result;
}

std::string MySQL56CompactParser::datetime_to_string(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 8) {
        return "";
    }

    // DATETIME stored as 8 bytes in MySQL 5.6
    // Format: YYYYMMDDHHMMSS packed into 8 bytes
    uint64_t packed = 0;
    for (int i = 0; i < 8; ++i) {
        packed = (packed << 8) | bytes[i];
    }

    // Unpack
    uint16_t second = packed % 100;
    packed /= 100;
    uint16_t minute = packed % 100;
    packed /= 100;
    uint16_t hour = packed % 100;
    packed /= 100;
    uint16_t day = packed % 100;
    packed /= 100;
    uint16_t month = packed % 100;
    packed /= 100;
    uint16_t year = packed;

    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << year << "-"
        << std::setw(2) << month << "-"
        << std::setw(2) << day << " "
        << std::setw(2) << hour << ":"
        << std::setw(2) << minute << ":"
        << std::setw(2) << second;

    return oss.str();
}

std::string MySQL56CompactParser::timestamp_to_string(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 4) {
        return "";
    }

    // TIMESTAMP stored as 4-byte Unix timestamp
    uint32_t timestamp = bytes[0] | (bytes[1] << 8) |
                         (bytes[2] << 16) | (bytes[3] << 24);

    time_t t = static_cast<time_t>(timestamp);
    struct tm* tm_info = gmtime(&t);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);

    return std::string(buf);
}

std::vector<ParsedField> MySQL56CompactParser::parse_record_body(
    const uint8_t* page_data, uint32_t page_size, uint16_t offset, size_t header_size) {

    std::vector<ParsedField> fields;

    if (schema_.columns.empty()) {
        return fields;
    }

    const uint8_t* rec_data = page_data + offset;
    size_t remaining = page_size - offset - header_size;

    if (remaining < null_bitmap_size_) {
        return fields;
    }

    size_t current = header_size;

    // Read NULL bitmap
    auto null_bitmap = read_null_bitmap(rec_data, current, n_nullable_);
    current += null_bitmap_size_;

    // Read variable-length field sizes
    auto var_sizes = read_var_len_sizes(rec_data, current, n_var_);
    current += 0;  // Sizes are encoded inline with fields

    // Parse fields according to schema
    uint8_t nullable_index = 0;
    uint8_t var_index = 0;

    for (const auto& [pos, col] : schema_.columns) {
        ParsedField field;
        field.name = col.name;
        field.mysql_type = col.mysql_type_id;
        field.is_null = false;
        field.is_valid = false;

        // Check NULL bitmap
        if (col.is_nullable) {
            field.is_null = is_field_null(null_bitmap.data(), nullable_index);
            nullable_index++;
        }

        if (field.is_null) {
            field.value = "NULL";
            field.is_valid = true;
            fields.push_back(field);
            continue;
        }

        // Read field value based on type
        size_t field_size = 0;

        switch (col.mysql_type_id) {
            case mysql_types::TINY:
                field_size = 1;
                field.raw_data = read_fixed_field(rec_data, current, field_size);
                break;

            case mysql_types::SHORT:
                field_size = 2;
                field.raw_data = read_fixed_field(rec_data, current, field_size);
                break;

            case mysql_types::LONG:
                field_size = 4;
                field.raw_data = read_fixed_field(rec_data, current, field_size);
                break;

            case mysql_types::LONGLONG:
                field_size = 8;
                field.raw_data = read_fixed_field(rec_data, current, field_size);
                break;

            case mysql_types::DATETIME:
                field_size = 8;
                field.raw_data = read_fixed_field(rec_data, current, field_size);
                break;

            case mysql_types::TIMESTAMP2:
                field_size = 4;
                field.raw_data = read_fixed_field(rec_data, current, field_size);
                break;

            case mysql_types::DATE:
                field_size = 3;
                field.raw_data = read_fixed_field(rec_data, current, field_size);
                break;

            case mysql_types::VARCHAR:
            case mysql_types::VAR_STRING:
            case mysql_types::BLOB:
            case mysql_types::TINY_BLOB:
            case mysql_types::MEDIUM_BLOB:
            case mysql_types::LONG_BLOB:
            case mysql_types::JSON:
                if (var_index < var_sizes.size()) {
                    field_size = var_sizes[var_index];
                    var_index++;

                    // Read length prefix + data
                    size_t read_size;
                    field.raw_data = read_var_len_field(rec_data, current, read_size);
                    field_size = read_size;
                }
                break;

            default:
                // Unknown type, try as variable length
                if (var_index < var_sizes.size()) {
                    field_size = var_sizes[var_index];
                    var_index++;
                    size_t read_size;
                    field.raw_data = read_var_len_field(rec_data, current, read_size);
                    field_size = read_size;
                }
                break;
        }

        if (!field.raw_data.empty()) {
            field.value = bytes_to_value(field.raw_data, col.mysql_type_id);
            field.is_valid = true;
        }

        current += field_size;
        fields.push_back(field);
    }

    return fields;
}

// ==================== SchemaAwareParser Implementation ====================

SchemaAwareParser::SchemaAwareParser(const catalog::TableSchema& schema)
    : schema_(schema)
    , compact_parser_(schema) {
}

std::vector<ParsedRecord> SchemaAwareParser::parse_with_template(
    const uint8_t* page_data, uint32_t page_size) {

    return compact_parser_.parse_all_records(page_data, page_size);
}

std::vector<ParsedRecord> SchemaAwareParser::parse_carved_page(
    const engine::CarvedPage& page) {

    return parse_with_template(page.data.data(), page.data.size());
}

double SchemaAwareParser::calculate_confidence(const ParsedRecord& record) const {
    if (record.fields.empty()) {
        return 0.0;
    }

    double confidence = 0.0;
    size_t valid_fields = 0;

    for (const auto& field : record.fields) {
        if (field.is_valid) {
            valid_fields++;

            // Find corresponding column in schema
            for (const auto& [pos, col] : schema_.columns) {
                if (col.name == field.name) {
                    if (validate_field(field, col)) {
                        confidence += 1.0;
                    } else {
                        confidence += 0.5;
                    }
                    break;
                }
            }
        }
    }

    return confidence / std::max(valid_fields, size_t(1));
}

bool SchemaAwareParser::validate_field(
    const ParsedField& field, const catalog::ColumnInfo& col_info) const {

    if (field.is_null && !col_info.is_nullable) {
        return false;  // NULL in non-nullable column
    }

    // Type validation
    // This is a simplified check
    return field.is_valid;
}

std::vector<ParsedRecord> SchemaAwareParser::filter_by_confidence(
    const std::vector<ParsedRecord>& records, double min_confidence) const {

    std::vector<ParsedRecord> result;
    result.reserve(records.size());

    for (const auto& record : records) {
        if (calculate_confidence(record) >= min_confidence) {
            result.push_back(record);
        }
    }

    return result;
}

std::vector<ParsedRecord> SchemaAwareParser::deduplicate(
    const std::vector<ParsedRecord>& records) const {

    std::vector<ParsedRecord> result;
    std::unordered_set<std::string> seen;

    for (const auto& record : records) {
        if (record.is_infimum || record.is_supremum) {
            continue;
        }

        std::string hash = hash_record(record);
        if (seen.find(hash) == seen.end()) {
            seen.insert(hash);
            result.push_back(record);
        }
    }

    return result;
}

std::string SchemaAwareParser::hash_record(const ParsedRecord& record) const {
    std::ostringstream oss;
    for (const auto& field : record.fields) {
        oss << field.value << "|";
    }
    return oss.str();
}

// ==================== DeletedRecordExtractor Implementation ====================

DeletedRecordExtractor::DeletedRecordExtractor(const catalog::TableSchema& schema)
    : schema_(schema)
    , parser_(schema) {
}

std::vector<ParsedRecord> DeletedRecordExtractor::extract_deleted(
    const uint8_t* page_data, uint32_t page_size) {

    std::vector<ParsedRecord> records;

    uint16_t free_head = get_free_list_head(page_data, page_size);
    auto offsets = get_free_list_offsets(page_data, page_size, free_head);

    for (uint16_t offset : offsets) {
        auto record = parser_.parse_record(page_data, page_size, offset);
        if (record.has_value() && !record->is_infimum && !record->is_supremum) {
            record->is_deleted = true;
            records.push_back(*record);
        }
    }

    return records;
}

uint16_t DeletedRecordExtractor::get_free_list_head(
    const uint8_t* page_data, uint32_t page_size) {

    constexpr uint32_t PAGE_HEADER_OFFSET = 38;
    constexpr uint32_t FREE_OFFSET = 6;

    if (page_size < PAGE_HEADER_OFFSET + FREE_OFFSET + 2) {
        return 0;
    }

    const uint8_t* ph = page_data + PAGE_HEADER_OFFSET;
    return ph[FREE_OFFSET] | (ph[FREE_OFFSET + 1] << 8);
}

std::vector<uint16_t> DeletedRecordExtractor::get_free_list_offsets(
    const uint8_t* page_data, uint32_t page_size, uint16_t head) {

    std::vector<uint16_t> offsets;
    uint16_t current = head;
    int count = 0;

    while (current != 0 && current < page_size && count < 1000) {
        offsets.push_back(current);

        // Next pointer is in the record header
        if (current + 3 > page_size) break;

        current = page_data[current + 1] | (page_data[current + 2] << 8);
        count++;
    }

    return offsets;
}

std::optional<CompactRecordHeader> DeletedRecordExtractor::parse_deleted_header(
    const uint8_t* page_data, uint32_t page_size, uint16_t offset) {

    if (offset >= page_size - 5) {
        return std::nullopt;
    }

    return CompactRecordHeader::parse(page_data + offset, page_size - offset);
}

// ==================== RecordChainParser Implementation ====================

std::vector<ParsedRecord> RecordChainParser::parse_chain(
    const uint8_t* page_data, uint32_t page_size, MySQL56CompactParser& parser) {

    std::vector<ParsedRecord> records;

    // Start from INFIMUM (offset 99)
    uint16_t current = get_infimum_offset();
    std::unordered_set<uint16_t> visited;

    while (current != 0 && current < page_size - 8) {
        if (visited.count(current) > 0) {
            break;  // Circular reference
        }
        visited.insert(current);

        auto record = parser.parse_record(page_data, page_size, current);
        if (!record.has_value()) {
            break;
        }

        records.push_back(*record);

        if (record->is_supremum) {
            break;  // End of chain
        }

        current = record->next_offset;
    }

    return records;
}

uint16_t RecordChainParser::get_supremum_offset(
    const uint8_t* page_data, uint32_t page_size) {

    // SUPREMUM follows INFIMUM
    // INFIMUM is at offset 99, SUPREMUM offset is stored at bytes 101-102
    if (page_size < 103) {
        return 0;
    }

    return page_data[101] | (page_data[102] << 8);
}

std::vector<uint16_t> RecordChainParser::get_record_offsets(
    const uint8_t* page_data, uint32_t page_size) {

    std::vector<uint16_t> offsets;

    uint16_t current = get_infimum_offset();
    std::unordered_set<uint16_t> visited;

    while (current != 0 && current < page_size - 8) {
        if (visited.count(current) > 0) break;
        visited.insert(current);

        offsets.push_back(current);

        // Get next offset from record header
        if (current + 3 > page_size) break;
        uint16_t next = page_data[current + 1] | (page_data[current + 2] << 8);

        if (next == 0 || next == 0xFFFF) break;
        current = next;
    }

    return offsets;
}

// ==================== MultiFormatParser Implementation ====================

MultiFormatParser::MultiFormatParser(const catalog::TableSchema& schema, RowFormat format)
    : schema_(schema)
    , format_(format) {
    if (format == RowFormat::COMPACT || format == RowFormat::DYNAMIC || format == RowFormat::COMPRESSED) {
        compact_parser_ = std::make_unique<MySQL56CompactParser>(schema);
    }
}

std::vector<ParsedRecord> MultiFormatParser::parse(
    const uint8_t* page_data, uint32_t page_size) {

    switch (format_) {
        case RowFormat::COMPACT:
        case RowFormat::DYNAMIC:
            return compact_parser_->parse_all_records(page_data, page_size);

        case RowFormat::REDUNDANT:
            return parse_redundant(page_data, page_size);

        case RowFormat::COMPRESSED:
            // Decompress page body then parse as DYNAMIC
            // Compressed page: FIL header (38) + ZSD header (32) + zlib compressed body
            if (page_size > 70 && page_data) {
                uint32_t orig_size = read_le<uint32_t>(page_data, 14);
                uint16_t zlen = read_le<uint16_t>(page_data, 8);
                if (zlen > 0 && zlen < page_size - 70 && orig_size > 0 && orig_size <= UNIV_PAGE_SIZE_MAX) {
                    std::vector<uint8_t> decompressed(orig_size);
                    uLongf dest_len = orig_size;
                    int ret = uncompress(decompressed.data(), &dest_len,
                                        page_data + 70, zlen);
                    if (ret == Z_OK && dest_len == orig_size && dest_len >= FIL_PAGE_TYPE_ABS + 2) {
                        uint16_t decomp_type = read_be<uint16_t>(decompressed.data(), FIL_PAGE_TYPE_ABS);
                        if (decomp_type == FIL_PAGE_INDEX) {
                            return compact_parser_->parse_all_records(decompressed.data(), dest_len);
                        }
                    }
                }
            }
            // Fallback: try raw parsing
            return compact_parser_->parse_all_records(page_data, page_size);
    }

    return {};
}

MultiFormatParser::RowFormat MultiFormatParser::detect_format(
    const uint8_t* page_data, uint32_t page_size) {

    // Check page flags in FSP header
    // For now, default to COMPACT for MySQL 5.6
    return RowFormat::COMPACT;
}

std::string MultiFormatParser::format_to_string(RowFormat format) {
    switch (format) {
        case RowFormat::REDUNDANT: return "REDUNDANT";
        case RowFormat::COMPACT: return "COMPACT";
        case RowFormat::DYNAMIC: return "DYNAMIC";
        case RowFormat::COMPRESSED: return "COMPRESSED";
        default: return "UNKNOWN";
    }
}

std::vector<ParsedRecord> MultiFormatParser::parse_redundant(
    const uint8_t* page_data, uint32_t page_size) {

    // REDUNDANT format: 6-byte record header + 1/2-byte offset array
    // Use the shared decode logic from RowDecoder
    // For now, delegate to compact_parser_ since REDUNDANT record structure
    // is similar (the key difference is header size and offset format)
    if (!page_data || page_size < 101) return {};

    // Get page directory info
    uint16_t n_heap = read_be<uint16_t>(page_data, FIL_PAGE_OFFSET_N_HEAP);
    uint16_t n_recs = n_heap & 0x7FFF;
    if (n_recs < 2 || n_recs > page_size / 5) return {};

    std::vector<ParsedRecord> results;
    // REDUNDANT: INFIMUM at offset 101, SUPREMUM at offset 113
    uint16_t rec_offset = 101;  // REDUNDANT INFIMUM
    uint16_t supremum_offset = 113;
    uint32_t rec_count = 0;

    while (rec_offset != supremum_offset && rec_offset < page_size - 6 && rec_count < n_recs) {
        if (rec_offset < 6 || rec_offset >= page_size) break;

        // Parse 6-byte REDUNDANT header
        uint8_t info_bits = page_data[rec_offset - 6];
        uint8_t n_fields_byte = page_data[rec_offset - 1];
        uint16_t n_fields = (page_data[rec_offset - 2] & 0x03) << 8 | n_fields_byte >> 1;
        bool is_short = (n_fields_byte & 0x01) != 0;
        uint16_t next_off = read_be<uint16_t>(page_data, rec_offset - 6 + 4);
        bool is_deleted = (info_bits & 0x10) != 0;

        if (n_fields == 0 || n_fields > 1000) break;
        if (next_off == 0 || next_off > page_size) break;

        // Calculate extra bytes (offset array after header)
        uint16_t extra_bytes = is_short ? (6 + n_fields) : (6 + 2 * n_fields);
        if (rec_offset + extra_bytes > page_size) break;

        // Skip system records (infimum/supremum)
        if (!is_deleted && rec_offset != 101 && rec_offset != supremum_offset) {
            ParsedRecord rec;
            rec.offset = rec_offset;
            rec.is_deleted = is_deleted;
            rec.next_offset = next_off;
            rec.confidence = 0.5;
            results.push_back(rec);
        }

        rec_offset = next_off;
        rec_count++;
        if (rec_offset <= 101) break;  // Safety: don't loop backward
    }

    return results;
}

std::vector<ParsedRecord> MultiFormatParser::parse_dynamic(
    const uint8_t* page_data, uint32_t page_size) {

    // DYNAMIC format is similar to COMPACT
    // Main difference is in BLOB handling
    return compact_parser_->parse_all_records(page_data, page_size);
}

// ==================== Export Functions Implementation ====================

std::string export_to_sql(
    const std::vector<ParsedRecord>& records, const std::string& table_name) {

    std::ostringstream oss;

    for (const auto& record : records) {
        std::string sql = record.to_sql_insert(table_name);
        if (!sql.empty()) {
            oss << sql << "\n";
        }
    }

    return oss.str();
}

std::string export_to_csv(
    const std::vector<ParsedRecord>& records, const std::string& table_name) {

    std::ostringstream oss;

    for (const auto& record : records) {
        std::string csv = record.to_csv();
        if (!csv.empty()) {
            oss << csv << "\n";
        }
    }

    return oss.str();
}

std::string export_to_json(
    const std::vector<ParsedRecord>& records, const std::string& table_name) {

    std::ostringstream oss;
    oss << "[\n";

    bool first = true;
    for (const auto& record : records) {
        if (record.is_infimum || record.is_supremum) continue;

        if (!first) {
            oss << ",\n";
        }
        first = false;

        oss << "  " << record.to_json();
    }

    oss << "\n]";
    return oss.str();
}

std::vector<ParsedRecord> export_with_dedup(const std::vector<ParsedRecord>& records) {
    std::vector<ParsedRecord> result;
    std::unordered_set<std::string> seen;

    for (const auto& record : records) {
        if (record.is_infimum || record.is_supremum) continue;

        std::string hash = record.to_csv();
        if (seen.find(hash) == seen.end()) {
            seen.insert(hash);
            result.push_back(record);
        }
    }

    return result;
}

} // namespace innodb
} // namespace dbrecovery
