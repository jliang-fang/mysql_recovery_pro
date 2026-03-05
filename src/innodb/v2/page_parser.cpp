#include <cstdint>
#include "dbrecovery/innodb/v2/page_parser.hpp"
#include <cstring>

namespace DBRecovery::InnoDB::v2 {

PageParser::PageParser(const uint8_t* buffer, size_t size)
    : buffer_(buffer), size_(size) {}

bool PageParser::parse() noexcept {
    // 1. 基础校验：页大小至少为 16KB（标准 InnoDB 页）
    if (!buffer_ || size_ < 16384) return false;

    // 2. 解析 FIL_PAGE_TYPE (offset 0)
    uint16_t raw_type = 0;
    std::memcpy(&raw_type, buffer_, sizeof(uint16_t));
    type_ = static_cast<PageType>(raw_type);

    // 3. 校验页类型合法性（仅允许索引/SDI/BLOB 等恢复相关类型）
    switch (type_) {
        case PageType::FIL_PAGE_INDEX:
        case PageType::FIL_PAGE_SDI:
        case PageType::FIL_PAGE_TYPE_BLOB:
            break;
        default:
            // 非数据页（如 UNDO、SYS）暂不解析
            return false;
    }

    // 4. 解析 PAGE_HEADER (offset 38 = FIL_PAGE_DATA)
    const uint8_t* header_ptr = buffer_ + 38;

    // 按 MySQL 8.0 官方偏移读取（storage/innobase/include/page0page.h）
    std::memcpy(&header_.n_dir_slots, header_ptr + 14, sizeof(uint16_t));
    std::memcpy(&header_.heap_top,    header_ptr + 18, sizeof(uint16_t));
    std::memcpy(&header_.free,        header_ptr + 20, sizeof(uint16_t));
    std::memcpy(&header_.garbage,     header_ptr + 22, sizeof(uint16_t));
    std::memcpy(&header_.insert_offset, header_ptr + 24, sizeof(uint32_t));
    std::memcpy(&header_.n_recs,      header_ptr + 28, sizeof(uint16_t));
    std::memcpy(&header_.max_trx_id,  header_ptr + 30, sizeof(uint32_t));
    std::memcpy(&header_.page_level,  header_ptr + 34, sizeof(uint16_t));
    std::memcpy(&header_.page_index_id, header_ptr + 36, sizeof(uint32_t));

    return true;
}

} // namespace DBRecovery::InnoDB::v2
