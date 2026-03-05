#include "dbrecovery/engine/field_decoder.hpp"

namespace dbrecovery {
namespace engine {

FieldDecoder::FieldDecoder(const uint8_t* page_base, size_t null_bytes)
    : page_base_(page_base), null_bytes_(null_bytes) {}

const uint8_t* FieldDecoder::DecodeNextField(const uint8_t* rec_ptr, int field_index) const {
    // 简化：返回下一个字段指针
    return rec_ptr + 10;
}

uint32_t FieldDecoder::DecodeMicrosecond(const uint8_t* data) {
    // 简化：返回固定值
    return 0;
}

} // namespace engine
} // namespace dbrecovery
