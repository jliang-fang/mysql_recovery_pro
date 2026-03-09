#ifndef DBRECOVERY_ENGINE_ROW_DECODER_HPP
#define DBRECOVERY_ENGINE_ROW_DECODER_HPP
#include <vector>
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/engine/schema.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace engine {

struct FieldValue {
    std::string value;
};

class RowDecoder {
public:
    // null_bytes: NULL位图占用的字节数（非位数），计算公式：(nullable_columns + 7) / 8
    RowDecoder(const uint8_t* page_base, uint32_t null_bytes)
        : page_base_(page_base), 
          null_bytes_(null_bytes),
          nullable_columns_(null_bytes * 8) { // 反推可支持的最大列数
        
        if (null_bytes > 0 && page_base_ == nullptr) {
            throw std::invalid_argument("Page base pointer is null but null_bytes > 0");
        }
    }

    uint32_t get_nullable_columns() const { return nullable_columns_; }

    std::vector<FieldValue> decode_record(size_t origin) const;

    // 辅助方法：检查字段是否为 NULL
    bool is_null(uint32_t col_idx) const {
        if (col_idx >= nullable_columns_) return false;
        
        uint32_t byte_idx = col_idx / 8;
        uint32_t bit_idx = col_idx % 8;
        
        // COMPACT 格式：NULL 位图从记录头后第一个字节开始，倒序存储
        // 列0对应bit7，列1对应bit6...列7对应bit0，然后是下一个字节
        const uint8_t* null_ptr = page_base_ + 1; // 跳过记录头
        return (null_ptr[byte_idx] & (1 << (7 - bit_idx))) != 0;
    }

private:
    const uint8_t* page_base_;
    uint32_t null_bytes_;          // NULL位图字节数
    uint32_t nullable_columns_;    // 可为空的最大列数（= null_bytes * 8）

    // 辅助方法：解码单个字段
    FieldValue decode_field(const uint8_t* data, uint32_t type, bool is_nullable, size_t length) const;
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_ROW_DECODER_HPP
