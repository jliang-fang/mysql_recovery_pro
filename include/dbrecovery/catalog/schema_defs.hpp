#ifndef DBRECOVERY_CATALOG_SCHEMA_DEFS_HPP
#define DBRECOVERY_CATALOG_SCHEMA_DEFS_HPP

#include <string>
#include <map>
#include <vector>
#include <optional>
#include <iostream>

namespace dbrecovery {
namespace catalog {

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

// 分区信息（MySQL 8.0+ 常用 RANGE/LIST/HASH）
struct PartitionInfo {
    std::string method; // "RANGE", "LIST", "HASH"
    std::string column_name;
    std::vector<std::string> partition_defs;
};

// 表级元数据
struct TableSchema {
    std::string table_name;
    uint32_t row_format;  // DYNAMIC=1, COMPACT=0, REDUNDANT=2
    uint32_t page_size;   // 4096/8192/.../65536
    std::map<uint32_t, ColumnInfo> columns; // key = ordinal position (1-based)
    std::vector<IndexInfo> indexes;
    std::optional<PartitionInfo> partition_info;
    std::string engine = "InnoDB";
    
    void debug_print() const;
};

} // namespace catalog
} // namespace dbrecovery

#endif // DBRECOVERY_CATALOG_SCHEMA_DEFS_HPP
