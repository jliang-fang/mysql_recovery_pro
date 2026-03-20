#include <cstdint>
#include "dbrecovery/innodb/v2/page_parser.hpp"
#include <cstring>

namespace DBRecovery {
namespace InnoDB {
namespace v2 {

// ✅ 新增：InnoDB 物理页常量定义（参考 MySQL 8.0 fil0fil.h）
constexpr size_t FIL_PAGE_SPACE_ID = 0;
constexpr size_t FIL_PAGE_OFFSET = 4;
constexpr size_t FIL_PAGE_TYPE = 24; // 修复：原代码错误地使用了 offset 0
constexpr size_t FIL_PAGE_NEXT = 26;
constexpr size_t FIL_PAGE_PREV = 20;
constexpr size_t FIL_PAGE_DATA = 38;

PageParser::PageParser(const uint8_t* buffer, size_t size)
    : buffer_(buffer), size_(size) {}

bool PageParser::parse() noexcept {
    // 1. 基础校验：页大小必须是标准 InnoDB 页大小之一
    if (!buffer_ || (size_ != 4096 && size_ != 8192 && size_ != 16384 && 
                     size_ != 32768 && size_ != 65536)) {
        return false;
    }

    // 2. 解析 FIL_PAGE_TYPE (offset 24)
    uint16_t raw_type = 0;
    std::memcpy(&raw_type, buffer_ + FIL_PAGE_TYPE, sizeof(uint16_t));
    
    // ✅ 新增：使用 dbrecovery::innodb 命名空间中的统一页类型常量
    bool is_valid_type = false;
    switch (raw_type) {
        case dbrecovery::innodb::FIL_PAGE_INDEX:
        case dbrecovery::innodb::FIL_PAGE_SDI:
        case dbrecovery::innodb::FIL_PAGE_TYPE_BLOB:
            // ✅ 新增：兼容旧版 FIL_PAGE_RTREE（9970），但标记为废弃
            if (raw_type == 9970) { // FIL_PAGE_RTREE / deprecated
                std::cerr << "[WARN] Page type 9970 (FIL_PAGE_RTREE) is deprecated in MySQL 8.0, "
                             "use FIL_PAGE_UNDO_LOG instead." << std::endl;
            }
            is_valid_type = true;
            break;
        default:
            // 非数据页（如 UNDO、SYS）暂不解析
            break;
    }
    
    if (!is_valid_type) {
        return false;
    }

    // 4. 解析 PAGE_HEADER (offset 38 = FIL_PAGE_DATA)
    const uint8_t* header_ptr = buffer_ + FIL_PAGE_DATA;

    // ✅ 按 MySQL 8.0 官方偏移读取（storage/innobase/include/page0page.h）
    // 所有偏移均相对于 PAGE_HEADER 起始位置（即 buffer_ + 38）
    std::memcpy(&header_.n_dir_slots, header_ptr + 0, sizeof(uint16_t));
    std::memcpy(&header_.heap_top,    header_ptr + 2, sizeof(uint16_t));
    std::memcpy(&header_.free,        header_ptr + 6, sizeof(uint16_t));
    std::memcpy(&header_.garbage,     header_ptr + 8, sizeof(uint16_t));
    std::memcpy(&header_.insert_offset, header_ptr + 10, sizeof(uint32_t));
    std::memcpy(&header_.n_recs,      header_ptr + 12, sizeof(uint16_t));
    
    // ✅ 修正：MySQL 8.0 的 max_trx_id 是 6 字节（48位），只读取低6字节
    std::memset(&header_.max_trx_id, 0, sizeof(uint64_t)); // 清零高2字节
    std::memcpy(&header_.max_trx_id, header_ptr + 14, 6);
    
    std::memcpy(&header_.page_level,  header_ptr + 22, sizeof(uint16_t));
    std::memcpy(&header_.page_index_id, header_ptr + 24, sizeof(uint64_t));

    return true;
}

} // namespace v2
} // namespace InnoDB
} // namespace DBRecovery
