#include "dbrecovery/engine/field_decoder.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <cstring>
#include <stdexcept>

// ✅ P0-1 修复：引入 mysql_types 命名空间
using namespace dbrecovery::innodb::mysql_types;

namespace dbrecovery {
namespace engine {

FieldDecoder::FieldDecoder(const uint8_t* page_base, size_t page_size, size_t null_bytes)
    : page_base_(page_base),
      page_size_(page_size),
      null_bytes_(null_bytes) {
    // ✅ 新增：验证指针有效性
    if (page_base_ == nullptr && page_size > 0) {
        throw std::invalid_argument("Page base pointer is null but page_size > 0");
    }
}

const uint8_t* FieldDecoder::DecodeNextField(const uint8_t* rec_ptr, int field_index, const catalog::ColumnInfo& col_info) const {
    // 根据列类型计算字段长度
    size_t field_len = 0;

    switch (col_info.mysql_type_id) {
        case TINY:  // 1
            field_len = 1;
            break;
        case SHORT:  // 2
            field_len = 2;
            break;
        case LONG:  // 3
            field_len = 4;
            break;
        case LONGLONG:  // 8
            field_len = 8;
            break;
        case VARCHAR: { // 15
            // InnoDB VARCHAR 长度前缀是独立存储的：
            // - 如果列最大长度 <= 255 字节，使用 1 字节前缀
            // - 否则使用 2 字节前缀
            bool use_two_byte_prefix = (col_info.column_length > 255);

            if (use_two_byte_prefix) {
                if (rec_ptr + 2 > page_base_ + page_size_) {
                    std::cerr << "[ERROR] VARCHAR length prefix out of bounds" << std::endl;
                    return nullptr;
                }
                uint16_t len = dbrecovery::innodb::read_le<uint16_t>(rec_ptr, 0);

                // ✅ 新增：验证长度前缀的有效性
                if (len > col_info.column_length) {
                    std::cerr << "[ERROR] VARCHAR length " << len
                              << " exceeds column max length " << col_info.column_length << std::endl;
                    return nullptr;
                }

                field_len = 2 + len;

                // 边界检查
                if (rec_ptr + field_len > page_base_ + page_size_) {
                    std::cerr << "[ERROR] VARCHAR data out of bounds" << std::endl;
                    return nullptr;
                }
            } else {
                if (rec_ptr + 1 > page_base_ + page_size_) {
                    std::cerr << "[ERROR] VARCHAR length prefix out of bounds" << std::endl;
                    return nullptr;
                }
                uint8_t len = rec_ptr[0];

                // ✅ 新增：验证长度前缀的有效性
                if (len > col_info.column_length) {
                    std::cerr << "[ERROR] VARCHAR length " << (int)len
                              << " exceeds column max length " << col_info.column_length << std::endl;
                    return nullptr;
                }

                field_len = 1 + len;

                // 边界检查
                if (rec_ptr + field_len > page_base_ + page_size_) {
                    std::cerr << "[ERROR] VARCHAR data out of bounds" << std::endl;
                    return nullptr;
                }
            }
            break;
        }
        case VAR_STRING: {  // 253 - VARCHAR alias
            // VAR_STRING 是 VARCHAR 的别名，使用相同的处理逻辑
            bool use_two_byte_prefix = (col_info.column_length > 255);

            if (use_two_byte_prefix) {
                if (rec_ptr + 2 > page_base_ + page_size_) {
                    std::cerr << "[ERROR] VAR_STRING length prefix out of bounds" << std::endl;
                    return nullptr;
                }
                uint16_t len = dbrecovery::innodb::read_le<uint16_t>(rec_ptr, 0);

                field_len = 2 + len;

                if (rec_ptr + field_len > page_base_ + page_size_) {
                    std::cerr << "[ERROR] VAR_STRING data out of bounds" << std::endl;
                    return nullptr;
                }
            } else {
                if (rec_ptr + 1 > page_base_ + page_size_) {
                    std::cerr << "[ERROR] VAR_STRING length prefix out of bounds" << std::endl;
                    return nullptr;
                }
                uint8_t len = rec_ptr[0];

                field_len = 1 + len;

                if (rec_ptr + field_len > page_base_ + page_size_) {
                    std::cerr << "[ERROR] VAR_STRING data out of bounds" << std::endl;
                    return nullptr;
                }
            }
            break;
        }
        case 245: { // JSON
            // JSON uses little-endian 2-byte length prefix
            if (rec_ptr + 2 > page_base_ + page_size_) return nullptr;
            uint16_t json_len = dbrecovery::innodb::read_le<uint16_t>(rec_ptr, 0);
            field_len = 2 + json_len;
            if (rec_ptr + field_len > page_base_ + page_size_) return nullptr;
            break;
        }
        case 30: { // GEOMETRY (InnoDB type 30)
            // Same layout as JSON: 2-byte length prefix + data (SRID + WKB)
            if (rec_ptr + 2 > page_base_ + page_size_) return nullptr;
            uint16_t geom_len = dbrecovery::innodb::read_le<uint16_t>(rec_ptr, 0);
            field_len = 2 + geom_len;
            if (rec_ptr + field_len > page_base_ + page_size_) return nullptr;
            break;
        }
        case 247: // ENUM
        case 248: // SET
            // ENUM: 1 or 2 bytes depending on value count
            // SET: 1,2,3,4,8 bytes depending on member count
            field_len = (col_info.enum_values.size() <= 255) ? 1 : 2;
            if (col_info.mysql_type_id == 248) { // SET
                uint32_t set_count = col_info.set_values.size();
                field_len = (set_count <= 8) ? 1 :
                            (set_count <= 16) ? 2 :
                            (set_count <= 24) ? 3 :
                            (set_count <= 32) ? 4 : 8;
            }
            if (rec_ptr + field_len > page_base_ + page_size_) return nullptr;
            break;
        case DATETIME2:  // 18
        case TIMESTAMP2: { // 17
            // DATETIME2/TIMESTAMP2 处理（根据精度）
            int precision = col_info.datetime_precision;
            if (precision < 0) precision = 6; // 默认精度

            field_len = 5; // 基础部分总是5字节
            if (precision <= 2) {
                field_len += 1; // 小数部分1字节
            } else if (precision <= 4) {
                field_len += 2; // 小数部分2字节
            } else { // precision >= 5
                field_len += 3; // 小数部分3字节
            }

            // 边界检查
            if (rec_ptr + field_len > page_base_ + page_size_) {
                return nullptr;
            }
            break;
        }
        case TINY_BLOB:   // 249
        case MEDIUM_BLOB: // 250
        case LONG_BLOB:   // 251
        case BLOB:        // 252
            // BLOB/TEXT 类型处理：
            // - 如果 is_off_page=true，使用 BLOB 指针（20 字节）
            // - 否则直接读取数据（inline 存储）
            if (col_info.is_off_page) {
                field_len = 20; // BLOB 指针长度
            } else {
                // inline 存储：根据列定义中的长度信息判断
                if (col_info.column_length >= 0x10000) {
                    field_len = 9; // LONGBLOB: 8 字节数据 + 1 字节长度前缀
                } else if (col_info.column_length >= 0x100) {
                    field_len = 5; // BLOB: 4 字节数据 + 1 字节长度前缀
                } else {
                    field_len = col_info.column_length;

                    // 边界检查
                    if (rec_ptr + field_len > page_base_ + page_size_) {
                        return nullptr;
                    }
                }
            }
            break;
        case TIME:     // 12
        case TIME2:    // 20
            // TIME stored as 3 bytes (HH:MM:SS) or 3-6 bytes with microseconds
            field_len = 3;
            break;
        case YEAR:     // 14
            field_len = 1;
            break;
        case BIT: {    // 17
            // BIT(N) stored as (N+7)/8 bytes
            uint32_t bit_count = col_info.column_length;
            field_len = (bit_count + 7) / 8;
            break;
        }
        case DECIMAL:  // 1 — old DECIMAL (pre-5.0), stored as fixed-length binary
        case NEWDECIMAL: { // 21 — DECIMAL(M,D), packed binary format
            // NEWDECIMAL: stored as (precision/9*4 + remainder) bytes
            // For schemaless recovery, use a reasonable default
            int precision = col_info.column_length;
            if (precision <= 0) precision = 10;
            int int_digits = precision - 2;  // default scale=2
            if (int_digits < 1) int_digits = 1;
            int int_bytes = (int_digits / 9) * 4;
            int remainder = int_digits % 9;
            if (remainder > 0) int_bytes += (remainder + 1) / 2;
            int frac_digits = 2;
            int frac_bytes = (frac_digits / 9) * 4;
            int rem_f = frac_digits % 9;
            if (rem_f > 0) frac_bytes += (rem_f + 1) / 2;
            field_len = int_bytes + frac_bytes;
            break;
        }
        default:
            // 默认假设为固定长度字段
            field_len = 4;
            break;
    }

    // ✅ 新增：最终边界检查
    if (rec_ptr + field_len > page_base_ + page_size_) {
        return nullptr;
    }

    return rec_ptr + field_len;
}

uint32_t FieldDecoder::DecodeMicrosecond(const uint8_t* data) {
    // MySQL 8.0 时间类型微秒部分编码
    // 实际实现需要参考 MySQL 源码
    if (!data) return 0;

    // 简化：读取前 3 字节作为微秒值（实际应使用专用解码函数）
    uint32_t val = dbrecovery::innodb::read_le<uint32_t>(data, 0);
    return val & 0xFFFFFF; // 只取低 24 位
}

} // namespace engine
} // namespace dbrecovery
