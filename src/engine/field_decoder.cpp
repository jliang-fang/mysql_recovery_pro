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
            // VARCHAR 长度前缀：1 字节（<256）或 2 字节（>=256）
            if (rec_ptr + 1 > page_base_ + size_) {
                return nullptr;
            }
            uint8_t len_prefix = *rec_ptr;
            field_len = 1 + len_prefix;
            break;
        case 245: // JSON
            // JSON 类型使用变长编码
            if (rec_ptr + 2 > page_base_ + size_) {
                return nullptr;
            }
            uint16_t json_len = DBRecovery::InnoDB::MachRead::read_le<uint16_t>(rec_ptr, 0);
            field_len = 2 + json_len;
            break;
        default:
            // 默认假设为固定长度字段
            field_len = 4;
            break;
    }
    
    return rec_ptr + field_len;
}

uint32_t FieldDecoder::DecodeMicrosecond(const uint8_t* data) {
    // MySQL 8.0 时间类型微秒部分编码
    // 实际实现需要参考 MySQL 源码
    if (!data) return 0;
    
    // 简化：读取前 3 字节作为微秒值（实际应使用专用解码函数）
    uint32_t val = DBRecovery::InnoDB::MachRead::read_le<uint32_t>(data, 0);
    return val & 0xFFFFFF; // 只取低 24 位
}

} // namespace engine
} // namespace dbrecovery
