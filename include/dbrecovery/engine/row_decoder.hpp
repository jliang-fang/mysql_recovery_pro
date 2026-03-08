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
    RowDecoder(const uint8_t* page_base, uint32_t null_bytes)
        : page_base_(page_base), null_bytes_(null_bytes) {}

    std::vector<FieldValue> decode_record(size_t origin) const;

    // 辅助方法：检查字段是否为 NULL
    bool is_null(uint32_t col_idx) const {
        if (col_idx >= null_bytes_ * 8) return false;
        uint32_t byte_idx = col_idx / 8;
        uint32_t bit_idx = col_idx % 8;
        return (page_base_[null_bytes_ - 1 - byte_idx] & (1 << bit_idx)) != 0;
    }

private:
    const uint8_t* page_base_;
    uint32_t null_bytes_;

    // 辅助方法：解码单个字段
    FieldValue decode_field(const uint8_t* data, uint32_t type, bool is_nullable, size_t length) const;
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_ROW_DECODER_HPP
