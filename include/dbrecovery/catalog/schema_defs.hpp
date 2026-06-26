#ifndef DBRECOVERY_CATALOG_SCHEMA_DEFS_HPP
#define DBRECOVERY_CATALOG_SCHEMA_DEFS_HPP

#include <string>
#include <map>
#include <vector>
#include <optional>
#include <iostream>
#include <algorithm>

// ✅ P0-1 修复：使用系统 nlohmann/json 库
#include <nlohmann/json.hpp>

// ✅ 新增：包含 page_defs.hpp 以获取 mysql_types 常量定义
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace catalog {

// MySQL 8.0 列定义（完整语义，参考 sql/dd/types/column.h）
struct ColumnInfo {
    std::string name;
    uint32_t mysql_type_id;       // 如: MYSQL_TYPE_VARCHAR=15, MYSQL_TYPE_JSON=245
    bool is_nullable = true;        // NULL / NOT NULL (default to nullable for safety)
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

    // ✅ 新增：ENUM/SET 类型值列表
    std::vector<std::string> enum_values;  // ENUM 类型的可选值列表
    std::vector<std::string> set_values;   // SET 类型的可选值列表

    // ✅ 关键：InnoDB物理存储顺序
    uint32_t physical_pos = 0;     // InnoDB物理存储位置（不同于ordinal position）
    uint32_t ordinal = 0;          // Schema定义顺序（用户看到的列顺序）
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
            return false;
        }

        if (columns.empty()) {
            return false;
        }

        // 验证列定义
        for (const auto& pair : columns) {
            const auto& col = pair.second;
            if (col.name.empty()) {
                return false;
            }

            // 验证 MySQL 类型
            if (col.mysql_type_id > 255) {
                return false;
            }
        }
        
        // 验证索引定义
        for (const auto& index : indexes) {
            if (index.columns.empty()) {
                return false;
            }

            // 验证索引列存在于表中
            // ✅ P2-1 优化：使用 std::any_of 算法替代原始循环
            for (const auto& col_name : index.columns) {
                bool found = std::any_of(columns.begin(), columns.end(),
                    [&col_name](const auto& pair) { return pair.second.name == col_name; });

                if (!found) {
                    return false;
                }
            }
        }

        return true;
    }

    // ✅ 新增：判断列是否为固定长度（用于 InnoDB DYNAMIC/COMPRESSED 存储顺序）
    // 参考 ibd2sql column_type.py 的 is_var 字段
    // is_var=False 的类型: DECIMAL, TINY, SHORT, LONG, FLOAT, DOUBLE, LONGLONG, INT24,
    //                       DATE, TIME, DATETIME, YEAR, NEWDATE, TIMESTAMP, TIMESTAMP2, DATETIME2, TIME2,
    //                       BIT, NEWDECIMAL, ENUM, SET
    // is_var=True 的类型: VARCHAR, TINY_BLOB, MEDIUM_BLOB, LONG_BLOB, BLOB, VAR_STRING, STRING, GEOMETRY, JSON, VECTOR
    static bool is_fixed_length(const ColumnInfo& col) {
        switch (col.mysql_type_id) {
            // Integer types - fixed size
            case dbrecovery::innodb::mysql_types::TINY:       // 2
            case dbrecovery::innodb::mysql_types::SHORT:      // 3
            case dbrecovery::innodb::mysql_types::LONG:       // 4
            case dbrecovery::innodb::mysql_types::LONGLONG:   // 9
            case dbrecovery::innodb::mysql_types::INT24:      // 10
                return true;
            // Floating point types - fixed size
            case dbrecovery::innodb::mysql_types::FLOAT:      // 5
            case dbrecovery::innodb::mysql_types::DOUBLE:     // 6
                return true;
            // Date and time types - fixed size
            case dbrecovery::innodb::mysql_types::DATE:       // 11
            case dbrecovery::innodb::mysql_types::TIME:       // 12 (old format, 3 bytes)
            case dbrecovery::innodb::mysql_types::DATETIME:   // 13 (old format, 8 bytes)
            case dbrecovery::innodb::mysql_types::YEAR:       // 14
            case dbrecovery::innodb::mysql_types::NEWDATE:    // 15
            case dbrecovery::innodb::mysql_types::TIMESTAMP:  // 7
            case dbrecovery::innodb::mysql_types::TIMESTAMP2: // 18
            case dbrecovery::innodb::mysql_types::DATETIME2:  // 19
            case dbrecovery::innodb::mysql_types::TIME2:      // 20
                return true;
            // BIT - fixed size based on bit count
            case dbrecovery::innodb::mysql_types::BIT:        // 17
                return true;
            // DECIMAL/NEWDECIMAL - fixed size based on precision/scale
            case dbrecovery::innodb::mysql_types::NEWDECIMAL: // 21
                return true;
            // ENUM - fixed size (1-2 bytes based on value count)
            case dbrecovery::innodb::mysql_types::ENUM:       // 22
                return true;
            // SET - fixed size (1-8 bytes based on member count)
            case dbrecovery::innodb::mysql_types::SET:        // 23
                return true;
            // Variable-length types
            // VARCHAR (16), VAR_STRING (28), STRING/CHAR (29) - variable for utf8mb4
            // BLOB variants (24, 25, 26, 27), GEOMETRY (30), JSON (31), VECTOR (32)
            default:
                return false;
        }
    }

    // ✅ 新增：计算列大小（用于行格式适配）
    uint32_t get_column_size(const ColumnInfo& col) const {
        switch (col.mysql_type_id) {
            case dbrecovery::innodb::mysql_types::TINY:
                return 1;
            case dbrecovery::innodb::mysql_types::SHORT:
                return 2;
            case dbrecovery::innodb::mysql_types::LONG:
                return 4;
            case dbrecovery::innodb::mysql_types::LONGLONG:
                return 8;
            case dbrecovery::innodb::mysql_types::INT24:
                return 3;
            case dbrecovery::innodb::mysql_types::FLOAT:
                return 4;
            case dbrecovery::innodb::mysql_types::DOUBLE:
                return 8;
            case dbrecovery::innodb::mysql_types::VARCHAR:
                // 考虑字符集编码（utf8mb4 最多4字节/字符）
                return col.column_length * 4 + 2; // 长度前缀
            case dbrecovery::innodb::mysql_types::DATE:
                return 3;
            case dbrecovery::innodb::mysql_types::DATETIME:
                return 5;
            case dbrecovery::innodb::mysql_types::DATETIME2:
                // 精度决定大小
                if (col.datetime_precision <= 2) return 3;
                if (col.datetime_precision <= 4) return 4;
                return 5;
            case dbrecovery::innodb::mysql_types::TIME2:
                return 3;
            case dbrecovery::innodb::mysql_types::YEAR:
                return 1;
            case dbrecovery::innodb::mysql_types::BIT:
                return (col.column_length + 7) / 8;
            case dbrecovery::innodb::mysql_types::JSON:
                // JSON 存储为 BLOB
                return col.column_length;
            default:
                // BLOB/TEXT 类型
                if (col.mysql_type_id >= dbrecovery::innodb::mysql_types::TINY_BLOB &&
                    col.mysql_type_id <= dbrecovery::innodb::mysql_types::BLOB) {
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
    bool from_json(const nlohmann::json& j) {
        // 支持 "name" 和 "table_name" 两种字段名
        if (j.contains("table_name")) {
            table_name = j["table_name"].get<std::string>();
        } else if (j.contains("name")) {
            table_name = j["name"].get<std::string>();
        }

        if (j.contains("table_id")) {
            table_id = j["table_id"].get<uint64_t>();
        }

        if (j.contains("columns") && j["columns"].is_array()) {
            uint32_t ordinal = 1;
            static bool debug_schema = false;
            for (const auto& col_json : j["columns"]) {
                catalog::ColumnInfo col;
                if (col_json.contains("name")) {
                    col.name = col_json["name"].get<std::string>();
                }

                // Debug: print all column fields for the first few columns
                if (!debug_schema) {
                    std::cerr << "[DEBUG SDI FIELDS] Column '" << col.name << "' fields: ";
                    for (auto it = col_json.begin(); it != col_json.end(); ++it) {
                        std::cerr << it.key() << " ";
                    }
                    std::cerr << "\n";
                }

                if (col_json.contains("type")) {
                    // SDI stores InnoDB type IDs directly as numbers
                    // Reference: ibd2sql/ibd2sql/innodb_page/column_type.py
                    if (col_json["type"].is_number()) {
                        col.mysql_type_id = col_json["type"].get<uint32_t>();
                    } else if (col_json["type"].is_string()) {
                        // Some SDI versions might store type as string
                        std::string type_str = col_json["type"].get<std::string>();
                        // Map string names to InnoDB type IDs
                        if (type_str == "DECIMAL") col.mysql_type_id = 1;
                        else if (type_str == "TINY") col.mysql_type_id = 2;
                        else if (type_str == "SHORT") col.mysql_type_id = 3;
                        else if (type_str == "LONG") col.mysql_type_id = 4;
                        else if (type_str == "FLOAT") col.mysql_type_id = 5;
                        else if (type_str == "DOUBLE") col.mysql_type_id = 6;
                        else if (type_str == "TIMESTAMP") col.mysql_type_id = 8;
                        else if (type_str == "LONGLONG") col.mysql_type_id = 9;
                        else if (type_str == "INT24") col.mysql_type_id = 10;
                        else if (type_str == "DATE") col.mysql_type_id = 11;
                        else if (type_str == "TIME") col.mysql_type_id = 12;
                        else if (type_str == "DATETIME") col.mysql_type_id = 13;
                        else if (type_str == "YEAR") col.mysql_type_id = 14;
                        else if (type_str == "NEWDATE") col.mysql_type_id = 15;
                        else if (type_str == "VARCHAR") col.mysql_type_id = 16;
                        else if (type_str == "BIT") col.mysql_type_id = 17;
                        else if (type_str == "TIMESTAMP2") col.mysql_type_id = 18;
                        else if (type_str == "DATETIME2") col.mysql_type_id = 19;
                        else if (type_str == "TIME2") col.mysql_type_id = 20;
                        else if (type_str == "NEWDECIMAL") col.mysql_type_id = 21;
                        else if (type_str == "ENUM") col.mysql_type_id = 22;
                        else if (type_str == "SET") col.mysql_type_id = 23;
                        else if (type_str == "TINY_BLOB") col.mysql_type_id = 24;
                        else if (type_str == "MEDIUM_BLOB") col.mysql_type_id = 25;
                        else if (type_str == "LONG_BLOB") col.mysql_type_id = 26;
                        else if (type_str == "BLOB") col.mysql_type_id = 27;
                        else if (type_str == "VAR_STRING") col.mysql_type_id = 28;
                        else if (type_str == "STRING") col.mysql_type_id = 29;
                        else if (type_str == "GEOMETRY") col.mysql_type_id = 30;
                        else if (type_str == "JSON") col.mysql_type_id = 31;
                        else col.mysql_type_id = 0;
                    }
                    if (!debug_schema) {
                        std::cerr << "[DEBUG SCHEMA] Column '" << col.name << "' type=" << col.mysql_type_id << "\n";
                    }
                }
                if (col_json.contains("is_nullable")) {
                    col.is_nullable = col_json["is_nullable"].get<bool>();
                }
                if (!debug_schema) {
                    std::cerr << "[DEBUG SCHEMA] Column '" << col.name << "' is_nullable=" << col.is_nullable << std::endl;
                }
                // Parse is_signed for numeric types (affects unsigned handling)
                if (col_json.contains("is_signed")) {
                    col.is_signed = col_json["is_signed"].get<bool>();
                } else if (col_json.contains("is_unsigned")) {
                    col.is_signed = !col_json["is_unsigned"].get<bool>();
                }
                if (!debug_schema && col.mysql_type_id == 9) {
                    std::cerr << "[DEBUG SCHEMA] Column '" << col.name << "' type=" << col.mysql_type_id
                              << " is_signed=" << col.is_signed << "\n";
                }
                // Parse DECIMAL precision/scale for type 21 (NEWDECIMAL)
                if (col_json.contains("numeric_precision")) {
                    col.numeric_precision = col_json["numeric_precision"].get<int>();
                }
                if (col_json.contains("numeric_scale")) {
                    col.numeric_scale = col_json["numeric_scale"].get<int>();
                }
                // Parse datetime_precision for temporal types
                if (col_json.contains("datetime_precision")) {
                    col.datetime_precision = col_json["datetime_precision"].get<int>();
                } else if (col_json.contains("decimals")) {
                    // Some SDI versions use "decimals" for fractional seconds precision
                    col.datetime_precision = col_json["decimals"].get<int>();
                }
                // Also parse char_length for VARCHAR
                if (col_json.contains("char_length")) {
                    col.column_length = col_json["char_length"].get<uint32_t>();
                } else if (col_json.contains("length")) {
                    col.column_length = col_json["length"].get<uint32_t>();
                }
                // Parse ENUM/SET elements
                // SDI stores element names in base64 encoding (MySQL 8.0+)
                // ✅ 修复：区分 ENUM 和 SET，将元素放入正确的列表
                if (col_json.contains("elements") && col_json["elements"].is_array()) {
                    std::vector<std::string>* target_values =
                        (col.mysql_type_id == 23) ? &col.set_values : &col.enum_values;
                    std::cerr << "[DEBUG ENUM] Column '" << col.name << "' has "
                              << col_json["elements"].size() << " elements\n";
                    for (const auto& elem : col_json["elements"]) {
                        if (elem.contains("name")) {
                            std::string encoded = elem["name"].get<std::string>();
                            // Decode base64: SDI stores ENUM values as base64
                            // Simple base64 decode
                            static const std::string base64_chars =
                                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                            std::string decoded;
                            std::vector<int> T(256, -1);
                            for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;
                            int val = 0, valb = -8;
                            for (unsigned char c : encoded) {
                                if (T[c] == -1) break;
                                val = (val << 6) + T[c];
                                valb += 6;
                                if (valb >= 0) {
                                    decoded.push_back(char((val >> valb) & 0xFF));
                                    valb -= 8;
                                }
                            }
                            std::cerr << "[DEBUG ENUM]  element: encoded='" << encoded
                                      << "' decoded='" << decoded << "'\n";
                            target_values->push_back(decoded);
                        }
                    }
                    std::cerr << "[DEBUG ENUM] Column '" << col.name << "' parsed "
                              << target_values->size() << " values\n";
                }

                // Parse physical_pos - critical for column storage order
                // InnoDB stores column DATA by physical_pos
                // But variable-length SIZES are stored by ordinal_position (schema definition order)
                if (col_json.contains("physical_pos")) {
                    col.physical_pos = col_json["physical_pos"].get<uint32_t>();
                }
                // Also read ordinal_position for variable-length size byte ordering
                // This is the schema definition order (CREATE TABLE column order)
                if (col_json.contains("ordinal_position")) {
                    col.ordinal = col_json["ordinal_position"].get<uint32_t>();
                }
                // Fallback: if physical_pos not present, use ordinal_position
                if (col.physical_pos == 0 && col_json.contains("ordinal_position")) {
                    col.physical_pos = col_json["ordinal_position"].get<uint32_t>();
                }

                // Debug: show physical_pos value for date/datetime columns
                if (col.name == "col_date" || col.name == "col_datetime" || col.name == "col_decimal") {
                    std::cerr << "[DEBUG POS] Column '" << col.name << "' physical_pos=" << col.physical_pos
                              << " type=" << col.mysql_type_id << "\n";
                }

                // Skip InnoDB system columns (DB_TRX_ID, DB_ROLL_PTR, DB_ROW_ID)
                // These are internal columns not part of user table schema
                if (col.name == "DB_TRX_ID" || col.name == "DB_ROLL_PTR" || col.name == "DB_ROW_ID") {
                    continue;
                }
                // If ordinal wasn't set from SDI, use the iteration counter
                if (col.ordinal == 0) {
                    col.ordinal = ordinal;
                }
                columns[ordinal++] = col;
            }
            debug_schema = true;  // Only print debug once

            // Sort columns by physical_pos (InnoDB storage order)
            // CRITICAL: InnoDB stores columns in physical_pos order, NOT grouped by fixed/variable!
            // Each column is stored in its physical_pos position regardless of whether it's fixed or variable length.
            if (!columns.empty()) {
                std::vector<std::pair<uint32_t, ColumnInfo>> cols_vec;
                for (const auto& p : columns) {
                    cols_vec.push_back(p);
                }
                // Sort by physical_pos (this is the InnoDB storage order)
                std::sort(cols_vec.begin(), cols_vec.end(),
                    [](const auto& a, const auto& b) {
                        return a.second.physical_pos < b.second.physical_pos;
                    });
                columns.clear();
                uint32_t new_ordinal = 1;
                for (const auto& p : cols_vec) {
                    std::cerr << "[DEBUG SORT] Column '" << p.second.name << "' new_ordinal=" << new_ordinal
                              << " physical_pos=" << p.second.physical_pos
                              << " ordinal=" << p.second.ordinal
                              << " type=" << p.second.mysql_type_id
                              << " is_fixed=" << TableSchema::is_fixed_length(p.second) << "\n";
                    columns[new_ordinal++] = p.second;
                }
            }
        }

        return validate();
    }
    
    // ✅ 新增：转换为 nlohmann/json 对象
    nlohmann::json to_json() const {
        nlohmann::json j;
        j["name"] = table_name;
        j["engine"] = engine;
        j["row_format"] = row_format;
        
        nlohmann::json columns_array = nlohmann::json::array();
        for (const auto& pair : columns) {
            nlohmann::json col_json;
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

} // namespace catalog
} // namespace dbrecovery

#endif // DBRECOVERY_CATALOG_SCHEMA_DEFS_HPP
