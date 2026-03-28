#ifndef DBRECOVERY_ENGINE_FIELD_DECODER_HPP
#define DBRECOVERY_ENGINE_FIELD_DECODER_HPP
#include <vector>
#include <cstdint>
#include <cstddef>

namespace dbrecovery {
namespace engine {

class FieldDecoder {
public:
    FieldDecoder(const uint8_t* page_base, size_t page_size, size_t null_bytes);

    const uint8_t* DecodeNextField(const uint8_t* rec_ptr, int field_index, const catalog::ColumnInfo& col_info) const;
    uint32_t DecodeMicrosecond(const uint8_t* data);

private:
    const uint8_t* page_base_;
    size_t page_size_;
    size_t null_bytes_;
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_FIELD_DECODER_HPP
