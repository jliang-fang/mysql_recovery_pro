#ifndef DBRECOVERY_INNODB_SDI_PARSER_HPP
#define DBRECOVERY_INNODB_SDI_PARSER_HPP

#include "dbrecovery/innodb/page_types_v2.hpp"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>

namespace dbrecovery {
namespace innodb {

/**
 * @brief SDI (Serialized Dictionary Info) 页类型
 */
enum class SDIPageType : uint16_t {
    SDI = 0x5441,      // SDI 页
    SDI2 = 0x5a5a,     // SDI 辅助页
};

/**
 * @brief SDI 记录类型
 */
enum class SDIRecordType : uint8_t {
    TABLE = 0x01,      // 表定义
    INDEX = 0x02,      // 索引定义
    COLUMNS = 0x03,    // 列定义
    FTS = 0x04,        // 全文索引
    SRS = 0x05,        // 空间参考系统
    GEOMETRY = 0x06,   // 几何定义
};

/**
 * @brief SDI 元数据版本
 */
enum class SDIVersion : uint32_t {
    V1 = 1,    // MySQL 8.0.0-8.0.15
    V2 = 2,    // MySQL 8.0.16+
    V3 = 3,    // MySQL 8.4+
};

/**
 * @brief SDI 表定义
 */
struct SDITable {
    std::string table_name;
    std::string database_name;
    uint64_t table_id;
    uint32_t space_id;
    uint32_t flags;
    uint32_t n_cols;
    uint32_t instant_cols;  // Instant DDL 列数
    
    // 列定义
    struct Column {
        std::string name;
        uint8_t type;
        uint32_t flags;
        uint32_t length;
        bool is_nullable;
        bool is_virtual;
        bool is_generated;
        std::string default_value;
    };
    std::vector<Column> columns;
    
    // 索引定义
    struct Index {
        std::string name;
        uint32_t type;
        uint32_t n_fields;
        std::vector<std::string> field_names;
    };
    std::vector<Index> indexes;
    
    std::string to_string() const;
};

/**
 * @brief SDI 索引定义
 */
struct SDIIndex {
    std::string index_name;
    std::string table_name;
    uint64_t index_id;
    uint32_t type;
    uint32_t n_fields;
    
    struct Field {
        std::string column_name;
        uint32_t prefix_len;
        bool is_nullable;
    };
    std::vector<Field> fields;
    
    std::string to_string() const;
};

/**
 * @brief SDI 页
 */
struct SDIPage {
    PageHeaderV2 header;
    
    // SDI Header
    uint32_t version;           // SDI 版本
    uint32_t flags;             // SDI 标志
    uint64_t table_id;          // 表ID
    uint32_t space_id;          // 空间ID
    uint32_t page_no;           // 页号
    uint32_t next_page_no;      // 下一页号
    
    // SDI Records
    std::vector<SDITable> tables;
    std::vector<SDIIndex> indexes;
    
    // 原始 JSON 数据
    std::string json_data;
    
    bool parse(const uint8_t* data, size_t size);
    
    // SDI 解析方法
    bool parse_sdi_header(const uint8_t* data, size_t size);
    bool parse_sdi_records(const uint8_t* data, size_t size);
    
    std::string to_string() const;
};

/**
 * @brief SDI 解析器
 */
class SDIParser {
public:
    SDIParser();
    
    /**
     * @brief 解析 SDI 页
     */
    std::shared_ptr<SDIPage> parse_page(const uint8_t* data, size_t size);
    
    /**
     * @brief 从 JSON 字符串解析 SDI
     */
    std::shared_ptr<SDIPage> parse_json(const std::string& json_str);
    
    /**
     * @brief 解析表定义
     */
    std::optional<SDITable> parse_table(const nlohmann::json& json);
    
    /**
     * @brief 解析索引定义
     */
    std::optional<SDIIndex> parse_index(const nlohmann::json& json);
    
    /**
     * @brief 获取表信息
     */
    std::vector<SDITable> get_tables() const;
    
    /**
     * @brief 根据表名查找表
     */
    std::optional<SDITable> find_table(const std::string& table_name) const;
    
    /**
     * @brief 根据表ID查找表
     */
    std::optional<SDITable> find_table(uint64_t table_id) const;
    
    /**
     * @brief 获取解析统计信息
     */
    struct Stats {
        size_t pages_parsed;
        size_t tables_found;
        size_t indexes_found;
        size_t errors;
    };
    
    Stats get_stats() const;

private:
    Stats stats_;
    std::vector<SDITable> tables_;
    std::vector<SDIIndex> indexes_;

    bool parse_sdi_header(const uint8_t* data, size_t size, SDIPage& page);
    bool parse_sdi_records(const uint8_t* data, size_t size, SDIPage& page);
    std::string decompress_sdi(const std::vector<uint8_t>& data);
    
