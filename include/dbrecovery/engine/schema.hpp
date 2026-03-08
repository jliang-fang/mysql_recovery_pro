#pragma once

#include <string>
#include <vector>
#include <map>
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace engine {

// 引擎层 Schema 定义，整合 catalog 层定义以避免重复
struct Schema {
    std::string table_name;
    uint32_t row_format;  // DYNAMIC=1, COMPACT=0, REDUNDANT=2
    uint32_t page_size;   // 4096/8192/.../65536
    
    // 列定义：key = ordinal position (1-based)
    std::map<uint32_t, catalog::ColumnInfo> columns;
    
    // 索引信息
    std::vector<catalog::IndexInfo> indexes;
    
    // 引擎名称
    std::string engine = "InnoDB";
    
    // MySQL 版本兼容性支持（用于未来扩展）
    uint32_t mysql_version_major = 8;
    uint32_t mysql_version_minor = 0;
};

} // namespace engine
} // namespace dbrecovery
