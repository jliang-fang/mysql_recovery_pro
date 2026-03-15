#ifndef DBRECOVERY_CATALOG_SCHEMA_DEFS_HPP
#define DBRECOVERY_CATALOG_SCHEMA_DEFS_HPP

#include <string>
#include <map>
#include <vector>
#include <optional>
#include <iostream>

// ✅ 新增：nlohmann/json 依赖声明
#ifdef JSON_HPP_INCLUDED
// 使用 nlohmann/json 库
#else
// 如果未定义 JSON_HPP_INCLUDED，需要包含 json 头文件或确保已定义相关类型
// 这里使用前向声明避免编译错误
namespace nlohmann {
    class json;
}
#endif

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
    uint32_t flags = 0;            // 列标志位（如 PRI_KEY, NOT_NULL 等）
    uint32_t col_flags = 0;        // MySQL 8.0 列标志
    std::optional<std::string> comment; // 列注释
    
    // ✅ 新增：用于处理不同行格式的字段
    bool is_variable_length = false; // 是否变长字段（VARCHAR, VARBINARY等）
    uint32_t actual_column_size = 0; // 实际列大小（考虑编码，如utf8mb4可能占用3-4字节/字符）
    
    // ✅ 新增：修复 btree.cpp 中的字段解析逻辑
    bool is_signed = true;         // 数值类型是否有符号（默认true，兼容INT等）
    int datetime_precision = -1;   // DATETIME/TIMESTAMP 的小数位精度 (0-6)
    int numeric_scale = -1;        // DECIMAL 的小数位数
    int numeric_precision = -1;    // 数值类型的总位数
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
    
    void debug_print() const {
        std::cout << "Table: " << table_name << std::endl;
        std::cout << "Engine: " << engine << ", Row Format: " << row_format 
                  << ", Page Size: " << page_size << std::endl;
        std::cout << "Columns:" << std::endl;
        for (const auto& pair : columns) {
            const auto& col = pair.second;
            std::cout << "  [" << pair.first << "] " << col.name 
                      << " (type=" << col.mysql_type_id << ", nullable=" 
                      << col.is_nullable << ")" << std::endl;
        }
        std::cout << "Indexes: " << indexes.size() << std::endl;
    }
    
    // ✅ 新增：表结构验证
    bool validate() const {
        if (table_name.empty()) {
            std::cerr << "[ERROR] Table name is empty" << std::endl;
            return false;
        }
        
        if (columns.empty()) {
            std::cerr << "[ERROR] Table has no columns" << std::endl;
            return false;
        }
        
        // 验证列定义
        for (const auto& pair : columns) {
            const auto& col = pair.second;
            if (col.name.empty()) {
                std::cerr << "[ERROR] Column has empty name at ordinal " << pair.first << std::endl;
                return false;
            }
            
            // 验证 MySQL 类型
            if (col.mysql_type_id > 255) {
                std::cerr << "[ERROR] Invalid MySQL type ID " << col.mysql_type_id 
                          << " for column " << col.name << std::endl;
                return false;
            }
        }
        
        // 验证索引定义
        for (const auto& index : indexes) {
            if (index.columns.empty()) {
                std::cerr << "[ERROR] Index " << index.name << " has no columns" << std::endl;
                return false;
            }
            
            // 验证索引列存在于表中
            for (const auto& col_name : index.columns) {
                bool found = false;
                for (const auto& pair : columns) {
                    if (pair.second.name == col_name) {
                        found = true;
                        break;
                    }
                }
                
                if (!found) {
                    std::cerr << "[ERROR] Index " << index.name << " references non-existent column " 
                              << col_name << std::endl;
                    return false;
                }
            }
        }
        
        return true;
    }
    
    // ✅ 新增：计算列大小（用于行格式适配）
    uint32_t get_column_size(const ColumnInfo& col) const {
        switch (col.mysql_type_id) {
            case mysql_types::TINY:
                return 1;
            case mysql_types::SHORT:
                return 2;
            case mysql_types::LONG:
                return 4;
            case mysql_types::LONGLONG:
                return 8;
            case mysql_types::INT24:
                return 3;
            case mysql_types::FLOAT:
                return 4;
            case mysql_types::DOUBLE:
                return 8;
            case mysql_types::VARCHAR:
            case mysql_types::VAR_STRING:
                // 考虑字符集编码（utf8mb4 最多4字节/字符）
                return col.column_length * 4 + 2; // 长度前缀
            case mysql_types::CHAR:
                return col.column_length * 4;
            case mysql_types::DATE:
                return 3;
            case mysql_types::DATETIME:
            case mysql_types::TIMESTAMP:
                return 5;
            case mysql_types::DATETIME2:
            case mysql_types::TIMESTAMP2:
                // 精度决定大小
                if (col.datetime_precision <= 2) return 3;
                if (col.datetime_precision <= 4) return 4;
                return 5;
            case mysql_types::TIME:
                return 3;
            case mysql_types::TIME2:
                return 3;
            case mysql_types::YEAR:
                return 1;
            case mysql_types::BIT:
                return (col.column_length + 7) / 8;
            case mysql_types::JSON:
                // JSON 存储为 BLOB
                return col.column_length;
            default:
                // BLOB/TEXT 类型
                if (col.mysql_type_id >= mysql_types::TINY_BLOB && 
                    col.mysql_type_id <= mysql_types::BLOB) {
                    return 4; // 指针大小
                }
                return col.column_length;
        }
    }
    
    // ✅ 新增：计算可空列数量（用于 NULL 位图）
    uint32_t get_nullable_column_count() const {
        uint32_t count = 0;
        for (const auto& pair : columns) {
            if (pair.second.is_nullable) {
                count++;
            }
        }
        return count;
    }
    
    // ✅ 新增：获取列在表中的物理偏移
    std::optional<uint32_t> get_column_offset(const std::string& col_name) const {
        uint32_t offset = 0;
        for (const auto& pair : columns) {
            if (pair.second.name == col_name) {
                return offset;
            }
            offset += get_column_size(pair.second);
        }
        return std::nullopt;
    }
    
    // ✅ 新增：序列化为 JSON 字符串（用于 SDI 兼容）
    std::string to_json_string() const {
        return to_json().dump(2);
    }
    
    // ✅ 新增：从 JSON 字符串反序列化（用于 SDI 解析）
    bool from_json_string(const std::string& json_str) {
        try {
            auto j = nlohmann::json::parse(json_str);
            return from_json(j);
        } catch (...) {
            return false;
        }
    }
    
    // ✅ 新增：从 nlohmann/json 对象加载
    bool from_json(const class json& j) {
        if (j.contains("name")) {
            table_name = j["name"].get<std::string>();
        }
        
        if (j.contains("columns") && j["columns"].is_array()) {
            uint32_t ordinal = 1;
            for (const auto& col_json : j["columns"]) {
                catalog::ColumnInfo col;
                if (col_json.contains("name")) {
                    col.name = col_json["name"].get<std::string>();
                }
                if (col_json.contains("type")) {
                    col.mysql_type_id = col_json["type"].get<uint32_t>();
                }
                if (col_json.contains("is_nullable")) {
                    col.is_nullable = col_json["is_nullable"].get<bool>();
                }
                columns[ordinal++] = col;
            }
        }
        
        return validate();
    }
    
    // ✅ 新增：转换为 nlohmann/json 对象
    json to_json() const {
        json j;
        j["name"] = table_name;
        j["engine"] = engine;
        j["row_format"] = row_format;
        
        json columns_array = json::array();
        for (const auto& pair : columns) {
            json col_json;
            col_json["ordinal"] = pair.first;
            col_json["name"] = pair.second.name;
            col_json["type_id"] = pair.second.mysql_type_id;
            col_json["is_nullable"] = pair.second.is_nullable;
            columns_array.push_back(col_json);
        }
        j["columns"] = columns_array;
        
        return j;
    }
};