    // SDI 页类型常量
    static constexpr uint16_t SDI_PAGE_TYPE = 21825;  // 0x5441
    static constexpr uint16_t SDI2_PAGE_TYPE = 23130; // 0x5a5a
};

// ==================== 实现 ====================

inline SDIParser::SDIParser() {
    stats_ = {0, 0, 0, 0};
}

inline SDIParser::Stats SDIParser::get_stats() const {
    return stats_;
}

inline std::vector<SDITable> SDIParser::get_tables() const {
    return tables_;
}

// ==================== SDITable ====================

inline std::string SDITable::to_string() const {
    std::ostringstream oss;
    oss << "SDITable{"
        << "name=" << database_name << "." << table_name
        << ", id=" << table_id
        << ", space=" << space_id
        << ", flags=0x" << std::hex << flags << std::dec
        << ", n_cols=" << n_cols
        << ", instant_cols=" << instant_cols
        << ", columns=" << columns.size()
        << ", indexes=" << indexes.size()
        << "}";
    return oss.str();
}

// ==================== SDIIndex ====================

inline std::string SDIIndex::to_string() const {
    std::ostringstream oss;
    oss << "SDIIndex{"
        << "name=" << index_name
        << ", table=" << table_name
        << ", id=" << index_id
        << ", type=" << type
        << ", n_fields=" << n_fields
        << "}";
    return oss.str();
}

// ==================== SDIPage ====================

inline bool SDIPage::parse(const uint8_t* data, size_t size) {
    if (!header.parse(data, size)) {
        return false;
    }

    if (!header.is_sdi_page()) {
        return false;
    }

    // 解析 SDI Header
    if (!parse_sdi_header(data, size)) {
        return false;
    }

    // 解析 SDI Records
    if (!parse_sdi_records(data, size)) {
        return false;
    }

    return true;
}

inline std::string SDIPage::to_string() const {
    std::ostringstream oss;
    oss << "SDIPage{" << header.to_string()
        << ", version=" << version
        << ", flags=0x" << std::hex << flags << std::dec
        << ", table_id=" << table_id
        << ", space=" << space_id
        << ", page=" << page_no
        << ", next=" << next_page_no
        << ", tables=" << tables.size()
        << ", indexes=" << indexes.size()
        << "}";
    return oss.str();
}

// ==================== SDIParser ====================

inline std::shared_ptr<SDIPage> SDIParser::parse_page(const uint8_t* data, size_t size) {
    auto page = std::make_shared<SDIPage>();
    if (page->parse(data, size)) {
        stats_.pages_parsed++;
        tables_.insert(tables_.end(), page->tables.begin(), page->tables.end());
        indexes_.insert(indexes_.end(), page->indexes.begin(), page->indexes.end());
        stats_.tables_found += page->tables.size();
        stats_.indexes_found += page->indexes.size();
        return page;
    }
    stats_.errors++;
    return nullptr;
}

inline std::shared_ptr<SDIPage> SDIParser::parse_json(const std::string& json_str) {
    try {
        auto page = std::make_shared<SDIPage>();
        
        // 解析 JSON
        nlohmann::json json = nlohmann::json::parse(json_str);
        
        // 提取 SDI 元数据
        if (json.contains("version")) {
            page->version = json["version"].get<uint32_t>();
        }
        if (json.contains("type")) {
            page->header.type = json["type"].get<uint16_t>();
        }
        
        // 解析表定义
        if (json.contains("table")) {
            auto table_opt = parse_table(json["table"]);
            if (table_opt.has_value()) {
                page->tables.push_back(table_opt.value());
                stats_.tables_found++;
            }
        }
        
        // 解析索引定义
        if (json.contains("indexes")) {
            for (const auto& idx_json : json["indexes"]) {
                auto index_opt = parse_index(idx_json);
                if (index_opt.has_value()) {
                    page->indexes.push_back(index_opt.value());
                    stats_.indexes_found++;
                }
            }
        }
        
        stats_.pages_parsed++;
        return page;
    } catch (const nlohmann::json::exception& e) {
        stats_.errors++;
        return nullptr;
    }
}

inline std::optional<SDITable> SDIParser::parse_table(const nlohmann::json& json) {
    SDITable table;
    
    try {
        if (json.contains("name")) {
            std::string full_name = json["name"].get<std::string>();
            // 解析 database.table 格式
            size_t pos = full_name.find('.');
            if (pos != std::string::npos) {
                table.database_name = full_name.substr(0, pos);
                table.table_name = full_name.substr(pos + 1);
            } else {
                table.table_name = full_name;
            }
        }
        
        if (json.contains("id")) {
            table.table_id = json["id"].get<uint64_t>();
        }
        
        if (json.contains("space_id")) {
            table.space_id = json["space_id"].get<uint32_t>();
        }
        
        if (json.contains("flags")) {
            table.flags = json["flags"].get<uint32_t>();
        }
        
        if (json.contains("n_cols")) {
            table.n_cols = json["n_cols"].get<uint32_t>();
        }
        
        if (json.contains("instant_cols")) {
            table.instant_cols = json["instant_cols"].get<uint32_t>();
        }
        
        // 解析列定义
        if (json.contains("columns")) {
            for (const auto& col_json : json["columns"]) {
                SDITable::Column col;
                
                if (col_json.contains("name")) {
                    col.name = col_json["name"].get<std::string>();
                }
                
                if (col_json.contains("type")) {
                    col.type = col_json["type"].get<uint8_t>();
                }
                
                if (col_json.contains("flags")) {
                    col.flags = col_json["flags"].get<uint32_t>();
                }
                
                if (col_json.contains("length")) {
                    col.length = col_json["length"].get<uint32_t>();
                }
                
                col.is_nullable = col_json.value("is_nullable", true);
                col.is_virtual = col_json.value("is_virtual", false);
                col.is_generated = col_json.value("is_generated", false);
                
                if (col_json.contains("default_value")) {
                    col.default_value = col_json["default_value"].get<std::string>();
                }
                
                table.columns.push_back(col);
            }
        }
        
        // 解析索引定义
        if (json.contains("indexes")) {
            for (const auto& idx_json : json["indexes"]) {
                SDITable::Index idx;
                
                if (idx_json.contains("name")) {
                    idx.name = idx_json["name"].get<std::string>();
                }
                
                if (idx_json.contains("type")) {
                    idx.type = idx_json["type"].get<uint32_t>();
                }
                
                if (idx_json.contains("n_fields")) {
                    idx.n_fields = idx_json["n_fields"].get<uint32_t>();
                }
                
                if (idx_json.contains("fields")) {
                    for (const auto& field_json : idx_json["fields"]) {
                        if (field_json.contains("column")) {
                            idx.field_names.push_back(field_json["column"].get<std::string>());
                        }
                    }
                }
                
                table.indexes.push_back(idx);
            }
        }
        
        stats_.tables_found++;
        return table;
    } catch (const nlohmann::json::exception& e) {
        stats_.errors++;
        return std::nullopt;
    }
}

inline std::optional<SDIIndex> SDIParser::parse_index(const nlohmann::json& json) {
    SDIIndex index;
    
    try {
        if (json.contains("name")) {
            index.index_name = json["name"].get<std::string>();
        }
        
        if (json.contains("table")) {
            index.table_name = json["table"].get<std::string>();
        }
        
        if (json.contains("id")) {
            index.index_id = json["id"].get<uint64_t>();
        }

        if (json.contains("type")) {
            index.type = json["type"].get<uint32_t>();
        }

        if (json.contains("n_fields")) {
            index.n_fields = json["n_fields"].get<uint32_t>();
        }

        // 解析字段定义
        if (json.contains("fields")) {
            for (const auto& field_json : json["fields"]) {
                SDIIndex::Field field;

                if (field_json.contains("column")) {
                    field.column_name = field_json["column"].get<std::string>();
                }

                if (field_json.contains("prefix_len")) {
                    field.prefix_len = field_json["prefix_len"].get<uint32_t>();
                }

                field.is_nullable = field_json.value("is_nullable", true);

                index.fields.push_back(field);
            }
        }

        stats_.indexes_found++;
        return index;
    } catch (const nlohmann::json::exception& e) {
        stats_.errors++;
        return std::nullopt;
    }
}

inline std::optional<SDITable> SDIParser::find_table(const std::string& table_name) const {
    for (const auto& table : tables_) {
        if (table.table_name == table_name) {
            return table;
        }
    }
    return std::nullopt;
}

inline std::optional<SDITable> SDIParser::find_table(uint64_t table_id) const {
    for (const auto& table : tables_) {
        if (table.table_id == table_id) {
            return table;
        }
    }
    return std::nullopt;
}

// ==================== SDIPage 实现 ====================

inline bool SDIPage::parse_sdi_header(const uint8_t* data, size_t size) {
    if (size < 70) {
        return false;
    }
    
    // SDI Header (在 FIL header 之后)
    size_t offset = FIL_PAGE_DATA;
    
    version = read_le<uint32_t>(data, offset);
    flags = read_le<uint32_t>(data, offset + 4);
    table_id = read_le<uint64_t>(data, offset + 8);
    space_id = read_le<uint32_t>(data, offset + 16);
    page_no = read_le<uint32_t>(data, offset + 20);
    next_page_no = read_le<uint32_t>(data, offset + 24);
    
    return true;
}

inline bool SDIPage::parse_sdi_records(const uint8_t* data, size_t size) {
    // 简化实现：SDI 记录在 JSON 格式中
    // 实际解析在 SDIParser::parse_json 中完成
    return true;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SDI_PARSER_V2_HPP
