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
    RowDecoder(const uint8_t* page_base, size_t page_size, uint32_t null_bytes)
        : page_base_(page_base),
          size_(page_size),
          null_bytes_(null_bytes),
          nullable_columns_(null_bytes * 8) {

        if (null_bytes > 0 && page_base_ == nullptr) {
            throw std::invalid_argument("Page base pointer is null but null_bytes > 0");
        }
    }

    uint32_t get_nullable_columns() const { return nullable_columns_; }

    // Decode a record at given origin offset using schema info
    // Supports COMPACT, DYNAMIC, and REDUNDANT row formats
    std::vector<FieldValue> decode_record(size_t origin, const catalog::TableSchema& schema) const;

    // Check if field is NULL (COMPACT/DYNAMIC format)
    bool is_null(uint32_t col_idx, const uint8_t* null_ptr) const {
        if (col_idx >= nullable_columns_) return false;
        uint32_t byte_idx = col_idx / 8;
        uint32_t bit_idx = col_idx % 8;
        return (null_ptr[byte_idx] & (1U << bit_idx)) != 0;
    }

    const uint8_t* get_null_bitmap_ptr() const {
        return page_base_ + 1;
    }

private:
    const uint8_t* page_base_;
    size_t size_;                  // ✅ 新增：页大小
    uint32_t null_bytes_;          // NULL位图字节数
    uint32_t nullable_columns_;    // 可为空的最大列数（= null_bytes * 8）

    // 辅助方法：解码单个字段
    FieldValue decode_field(const uint8_t* data, uint32_t type, bool is_nullable, size_t length) const;
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_ROW_DECODER_HPP
