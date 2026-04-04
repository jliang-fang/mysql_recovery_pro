#ifndef DBRECOVERY_INNODB_INSTANT_DDL_PARSER_HPP
#define DBRECOVERY_INNODB_INSTANT_DDL_PARSER_HPP

#include "dbrecovery/innodb/page_types_v2.hpp"
#include <vector>
#include <optional>
#include <map>

namespace dbrecovery {
namespace innodb {

/**
 * @brief Instant DDL 元数据版本
 */
enum class InstantDDLVersion : uint16_t {
    V1 = 1,    // MySQL 8.0.0-8.0.22
    V2 = 2,    // MySQL 8.0.23+
    V3 = 3,    // MySQL 8.4+
};

/**
 * @brief Instant DDL 操作类型
 */
enum class InstantDDLOperation : uint8_t {
    ADD_COLUMN = 0x01,      // 添加列
    DROP_COLUMN = 0x02,     // 删除列
    SET_DEFAULT = 0x03,     // 设置默认值
    DROP_DEFAULT = 0x04,    // 删除默认值
    ADD_INDEX = 0x05,       // 添加索引
    DROP_INDEX = 0x06,      // 删除索引
    CHANGE_COLUMN = 0x07,   // 修改列
    REORDER_COLUMNS = 0x08, // 重新排序列
    ADD_PRIMARY_KEY = 0x09, // 添加主键
    DROP_PRIMARY_KEY = 0x0a, // 删除主键
};

/**
 * @brief Instant DDL 元数据
 */
struct InstantDDLMetadata {
    uint16_t version;              // 元数据版本
    uint16_t n_added_cols;         // 添加的列数
    uint16_t n_dropped_cols;       // 删除的列数
    uint16_t n_added_indexes;      // 添加的索引数
    uint16_t n_dropped_indexes;    // 删除的索引数
    
    // 添加的列信息
    struct AddedColumn {
        uint16_t col_idx;          // 原始列索引
        uint16_t new_col_idx;      // 新列索引
        uint8_t type;              // 列类型
        uint32_t flags;            // 列标志
        uint32_t length;           // 列长度
        bool is_nullable;          // 是否可为空
        bool is_virtual;           // 是否虚拟列
        bool is_generated;         // 是否生成列
        std::string col_name;
        std::optional<std::string> default_value;
    };
    std::vector<AddedColumn> added_columns;
    
    // 删除的列信息
    struct DroppedColumn {
        uint16_t col_idx;          // 原始列索引
        std::string col_name;
    };
    std::vector<DroppedColumn> dropped_columns;
    
    // 添加的索引信息
    struct AddedIndex {
        uint64_t index_id;         // 索引ID
        std::string index_name;
        uint32_t type;             // 索引类型
        std::vector<std::string> column_names;
    };
    std::vector<AddedIndex> added_indexes;
    
    // 删除的索引信息
    struct DroppedIndex {
        uint64_t index_id;         // 索引ID
        std::string index_name;
    };
    std::vector<DroppedIndex> dropped_indexes;
    
    // 默认值信息
    struct DefaultValue {
        uint16_t col_idx;
        std::string value;
    };
    std::vector<DefaultValue> default_values;
    
    // Instant DDL 页头字段
    uint16_t n_all_cols;           // 总列数 (包括删除的)
    uint16_t n_existing_cols;      // 现有列数
    
    std::string to_string() const;
};

/**
 * @brief Instant DDL 页头
 */
struct InstantDDLPageHeader {
    uint16_t metadata_offset;      // 元数据偏移
    uint16_t metadata_version;     // 元数据版本
    uint32_t metadata_size;        // 元数据大小
    
    // 变长字段
    uint16_t n_all_cols;           // 总列数 (包括删除的)
    uint16_t n_existing_cols;      // 现有列数
    
    bool parse(const uint8_t* data, size_t size);
    std::string to_string() const;
};

/**
 * @brief Instant DDL 解析器
 */
class InstantDDLParser {
public:
    InstantDDLParser();
    
    /**
     * @brief 解析页头中的 Instant DDL 元数据
     */
    std::optional<InstantDDLMetadata> parse_metadata(const uint8_t* data, size_t size,
                                                     uint16_t metadata_offset,
                                                     uint16_t metadata_version);
    
