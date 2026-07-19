#ifndef DBRECOVERY_INNODB_METADATA_RECOVERY_HPP
#define DBRECOVERY_INNODB_METADATA_RECOVERY_HPP

#include "dbrecovery/innodb/page_types_v2.hpp"
#include "dbrecovery/innodb/sdi_parser_v2.hpp"
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace dbrecovery {
namespace innodb {

/**
 * @brief 元数据恢复状态
 */
enum class MetadataRecoveryStatus {
    SUCCESS,           // 成功
    PARTIAL,           // 部分成功
    FAILED,            // 失败
    INCOMPLETE,        // 不完整
    CORRUPTED,         // 损坏
};

/**
 * @brief 表元数据
 */
struct TableMetadata {
    std::string database_name;
    std::string table_name;
    uint64_t table_id;
    uint32_t space_id;
    uint32_t flags;
    
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
        std::vector<std::string> column_names;
    };
    std::vector<Index> indexes;
    
    // 行格式
    RowFormat row_format;
    
    // 物理信息
    uint32_t page_size;
    uint16_t max_row_length;
    
    std::string to_string() const;
};

/**
 * @brief 元数据恢复结果
 */
struct MetadataRecoveryResult {
    MetadataRecoveryStatus status;
    std::vector<TableMetadata> tables;
    std::string error_message;
    size_t pages_scanned = 0;
    size_t pages_found = 0;
    size_t total_tables_found = 0;

    std::string to_string() const;
};

/**
 * @brief 元数据恢复器
 */
class MetadataRecovery {
public:
    MetadataRecovery();
    
    /**
     * @brief 从 SDI 页恢复元数据
     */
    MetadataRecoveryResult recover_from_sdi(const uint8_t* data, size_t size);
    
    /**
     * @brief 从页头推断元数据
     */
    std::optional<TableMetadata> infer_from_page_header(const uint8_t* data, size_t size);
    
    /**
     * @brief 从记录推断表结构
     */
    std::optional<TableMetadata> infer_from_records(const uint8_t* data, size_t size);
    
    /**
     * @brief 从文件恢复元数据
     */
    MetadataRecoveryResult recover_from_file(const std::string& path);
    
    /**
     * @brief 扫描页查找元数据
     */
    std::vector<TableMetadata> scan_for_metadata(const uint8_t* data, size_t size);
    
    /**
     * @brief 重建表结构
     */
    std::string rebuild_create_statement(const TableMetadata& table);
    
    /**
     * @brief 获取恢复统计信息
     */
    struct Stats {
        size_t pages_scanned;
        size_t tables_found;
        size_t indexes_found;
        size_t columns_found;
        size_t errors;
    };
    
    Stats get_stats() const;
    
private:
    Stats stats_;
    
    std::optional<TableMetadata> parse_sdi_table(const SDITable& sdi_table);
    std::optional<TableMetadata> infer_table_from_page(const uint8_t* data, size_t size);
    std::vector<ColumnInfo> infer_columns_from_records(const uint8_t* data, size_t size);
};

// ==================== 实现 ====================

inline MetadataRecovery::MetadataRecovery() {
    stats_ = {0, 0, 0, 0, 0};
}

inline MetadataRecovery::Stats MetadataRecovery::get_stats() const {
    return stats_;
}

// ==================== TableMetadata ====================

inline std::string TableMetadata::to_string() const {
    std::ostringstream oss;
    oss << "TableMetadata{"
        << "name=" << database_name << "." << table_name
        << ", id=" << table_id
        << ", space=" << space_id
        << ", flags=0x" << std::hex << flags << std::dec
        << ", format=" << row_format_to_string(row_format)
        << ", cols=" << columns.size()
        << ", idx=" << indexes.size()
        << "}";
    return oss.str();
}

// ==================== MetadataRecoveryResult ====================

inline std::string MetadataRecoveryResult::to_string() const {
    std::ostringstream oss;
    oss << "MetadataRecoveryResult{"
        << "status=";
    switch (status) {
        case MetadataRecoveryStatus::SUCCESS: oss << "SUCCESS"; break;
        case MetadataRecoveryStatus::PARTIAL: oss << "PARTIAL"; break;
        case MetadataRecoveryStatus::FAILED: oss << "FAILED"; break;
        case MetadataRecoveryStatus::INCOMPLETE: oss << "INCOMPLETE"; break;
        case MetadataRecoveryStatus::CORRUPTED: oss << "CORRUPTED"; break;
    }
    oss << ", tables=" << tables.size()
        << ", pages=" << pages_found;
    if (!error_message.empty()) {
        oss << ", error=" << error_message;
    }
    oss << "}";
    return oss.str();
}

// ==================== MetadataRecovery ====================

inline MetadataRecoveryResult MetadataRecovery::recover_from_sdi(const uint8_t* data, size_t size) {
    MetadataRecoveryResult result;
    result.pages_scanned = 1;
    
    SDIParser parser;
    auto sdi_page = parser.parse_page(data, size);
    
    if (!sdi_page) {
        result.status = MetadataRecoveryStatus::FAILED;
        result.error_message = "Failed to parse SDI page";
        stats_.errors++;
        return result;
    }
    
    for (const auto& table : sdi_page->tables) {
        auto table_meta = parse_sdi_table(table);
        if (table_meta) {
            result.tables.push_back(table_meta.value());
            stats_.tables_found++;
            stats_.columns_found += table_meta->columns.size();
            stats_.indexes_found += table_meta->indexes.size();
        }
    }
    
    result.status = result.tables.empty() ? MetadataRecoveryStatus::PARTIAL : MetadataRecoveryStatus::SUCCESS;
    result.pages_found = 1;
    
    return result;
}

inline std::optional<TableMetadata> MetadataRecovery::parse_sdi_table(const SDITable& sdi_table) {
    TableMetadata table;
    
    table.database_name = sdi_table.database_name;
    table.table_name = sdi_table.table_name;
    table.table_id = sdi_table.table_id;
    table.space_id = sdi_table.space_id;
    table.flags = sdi_table.flags;
    
    // 推断行格式
    if (sdi_table.flags & 0x01) {
        table.row_format = RowFormat::DYNAMIC;
    } else if (sdi_table.flags & 0x02) {
        table.row_format = RowFormat::COMPACT;
    } else {
        table.row_format = RowFormat::REDUNDANT;
    }
    
    // 解析列
    for (const auto& col : sdi_table.columns) {
        TableMetadata::Column table_col;
        table_col.name = col.name;
        table_col.type = col.type;
        table_col.flags = col.flags;
        table_col.length = col.length;
        table_col.is_nullable = col.is_nullable;
        table_col.is_virtual = col.is_virtual;
        table_col.is_generated = col.is_generated;
        table_col.default_value = col.default_value.value_or("");
        
        table.columns.push_back(table_col);
    }
    
    // 解析索引
    for (const auto& idx : sdi_table.indexes) {
        TableMetadata::Index table_idx;
        table_idx.name = idx.name;
        table_idx.type = idx.type;
        table_idx.column_names = idx.field_names;
        
        table.indexes.push_back(table_idx);
    }
    
    return table;
}

inline std::optional<TableMetadata> MetadataRecovery::infer_from_page_header(const uint8_t* data, size_t size) {
    PageHeaderV2 header;
    if (!header.parse(data, size)) {
        return std::nullopt;
    }
    
    if (!header.is_index_page()) {
        return std::nullopt;
    }
    
    TableMetadata table;
    table.space_id = header.space_id;
    table.page_size = header.get_page_size();
    
    // 推断行格式
    if (header.version_detected >= MySQLVersion::VERSION_8_0) {
        table.row_format = RowFormat::DYNAMIC;
    } else if (header.version_detected == MySQLVersion::VERSION_5_7) {
        table.row_format = RowFormat::COMPACT;
    } else {
        table.row_format = RowFormat::REDUNDANT;
    }
    
    return table;
}

inline std::optional<TableMetadata> MetadataRecovery::infer_from_records(const uint8_t* data, size_t size) {
    auto table = infer_from_page_header(data, size);
    if (!table) {
        return std::nullopt;
    }
    
    // 解析记录以推断列信息
    table->columns = infer_columns_from_records(data, size);
    
    return table;
}

inline std::vector<ColumnInfo> MetadataRecovery::infer_columns_from_records(const uint8_t* data, size_t size) {
    std::vector<ColumnInfo> columns;
    
    IndexPageV2 page;
    if (!page.parse(data, size, RowFormat::COMPACT)) {
        return columns;
    }
    
    // 分析第一条记录的列数
    if (!page.records.empty()) {
        const auto& rec = page.records[0];
        if (rec.compact_row.has_value()) {
            columns = rec.compact_row->columns;
        } else if (rec.dynamic_row.has_value()) {
            columns = rec.dynamic_row->columns;
        } else if (rec.redundant_row.has_value()) {
            columns = rec.redundant_row->columns;
        }
    }
    
    return columns;
}

inline MetadataRecoveryResult MetadataRecovery::recover_from_file(const std::string& path) {
    MetadataRecoveryResult result;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        result.status = MetadataRecoveryStatus::INCOMPLETE;
        result.error_message = "Cannot open file: " + path;
        return result;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    file.close();

    if (file_size < 16384) {
        result.status = MetadataRecoveryStatus::INCOMPLETE;
        result.error_message = "File too small to contain InnoDB metadata";
        return result;
    }

    // Use existing scan_for_metadata to find tables
    result.tables = scan_for_metadata(buffer.data(), buffer.size());

    if (result.tables.empty()) {
        result.status = MetadataRecoveryStatus::INCOMPLETE;
        result.error_message = "No table metadata found in file";
    } else {
        result.status = MetadataRecoveryStatus::SUCCESS;
        result.total_tables_found = result.tables.size();
    }

    return result;
}

inline std::vector<TableMetadata> MetadataRecovery::scan_for_metadata(const uint8_t* data, size_t size) {
    std::vector<TableMetadata> tables;
    
    // 扫描所有页
    size_t page_size = 16384;
    size_t num_pages = size / page_size;
    
    for (size_t i = 0; i < num_pages; i++) {
        const uint8_t* page_data = data + i * page_size;
        
        auto table = infer_from_page_header(page_data, page_size);
        if (table) {
            tables.push_back(table.value());
            stats_.pages_scanned++;
        }
    }
    
    return tables;
}

inline std::string MetadataRecovery::rebuild_create_statement(const TableMetadata& table) {
    std::ostringstream oss;
    
    oss << "CREATE TABLE `" << table.table_name << "` (\n";
    
    // 列定义
    for (size_t i = 0; i < table.columns.size(); i++) {
        const auto& col = table.columns[i];
        
        oss << "  `" << col.name << "` ";
        
        // 类型映射
        switch (col.type) {
            case 1: oss << "TINYINT"; break;
            case 2: oss << "SMALLINT"; break;
            case 3: oss << "INT"; break;
            case 4: oss << "FLOAT"; break;
            case 5: oss << "DOUBLE"; break;
            case 8: oss << "BIGINT"; break;
            case 9: oss << "MEDIUMINT"; break;
            case 10: oss << "DATE"; break;
            case 11: oss << "TIME"; break;
            case 12: oss << "DATETIME"; break;
            case 13: oss << "YEAR"; break;
            case 15: oss << "VARCHAR(" << col.length << ")"; break;
            case 253: oss << "VAR_STRING(" << col.length << ")"; break;
            case 252: oss << "BLOB"; break;
            default: oss << "BINARY(" << col.length << ")"; break;
        }
        
        if (!col.is_nullable) {
            oss << " NOT NULL";
        }
        
        if (!col.default_value.empty()) {
            oss << " DEFAULT " << col.default_value;
        }
        
        if (i < table.columns.size() - 1) {
            oss << ",";
        }
        oss << "\n";
    }
    
    // 主键
    for (const auto& idx : table.indexes) {
        if (idx.type == 1) {  // 主键
            oss << ", PRIMARY KEY (";
            for (size_t i = 0; i < idx.column_names.size(); i++) {
                if (i > 0) oss << ", ";
                oss << "`" << idx.column_names[i] << "`";
            }
            oss << ")\n";
            break;
        }
    }
    
    oss << ") ENGINE=InnoDB";
    
    // 行格式
    if (table.row_format != RowFormat::COMPACT) {
        oss << " ROW_FORMAT=" << row_format_to_string(table.row_format);
    }
    
    oss << ";\n";
    
    return oss.str();
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_METADATA_RECOVERY_HPP
