#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>

// 包含 nlohmann/json 头文件
#include <nlohmann/json.hpp>

namespace dbrecovery {
namespace catalog {

void TableSchema::debug_print() const {
    std::cout << "Table: " << table_name 
              << ", Engine: " << engine 
              << ", Row Format: " << static_cast<int>(row_format) << std::endl;
    
    std::cout << "Columns:" << std::endl;
    for (const auto& [pos, col] : columns) {
        std::cout << "  [" << pos << "] " << col.name 
                  << " (type=" << col.mysql_type_id << ", nullable=" << col.is_nullable
                  << ", length=" << col.column_length << ")" << std::endl;
    }
    
    std::cout << "Indexes:" << std::endl;
    for (const auto& idx : indexes) {
        std::cout << "  - " << idx.name << " (primary=" << idx.is_primary 
                  << ", unique=" << idx.is_unique << ")" << std::endl;
        for (const auto& col : idx.columns) {
            std::cout << "    Column: " << col << std::endl;
        }
    }
    
    if (partition_info.has_value()) {
        const auto& p = partition_info.value();
        std::cout << "Partitioning: method=" << p.method 
                  << ", columns=";
        for (const auto& col : p.column_names) {
            std::cout << col << " ";
        }
        std::cout << std::endl;
    }
}

// ✅ 新增：表结构验证函数
bool TableSchema::validate() const {
    // 检查引擎类型
    if (engine != "InnoDB") {
        std::cerr << "[ERROR] Unsupported engine: " << engine << std::endl;
        return false;
    }
    
    // 检查行格式
    if (row_format > row_formats::COMPRESSED) {
        std::cerr << "[ERROR] Invalid row format: " << row_format << std::endl;
        return false;
    }
    
    // ✅ 修复：确保包含正确的头文件以使用 UNIV_PAGE_SIZE_MIN 和 UNIV_PAGE_SIZE_MAX
    // 这些常量定义在 page_defs.hpp 中，需要确保包含该头文件
    if (page_size != 0 && 
        (page_size < dbrecovery::innodb::UNIV_PAGE_SIZE_MIN || 
         page_size > dbrecovery::innodb::UNIV_PAGE_SIZE_MAX)) {
        std::cerr << "[ERROR] Invalid page size: " << page_size << std::endl;
        return false;
    }
    
    // 检查列定义
    if (columns.empty()) {
        std::cerr << "[ERROR] Table must have at least one column" << std::endl;
        return false;
    }
    
    // 验证主键索引
    bool has_primary = false;
    for (const auto& idx : indexes) {
        if (idx.is_primary) {
            if (has_primary) {
                std::cerr << "[ERROR] Multiple primary keys defined" << std::endl;
                return false;
            }
            has_primary = true;
            
            // 验证主键列存在
            for (const auto& col_name : idx.columns) {
                bool found = false;
                for (const auto& [pos, col] : columns) {
                    if (col.name == col_name) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    std::cerr << "[ERROR] Primary key column not found: " << col_name << std::endl;
                    return false;
                }
            }
        }
    }
    
    // ✅ 增强：检查列大小总和
    uint32_t current_offset = 0;
    for (const auto& [pos, col] : columns) {
        // 计算固定长度部分
        if (!col.is_variable_length || col.mysql_type_id == mysql_types::VARCHAR) {
            uint32_t col_size = get_column_size(col);
            if (current_offset + col_size > 8192) { // InnoDB 行大小限制
                std::cerr << "[ERROR] Row size too large: " << current_offset + col_size << std::endl;
                return false;
            }
        } else {
            // 对于变长字段，只计算最大可能长度
            if (col.mysql_type_id == mysql_types::VARCHAR) {
                uint32_t max_var_len = (col.column_length > 255) ? col.column_length + 2 : col.column_length + 1;
                current_offset += max_var_len;
            }
        }
    }
    
    return true;
}

// ✅ 新增：计算列大小（用于行格式适配）
uint32_t TableSchema::get_column_size(const ColumnInfo& col) const {
    switch (col.mysql_type_id) {
        case mysql_types::TINY:       return 1;
        case mysql_types::SHORT:      return 2;
        case mysql_types::LONG:       return 4;
        case mysql_types::LONGLONG:   return 8;
        case mysql_types::FLOAT:      return 4;
        case mysql_types::DOUBLE:     return 8;
        case mysql_types::DATE:       return 3;
        case mysql_types::DATETIME:   return 8;
        case mysql_types::TIMESTAMP:  return 4;
        case mysql_types::YEAR:       return 1;
        case mysql_types::NEWDATE:    return 3;
        case mysql_types::TIME:       return 3;
        case mysql_types::DATETIME2:  return 5 + (col.datetime_precision >= 0 ? (col.datetime_precision + 1) / 2 : 0);
        case mysql_types::TIME2:      return 3 + (col.datetime_precision >= 0 ? (col.datetime_precision + 1) / 2 : 0);
        case mysql_types::VARCHAR:
        case mysql_types::VAR_STRING:
            // VARCHAR 使用变长存储：1-2 字节长度前缀 + 实际数据
            if (col.column_length > 255) {
                return col.column_length + 2;
            } else {
                return col.column_length + 1;
            }
        case mysql_types::CHAR:
            // CHAR 使用固定长度存储（考虑字符集）
            return col.column_length * 4; // utf8mb4 最多 4 字节/字符
        case mysql_types::JSON:
            return col.column_length + 2; // JSON 长度前缀
        case mysql_types::BLOB:
        case mysql_types::TINY_BLOB:
        case mysql_types::MEDIUM_BLOB:
        case mysql_types::LONG_BLOB:
            return 20; // BLOB 指针大小
        default:
            // 对于其他类型，使用列长度作为估计
            return col.column_length > 0 ? col.column_length : 1;
    }
}

// ✅ 新增：获取列在表中的物理偏移
std::optional<uint32_t> TableSchema::get_column_offset(const std::string& col_name) const {
    uint32_t current_offset = 0;
    
    for (const auto& [pos, col] : columns) {
        if (col.name == col_name) {
            return current_offset;
        }
        
        // 计算当前列的大小
        if (!col.is_variable_length || col.mysql_type_id == mysql_types::VARCHAR) {
            current_offset += get_column_size(col);
        } else {
            // VARCHAR 使用变长存储，需要在解析时动态计算
            // 对于偏移计算，我们使用最大可能长度作为估计
            if (col.column_length > 255) {
                current_offset += col.column_length + 2;
            } else {
                current_offset += col.column_length + 1;
            }
        }
    }
    
    return std::nullopt;
}

// ✅ 新增：计算可空列数量（用于 NULL 位图）
uint32_t TableSchema::get_nullable_column_count() const {
    uint32_t count = 0;
    for (const auto& [pos, col] : columns) {
        if (col.is_nullable && !col.is_auto_increment) {
            count++;
        }
    }
    return count;
}

// ✅ 新增：序列化为 JSON 字符串（用于 SDI 兼容）
std::string TableSchema::to_json_string() const {
    nlohmann::json j = to_json();
    return j.dump(2);
}

nlohmann::json TableSchema::to_json() const {
    nlohmann::json j;
    j["table_name"] = table_name;
    j["engine"] = engine;
    j["row_format"] = row_format;
    j["page_size"] = page_size;
    j["table_id"] = table_id;
    j["flags"] = flags;
    j["auto_inc_value"] = auto_inc_value;
    
    // 序列化列
    nlohmann::json columns_json = nlohmann::json::array();
    for (const auto& [pos, col] : columns) {
        nlohmann::json col_json;
        col_json["position"] = pos;
        col_json["name"] = col.name;
        col_json["mysql_type_id"] = col.mysql_type_id;
        col_json["is_nullable"] = col.is_nullable;
        if (col.default_value.has_value()) {
            col_json["default_value"] = col.default_value.value();
        }
        col_json["column_length"] = col.column_length;
        col_json["charset_id"] = col.charset_id;
        col_json["collation_id"] = col.collation_id;
        if (col.generated_expr.has_value()) {
            col_json["generated_expr"] = col.generated_expr.value();
        }
        col_json["is_auto_increment"] = col.is_auto_increment;
        col_json["datetime_precision"] = col.datetime_precision;
        col_json["numeric_scale"] = col.numeric_scale;
        col_json["numeric_precision"] = col.numeric_precision;
        col_json["is_off_page"] = col.is_off_page;
        col_json["virtual_gen_store_type"] = col.virtual_gen_store_type;
        col_json["is_signed"] = col.is_signed;
        col_json["flags"] = col.flags;
        col_json["col_flags"] = col.col_flags;
        if (col.comment.has_value()) {
            col_json["comment"] = col.comment.value();
        }
        col_json["is_variable_length"] = col.is_variable_length;
        col_json["actual_column_size"] = col.actual_column_size;
        columns_json.push_back(col_json);
    }
    j["columns"] = columns_json;
    
    // 序列化索引
    nlohmann::json indexes_json = nlohmann::json::array();
    for (const auto& idx : indexes) {
        nlohmann::json idx_json;
        idx_json["name"] = idx.name;
        idx_json["is_primary"] = idx.is_primary;
        idx_json["columns"] = idx.columns;
        idx_json["column_offsets"] = idx.column_offsets;
        idx_json["is_unique"] = idx.is_unique;
        idx_json["algorithm"] = idx.algorithm;
        idx_json["is_visible"] = idx.is_visible;
        indexes_json.push_back(idx_json);
    }
    j["indexes"] = indexes_json;
    
    // 序列化分区信息
    if (partition_info.has_value()) {
        const auto& p = partition_info.value();
        nlohmann::json p_json;
        p_json["method"] = p.method;
        p_json["column_names"] = p.column_names;
        p_json["num_partitions"] = p.num_partitions;
        
        nlohmann::json bounds_json = nlohmann::json::array();
        for (const auto& bound : p.bounds) {
            nlohmann::json bound_json;
            bound_json["values"] = bound.values;
            bound_json["is_maxvalue"] = bound.is_maxvalue;
            bounds_json.push_back(bound_json);
        }
        p_json["bounds"] = bounds_json;
        
        if (p.subpartition_method.has_value()) {
            p_json["subpartition_method"] = p.subpartition_method.value();
        }
        
        j["partition_info"] = p_json;
    }
    
    return j;
}

bool TableSchema::from_json_string(const std::string& json_str) {
    try {
        nlohmann::json j = nlohmann::json::parse(json_str);
        return from_json(j);
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[ERROR] Failed to parse JSON: " << e.what() << std::endl;
        return false;
    }
}

bool TableSchema::from_json(const nlohmann::json& j) {
    if (!j.is_object()) {
        std::cerr << "[ERROR] JSON is not an object" << std::endl;
        return false;
    }
    
    table_name = j.value("table_name", "");
    engine = j.value("engine", "InnoDB");
    row_format = j.value("row_format", 1U);
    page_size = j.value("page_size", 16384U);
    table_id = j.value("table_id", 0ULL);
    flags = j.value("flags", 0U);
    auto_inc_value = j.value("auto_inc_value", 0U);
    
    // 反序列化列
    if (j.contains("columns") && j["columns"].is_array()) {
        columns.clear();
        for (const auto& col_json : j["columns"]) {
            uint32_t pos = col_json.value("position", 0U);
            ColumnInfo col;
            col.name = col_json.value("name", "");
            col.mysql_type_id = col_json.value("mysql_type_id", 0U);
            col.is_nullable = col_json.value("is_nullable", true);
            
            if (col_json.contains("default_value") && !col_json["default_value"].is_null()) {
                col.default_value = col_json.value("default_value", std::string{});
            }
            
            col.column_length = col_json.value("column_length", 0U);
            col.charset_id = col_json.value("charset_id", 0U);
            col.collation_id = col_json.value("collation_id", 0U);
            
            if (col_json.contains("generated_expr") && !col_json["generated_expr"].is_null()) {
                col.generated_expr = col_json.value("generated_expr", std::string{});
            }
            
            col.is_auto_increment = col_json.value("is_auto_increment", false);
            col.datetime_precision = col_json.value("datetime_precision", -1);
            col.numeric_scale = col_json.value("numeric_scale", -1);
            col.numeric_precision = col_json.value("numeric_precision", -1);
            col.is_off_page = col_json.value("is_off_page", false);
            col.virtual_gen_store_type = col_json.value("virtual_gen_store_type", 0U);
            col.is_signed = col_json.value("is_signed", true);
            col.flags = col_json.value("flags", 0U);
            col.col_flags = col_json.value("col_flags", 0U);
            
            if (col_json.contains("comment") && !col_json["comment"].is_null()) {
                col.comment = col_json.value("comment", std::string{});
            }
            
            col.is_variable_length = col_json.value("is_variable_length", false);
            col.actual_column_size = col_json.value("actual_column_size", 0U);
            
            columns[pos] = col;
        }
    }
    
    // 反序列化索引
    if (j.contains("indexes") && j["indexes"].is_array()) {
        indexes.clear();
        for (const auto& idx_json : j["indexes"]) {
            IndexInfo idx;
            idx.name = idx_json.value("name", "");
            idx.is_primary = idx_json.value("is_primary", false);
            
            if (idx_json.contains("columns") && idx_json["columns"].is_array()) {
                for (const auto& col : idx_json["columns"]) {
                    if (col.is_string()) {
                        idx.columns.push_back(col.get<std::string>());
                    }
                }
            }
            
            if (idx_json.contains("column_offsets") && idx_json["column_offsets"].is_array()) {
                for (const auto& offset : idx_json["column_offsets"]) {
                    if (offset.is_number_unsigned()) {
                        idx.column_offsets.push_back(offset.get<uint32_t>());
                    }
                }
            }
            
            idx.is_unique = idx_json.value("is_unique", false);
            idx.algorithm = idx_json.value("algorithm", 0U);
            idx.is_visible = idx_json.value("is_visible", true);
            
            indexes.push_back(idx);
        }
    }
    
    // 反序列化分区信息
    if (j.contains("partition_info") && !j["partition_info"].is_null()) {
        const auto& p_json = j["partition_info"];
        PartitionInfo p;
        p.method = p_json.value("method", "");
        p.num_partitions = p_json.value("num_partitions", 0U);
        
        if (p_json.contains("column_names") && p_json["column_names"].is_array()) {
            for (const auto& col : p_json["column_names"]) {
                if (col.is_string()) {
                    p.column_names.push_back(col.get<std::string>());
                }
            }
        }
        
        if (p_json.contains("bounds") && p_json["bounds"].is_array()) {
            for (const auto& bound_json : p_json["bounds"]) {
                PartitionBound bound;
                
                if (bound_json.contains("values") && bound_json["values"].is_array()) {
                    for (const auto& val : bound_json["values"]) {
                        if (val.is_string()) {
                            bound.values.push_back(val.get<std::string>());
                        }
                    }
                }
                
                bound.is_maxvalue = bound_json.value("is_maxvalue", false);
                p.bounds.push_back(bound);
            }
        }
        
        if (p_json.contains("subpartition_method") && !p_json["subpartition_method"].is_null()) {
            p.subpartition_method = p_json.value("subpartition_method", std::string{});
        }
        
        partition_info = p;
    }
    
    return true;
}

} // namespace catalog
} // namespace dbrecovery

} // namespace catalog
} // namespace dbrecovery