    /**
     * @brief 解析元数据变长部分
     */
    std::optional<InstantDDLMetadata> parse_variable_part(const uint8_t* data, size_t size,
                                                          InstantDDLMetadata& metadata);
    
    /**
     * @brief 从页数据解析
     */
    std::optional<InstantDDLMetadata> parse_from_page(const uint8_t* data, size_t size);
    
    /**
     * @brief 重建列映射
     * @param old_cols 原始列数
     * @param metadata Instant DDL 元数据
     * @return 列映射表 (old_idx -> new_idx)
     */
    std::map<uint16_t, uint16_t> rebuild_column_mapping(uint16_t old_cols,
                                                        const InstantDDLMetadata& metadata);
    
    /**
     * @brief 重建索引映射
     */
    std::map<uint64_t, uint64_t> rebuild_index_mapping(const InstantDDLMetadata& metadata);
    
    /**
     * @brief 应用 Instant DDL 变更到行数据
     */
    std::vector<uint8_t> apply_changes(const std::vector<uint8_t>& row_data,
                                       const InstantDDLMetadata& metadata,
                                       uint16_t old_n_cols);
    
    /**
     * @brief 获取解析统计信息
     */
    struct Stats {
        size_t metadata_parsed;
        size_t columns_reconstructed;
        size_t indexes_reconstructed;
        size_t errors;
    };
    
    Stats get_stats() const;
    
private:
    Stats stats_;
    
