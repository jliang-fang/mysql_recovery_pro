#include "dbrecovery/engine/row_decoder.hpp"
#include <cstring>
#include <stdexcept>
#include <cmath>

namespace dbrecovery {
namespace engine {

std::vector<FieldValue> RowDecoder::decode_record(size_t origin) const {
    std::vector<FieldValue> fields;
    
    // 基础校验：至少包含记录头（5 字节）+ 1 字节数据
    if (!page_base_ || null_bytes_ == 0 || origin + 6 > page_size_) {
        return fields;
    }

    const uint8_t* record_ptr = page_base_ + origin;

    // Step 1: 解析记录头（COMPACT 格式）
    uint8_t info_bits = *record_ptr;
    bool is_deleted = (info_bits & REC_INFO_DELETED_FLAG) != 0;
    
    if (is_deleted) {
        return fields; // 跳过已删除记录
    }

    // Step 2: 解析 NULL 位图
    size_t null_bytes = null_bytes_;
    std::vector<bool> is_null(null_bytes * 8, false);
    
    for (size_t i = 0; i < null_bytes; ++i) {
        uint8_t byte = *(record_ptr + 1 + i);
        for (int j = 0; j < 8; ++j) {
            if (byte & (1 << j)) {
                is_null[i * 8 + j] = true;
            }
        }
    }

    // Step 3: 跳过记录头和 NULL 字节，开始解析字段
    const uint8_t* data_ptr = record_ptr + 1 + null_bytes;

    // TODO: 实际应用中应从 TableSchema 获取列定义
    // 此处为简化示例：假设前两列为 INT 和 VARCHAR
    // 实际需遍历 schema.columns 并按类型解码

    // 示例：解析第一个字段（INT）
    if (data_ptr + 8 <= page_base_ + size_) {
        uint64_t int_val = *reinterpret_cast<const uint64_t*>(data_ptr);
        FieldValue fv;
        fv.value = std::to_string(int_val);
        fields.push_back(fv);
        data_ptr += 8;
    }

    // 示例：解析第二个字段（VARCHAR）
    if (data_ptr + 2 <= page_base_ + size_) {
        uint16_t len = *reinterpret_cast<const uint16_t*>(data_ptr);
        data_ptr += 2;

        if (len > 0 && data_ptr + len <= page_base_ + size_) {
            FieldValue fv;
            fv.value = std::string(reinterpret_cast<const char*>(data_ptr), len);
            fields.push_back(fv);
        }
    }

    return fields;
}

} // namespace engine
} // namespace dbrecovery