// MySQL 8.0 类型常量（参考 include/mysql/mysql_type.h）
namespace mysql_types {
    // 数值类型
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
    
    // 日期时间类型
    constexpr uint8_t DATE        = 10;
    constexpr uint8_t TIME        = 11;
    constexpr uint8_t DATETIME    = 12;
    constexpr uint8_t YEAR        = 13;
    constexpr uint8_t NEWDATE     = 14;
    
    // 字符串类型
    constexpr uint8_t VARCHAR     = 15;
    constexpr uint8_t BIT         = 16;
    constexpr uint8_t TIMESTAMP2  = 17;
    constexpr uint8_t DATETIME2   = 18;
    constexpr uint8_t TIME2       = 19;
    
    // JSON 和新类型
    constexpr uint8_t JSON        = 245;
    constexpr uint8_t NEWDECIMAL  = 246;
    constexpr uint8_t ENUM        = 247;
    constexpr uint8_t SET         = 248;
    constexpr uint8_t TINY_BLOB   = 249;
    constexpr uint8_t MEDIUM_BLOB = 250;
    constexpr uint8_t LONG_BLOB   = 251;
    constexpr uint8_t BLOB        = 252;
    constexpr uint8_t VAR_STRING  = 253;
    constexpr uint8_t STRING      = 254;
    constexpr uint8_t GEOMETRY    = 255;

    // 辅助函数：判断是否为 BLOB 类型
    inline bool is_blob_type(uint8_t type_id) {
        return (type_id >= TINY_BLOB && type_id <= GEOMETRY);
    }

    // 辅助函数：判断是否为日期时间类型
    inline bool is_datetime_type(uint8_t type_id) {
        return (type_id == DATE || type_id == DATETIME || type_id == TIMESTAMP ||
                type_id == TIME || type_id == YEAR ||
                type_id == NEWDATE || type_id == DATETIME2 || type_id == TIMESTAMP2 || type_id == TIME2);
    }

    // 辅助函数：判断是否为数值类型
    inline bool is_numeric_type(uint8_t type_id) {
        return (type_id <= DOUBLE || type_id == NEWDECIMAL);
    }
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
