#include "dbrecovery/engine/row_decoder.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <cstring>
#include <stdexcept>

namespace dbrecovery {
namespace engine {

std::vector<FieldValue> RowDecoder::decode_record(size_t origin, const catalog::TableSchema& schema) const {
    std::vector<FieldValue> fields;
    
    // 基础校验：至少包含记录头（5 字节）+ 1 字节数据
    if (!page_base_ || size_ == 0) {
        return fields;
    }
    
    constexpr size_t MAX_PAGE_SIZE = 65536; // InnoDB 最大页大小 (64KB)
    if (origin + 6 > size_ || origin + 6 > MAX_PAGE_SIZE) {
        return fields;
    }

    const uint8_t* record_ptr = page_base_ + origin;

    // Step 1: 解析记录头（COMPACT 格式，5 字节）
    if (origin + 5 > size_ || origin + 5 > MAX_PAGE_SIZE) {
        return fields;
    }
    
    // ✅ 修复：检查记录删除标志（REC_INFO_DELETED_FLAG = bit 3, mask = 0x08）
    uint8_t info_bits = record_ptr[0];
    bool is_deleted = (info_bits & 0x08) != 0;
    
    if (is_deleted) {
        return fields; // 跳过已删除记录
    }

    // Step 2: 解析 NULL 位图（从记录头后开始）
    const uint8_t* null_ptr = record_ptr + 5; // 跳过 5-byte record header (COMPACT)
    
    // ✅ 修复：正确计算可空列数量
    uint32_t nullable_count = schema.get_nullable_column_count();
    if (nullable_count == 0) {
        null_bytes_ = 0;
    } else {
        null_bytes_ = (nullable_count + 7) / 8;
    }
    
    // 按列顺序解析字段值
    for (const auto& pair : schema.columns) {
        const auto& col = pair.second;
        
        FieldValue fv;
        
        // ✅ 修复：正确计算NULL位图索引（只计数可空列）
        if (col.is_nullable && null_bytes_ > 0) {
            uint32_t col_idx = 0;
            for (const auto& prev_pair : schema.columns) {
                const auto& prev_col = prev_pair.second;
                // 只计算在当前列之前定义的可空列
                if (prev_pair.first >= pair.first) {
                    break; // 已经处理完所有前置列
                }
                if (prev_col.is_nullable && !prev_col.is_off_page && prev_col.virtual_gen_store_type == 0) {
                    col_idx++;
                }
            }
            
            uint32_t byte_idx = col_idx / 8;
            uint32_t bit_idx = col_idx % 8;
            
            // ✅ 新增：边界检查确保不会越界访问
            if (byte_idx >= null_bytes_) {
                std::cerr << "[WARN] NULL bitmap index out of range: " << byte_idx 
                          << " >= " << null_bytes_ << std::endl;
                return fields; // 跳过此字段
            }
            
            // ✅ 使用统一的NULL位图解析逻辑（小端序：低位在前）
            if (null_ptr != nullptr && null_ptr + byte_idx < page_base_ + size_) {
                bool is_null = (null_ptr[byte_idx] & (1U << bit_idx)) != 0;
                if (is_null) {
                    fv.value = "NULL";
                    fields.push_back(fv);
                    continue;
                }
            }
        }
        
        // ✅ 修复：正确解析字段偏移（跳过所有前置字段）
        const uint8_t* data_ptr = null_ptr + null_bytes_;
        for (const auto& prev_pair : schema.columns) {
            if (prev_pair.first >= pair.first) {
                break; // 已经处理完所有前置列
            }
            
            const auto& prev_col = prev_pair.second;
            
            // ✅ 修复：正确跳过每个字段的数据
            if (prev_col.mysql_type_id == mysql_types::VARCHAR || 
                prev_col.mysql_type_id == mysql_types::VAR_STRING) { // VARCHAR/VAR_STRING
                bool use_two_byte_prefix = (prev_col.column_length > 255);
                
                if (data_ptr + (use_two_byte_prefix ? 2 : 1) > page_base_ + size_) {
                    break; // 数据不足，停止解析
                }
                
                uint32_t len = 0;
                if (use_two_byte_prefix) {
                    len = MachRead::read_le<uint16_t>(data_ptr, 0);
                    data_ptr += 2;
                } else {
                    len = *data_ptr;
                    data_ptr += 1;
                }
                
                if (len > 0 && data_ptr + len <= page_base_ + size_) {
                    data_ptr += len;
                }
            } else if (prev_col.mysql_type_id == mysql_types::JSON) { // JSON
                if (data_ptr + 2 > page_base_ + size_) {
                    break; // 数据不足，停止解析
                }
                
                uint16_t len = MachRead::read_le<uint16_t>(data_ptr, 0);
                data_ptr += 2;
                
                if (len > 0 && data_ptr + len <= page_base_ + size_) {
                    data_ptr += len;
                }
            } else if (prev_col.mysql_type_id == mysql_types::LONGLONG) { // LONGLONG
                if (data_ptr + 8 <= page_base_ + size_) {
                    data_ptr += 8;
                } else {
                    break; // 数据不足
                }
            } else if (prev_col.mysql_type_id == mysql_types::LONG) { // LONG
                if (data_ptr + 4 <= page_base_ + size_) {
                    data_ptr += 4;
                } else {
                    break; // 数据不足
                }
            } else if (prev_col.mysql_type_id == mysql_types::INT24) { // INT24
                if (data_ptr + 3 <= page_base_ + size_) {
                    data_ptr += 3;
                } else {
                    break; // 数据不足
                }
            } else if (prev_col.mysql_type_id == mysql_types::SHORT) { // SHORT
                if (data_ptr + 2 <= page_base_ + size_) {
                    data_ptr += 2;
                } else {
                    break; // 数据不足
                }
            } else if (prev_col.mysql_type_id == mysql_types::TINY) { // TINY
                if (data_ptr + 1 <= page_base_ + size_) {
                    data_ptr += 1;
                } else {
                    break; // 数据不足
                }
            } else if (prev_col.mysql_type_id == mysql_types::DATE) { // DATE
                if (data_ptr + 3 <= page_base_ + size_) {
                    data_ptr += 3;
                } else {
                    break; // 数据不足
                }
            } else if (prev_col.mysql_type_id == mysql_types::DATETIME || 
                       prev_col.mysql_type_id == mysql_types::TIMESTAMP) { // DATETIME/TIMESTAMP
                if (data_ptr + 5 <= page_base_ + size_) {
                    data_ptr += 5;
                } else {
                    break; // 数据不足
                }
            } else if (prev_col.mysql_type_id == mysql_types::DATETIME2 || 
                       prev_col.mysql_type_id == mysql_types::TIMESTAMP2) { // DATETIME2/TIMESTAMP2
                // 根据精度计算大小
                int precision = prev_col.datetime_precision;
                if (precision < 0) precision = 6; // 默认精度
                
                uint32_t size = 5; // 基础部分总是5字节
                if (precision <= 2) {
                    size += 3; // 小数部分3字节
                } else if (precision <= 4) {
                    size += 3; // 小数部分3字节
                } else {
                    size += 3; // 小数部分3字节
                }
                
                if (data_ptr + size <= page_base_ + size_) {
                    data_ptr += size;
                } else {
                    break; // 数据不足
                }
            } else if (prev_col.mysql_type_id == mysql_types::TIME || 
                       prev_col.mysql_type_id == mysql_types::TIME2) { // TIME/TIME2
                if (data_ptr + 3 <= page_base_ + size_) {
                    data_ptr += 3;
                } else {
                    break; // 数据不足
                }
            } else if (prev_col.mysql_type_id == mysql_types::YEAR) { // YEAR
                if (data_ptr + 1 <= page_base_ + size_) {
                    data_ptr += 1;
                } else {
                    break; // 数据不足
                }
            } else if (prev_col.mysql_type_id == mysql_types::BIT) { // BIT
                uint32_t bit_count = prev_col.column_length;
                uint32_t byte_size = (bit_count + 7) / 8;
                if (data_ptr + byte_size <= page_base_ + size_) {
                    data_ptr += byte_size;
                } else {
                    break; // 数据不足
                }
            } else if (prev_col.mysql_type_id == mysql_types::FLOAT) { // FLOAT
                if (data_ptr + 4 <= page_base_ + size_) {
                    data_ptr += 4;
                } else {
                    break; // 数据不足
                }
            } else if (prev_col.mysql_type_id == mysql_types::DOUBLE) { // DOUBLE
                if (data_ptr + 8 <= page_base_ + size_) {
                    data_ptr += 8;
                } else {
                    break; // 数据不足
                }
            } else if (prev_col.mysql_type_id >= mysql_types::TINY_BLOB && 
                       prev_col.mysql_type_id <= mysql_types::BLOB) { // BLOB类型
                // BLOB指针固定4字节
                if (data_ptr + 4 <= page_base_ + size_) {
                    data_ptr += 4;
                } else {
                    break; // 数据不足
                }
            } else {
                // 其他类型暂不支持，跳过固定长度1字节
                if (data_ptr + 1 <= page_base_ + size_) {
                    data_ptr += 1;
                } else {
                    break; // 数据不足
                }
            }
        }
        
        // ✅ 修复：解析当前字段（添加边界检查）
        if (col.mysql_type_id == mysql_types::VARCHAR || 
            col.mysql_type_id == mysql_types::VAR_STRING) { // VARCHAR/VAR_STRING
            bool use_two_byte_prefix = (col.column_length > 255);
            
            if (data_ptr + (use_two_byte_prefix ? 2 : 1) > page_base_ + size_) {
                fv.value = "[TRUNCATED]";
            } else {
                uint32_t len = 0;
                if (use_two_byte_prefix) {
                    len = MachRead::read_le<uint16_t>(data_ptr, 0);
                    data_ptr += 2;
                } else {
                    len = *data_ptr;
                    data_ptr += 1;
                }
                
                if (len > 0 && data_ptr + len <= page_base_ + size_) {
                    fv.value = std::string(reinterpret_cast<const char*>(data_ptr), len);
                } else {
                    fv.value = "[TRUNCATED]";
                }
            }
        } else if (col.mysql_type_id == mysql_types::JSON) { // JSON
            if (data_ptr + 2 > page_base_ + size_) {
                fv.value = "[TRUNCATED_JSON]";
            } else {
                uint16_t len = MachRead::read_le<uint16_t>(data_ptr, 0);
                data_ptr += 2;
                
                if (len > 0 && data_ptr + len <= page_base_ + size_) {
                    fv.value = "[JSON_DATA]";
                } else {
                    fv.value = "[TRUNCATED_JSON]";
                }
            }
        } else if (col.mysql_type_id == mysql_types::LONGLONG) { // LONGLONG
            if (data_ptr + 8 <= page_base_ + size_) {
                uint64_t val = MachRead::read_le<uint64_t>(data_ptr, 0);
                fv.value = std::to_string(val);
            } else {
                fv.value = "[TRUNCATED]";
            }
        } else if (col.mysql_type_id == mysql_types::LONG) { // LONG
            if (data_ptr + 4 <= page_base_ + size_) {
                uint32_t val = MachRead::read_le<uint32_t>(data_ptr, 0);
                fv.value = std::to_string(val);
            } else {
                fv.value = "[TRUNCATED]";
            }
        } else if (col.mysql_type_id == mysql_types::INT24) { // INT24
            if (data_ptr + 3 <= page_base_ + size_) {
                uint32_t val = MachRead::read_le<uint32_t>(data_ptr, 0);
                // 处理符号扩展
                if (val & 0x800000) {
                    val |= 0xFF000000;
                }
                fv.value = std::to_string(val);
            } else {
                fv.value = "[TRUNCATED]";
            }
        } else if (col.mysql_type_id == mysql_types::SHORT) { // SHORT
            if (data_ptr + 2 <= page_base_ + size_) {
                uint16_t val = MachRead::read_le<uint16_t>(data_ptr, 0);
                fv.value = std::to_string(val);
            } else {
                fv.value = "[TRUNCATED]";
            }
        } else if (col.mysql_type_id == mysql_types::TINY) { // TINY
            if (data_ptr + 1 <= page_base_ + size_) {
                uint8_t val = *data_ptr;
                fv.value = std::to_string(val);
            } else {
                fv.value = "[TRUNCATED]";
            }
        } else if (col.mysql_type_id == mysql_types::DATE) { // DATE
            if (data_ptr + 3 <= page_base_ + size_) {
                uint8_t day = data_ptr[0];
                uint8_t month = data_ptr[1];
                uint16_t year = data_ptr[2] | (data_ptr[1] << 8); // 错误修正：正确解析year
                
                // 正确的DATE编码：[day, month, year_low, year_high]
                uint8_t real_day = data_ptr[0];
                uint8_t real_month = data_ptr[1];
                uint16_t real_year = data_ptr[2] | (data_ptr[3] << 8);
                
                fv.value = std::to_string(real_year) + "-" + 
                          std::to_string(real_month) + "-" + 
                          std::to_string(real_day);
            } else {
                fv.value = "[TRUNCATED]";
            }
        } else if (col.mysql_type_id == mysql_types::DATETIME || 
                   col.mysql_type_id == mysql_types::TIMESTAMP) { // DATETIME/TIMESTAMP
            if (data_ptr + 5 <= page_base_ + size_) {
                uint8_t byte0 = data_ptr[0];
                uint8_t byte1 = data_ptr[1];
                uint8_t byte2 = data_ptr[2];
                uint8_t byte3 = data_ptr[3];
                uint8_t byte4 = data_ptr[4];
                
                uint8_t year = (byte0 & 0x7F);
                uint8_t month = ((byte0 >> 7) & 0x0F) | ((byte1 << 1) & 0x07);
                uint8_t day = (byte1 >> 2) & 0x1F;
                
                uint8_t hour = byte2 & 0x1F;
                uint8_t minute = ((byte2 >> 5) & 0x03) | ((byte3 << 2) & 0x3F);
                uint8_t second = (byte3 >> 6) & 0x03 | (byte4 << 2);
                
                fv.value = std::to_string(year + 1900) + "-" + 
                          std::to_string(month) + "-" + 
                          std::to_string(day) + " " +
                          std::to_string(hour) + ":" + 
                          std::to_string(minute) + ":" + 
                          std::to_string(second);
            } else {
                fv.value = "[TRUNCATED]";
            }
        } else if (col.mysql_type_id == mysql_types::LONGLONG) { // LONGLONG
            if (data_ptr + 8 <= page_base_ + size_) {
                uint64_t val = MachRead::read_le<uint64_t>(data_ptr, 0);
                fv.value = std::to_string(val);
            } else {
                fv.value = "[TRUNCATED]";
            }
        } else if (col.mysql_type_id == mysql_types::FLOAT) { // FLOAT
            if (data_ptr + 4 <= page_base_ + size_) {
                float val;
                std::memcpy(&val, data_ptr, sizeof(float));
                fv.value = std::to_string(val);
            } else {
                fv.value = "[TRUNCATED]";
            }
        } else if (col.mysql_type_id == mysql_types::DOUBLE) { // DOUBLE
            if (data_ptr + 8 <= page_base_ + size_) {
                double val;
                std::memcpy(&val, data_ptr, sizeof(double));
                fv.value = std::to_string(val);
            } else {
                fv.value = "[TRUNCATED]";
            }
        } else {
            fv.value = "[UNIMPLEMENTED_TYPE_" + std::to_string(col.mysql_type_id) + "]";
        }
        
        fields.push_back(fv);
    }

    return fields;
}

// 辅助方法：解码单个字段
FieldValue RowDecoder::decode_field(const uint8_t* data, uint32_t type, bool is_nullable, size_t length) const {
    FieldValue fv;
    
    switch (type) {
        case mysql_types::LONG: { // 3
            if (length >= 4) {
                uint32_t val = MachRead::read_le<uint32_t>(data, 0);
                fv.value = std::to_string(val);
            } else {
                fv.value = "[INVALID_LENGTH]";
            }
            break;
        }
        case mysql_types::LONGLONG: { // 8
            if (length >= 8) {
                uint64_t val = MachRead::read_le<uint64_t>(data, 0);
                fv.value = std::to_string(val);
            } else {
                fv.value = "[INVALID_LENGTH]";
            }
            break;
        }
        case mysql_types::VARCHAR: { // 15
            if (length > 0) {
                uint8_t len = *data;
                if (len > 0 && length >= len + 1) {
                    fv.value = std::string(reinterpret_cast<const char*>(data + 1), len);
                } else {
                    fv.value = "";
                }
            } else {
                fv.value = "[INVALID_LENGTH]";
            }
            break;
        }
        case mysql_types::JSON: { // 245
            if (length >= 2) {
                uint16_t len = MachRead::read_le<uint16_t>(data, 0);
                if (len > 0 && length >= len + 2) {
                    fv.value = "[JSON_DATA]";
                } else {
                    fv.value = "";
                }
            } else {
                fv.value = "[INVALID_LENGTH]";
            }
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
