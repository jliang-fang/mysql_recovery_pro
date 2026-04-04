#ifndef DBRECOVERY_INNODB_PAGE_PARSER_V82_HPP
#define DBRECOVERY_INNODB_PAGE_PARSER_V82_HPP

/**
 * @file page_parser_v82.hpp
 * @brief MySQL 8.1/8.2/8.3 兼容页解析器
 *
 * MySQL 8.1/8.2/8.3 是 MySQL 8.0 的小版本更新，主要变化：
 * - 更好的并行查询支持
 * - 增强的 JSON 功能
 * - 更好的索引压缩
 * - Instant DDL 改进
 */

#include <cstdint>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <array>
#include <string_view>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_types_v2.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/page_parser_v3.hpp"  // For RowValue

namespace dbrecovery {
namespace innodb {

// ==================== MySQL 8.1/8.2/8.3 页解析器 ====================
/**
 * @brief MySQL82PageParser: MySQL 8.1/8.2/8.3 页解析器
 * 
 * 特征：
 * - 支持所有 MySQL 8.0 行格式
 * - 增强的 SDI 支持
 * - 更好的压缩支持
 * - Instant DDL 改进
 */
class MySQL82PageParser : public GenericPageParser {
public:
    explicit MySQL82PageParser(RowFormat format);

    bool parse_header(const uint8_t* data, size_t size) override;
    std::vector<Record> parse_records(const uint8_t* data, size_t size) override;
    std::optional<catalog::TableSchema> parse_sdi_metadata(
        const uint8_t* data, size_t size) override;

    // MySQL 8.2 特有：增强的 Instant DDL 支持
    bool parse_instant_ddl_metadata_v82(const uint8_t* data, size_t size);
    uint64_t get_instant_ddl_version() const { return instant_ddl_version_; }

    // 压缩支持增强
    bool is_compressed_page() const { return is_compressed_; }
    uint16_t get_zip_size() const { return zip_size_; }

private:
    uint64_t instant_ddl_version_;
    bool is_compressed_;
    uint16_t zip_size_;
};

// ==================== MySQL 8.3 页解析器 ====================
/**
 * @brief MySQL83PageParser: MySQL 8.3 页解析器
 * 
 * 特征：
 * - 支持所有 MySQL 8.2 行格式
 * - 更好的并行查询支持
 * - 增强的 JSON 功能
 */
class MySQL83PageParser : public MySQL82PageParser {
public:
    explicit MySQL83PageParser(RowFormat format);

    bool parse_header(const uint8_t* data, size_t size) override;
    std::optional<catalog::TableSchema> parse_sdi_metadata(
        const uint8_t* data, size_t size) override;

    // MySQL 8.3 特有：并行查询支持
    bool parse_parallel_query_metadata(const uint8_t* data, size_t size);
    uint32_t get_parallel_degree() const { return parallel_degree_; }

private:
    uint32_t parallel_degree_;
};

// ==================== 增强的页解析器工厂 ====================
/**
 * @brief PageParserFactoryV82: MySQL 8.1/8.2/8.3 页解析器工厂
 */
class PageParserFactoryV82 {
public:
    // 创建页解析器（自动探测）
    static std::unique_ptr<IPageParserStrategy> create_parser(
        const uint8_t* data, size_t size);
    
    // 识别 MySQL 版本（8.1/8.2/8.3）
    static MySQLVersion detect_mysql_version_v82(const uint8_t* data, size_t size);

    // 从 SDI 页识别版本
    static MySQLVersion detect_version_from_sdi_v82(const uint8_t* data, size_t size);

    // 从页头识别版本
    static MySQLVersion detect_version_from_header_v82(const uint8_t* data, size_t size);

private:
    // 从 SDI 签名识别版本
    static bool check_sdi_signature(const uint8_t* data, size_t size, const uint8_t* sig);
    
    // 从页头字段识别版本
    static bool has_mysql82_features(const uint8_t* data, size_t size);
    static bool has_mysql83_features(const uint8_t* data, size_t size);
};

// ==================== B+ 树断裂页链修复器 ====================
/**
 * @brief BTreeChainReconstructor: B+ 树断裂页链修复器
 * 
 * 用于重建断裂的 B+ 树页链（Tree Grafting）
 */
class BTreeChainReconstructor {
public:
    struct ChainStats {
        uint32_t total_pages;
        uint32_t valid_chain;
        uint32_t broken_chain;
        uint32_t circular_reference;
        uint32_t grafts_performed;
    };

