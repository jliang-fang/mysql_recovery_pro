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
    if (!page_base_ || origin + 6 > 16384) {
        return fields;
    }

    const uint8_t* record_ptr = page_base_ + origin;

    // Step 1: 解析记录头（COMPACT 格式，5 字节）
    if (origin + 5 > 16384) {
        return fields;
    }
    
    // 检查记录头标志
    uint8_t flags = record_ptr[0];
    bool is_deleted = (flags & 0x80) != 0;
    
    if (is_deleted) {
        return fields; // 跳过已删除记录
    }

    // Step 2: 解析 NULL 位图（从记录头后开始）
    const uint8_t* null_ptr = record_ptr + 5; // 跳过 5-byte record header (COMPACT)
    
    // 按列顺序解析字段值
    for (const auto& pair : schema.columns) {
        const auto& col = pair.second;
        
        FieldValue fv;
        
        // 检查 NULL
        if (col.is_nullable && null_bytes_ > 0) {
            uint32_t col_idx = 0;
            for (uint32_t j = 1; j < pair.first; ++j) {
                if (schema.columns.at(j).is_nullable) {
                    col_idx++;
                }
            }
            
            uint32_t byte_idx = col_idx / 8;
            uint32_t bit_idx = col_idx % 8;
            
            if (byte_idx < null_bytes_) {
                bool is_null = (null_ptr[byte_idx] & (1 << (7 - bit_idx))) != 0;
                if (is_null) {
                    fv.value = "NULL";
                    fields.push_back(fv);
                    continue;
                }
            }
        }
        
        // 解析非 NULL 字段值
        const uint8_t* data_ptr = null_ptr + null_bytes_;
        for (uint32_t j = 1; j < pair.first; ++j) {
            const auto& prev_col = schema.columns.at(j);
            
            if (prev_col.mysql_type_id == 15) { // VARCHAR
                uint8_t len = *data_ptr;
                data_ptr += 1;
                
                if (len > 0 && data_ptr + len <= page_base_ + 16384) {
                    data_ptr += len;
                }
            } else if (prev_col.mysql_type_id == 245) { // JSON
                uint16_t len = MachRead::read_le<uint16_t>(data_ptr, 0);
                data_ptr += 2;
                
                if (len > 0 && data_ptr + len <= page_base_ + 16384) {
                    data_ptr += len;
                }
            } else if (prev_col.mysql_type_id == 3 || prev_col.mysql_type_id == 8) { // LONG/LONGLONG
                data_ptr += (prev_col.mysql_type_id == 8) ? 8 : 4;
            } else {
                // 其他类型暂不支持，跳过固定长度
                data_ptr += 1; // 默认假设为 1 字节
            }
        }
        
        // 解析当前字段
        if (col.mysql_type_id == 15) { // VARCHAR
            uint8_t len = *data_ptr;
            data_ptr += 1;
            
            if (len > 0 && data_ptr + len <= page_base_ + 16384) {
                fv.value = std::string(reinterpret_cast<const char*>(data_ptr), len);
            } else {
                fv.value = "";
            }
        } else if (col.mysql_type_id == 245) { // JSON
            uint16_t len = MachRead::read_le<uint16_t>(data_ptr, 0);
            data_ptr += 2;
            
            if (len > 0 && data_ptr + len <= page_base_ + 16384) {
                fv.value = "[JSON_DATA]";
            } else {
                fv.value = "";
            }
        } else if (col.mysql_type_id == 3) { // LONG
            if (data_ptr + 4 <= page_base_ + 16384) {
                uint32_t val = MachRead::read_le<uint32_t>(data_ptr, 0);
                fv.value = std::to_string(val);
            } else {
                fv.value = "INVALID";
            }
        } else if (col.mysql_type_id == 8) { // LONGLONG
            if (data_ptr + 8 <= page_base_ + 16384) {
                uint64_t val = MachRead::read_le<uint64_t>(data_ptr, 0);
                fv.value = std::to_string(val);
            } else {
                fv.value = "INVALID";
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
        case 3: { // MYSQL_TYPE_LONG
            if (length >= 4) {
                uint32_t val = MachRead::read_le<uint32_t>(data, 0);
                fv.value = std::to_string(val);
            } else {
                fv.value = "[INVALID_LENGTH]";
            }
            break;
        }
        case 8: { // MYSQL_TYPE_LONGLONG
            if (length >= 8) {
                uint64_t val = MachRead::read_le<uint64_t>(data, 0);
                fv.value = std::to_string(val);
            } else {
                fv.value = "[INVALID_LENGTH]";
            }
            break;
        }
        case 15: { // MYSQL_TYPE_VARCHAR
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
        case 245: { // MYSQL_TYPE_JSON
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
