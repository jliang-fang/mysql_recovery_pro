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

    // Step 2: 解析 NULL 位图（从记录头后开始）
    std::vector<bool> is_null(null_bytes_ * 8, false);
    
    // InnoDB NULL 位图总是从记录头后第一个字节开始，倒序存储
    // COMPACT 格式：记录头 5 字节；DYNAMIC/COMPRESSED 格式：记录头可变（12+ 字节）
    // 此处假设为 COMPACT 格式（最常见于恢复场景），后续需扩展支持 DYNAMIC
    const uint8_t* null_ptr = record_ptr + 5; // 跳过 5-byte record header (COMPACT)
    
    for (size_t i = 0; i < null_bytes_; ++i) {
        uint8_t byte = null_ptr[i];
        for (int j = 0; j < 8; ++j) {
            // 列0对应bit7，列1对应bit6...列7对应bit0
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

// 辅助方法：解码单个字段（占位实现，待后续完善）
FieldValue RowDecoder::decode_field(const uint8_t* data, uint32_t type, bool is_nullable, size_t length) const {
    // TODO: 实现基于 type 的字段解码逻辑
    FieldValue fv;
    fv.value = "[UNIMPLEMENTED]";
    return fv;
}

} // namespace engine
} // namespace dbrecovery