    explicit BTreeChainReconstructor();

    // 添加页
    void add_page(uint32_t page_no, const std::vector<uint8_t>& data);

    // 重建页链
    bool reconstruct();

    // 重建页链（从指定页开始）
    bool reconstruct_from_page(uint32_t start_page);

    // 获取重建后的页链
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages() const { return pages_; }

    // 获取页链统计
    ChainStats stats() const { return stats_; }

    // 获取断裂点
    std::vector<uint32_t> get_broken_points() const { return broken_points_; }

    // 获取修复后的页链
    std::vector<uint32_t> get_reconstructed_chain() const;

private:
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pages_;
    std::map<uint32_t, std::vector<uint8_t>> page_map_;
    ChainStats stats_;
    std::vector<uint32_t> broken_points_;
    
    // 页链重建
    bool build_chain_from_map();
    bool detect_and_fix_broken_chain();
    void graft_pages(uint32_t from_page, uint32_t to_page);
};

// ==================== Undo Log Flashback 恢复器 ====================
/**
 * @brief UndoLogFlashback: Undo Log Flashback 恢复器
 * 
 * 用于从 Undo Log 恢复已删除的数据（Flashback）
 */
class UndoLogFlashback {
public:
    struct FlashbackEntry {
        uint64_t trx_id;
        uint64_t roll_ptr;
        uint32_t page_no;
        uint16_t offset;
        uint8_t type;  // TRX_UNDO_INSERT/UPDATE/DELETE
        std::vector<uint8_t> data;
        std::vector<catalog::ColumnInfo> columns;
    };

    explicit UndoLogFlashback(const uint8_t* data, size_t size);

    // 从 Undo Log 页解析 Flashback 数据
    std::vector<FlashbackEntry> parse_flashback(uint32_t first_page);
    
    // 获取所有 Undo Log 页
    std::vector<uint32_t> get_undo_log_pages();

    // 生成 Flashback SQL
    std::string generate_flashback_sql(const std::vector<FlashbackEntry>& entries);

    // 恢复数据
    std::vector<std::vector<RowValue>> restore_data(const std::vector<FlashbackEntry>& entries);

private:
    const uint8_t* data_;
    size_t size_;
    
    // 解析 Undo Log Header
    struct UndoLogHeader {
        uint16_t state;  // TRX_UNDO_ACTIVE/REMOVED
        uint16_t direction;  // TRX_UNDO_INSERT/DELETE
        uint32_t last_page;
        uint16_t last_offset;
        uint32_t prev_page;
        uint32_t next_page;
    };
    
    UndoLogHeader parse_undo_log_header(const uint8_t* page_data);
};

// ==================== Instant DDL 元数据解析器 ====================
/**
 * @brief InstantDDLParser: Instant DDL 元数据解析器
 * 
 * 支持 MySQL 8.0+ 的 Instant DDL 功能
 */
class InstantDDLParser {
public:
    struct InstantMetadata {
        uint64_t version;
        uint32_t added_columns;
        uint32_t dropped_columns;
        uint32_t modified_columns;
        std::vector<uint32_t> column_offsets;
    };

    explicit InstantDDLParser(const uint8_t* data, size_t size);

    // 解析 Instant DDL 元数据
    std::optional<InstantMetadata> parse_metadata(const uint8_t* data, size_t size);

    // 检查是否为 Instant DDL 页
    static bool is_instant_ddl_page(const uint8_t* data, size_t size);

    // 获取 Instant DDL 版本
    static uint64_t get_instant_ddl_version(const uint8_t* data, size_t size);

private:
    const uint8_t* data_;
    size_t size_;
};

// ==================== 实用工具函数 ====================

/**
 * @brief 从页数据中提取页级别
 */
inline uint16_t get_page_level_v82(const uint8_t* data) {
    return read_le<uint16_t>(data, FIL_HEADER_SIZE + 4);
}

/**
 * @brief 从页数据中提取页号
 */
inline uint32_t get_page_offset_v82(const uint8_t* data) {
    return read_le<uint32_t>(data, 20);
}

/**
 * @brief 从页数据中提取页链信息
 */
inline std::pair<uint32_t, uint32_t> get_page_chain_v82(const uint8_t* data) {
    uint32_t prev = read_le<uint32_t>(data, 24);
    uint32_t next = read_le<uint32_t>(data, 28);
    return {prev, next};
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_PARSER_V82_HPP
