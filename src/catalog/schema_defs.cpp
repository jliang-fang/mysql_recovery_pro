#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>

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

// ✅ 新增：序列化为 JSON 字符串（用于 SDI 兼容）
std::string TableSchema::to_json_string() const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"table_name\":\"" << table_name << "\",";
    oss << "\"engine\":\"" << engine << "\",";
    oss << "\"row_format\":" << row_format << ",";
    oss << "\"page_size\":" << page_size << ",";
    
    // 序列化列
    oss << "\"columns\":[";
    bool first_col = true;
    for (const auto& [pos, col] : columns) {
        if (!first_col) oss << ",";
        first_col = false;
        
        oss << "{";
        oss << "\"name\":\"" << col.name << "\",";
        oss << "\"type\":" << col.mysql_type_id << ",";
        oss << "\"nullable\":" << (col.is_nullable ? "true" : "false") << ",";
        oss << "\"length\":" << col.column_length;
        if (col.datetime_precision >= 0) {
            oss << ",\"datetime_precision\":" << col.datetime_precision;
        }
        oss << "}";
    }
    oss << "],";
    
    // 序列化索引
    oss << "\"indexes\":[";
    bool first_idx = true;
    for (const auto& idx : indexes) {
        if (!first_idx) oss << ",";
        first_idx = false;
        
        oss << "{";
        oss << "\"name\":\"" << idx.name << "\",";
        oss << "\"primary\":" << (idx.is_primary ? "true" : "false") << ",";
        oss << "\"unique\":" << (idx.is_unique ? "true" : "false") << ",";
        oss << "\"columns\":[";
        
        bool first_col_in_idx = true;
        for (const auto& col : idx.columns) {
            if (!first_col_in_idx) oss << ",";
            first_col_in_idx = false;
            oss << "\"" << col << "\"";
        }
        oss << "]}";
    }
    oss << "]";
    
    oss << "}";
    
    return oss.str();
}

// ✅ 新增：从 JSON 字符串反序列化（用于 SDI 解析）
bool TableSchema::from_json_string(const std::string& json_str) {
    // TODO: 使用 nlohmann/json 库解析 JSON
    // 这里仅提供占位实现，实际应使用 JSON 库
    std::cerr << "[WARN] JSON deserialization not implemented yet" << std::endl;
    return false;
}

} // namespace catalog
} // namespace dbrecovery
