#ifndef DBRECOVERY_INNODB_SDI_TYPES_HPP
#define DBRECOVERY_INNODB_SDI_TYPES_HPP

#include <string>
#include <vector>
#include <map>

namespace dbrecovery {
namespace innodb {

// MySQL 8.0 列定义（完整语义）
struct ColumnInfo {
    std::string name;
    uint32_t mysql_type_id;       // 如: MYSQL_TYPE_VARCHAR=15, MYSQL_TYPE_JSON=245
    bool is_nullable;             // NULL / NOT NULL
    std::optional<std::string> default_value;  // DEFAULT 'x' 或 DEFAULT NOW()
    uint32_t column_length;       // 字符长度（VARCHAR）或字节长度（INT）
    uint32_t charset_id;          // 如: utf8mb4_0900_ai_ci=255
    uint32_t collation_id;
    std::optional<std::string> generated_expr;  // VIRTUAL/STORED 表达式
    bool is_auto_increment;
};

// 索引定义（PRIMARY / UNIQUE / SECONDARY）
struct IndexInfo {
    std::string name;
    bool is_primary;
    std::vector<std::string> columns;
    bool is_unique;
};

// 表级元数据
struct TableSchema {
    std::string table_name;
    uint32_t row_format;  // DYNAMIC=1, COMPACT=0, REDUNDANT=2
    uint32_t page_size;   // 4096/8192/.../65536
    std::map<uint32_t, ColumnInfo> columns; // key = ordinal position (1-based)
    std::vector<IndexInfo> indexes;
};

// SDI 描述符：用于缓存解析后的表结构信息
struct SDIDescriptor {
    uint32_t space_id;
    TableSchema table_schema;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SDI_TYPES_HPP
