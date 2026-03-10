#ifndef DBRECOVERY_CATALOG_SCHEMA_DEFS_HPP
#define DBRECOVERY_CATALOG_SCHEMA_DEFS_HPP

#include <string>
#include <map>
#include <vector>
#include <optional>
#include <iostream>

namespace dbrecovery {
namespace catalog {

// MySQL 8.0 列定义（完整语义，参考 sql/dd/types/column.h）
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
    
    // MySQL 8.0 新增字段（用于精确解析）
    int datetime_precision = -1;   // DATETIME/TIMESTAMP 的小数位精度 (0-6)
    int numeric_scale = -1;        // DECIMAL 的小数位数
    int numeric_precision = -1;    // 数值类型的总位数
    
    // 行格式相关（DYNAMIC/COMPRESSED）
    bool is_off_page = false;      // 是否存储在溢出页（BLOB）
    uint32_t virtual_gen_store_type = 0; // VIRTUAL/STORED 的存储类型 (mysql 8.0.13+)
    
    // ✅ 新增：补充字段以支持完整解析
    bool is_signed = true;         // 数值类型是否有符号（默认true，兼容INT等）
    uint32_t flags = 0;            // 列标志位（如 PRI_KEY, NOT_NULL 等）
    uint32_t col_flags = 0;        // MySQL 8.0 列标志
    std::optional<std::string> comment; // 列注释
    
    // ✅ 新增：用于处理不同行格式的字段
    bool is_variable_length = false; // 是否变长字段
    uint32_t actual_column_size = 0; // 实际列大小（考虑编码）
};

// 索引定义（PRIMARY / UNIQUE / SECONDARY，参考 sql/dd/types/index.h）
struct IndexInfo {
    std::string name;
    bool is_primary;
    std::vector<std::string> columns;       // 索引列名列表
    std::vector<uint32_t> column_offsets;   // 索引列在表中的偏移位置（用于快速定位）
    bool is_unique;
    uint32_t algorithm = 0;                 // BTREE(0), HASH(1) 等
    bool is_visible = true;                 // MySQL 8.0 新增：索引是否可见
};

// 分区信息（MySQL 8.0+ 常用 RANGE/LIST/KEY/HASH，参考 sql/dd/types/partition.h）
struct PartitionInfo {
    std::string method;                     // "RANGE", "LIST", "HASH", "KEY"
    std::vector<std::string> column_names;  // 分区键列名（支持多列分区）
    std::vector<std::string> partition_defs;
    
    // RANGE/LIST 分区的边界值
    struct PartitionBound {
        std::vector<std::string> values;
        bool is_maxvalue = false;
    };
    std::vector<PartitionBound> bounds;
    
    uint32_t num_partitions = 0;
    std::optional<std::string> subpartition_method; // SUBPARTITION BY HASH/KEY
};

// 表级元数据（参考 sql/dd/table.h）
struct TableSchema {
    std::string table_name;
    uint32_t row_format;  // DYNAMIC=1, COMPACT=0, REDUNDANT=2
    uint32_t page_size;   // 4096/8192/.../65536
    std::map<uint32_t, ColumnInfo> columns; // key = ordinal position (1-based)
    std::vector<IndexInfo> indexes;
    std::optional<PartitionInfo> partition_info;
    std::string engine = "InnoDB";
    
    uint64_t table_id = 0;                // InnoDB 内部表 ID（用于关联 SDI）
    uint32_t flags = 0;                   // FSP_FLAGS (页空间标志)
    uint32_t auto_inc_value = 0;          // AUTO_INCREMENT 当前值
    
    void debug_print() const;
};

// MySQL 8.0 类型常量（参考 include/mysql/mysql_type.h）
namespace mysql_types {
    constexpr uint8_t DECIMAL     = 0;
    constexpr uint8_t TINY        = 1;
    constexpr uint8_t SHORT       = 2;
    constexpr uint8_t LONG        = 3;
    constexpr uint8_t FLOAT       = 4;
    constexpr uint8_t DOUBLE      = 5;
    constexpr uint8_t NULL_TYPE   = 6;
    constexpr uint8_t TIMESTAMP   = 7;
    constexpr uint8_t LONGLONG    = 8;
    constexpr uint8_t INT24       = 9;
    constexpr uint8_t DATE        = 10;
    constexpr uint8_t TIME        = 11;
    constexpr uint8_t DATETIME    = 12;
    constexpr uint8_t YEAR        = 13;
    constexpr uint8_t NEWDATE     = 14;
    constexpr uint8_t VARCHAR     = 15;
    constexpr uint8_t BIT         = 16;
    constexpr uint8_t TIMESTAMP2  = 17;
    constexpr uint8_t DATETIME2   = 18;
    constexpr uint8_t TIME2       = 19;
    constexpr uint8_t JSON        = 245;
    constexpr uint8_t NEWDECIMAL  = 246;
    constexpr uint8_t ENUM        = 247;
    constexpr uint8_t SET         = 248;
    constexpr uint8_t TINY_BLOB   = 249;
    constexpr uint8_t MEDIUM_BLOB  = 250;
    constexpr uint8_t LONG_BLOB    = 251;
    constexpr uint8_t BLOB         = 252;
    constexpr uint8_t VAR_STRING   = 253;
    constexpr uint8_t STRING       = 254;
    constexpr uint8_t GEOMETRY     = 255;
}

// 行格式常量
namespace row_formats {
    constexpr uint32_t REDUNDANT = 0;
    constexpr uint32_t COMPACT   = 1;
    constexpr uint32_t DYNAMIC   = 2;
    constexpr uint32_t COMPRESSED = 3;
}

} // namespace catalog
} // namespace dbrecovery

#endif // DBRECOVERY_CATALOG_SCHEMA_DEFS_HPP
