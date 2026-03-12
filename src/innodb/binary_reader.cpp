#include "dbrecovery/innodb/binary_reader.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <iostream>
#include <sstream>

namespace dbrecovery {
namespace innodb {

void BinaryReader::parse_table_name(const std::string& json_str, catalog::TableSchema& schema) {
    if (json_str.empty()) return;
    
    try {
        // ✅ 使用 nlohmann/json 进行正式解析
        auto j = nlohmann::json::parse(json_str);
        
        if (j.contains("name")) {
            schema.table_name = j["name"].get<std::string>();
        }
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[WARN] Failed to parse JSON for table name: " << e.what() << "\n";
        
        // 降级方案：查找 "name" 字段
        size_t name_pos = json_str.find("\"name\"");
        if (name_pos != std::string::npos) {
            size_t colon_pos = json_str.find(':', name_pos);
            if (colon_pos != std::string::npos) {
                // 跳过冒号和可能的空格
                size_t start = json_str.find_first_not_of(" \t\n\r", colon_pos + 1);
                if (start != std::string::npos && json_str[start] == '"') {
                    size_t end = json_str.find('"', start + 1);
                    if (end != std::string::npos) {
                        schema.table_name = json_str.substr(start + 1, end - start - 1);
                    }
                }
            }
        }
    }
}

// ✅ 新增：解析表的列定义
void BinaryReader::parse_columns(const std::string& json_str, catalog::TableSchema& schema) {
    try {
        // ✅ 使用 nlohmann/json 进行正式解析
        auto j = nlohmann::json::parse(json_str);
        
        if (j.contains("columns") && j["columns"].is_array()) {
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
                
                if (col_json.contains("length")) {
                    col.column_length = col_json["length"].get<uint32_t>();
                }
                
                if (col_json.contains("charset_id")) {
                    col.charset_id = col_json["charset_id"].get<uint32_t>();
                }
                
                // 添加到表结构
                if (!col.name.empty()) {
                    schema.columns[schema.columns.size() + 1] = col;
                }
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[WARN] Failed to parse JSON for columns: " << e.what() << "\n";
        
        // 降级方案：原始字符串解析
        size_t columns_pos = json_str.find("\"columns\"");
        if (columns_pos == std::string::npos) return;
        
        size_t array_start = json_str.find('[', columns_pos);
        if (array_start == std::string::npos) return;
        
        int brace_count = 0;
        bool in_column = false;
        size_t col_start = 0;
        
        for (size_t i = array_start + 1; i < json_str.length(); ++i) {
            if (json_str[i] == '{') {
                if (brace_count == 0) {
                    col_start = i;
                    in_column = true;
                }
                brace_count++;
            } else if (json_str[i] == '}') {
                brace_count--;
                if (brace_count == 0 && in_column) {
                    std::string column_json = json_str.substr(col_start, i - col_start + 1);
                    parse_single_column(column_json, schema);
                    in_column = false;
                }
            } else if (json_str[i] == '[') {
                brace_count++;
            } else if (json_str[i] == ']') {
                brace_count--;
            }
        }
    }
}

// ✅ 新增：解析单个列定义
void BinaryReader::parse_single_column(const std::string& json_str, catalog::TableSchema& schema) {
    catalog::ColumnInfo col;
    
    // 解析列名
    size_t name_pos = json_str.find("\"name\"");
    if (name_pos != std::string::npos) {
        size_t colon_pos = json_str.find(':', name_pos);
        if (colon_pos != std::string::npos) {
            size_t start = json_str.find_first_not_of(" \t\n\r", colon_pos + 1);
            if (start != std::string::npos && json_str[start] == '"') {
                size_t end = json_str.find('"', start + 1);
                if (end != std::string::npos) {
                    col.name = json_str.substr(start + 1, end - start - 1);
                }
            }
        }
    }
    
    // 解析类型
    size_t type_pos = json_str.find("\"type\"");
    if (type_pos != std::string::npos) {
        size_t colon_pos = json_str.find(':', type_pos);
        if (colon_pos != std::string::npos) {
            size_t start = json_str.find_first_not_of(" \t\n\r", colon_pos + 1);
            if (start != std::string::npos && std::isdigit(json_str[start])) {
                size_t end = start;
                while (end < json_str.length() && std::isdigit(json_str[end])) {
                    end++;
                }
                col.mysql_type_id = static_cast<uint32_t>(std::stoul(json_str.substr(start, end - start)));
            }
        }
    }
    
    // 解析是否可空
    size_t nullable_pos = json_str.find("\"is_nullable\"");
    if (nullable_pos != std::string::npos) {
        size_t colon_pos = json_str.find(':', nullable_pos);
        if (colon_pos != std::string::npos) {
            size_t start = json_str.find_first_not_of(" \t\n\r", colon_pos + 1);
            if (start != std::string::npos) {
                if (json_str.substr(start, 4) == "true") {
                    col.is_nullable = true;
                } else if (json_str.substr(start, 5) == "false") {
                    col.is_nullable = false;
                }
            }
        }
    }
    
    // 解析长度
    size_t length_pos = json_str.find("\"length\"");
    if (length_pos != std::string::npos) {
        size_t colon_pos = json_str.find(':', length_pos);
        if (colon_pos != std::string::npos) {
            size_t start = json_str.find_first_not_of(" \t\n\r", colon_pos + 1);
            if (start != std::string::npos && std::isdigit(json_str[start])) {
                size_t end = start;
                while (end < json_str.length() && std::isdigit(json_str[end])) {
                    end++;
                }
                col.column_length = static_cast<uint32_t>(std::stoul(json_str.substr(start, end - start)));
            }
        }
    }
    
    // 解析字符集ID
    size_t charset_pos = json_str.find("\"charset_id\"");
    if (charset_pos != std::string::npos) {
        size_t colon_pos = json_str.find(':', charset_pos);
        if (colon_pos != std::string::npos) {
            size_t start = json_str.find_first_not_of(" \t\n\r", colon_pos + 1);
            if (start != std::string::npos && std::isdigit(json_str[start])) {
                size_t end = start;
                while (end < json_str.length() && std::isdigit(json_str[end])) {
                    end++;
                }
                col.charset_id = static_cast<uint32_t>(std::stoul(json_str.substr(start, end - start)));
            }
        }
    }
    
    // 添加到表结构
    if (!col.name.empty()) {
        schema.columns[schema.columns.size() + 1] = col;
    }
}

// ✅ 新增：解析索引定义
void BinaryReader::parse_indexes(const std::string& json_str, catalog::TableSchema& schema) {
    try {
        // ✅ 使用 nlohmann/json 进行正式解析
        auto j = nlohmann::json::parse(json_str);
        
        if (j.contains("indexes") && j["indexes"].is_array()) {
            for (const auto& idx_json : j["indexes"]) {
                catalog::IndexInfo index;
                
                if (idx_json.contains("name")) {
                    index.name = idx_json["name"].get<std::string>();
                }
                
                if (idx_json.contains("is_primary")) {
                    index.is_primary = idx_json["is_primary"].get<bool>();
                }
                
                if (idx_json.contains("is_unique")) {
                    index.is_unique = idx_json["is_unique"].get<bool>();
                }
                
                if (idx_json.contains("columns") && idx_json["columns"].is_array()) {
                    for (const auto& col : idx_json["columns"]) {
                        if (col.is_string()) {
                            index.columns.push_back(col.get<std::string>());
                        }
                    }
                }
                
                // 添加到表结构
                if (!index.name.empty()) {
                    schema.indexes.push_back(index);
                }
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[WARN] Failed to parse JSON for indexes: " << e.what() << "\n";
        
        // 降级方案：原始字符串解析
        size_t indexes_pos = json_str.find("\"indexes\"");
        if (indexes_pos == std::string::npos) return;
        
        int brace_count = 0;
        bool in_index = false;
        size_t idx_start = 0;
        
        for (size_t i = indexes_pos; i < json_str.length(); ++i) {
            if (json_str[i] == '{') {
                if (brace_count == 0) {
                    idx_start = i;
                    in_index = true;
                }
                brace_count++;
            } else if (json_str[i] == '}') {
                brace_count--;
                if (brace_count == 0 && in_index) {
                    std::string index_json = json_str.substr(idx_start, i - idx_start + 1);
                    parse_single_index(index_json, schema);
                    in_index = false;
                }
            } else if (json_str[i] == '[') {
                brace_count++;
            } else if (json_str[i] == ']') {
                brace_count--;
            }
        }
    }
}

// ✅ 新增：解析单个索引定义
void BinaryReader::parse_single_index(const std::string& json_str, catalog::TableSchema& schema) {
    catalog::IndexInfo index;
    
    // 解析索引名
    size_t name_pos = json_str.find("\"name\"");
    if (name_pos != std::string::npos) {
        size_t colon_pos = json_str.find(':', name_pos);
        if (colon_pos != std::string::npos) {
            size_t start = json_str.find_first_not_of(" \t\n\r", colon_pos + 1);
            if (start != std::string::npos && json_str[start] == '"') {
                size_t end = json_str.find('"', start + 1);
                if (end != std::string::npos) {
                    index.name = json_str.substr(start + 1, end - start - 1);
                }
            }
        }
    }
    
    // 解析是否为主键
    size_t primary_pos = json_str.find("\"is_primary\"");
    if (primary_pos != std::string::npos) {
        size_t colon_pos = json_str.find(':', primary_pos);
        if (colon_pos != std::string::npos) {
            size_t start = json_str.find_first_not_of(" \t\n\r", colon_pos + 1);
            if (start != std::string::npos) {
                if (json_str.substr(start, 4) == "true") {
                    index.is_primary = true;
                } else if (json_str.substr(start, 5) == "false") {
                    index.is_primary = false;
                }
            }
        }
    }
    
    // 解析唯一性
    size_t unique_pos = json_str.find("\"is_unique\"");
    if (unique_pos != std::string::npos) {
        size_t colon_pos = json_str.find(':', unique_pos);
        if (colon_pos != std::string::npos) {
            size_t start = json_str.find_first_not_of(" \t\n\r", colon_pos + 1);
            if (start != std::string::npos) {
                if (json_str.substr(start, 4) == "true") {
                    index.is_unique = true;
                } else if (json_str.substr(start, 5) == "false") {
                    index.is_unique = false;
                }
            }
        }
    }
    
    // 解析列名列表
    size_t columns_pos = json_str.find("\"columns\"");
    if (columns_pos != std::string::npos) {
        size_t array_start = json_str.find('[', columns_pos);
        if (array_start != std::string::npos) {
            size_t array_end = json_str.find(']', array_start);
            if (array_end != std::string::npos) {
                std::string columns_str = json_str.substr(array_start + 1, array_end - array_start - 1);
                
                // 解析列名列表
                size_t pos = 0;
                while ((pos = columns_str.find('"', pos)) != std::string::npos) {
                    size_t end_pos = columns_str.find('"', pos + 1);
                    if (end_pos != std::string::npos) {
                        index.columns.push_back(columns_str.substr(pos + 1, end_pos - pos - 1));
                        pos = end_pos + 1;
                    } else {
                        break;
                    }
                }
            }
        }
    }
    
    // 添加到表结构
    if (!index.name.empty()) {
        schema.indexes.push_back(index);
    }
}

} // namespace innodb
} // namespace dbrecovery
