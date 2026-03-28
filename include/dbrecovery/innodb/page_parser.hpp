#ifndef DBRECOVERY_INNODB_PAGE_PARSER_HPP
#define DBRECOVERY_INNODB_PAGE_PARSER_HPP

#include <vector>
#include <cstdint>
#include <cstddef>

// ✅ 修复：移除重复的常量定义，统一使用 page_defs.hpp
// page_defs.hpp 已包含所有必要的 InnoDB 常量定义

#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ✅ 修复：移除重复的 Record 和 PageHeader 结构体定义
// 这些结构体已在 page_defs.hpp 中定义

class PageParser {
public:
    explicit PageParser(const uint8_t* buffer, size_t size);

    bool parse_header();
    
    std::vector<Record> parse_records() const;

    // ✅ 新增：获取页头信息
    [[nodiscard]] const PageHeader& header() const { return header_; }
    
    // ✅ 新增：获取原始数据指针
    const uint8_t* data() const;
    size_t size() const;
    
    // ✅ 新增：获取页类型
    [[nodiscard]] uint16_t type() const { return page_type_; }
    
    // ✅ 新增：获取页级别（B+树高度）
    [[nodiscard]] uint16_t level() const { return page_level_; }
    
    // ✅ 新增：验证页链闭环
    bool validate_page_chain() const;

private:
    uint16_t page_type_;
    uint16_t flags_;
    uint16_t page_level_;
    PageHeader header_;
    const uint8_t* buffer_;
    size_t size_;
    
    // 页链信息（用于校验）
    uint32_t fil_prev_ = 0xFFFFFFFF;
    uint32_t fil_next_ = 0xFFFFFFFF;
    uint32_t page_offset_ = 0;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_PARSER_HPP
