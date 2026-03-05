#pragma once
#include <cstdint>
#include <cstddef>   // ← 关键修复：添加 size_t 定义
#include <vector>

namespace DBRecovery::InnoDB::v2 {

enum class PageType : uint16_t {
    FIL_PAGE_TYPE_ALLOCATED = 0,
    FIL_PAGE_INDEX        = 9969,
    FIL_PAGE_RTREE        = 9970,
    FIL_PAGE_SDI          = 9971,
    FIL_PAGE_TYPE_BLOB    = 9972,
    FIL_PAGE_COMPRESSED   = 9973,
};

struct PageHeader {
    uint16_t n_dir_slots;
    uint16_t heap_top;
    uint16_t free;
    uint16_t garbage;
    uint32_t insert_offset;
    uint16_t n_recs;
    uint64_t max_trx_id; // MySQL 8.0: 6 bytes (only lower 48 bits valid)
    uint16_t page_level;
    uint64_t page_index_id; // InnoDB index_id_t is 8 bytes
};

class PageParser {
public:
    PageParser(const uint8_t* buffer, size_t size);
    
    bool parse() noexcept;
    
    [[nodiscard]] PageType type() const { return type_; }
    [[nodiscard]] const PageHeader& header() const { return header_; }
    [[nodiscard]] const uint8_t* data() const { return buffer_ + 38; }
    
private:
    const uint8_t* buffer_;
    size_t size_;
    PageType type_{PageType::FIL_PAGE_TYPE_ALLOCATED};
    PageHeader header_{};
};

} // namespace DBRecovery::InnoDB::v2
