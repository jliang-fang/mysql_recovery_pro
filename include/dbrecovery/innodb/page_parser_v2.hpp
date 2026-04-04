#ifndef DBRECOVERY_INNODB_PAGE_PARSER_V2_HPP
#define DBRECOVERY_INNODB_PAGE_PARSER_V2_HPP

/**
 * @file page_parser_v2.hpp
 * @brief MySQL 5.6-9.0 全版本兼容页解析层架构
 *
 * 架构设计原则：
 * 1. 策略模式：不同版本使用不同的解析策略
 * 2. 工厂模式：自动创建对应版本的解析器
 * 3. 模板方法：统一接口，差异化实现
 * 4. 零拷贝读取：使用 mmap + io_uring 提升性能
 */

#include <cstdint>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <optional>
#include <array>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_types_v2.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== PageParserV2: 统一页解析器 ====================
/**
 * @brief PageParserV2: 统一页解析器（V2 版本）
 *
 * 提供统一的页解析接口，自动识别版本和行格式
 */
class PageParserV2 {
public:
    explicit PageParserV2(const uint8_t* data, size_t size);

    // 解析页
    PageParseResult parse();

    // 获取解析结果
    const PageParseResult& result() const { return result_; }
    PageParseResult& result() { return result_; }

    // 重新解析（使用指定版本）
    PageParseResult parse_as(MySQLVersion version);

private:
    const uint8_t* data_;
    size_t size_;
    PageParseResult result_;
    std::unique_ptr<IPageParserStrategy> parser_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_PARSER_V2_HPP