    bool parse_added_columns(const uint8_t* data, size_t size,
                             InstantDDLMetadata& metadata);
    bool parse_dropped_columns(const uint8_t* data, size_t size,
                               InstantDDLMetadata& metadata);
    bool parse_added_indexes(const uint8_t* data, size_t size,
                             InstantDDLMetadata& metadata);
    bool parse_dropped_indexes(const uint8_t* data, size_t size,
                               InstantDDLMetadata& metadata);
    bool parse_default_values(const uint8_t* data, size_t size,
                              InstantDDLMetadata& metadata);
};

// ==================== 实现 ====================

inline InstantDDLParser::InstantDDLParser() {
    stats_ = {0, 0, 0, 0};
}

inline InstantDDLParser::Stats InstantDDLParser::get_stats() const {
    return stats_;
}

// ==================== InstantDDLMetadata ====================

inline std::string InstantDDLMetadata::to_string() const {
    std::ostringstream oss;
    oss << "InstantDDLMetadata{"
        << "version=" << version
        << ", added_cols=" << n_added_cols
        << ", dropped_cols=" << n_dropped_cols
        << ", added_indexes=" << n_added_indexes
        << ", dropped_indexes=" << n_dropped_indexes;
    
    if (!added_columns.empty()) {
        oss << ", added_cols=[";
        for (size_t i = 0; i < std::min(size_t(3), added_columns.size()); i++) {
            oss << added_columns[i].col_name << "(" << added_columns[i].new_col_idx << ")";
            if (i < std::min(size_t(3), added_columns.size()) - 1) oss << ", ";
        }
        if (added_columns.size() > 3) oss << "...";
        oss << "]";
    }
    
    oss << "}";
    return oss.str();
}

// ==================== InstantDDLPageHeader ====================

inline bool InstantDDLPageHeader::parse(const uint8_t* data, size_t size) {
    if (size < 12) {
        return false;
    }
    
    metadata_offset = read_le<uint16_t>(data, 0);
    metadata_version = read_le<uint16_t>(data, 2);
    metadata_size = read_le<uint32_t>(data, 4);
    n_all_cols = read_le<uint16_t>(data, 8);
    n_existing_cols = read_le<uint16_t>(data, 10);
    
    return true;
}

inline std::string InstantDDLPageHeader::to_string() const {
    std::ostringstream oss;
    oss << "InstantDDLPageHeader{"
        << "offset=" << metadata_offset
        << ", version=" << metadata_version
        << ", size=" << metadata_size
        << ", all_cols=" << n_all_cols
        << ", existing_cols=" << n_existing_cols
        << "}";
    return oss.str();
}

// ==================== InstantDDLParser ====================

inline std::optional<InstantDDLMetadata> InstantDDLParser::parse_metadata(
    const uint8_t* data, size_t size, uint16_t metadata_offset, uint16_t metadata_version) {
    if (metadata_offset + 12 > size) {
        return std::nullopt;
    }

    InstantDDLMetadata metadata;
    metadata.version = metadata_version;

    // 读取固定部分
    size_t offset = metadata_offset;
    metadata.n_added_cols = read_le<uint16_t>(data, offset);
    metadata.n_dropped_cols = read_le<uint16_t>(data, offset + 2);
    metadata.n_added_indexes = read_le<uint16_t>(data, offset + 4);
    metadata.n_dropped_indexes = read_le<uint16_t>(data, offset + 6);
    metadata.n_all_cols = read_le<uint16_t>(data, offset + 8);
    metadata.n_existing_cols = read_le<uint16_t>(data, offset + 10);

    offset += 12;

    // 解析变长部分
    if (!parse_variable_part(data + offset, size - offset, metadata)) {
        return std::nullopt;
    }

    stats_.metadata_parsed++;
    return metadata;
}

inline std::optional<InstantDDLMetadata> InstantDDLParser::parse_variable_part(
    const uint8_t* data, size_t size, InstantDDLMetadata& metadata) {
    size_t offset = 0;
    
    // 解析添加的列
    if (!parse_added_columns(data + offset, size - offset, metadata)) {
        return std::nullopt;
    }
    offset += metadata.n_added_cols * 16;  // 简化估计
    
    // 解析删除的列
    if (!parse_dropped_columns(data + offset, size - offset, metadata)) {
        return std::nullopt;
    }
    offset += metadata.n_dropped_cols * 4;
    
    // 解析添加的索引
    if (!parse_added_indexes(data + offset, size - offset, metadata)) {
        return std::nullopt;
    }
    offset += metadata.n_added_indexes * 24;
    
    // 解析删除的索引
    if (!parse_dropped_indexes(data + offset, size - offset, metadata)) {
        return std::nullopt;
    }
    offset += metadata.n_dropped_indexes * 10;
    
    // 解析默认值
    if (!parse_default_values(data + offset, size - offset, metadata)) {
        return std::nullopt;
    }
    
    return metadata;
}

inline std::optional<InstantDDLMetadata> InstantDDLParser::parse_from_page(
    const uint8_t* data, size_t size) {
    if (size < 58) {
        return std::nullopt;
    }
    
    // 读取页头中的 Instant DDL 信息
    uint16_t metadata_offset = read_le<uint16_t>(data, 54);
    uint16_t metadata_version = read_le<uint16_t>(data, 56);
    
    return parse_metadata(data, size, metadata_offset, metadata_version);
}

inline std::map<uint16_t, uint16_t> InstantDDLParser::rebuild_column_mapping(
    uint16_t old_cols, const InstantDDLMetadata& metadata) {
    std::map<uint16_t, uint16_t> mapping;
    
    // 初始化映射 (保留原有列)
    for (uint16_t i = 0; i < old_cols; i++) {
        mapping[i] = i;
    }
    
    // 应用添加的列
    uint16_t new_idx = old_cols;
    for (const auto& col : metadata.added_columns) {
        mapping[col.col_idx] = col.new_col_idx;
        new_idx++;
    }
    
    // 应用删除的列
    for (const auto& col : metadata.dropped_columns) {
        mapping[col.col_idx] = static_cast<uint16_t>(0xFFFF);  // 标记为删除
    }
    
    return mapping;
}

inline std::map<uint64_t, uint64_t> InstantDDLParser::rebuild_index_mapping(
    const InstantDDLMetadata& metadata) {
    std::map<uint64_t, uint64_t> mapping;
    
    // 添加的索引
    for (const auto& idx : metadata.added_indexes) {
        mapping[idx.index_id] = idx.index_id;
    }
    
    // 删除的索引标记为 0
    for (const auto& idx : metadata.dropped_indexes) {
        mapping[idx.index_id] = 0;
    }
    
    return mapping;
}

inline std::vector<uint8_t> InstantDDLParser::apply_changes(
    const std::vector<uint8_t>& row_data, const InstantDDLMetadata& metadata,
    uint16_t old_n_cols) {
    std::vector<uint8_t> result;
    
    // 简化实现：保留原有数据
    result = row_data;
    
    // TODO: 实现完整的列变更逻辑
    stats_.columns_reconstructed += metadata.n_added_cols + metadata.n_dropped_cols;
    
    return result;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_INSTANT_DDL_PARSER_HPP
