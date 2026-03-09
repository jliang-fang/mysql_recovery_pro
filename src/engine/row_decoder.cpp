#include "dbrecovery/engine/row_decoder.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/binary_reader.hpp"
#include <cstring>
#include <stdexcept>

namespace dbrecovery {
namespace engine {

std::vector<FieldValue> RowDecoder::decode_record(size_t origin) const {
    std::vector<FieldValue> fields;
    
    // 基础校验：至少包含记录头（5 字节）+ 1 字节数据
    if (!page_base_ || null_bytes_ == 0 || origin + 6 > 16384) {
        return fields;
    }

    const uint8_t* record_ptr = page_base_ + origin;

    // Step 1: 解析记录头（COMPACT 格式，5 字节）
    if (origin + 5 > 16384) {
        return fields;
    }
    
    RecordHeader header = RecordHeader::parse(record_ptr);
    bool is_deleted = header.is_deleted();
    
    if (is_deleted) {
        return fields; // 跳过已删除记录
    }

    // Step 2: 解析 NULL 位图（从 extra_bytes[0] 开始）
    std::vector<bool> is_null(null_bytes_ * 8, false);
    
    for (size_t i = 0; i < null_bytes_; ++i) {
        uint8_t byte = record_ptr[5 + i]; // 跳过 5 字节记录头
        for (int j = 0; j < 8; ++j) {
            // COMPACT 格式：NULL 位图倒序存储，bit 7 是第一个列
            is_null[i * 8 + j] = (byte >> (7 - j)) & 1;
        }
    }

    // Step 3: 跳过记录头和 NULL 字节，开始解析字段
    const uint8_t* data_ptr = record_ptr + 5 + null_bytes_;

    // TODO: 实际应用中应从 TableSchema 获取列定义
    // 此处为简化示例：假设前两列为 INT 和 VARCHAR
    // 实际需遍历 schema.columns 并按类型解码

    // 示例：解析第一个字段（INT，8 字节）
    if (data_ptr + 8 <= page_base_ + 16384) {
        uint64_t int_val = dbrecovery::innodb::MachRead::read_le<uint64_t>(data_ptr, 0);
        FieldValue fv;
        fv.value = std::to_string(int_val);
        fields.push_back(fv);
        data_ptr += 8;
    }

    // 示例：解析第二个字段（VARCHAR，1 字节长度前缀）
    if (data_ptr + 1 <= page_base_ + 16384) {
        uint8_t len = *data_ptr;
        data_ptr += 1;

        if (len > 0 && data_ptr + len <= page_base_ + 16384) {
            FieldValue fv;
            fv.value = std::string(reinterpret_cast<const char*>(data_ptr), len);
            fields.push_back(fv);
        }
    }

    return fields;
}

} // namespace engine
} // namespace dbrecovery
