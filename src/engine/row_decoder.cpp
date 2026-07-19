#include "dbrecovery/engine/row_decoder.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <cstring>
#include <stdexcept>
#include <cmath>
#include <limits>
#include <sstream>
#include <iomanip>

using namespace dbrecovery::innodb::mysql_types;

namespace dbrecovery {
namespace engine {

// REDUNDANT format: field data starts after header + offset array
// REDUNDANT has no NULL bitmap - NULL encoded as adjacent offsets being equal
// REDUNDANT has no variable-length prefixes in data - all offsets from schema

static std::vector<FieldValue> decode_redundant_record(
    const uint8_t* page_base, size_t size, size_t origin,
    const catalog::TableSchema& schema) {

    std::vector<FieldValue> fields;
    fields.reserve(schema.columns.size());

    if (!page_base || size == 0 || origin + 6 > size) return fields;

    const uint8_t* record_ptr = page_base + origin;

    auto header_opt = dbrecovery::innodb::RedundantHeader::parse(record_ptr, 6);
    if (!header_opt.has_value()) return fields;

    auto header = header_opt.value();
    if (header.is_deleted()) return fields;

    uint16_t n_fields = header.get_n_fields();
    if (n_fields == 0 || n_fields > 1023) return fields;

    bool is_1byte = header.is_1byte_offset_format();
    uint16_t extra_bytes = header.get_extra_bytes_size();

    if (origin + extra_bytes > size) return fields;

    const uint8_t* offsets_ptr = record_ptr + 6;

    // Build column list sorted by ordinal
    std::vector<const catalog::ColumnInfo*> cols_by_pos;
    for (uint32_t i = 1; i <= schema.columns.size(); i++) {
        auto it = schema.columns.find(i);
        if (it != schema.columns.end()) {
            cols_by_pos.push_back(&it->second);
        }
    }

    // Build field offset array
    std::vector<uint32_t> field_offsets;
    field_offsets.reserve(n_fields);
    for (uint16_t i = 0; i < n_fields; i++) {
        uint32_t off;
        if (is_1byte) {
            off = offsets_ptr[i];
        } else {
            off = (static_cast<uint32_t>(offsets_ptr[i * 2]) << 8) |
                  static_cast<uint32_t>(offsets_ptr[i * 2 + 1]);
        }
        field_offsets.push_back(off);
    }

    uint32_t data_start_offset = extra_bytes;

    for (size_t ci = 0; ci < cols_by_pos.size() && ci < n_fields; ci++) {
        const auto& col = *cols_by_pos[ci];
        FieldValue fv;

        uint32_t start_off = field_offsets[ci];
        // Check for NULL: adjacent offsets equal means field is NULL
        bool is_null = (ci + 1 < n_fields) ? (start_off == field_offsets[ci + 1]) : false;

        if (is_null) {
            fv.value = "NULL";
            fields.push_back(fv);
            continue;
        }

        uint32_t end_off;
        if (ci + 1 < n_fields) {
            end_off = field_offsets[ci + 1];
        } else {
            end_off = header.get_next_record_offset();
        }

        if (end_off <= start_off || start_off < data_start_offset ||
            origin + end_off > size) {
            fv.value = "[TRUNCATED]";
            fields.push_back(fv);
            continue;
        }

        uint32_t field_size = end_off - start_off;
        const uint8_t* field_data = record_ptr + start_off;

        // Decode based on type
        if (col.mysql_type_id == VARCHAR || col.mysql_type_id == VAR_STRING) {
            fv.value = std::string(reinterpret_cast<const char*>(field_data), field_size);
        } else if (col.mysql_type_id == LONGLONG && field_size >= 8) {
            uint64_t val = dbrecovery::innodb::read_le<uint64_t>(field_data, 0);
            fv.value = std::to_string(val);
        } else if (col.mysql_type_id == LONG && field_size >= 4) {
            uint32_t val = dbrecovery::innodb::read_le<uint32_t>(field_data, 0);
            fv.value = std::to_string(val);
        } else if (col.mysql_type_id == INT24 && field_size >= 3) {
            uint32_t val = dbrecovery::innodb::read_le<uint32_t>(field_data, 0);
            if (val & 0x800000) val |= 0xFF000000;
            fv.value = std::to_string(val);
        } else if (col.mysql_type_id == SHORT && field_size >= 2) {
            uint16_t val = dbrecovery::innodb::read_le<uint16_t>(field_data, 0);
            fv.value = std::to_string(val);
        } else if (col.mysql_type_id == TINY && field_size >= 1) {
            fv.value = std::to_string(static_cast<int>(*field_data));
        } else if (col.mysql_type_id == FLOAT && field_size >= 4) {
            float val;
            std::memcpy(&val, field_data, sizeof(float));
            fv.value = std::to_string(val);
        } else if (col.mysql_type_id == DOUBLE && field_size >= 8) {
            double val;
            std::memcpy(&val, field_data, sizeof(double));
            fv.value = std::to_string(val);
        } else if (col.mysql_type_id == DATE && field_size >= 3) {
            uint16_t year = field_data[2] | (static_cast<uint16_t>(field_data[3]) << 8);
            fv.value = std::to_string(year) + "-" +
                       std::to_string(static_cast<int>(field_data[1])) + "-" +
                       std::to_string(static_cast<int>(field_data[0]));
        } else if ((col.mysql_type_id == DATETIME || col.mysql_type_id == TIMESTAMP2) && field_size >= 5) {
            uint8_t b0 = field_data[0], b1 = field_data[1], b2 = field_data[2];
            uint8_t b3 = field_data[3], b4 = field_data[4];
            uint16_t year = ((b0 & 0x7F) << 9) | ((b1 & 0xE0) << 3) | (b1 >> 5);
            uint8_t month = ((b1 & 0x1C) >> 2) | ((b2 & 0x80) >> 7);
            uint8_t day = (b2 & 0x7C) >> 2;
            uint8_t hour = ((b2 & 0x03) << 4) | ((b3 & 0xF0) >> 4);
            uint8_t minute = ((b3 & 0x0F) << 2) | ((b4 & 0xC0) >> 6);
            uint8_t second = (b4 & 0x3F);
            fv.value = std::to_string(year) + "-" + std::to_string(month) + "-" +
                       std::to_string(day) + " " + std::to_string(hour) + ":" +
                       std::to_string(minute) + ":" + std::to_string(second);
        } else if ((col.mysql_type_id == DATETIME2 || col.mysql_type_id == TIMESTAMP2) && field_size >= 5) {
            int precision = col.datetime_precision;
            if (precision < 0) precision = 6;
            int year = (field_data[1] << 8) | field_data[0];
            int month = field_data[2] >> 4;
            int day = field_data[2] & 0x0F;
            int hour = field_data[3] & 0x1F;
            int minute = field_data[3] >> 2;
            int second = field_data[4] >> 2;
            std::ostringstream oss;
            oss << year << "-" << month << "-" << day << " "
                << hour << ":" << minute << ":" << second;
            if (precision > 0 && field_size > 5) {
                size_t frac_bytes = field_size - 5;
                uint64_t frac_val = 0;
                for (size_t i = 0; i < frac_bytes; ++i)
                    frac_val |= static_cast<uint64_t>(field_data[5 + i]) << (i * 8);
                double frac_sec = static_cast<double>(frac_val) / std::pow(10, precision);
                oss << std::fixed << std::setprecision(precision) << frac_sec;
            }
            fv.value = oss.str();
        } else if (col.mysql_type_id == TIME || col.mysql_type_id == TIME2) {
            if (field_size >= 3) {
                uint32_t t = dbrecovery::innodb::read_le<uint32_t>(field_data, 0) & 0xFFFFFF;
                int sign = (t & 0x800000) ? -1 : 1;
                t &= 0x7FFFFF;
                int h = t / 3600, m = (t % 3600) / 60, s = t % 60;
                std::ostringstream oss;
                if (sign < 0) oss << "-";
                oss << h << ":" << (m < 10 ? "0" : "") << m << ":" << (s < 10 ? "0" : "") << s;
                if (field_size > 3) {
                    uint64_t frac = 0;
                    for (size_t i = 3; i < field_size; ++i)
                        frac |= static_cast<uint64_t>(field_data[i]) << ((i - 3) * 8);
                    oss << "." << frac;
                }
                fv.value = oss.str();
            } else { fv.value = "[TRUNCATED]"; }
        } else if (col.mysql_type_id == YEAR && field_size >= 1) {
            fv.value = std::to_string(1900 + *field_data);
        } else if (col.mysql_type_id == BIT) {
            uint32_t bit_count = col.column_length;
            uint32_t byte_size = (bit_count + 7) / 8;
            uint64_t val = 0;
            for (uint32_t i = 0; i < byte_size && i < field_size; i++)
                val |= static_cast<uint64_t>(field_data[i]) << (i * 8);
            fv.value = std::to_string(val);
        } else if (col.mysql_type_id >= TINY_BLOB && col.mysql_type_id <= BLOB) {
            if (field_size >= 20) {
                uint32_t blob_page_no = dbrecovery::innodb::read_le<uint32_t>(field_data, 4);
                uint32_t blob_offset = dbrecovery::innodb::read_le<uint32_t>(field_data, 8);
                uint64_t blob_len_flags = dbrecovery::innodb::read_le<uint64_t>(field_data, 12);
                uint32_t blob_len = static_cast<uint32_t>(blob_len_flags & 0x3FFFFFFF);
                std::ostringstream oss;
                oss << "[BLOB: page=" << blob_page_no
                    << " off=" << blob_offset << " len=" << blob_len << "]";
                fv.value = oss.str();
            } else {
                fv.value = "[BLOB_POINTER]";
            }
        } else if (col.mysql_type_id == JSON) {
            fv.value = "[JSON_DATA]";
        } else if (col.mysql_type_id == 30) { // GEOMETRY
            // REDUNDANT stores GEOMETRY as raw data with SRID prefix
            if (field_size > 4) {
                std::ostringstream oss;
                oss << "[GEOMETRY_" << field_size << "B]";
                fv.value = oss.str();
            } else {
                fv.value = "[GEOMETRY_EMPTY]";
            }
        } else {
            // Generic: treat as raw bytes
            fv.value = std::string(reinterpret_cast<const char*>(field_data),
                                   std::min(field_size, 256u));
        }

        fields.push_back(fv);
    }

    return fields;
}

std::vector<FieldValue> RowDecoder::decode_record(size_t origin, const catalog::TableSchema& schema) const {
    std::vector<FieldValue> fields;
    fields.reserve(schema.columns.size());

    if (!page_base_ || size_ == 0) return fields;

    constexpr size_t MAX_PAGE_SIZE = 65536;
    constexpr size_t MIN_RECORD_SIZE = 10;

    if (origin + MIN_RECORD_SIZE > size_ || origin + MIN_RECORD_SIZE > MAX_PAGE_SIZE)
        return fields;

    const uint8_t* record_ptr = page_base_ + origin;

    // REDUNDANT format path (row_format == 2)
    if (schema.row_format == 2) {
        return decode_redundant_record(page_base_, size_, origin, schema);
    }

    // ===== COMPACT/DYNAMIC format (5-byte header) =====
    if (origin + 5 > size_ || origin + 5 > MAX_PAGE_SIZE) return fields;

    uint8_t info_bits = record_ptr[0];
    if ((info_bits & 0x08) != 0) return fields;

    const uint8_t* null_ptr = record_ptr + 5;

    uint32_t nullable_count = schema.get_nullable_column_count();
    if (nullable_count == 0) {
        const_cast<uint32_t&>(null_bytes_) = 0;
    } else {
        const_cast<uint32_t&>(null_bytes_) = (nullable_count + 7) / 8;
    }

    // Iterate columns in physical_pos order for COMPACT/DYNAMIC
    for (const auto& pair : schema.columns) {
        const auto& col = pair.second;
        FieldValue fv;

        // NULL bitmap check
        if (col.is_nullable && null_bytes_ > 0) {
            uint32_t bitmap_index = 0;
            for (const auto& prev_pair : schema.columns) {
                const auto& prev_col = prev_pair.second;
                if (prev_pair.first >= pair.first) break;
                if (prev_col.is_nullable && !prev_col.is_off_page &&
                    prev_col.virtual_gen_store_type == 0) {
                    bitmap_index++;
                }
            }

            uint32_t byte_idx = bitmap_index / 8;
            uint32_t bit_idx = bitmap_index % 8;

            if (byte_idx >= null_bytes_) {
                return fields;
            }

            if (null_ptr && null_ptr + byte_idx < page_base_ + size_) {
                bool is_null = (null_ptr[byte_idx] & (1U << bit_idx)) != 0;
                if (is_null) {
                    fv.value = "NULL";
                    fields.push_back(fv);
                    continue;
                }
            } else {
                return fields;
            }
        }

        // Calculate data pointer by skipping previous fields
        const uint8_t* data_ptr = null_ptr + null_bytes_;
        for (const auto& prev_pair : schema.columns) {
            if (prev_pair.first >= pair.first) break;
            const auto& prev_col = prev_pair.second;

            if (prev_col.mysql_type_id == VARCHAR || prev_col.mysql_type_id == VAR_STRING) {
                bool use_two = (prev_col.column_length > 255);
                if (data_ptr + (use_two ? 2 : 1) > page_base_ + size_) break;
                uint32_t len = use_two ? dbrecovery::innodb::read_le<uint16_t>(data_ptr, 0) : *data_ptr;
                data_ptr += (use_two ? 2 : 1) + len;
            } else if (prev_col.mysql_type_id == JSON || prev_col.mysql_type_id == 30) { // JSON, GEOMETRY
                if (data_ptr + 2 > page_base_ + size_) break;
                uint16_t len = dbrecovery::innodb::read_le<uint16_t>(data_ptr, 0);
                data_ptr += 2 + len;
            } else if (prev_col.mysql_type_id == LONGLONG) {
                if (data_ptr + 8 <= page_base_ + size_) data_ptr += 8; else break;
            } else if (prev_col.mysql_type_id == LONG) {
                if (data_ptr + 4 <= page_base_ + size_) data_ptr += 4; else break;
            } else if (prev_col.mysql_type_id == INT24) {
                if (data_ptr + 3 <= page_base_ + size_) data_ptr += 3; else break;
            } else if (prev_col.mysql_type_id == SHORT) {
                if (data_ptr + 2 <= page_base_ + size_) data_ptr += 2; else break;
            } else if (prev_col.mysql_type_id == TINY) {
                if (data_ptr + 1 <= page_base_ + size_) data_ptr += 1; else break;
            } else if (prev_col.mysql_type_id == DATE) {
                if (data_ptr + 3 <= page_base_ + size_) data_ptr += 3; else break;
            } else if (prev_col.mysql_type_id == DATETIME || prev_col.mysql_type_id == TIMESTAMP2) {
                if (data_ptr + 5 <= page_base_ + size_) data_ptr += 5; else break;
            } else if (prev_col.mysql_type_id == DATETIME2 || prev_col.mysql_type_id == TIMESTAMP2) {
                int precision = prev_col.datetime_precision;
                if (precision < 0) precision = 6;
                uint32_t sz = 5 + (precision <= 2 ? 1 : precision <= 4 ? 2 : 3);
                if (data_ptr + sz <= page_base_ + size_) data_ptr += sz; else break;
            } else if (prev_col.mysql_type_id == TIME || prev_col.mysql_type_id == TIME2) {
                if (data_ptr + 3 <= page_base_ + size_) data_ptr += 3; else break;
            } else if (prev_col.mysql_type_id == YEAR) {
                if (data_ptr + 1 <= page_base_ + size_) data_ptr += 1; else break;
            } else if (prev_col.mysql_type_id == BIT) {
                uint32_t byte_size = (prev_col.column_length + 7) / 8;
                if (data_ptr + byte_size <= page_base_ + size_) data_ptr += byte_size; else break;
            } else if (prev_col.mysql_type_id == FLOAT) {
                if (data_ptr + 4 <= page_base_ + size_) data_ptr += 4; else break;
            } else if (prev_col.mysql_type_id == DOUBLE) {
                if (data_ptr + 8 <= page_base_ + size_) data_ptr += 8; else break;
            } else if (prev_col.mysql_type_id >= TINY_BLOB && prev_col.mysql_type_id <= BLOB) {
                if (data_ptr + 20 <= page_base_ + size_) data_ptr += 20; else break;
            } else {
                if (data_ptr + 1 <= page_base_ + size_) data_ptr += 1; else break;
            }
        }

        // Decode current field
        if (col.mysql_type_id == VARCHAR || col.mysql_type_id == VAR_STRING) {
            bool use_two = (col.column_length > 255);
            if (data_ptr + (use_two ? 2 : 1) > page_base_ + size_) {
                fv.value = "[TRUNCATED]";
            } else {
                uint32_t len = use_two ? dbrecovery::innodb::read_le<uint16_t>(data_ptr, 0) : *data_ptr;
                data_ptr += (use_two ? 2 : 1);
                if (len > 0 && data_ptr + len <= page_base_ + size_)
                    fv.value = std::string(reinterpret_cast<const char*>(data_ptr), len);
                else
                    fv.value = "[TRUNCATED]";
            }
        } else if (col.mysql_type_id == JSON) {
            if (data_ptr + 2 > page_base_ + size_) {
                fv.value = "[TRUNCATED_JSON]";
            } else {
                uint16_t len = dbrecovery::innodb::read_le<uint16_t>(data_ptr, 0);
                data_ptr += 2;
                if (len > 0 && data_ptr + len <= page_base_ + size_) {
                    // Basic JSON detection: try to interpret binary header
                    if (len >= 1) {
                        uint8_t json_type = *data_ptr;
                        std::ostringstream oss;
                        if (json_type == 0x00) oss << "[JSON_SMALL_OBJECT " << len << "B]";
                        else if (json_type == 0x01) oss << "[JSON_LARGE_OBJECT " << len << "B]";
                        else if (json_type == 0x02) oss << "[JSON_SMALL_ARRAY " << len << "B]";
                        else if (json_type == 0x03) oss << "[JSON_LARGE_ARRAY " << len << "B]";
                        else if (json_type == 0x04) oss << "[JSON_SCALAR " << len << "B]";
                        else oss << "[JSON_BINARY " << len << "B]";
                        fv.value = oss.str();
                    } else {
                        fv.value = "[JSON_EMPTY]";
                    }
                } else {
                    fv.value = "[TRUNCATED_JSON]";
                }
            }
        } else if (col.mysql_type_id == 30) { // GEOMETRY
            if (data_ptr + 2 > page_base_ + size_) {
                fv.value = "[TRUNCATED_GEOMETRY]";
            } else {
                uint16_t len = dbrecovery::innodb::read_le<uint16_t>(data_ptr, 0);
                data_ptr += 2;
                if (len >= 4 && data_ptr + len <= page_base_ + size_) {
                    // SRID is first 4 bytes, then WKB follows
                    uint32_t srid = dbrecovery::innodb::read_le<uint32_t>(data_ptr, 0);
                    std::ostringstream oss;
                    oss << "[GEOMETRY srid=" << srid << " size=" << len << "B]";
                    fv.value = oss.str();
                } else {
                    fv.value = "[TRUNCATED_GEOMETRY]";
                }
            }
        } else if (col.mysql_type_id == LONGLONG) {
            if (data_ptr + 8 <= page_base_ + size_)
                fv.value = std::to_string(dbrecovery::innodb::read_le<uint64_t>(data_ptr, 0));
            else fv.value = "[TRUNCATED]";
        } else if (col.mysql_type_id == LONG) {
            if (data_ptr + 4 <= page_base_ + size_)
                fv.value = std::to_string(dbrecovery::innodb::read_le<uint32_t>(data_ptr, 0));
            else fv.value = "[TRUNCATED]";
        } else if (col.mysql_type_id == INT24) {
            if (data_ptr + 3 <= page_base_ + size_) {
                uint32_t val = dbrecovery::innodb::read_le<uint32_t>(data_ptr, 0);
                if (val & 0x800000) val |= 0xFF000000;
                fv.value = std::to_string(val);
            } else fv.value = "[TRUNCATED]";
        } else if (col.mysql_type_id == SHORT) {
            if (data_ptr + 2 <= page_base_ + size_)
                fv.value = std::to_string(dbrecovery::innodb::read_le<uint16_t>(data_ptr, 0));
            else fv.value = "[TRUNCATED]";
        } else if (col.mysql_type_id == TINY) {
            if (data_ptr + 1 <= page_base_ + size_)
                fv.value = std::to_string(static_cast<int>(*data_ptr));
            else fv.value = "[TRUNCATED]";
        } else if (col.mysql_type_id == DATE) {
            if (data_ptr + 3 <= page_base_ + size_) {
                uint16_t year = data_ptr[2] | (static_cast<uint16_t>(data_ptr[3]) << 8);
                fv.value = std::to_string(year) + "-" +
                           std::to_string(static_cast<int>(data_ptr[1])) + "-" +
                           std::to_string(static_cast<int>(data_ptr[0]));
            } else fv.value = "[TRUNCATED]";
        } else if (col.mysql_type_id == DATETIME || col.mysql_type_id == TIMESTAMP2) {
            if (data_ptr + 5 <= page_base_ + size_) {
                uint8_t b0 = data_ptr[0], b1 = data_ptr[1], b2 = data_ptr[2];
                uint8_t b3 = data_ptr[3], b4 = data_ptr[4];
                uint16_t year = ((b0 & 0x7F) << 9) | ((b1 & 0xE0) << 3) | (b1 >> 5);
                uint8_t month = ((b1 & 0x1C) >> 2) | ((b2 & 0x80) >> 7);
                uint8_t day = (b2 & 0x7C) >> 2;
                uint8_t hour = ((b2 & 0x03) << 4) | ((b3 & 0xF0) >> 4);
                uint8_t minute = ((b3 & 0x0F) << 2) | ((b4 & 0xC0) >> 6);
                uint8_t second = (b4 & 0x3F);
                fv.value = std::to_string(year) + "-" + std::to_string(month) + "-" +
                           std::to_string(day) + " " + std::to_string(hour) + ":" +
                           std::to_string(minute) + ":" + std::to_string(second);
            } else fv.value = "[TRUNCATED]";
        } else if (col.mysql_type_id == DATETIME2 || col.mysql_type_id == TIMESTAMP2) {
            int precision = col.datetime_precision;
            if (precision < 0) precision = 6;
            uint32_t sz = 5 + (precision <= 2 ? 1 : precision <= 4 ? 2 : 3);
            if (data_ptr + sz > page_base_ + size_) {
                fv.value = "[TRUNCATED]";
            } else {
                int year = (data_ptr[1] << 8) | data_ptr[0];
                int month = data_ptr[2] >> 4;
                int day = data_ptr[2] & 0x0F;
                int hour = data_ptr[3] & 0x1F;
                int minute = data_ptr[3] >> 2;
                int second = data_ptr[4] >> 2;
                std::ostringstream oss;
                oss << year << "-" << month << "-" << day << " "
                    << hour << ":" << minute << ":" << second;
                if (precision > 0) {
                    size_t frac_bytes = sz - 5;
                    uint64_t frac_val = 0;
                    for (size_t i = 0; i < frac_bytes; ++i)
                        frac_val |= static_cast<uint64_t>(data_ptr[5 + i]) << (i * 8);
                    oss << std::fixed << std::setprecision(precision)
                        << (static_cast<double>(frac_val) / std::pow(10, precision));
                }
                fv.value = oss.str();
            }
        } else if (col.mysql_type_id >= TINY_BLOB && col.mysql_type_id <= BLOB) {
            if (data_ptr + 20 <= page_base_ + size_) {
                // Parse BTR_EXTERN field reference (20 bytes):
                // 0-3: space_id, 4-7: page_no, 8-11: offset, 12-19: length+flags
                uint32_t blob_space_id = dbrecovery::innodb::read_le<uint32_t>(data_ptr, 0);
                uint32_t blob_page_no = dbrecovery::innodb::read_le<uint32_t>(data_ptr, 4);
                uint32_t blob_offset = dbrecovery::innodb::read_le<uint32_t>(data_ptr, 8);
                uint64_t blob_len_flags = dbrecovery::innodb::read_le<uint64_t>(data_ptr, 12);
                uint32_t blob_len = static_cast<uint32_t>(blob_len_flags & 0x3FFFFFFF);
                bool is_owner = (blob_len_flags & 0x80000000) != 0;

                if (blob_page_no == dbrecovery::innodb::FIL_NULL || blob_len == 0) {
                    fv.value = "NULL";
                } else if (is_owner) {
                    std::ostringstream oss;
                    oss << "[BLOB: space=" << blob_space_id
                        << " page=" << blob_page_no
                        << " offset=" << blob_offset
                        << " len=" << blob_len
                        << (blob_page_no != 0xFFFFFFFF ? "" : " inline")
                        << "]";
                    fv.value = oss.str();
                } else {
                    // Non-owner: reference to parent's BLOB, output as inline
                    std::ostringstream oss;
                    oss << "[BLOB_REF: page=" << blob_page_no << " off=" << blob_offset << " len=" << blob_len << "]";
                    fv.value = oss.str();
                }
            } else fv.value = "[TRUNCATED]";
        } else if (col.mysql_type_id == FLOAT) {
            if (data_ptr + 4 <= page_base_ + size_) {
                float val;
                std::memcpy(&val, data_ptr, sizeof(float));
                fv.value = std::to_string(val);
            } else fv.value = "[TRUNCATED]";
        } else if (col.mysql_type_id == DOUBLE) {
            if (data_ptr + 8 <= page_base_ + size_) {
                double val;
                std::memcpy(&val, data_ptr, sizeof(double));
                fv.value = std::to_string(val);
            } else fv.value = "[TRUNCATED]";
        } else {
            fv.value = "[UNIMPLEMENTED_TYPE_" + std::to_string(col.mysql_type_id) + "]";
        }

        fields.push_back(fv);
    }

    return fields;
}

FieldValue RowDecoder::decode_field(const uint8_t* data, uint32_t type, bool is_nullable, size_t length) const {
    FieldValue fv;

    switch (type) {
        case LONG: {
            if (length >= 4)
                fv.value = std::to_string(dbrecovery::innodb::read_le<uint32_t>(data, 0));
            else fv.value = "[INVALID_LENGTH]";
            break;
        }
        case LONGLONG: {
            if (length >= 8)
                fv.value = std::to_string(dbrecovery::innodb::read_le<uint64_t>(data, 0));
            else fv.value = "[INVALID_LENGTH]";
            break;
        }
        case VARCHAR: {
            if (length > 0) {
                uint8_t len = *data;
                if (len > 0 && length >= len + 1)
                    fv.value = std::string(reinterpret_cast<const char*>(data + 1), len);
                else fv.value = "";
            } else fv.value = "[INVALID_LENGTH]";
            break;
        }
        case JSON: {
            if (length >= 2) {
                uint16_t len = dbrecovery::innodb::read_le<uint16_t>(data, 0);
                if (len > 0 && length >= len + 2) {
                    uint8_t json_type = data[2];
                    if (json_type == 0x00) fv.value = "[JSON_SMALL_OBJECT]";
                    else if (json_type == 0x01) fv.value = "[JSON_LARGE_OBJECT]";
                    else if (json_type == 0x02) fv.value = "[JSON_SMALL_ARRAY]";
                    else if (json_type == 0x03) fv.value = "[JSON_LARGE_ARRAY]";
                    else fv.value = "[JSON_BINARY]";
                } else fv.value = "";
            } else fv.value = "[INVALID_LENGTH]";
            break;
        }
        default:
            fv.value = "[UNIMPLEMENTED_TYPE_" + std::to_string(type) + "]";
            break;
    }

    return fv;
}

} // namespace engine
} // namespace dbrecovery