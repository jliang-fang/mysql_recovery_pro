#include "dbrecovery/engine/field_decoder.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <cstring>
#include <stdexcept>

namespace dbrecovery {
namespace engine {

FieldDecoder::FieldDecoder(const uint8_t* page_base, size_t null_bytes)
    : page_base_(page_base), null_bytes_(null_bytes) {}

const uint8_t* FieldDecoder::DecodeNextField(const uint8_t* rec_ptr, int field_index, const catalog::ColumnInfo& col_info) const {
    // 根据列类型计算字段长度
    size_t field_len = 0;
    
    switch (col_info.mysql_type_id) {
        case 1:  // TINYINT
            field_len = 1;
            break;
        case 2:  // SHORTINT
            field_len = 2;
            break;
        case 3:  // LONGINT
            field_len = 4;
            break;
        case 8:  // LONGLONG
            field_len = 8;
            break;
        case 15: // VARCHAR
            // ✅ 修复：处理 1 字节和 2 字节长度前缀
            if (rec_ptr + 2 > page_base_ + size_) {
                return nullptr;
            }
            // InnoDB VARCHAR 长度前缀是独立存储的：
            // - 如果列最大长度 <= 255 字节，使用 1 字节前缀
            // - 否则使用 2 字节前缀
            bool use_two_byte_prefix = (col_info.column_length > 255);
            
            if (use_two_byte_prefix) {
                uint16_t len = dbrecovery::innodb::MachRead::read_le<uint16_t>(rec_ptr, 0);
                field_len = 2 + len;
                
                // ✅ 新增：边界检查
                if (rec_ptr + field_len > page_base_ + size_) {
                    return nullptr;
                }
            } else {
                uint8_t len = rec_ptr[0];
                field_len = 1 + len;
                
                // ✅ 新增：边界检查
                if (rec_ptr + field_len > page_base_ + size_) {
                    return nullptr;
                }
            }
            break;
        case 245: // JSON
            // ✅ 修复：JSON 使用小端 2 字节长度前缀
            if (rec_ptr + 2 > page_base_ + size_) {
                return nullptr;
            }
            uint16_t json_len = dbrecovery::innodb::MachRead::read_le<uint16_t>(rec_ptr, 0);
            field_len = 2 + json_len;
            
            // ✅ 新增：边界检查
            if (rec_ptr + field_len > page_base_ + size_) {
                return nullptr;
            }
            break;
        case 248: // TEXT
        case 249: // TINYBLOB
        case 250: // BLOB
        case 251: // MEDIUMBLOB
        case 252: // LONGBLOB
        case 253: // VARCHAR (newer MySQL)
        case 254: // VARSTRING
            // ✅ 新增：处理 BLOB/TEXT 类型（使用 BLOB 指针）
            // 根据列定义中的长度信息判断是 inline 还是 overflow
            if (col_info.column_length >= 0x10000) {
                field_len = 9; // LONGBLOB 指针长度（8 字节 + 1 字节长度前缀）
            } else if (col_info.column_length >= 0x100) {
                field_len = 5; // BLOB 指针长度（4 字节 + 1 字节长度前缀）
            } else {
                // 对于较小的 BLOB/TEXT，可能直接存储在记录中（inline）
                // 需要根据实际记录格式动态判断
                field_len = col_info.column_length;
                
                // ✅ 新增：边界检查
                if (rec_ptr + field_len > page_base_ + size_) {
                    return nullptr;
                }
            }
            break;
        default:
            // 默认假设为固定长度字段
            field_len = 4;
            break;
    }
    
    // ✅ 新增：最终边界检查
    if (rec_ptr + field_len > page_base_ + size_) {
        return nullptr;
    }
    
    return rec_ptr + field_len;
}

uint32_t FieldDecoder::DecodeMicrosecond(const uint8_t* data) {
    // MySQL 8.0 时间类型微秒部分编码
    // 实际实现需要参考 MySQL 源码
    if (!data) return 0;
    
    // 简化：读取前 3 字节作为微秒值（实际应使用专用解码函数）
    uint32_t val = dbrecovery::innodb::MachRead::read_le<uint32_t>(data, 0);
    return val & 0xFFFFFF; // 只取低 24 位
}

} // namespace engine
} // namespace dbrecovery
