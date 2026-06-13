#ifndef DBRECOVERY_INNODB_PAGE_PARSER_V3_HPP
#define DBRECOVERY_INNODB_PAGE_PARSER_V3_HPP

/**
 * @file page_parser_v3.hpp
 * @brief MySQL 5.6-9.0 全版本兼容页解析层 V3
 * 
 * 架构改进：
 * 1. 策略模式 + 模板方法模式组合
 * 2. 零拷贝读取支持
 * 3. 增强的错误恢复能力
 * 4. 完整的行格式支持（Redundant/Compact/Dynamic/Compressed/Instant）
 */

#include <cstdint>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <optional>
#include <array>
#include <string_view>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_types_v2.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== 行值类型定义 ====================
using RowValue = std::optional<std::string>;

// ==================== 页解析器 V3: 统一页解析器 ====================
/**
 * @brief PageParserV3: 统一页解析器（V3 版本）
 * 
 * 核心特性：
 * - 自动版本探测
 * - 行格式智能识别
 * - 完整的 B+ 树遍历支持
 * - Undo Log 链条解析
 */
class PageParserV3 {
public:
    explicit PageParserV3(const uint8_t* data, size_t size);

    // 主解析接口
    PageParseResult parse();
    
    // 重新解析（使用指定版本）
    PageParseResult parse_as(MySQLVersion version);
    
    // 重新解析（使用指定行格式）
    PageParseResult parse_with_format(RowFormat format);

    // 获取解析结果
    const PageParseResult& result() const { return result_; }
    PageParseResult& result() { return result_; }

    // B+ 树遍历
    std::vector<PageParseResult> traverse_btree();
    
    // 获取页链
    std::vector<uint32_t> get_page_chain();

private:
    const uint8_t* data_;
    size_t size_;
    PageParseResult result_;
    std::unique_ptr<IPageParserStrategy> parser_;
    
    // 缓存
    mutable std::map<uint32_t, std::vector<uint8_t>> page_cache_;
};

// ==================== 行格式解析器接口 ====================
/**
 * @brief IRowParser: 行格式解析器接口
 */
class IRowParser {
public:
    virtual ~IRowParser() = default;

    // 解析记录头
    virtual RecordHeader parse_record_header(const uint8_t* data) = 0;
    
    // 解析记录数据
    virtual std::vector<RowValue> parse_record_data(
        const uint8_t* data,
        size_t data_size,
        const RecordHeader& header,
        const std::vector<catalog::ColumnInfo>& columns
    ) = 0;
    
    // 获取记录大小
    virtual size_t get_record_size(const uint8_t* data, size_t max_size) = 0;
    
    // 检查记录是否被删除
    virtual bool is_record_deleted(const uint8_t* data) = 0;
    
    // 检查是否为最小记录（infimum）
    virtual bool is_min_record(const uint8_t* data) = 0;
    
    // 检查是否为最大记录（supremum）
    virtual bool is_max_record(const uint8_t* data) = 0;
};

// ==================== Redundant 行格式解析器 ====================
/**
 * @brief RedundantRowParser: Redundant 行格式解析器
 *
 * 记录格式（6字节头 + 可变长度字段）：
 * InnoDB REDUNDANT/REDUNDANT row format header layout (6 bytes, big-endian):
 * Byte 0-3: info bits (32-bit field)
 *   bit 31: instant_flag
 *   bit 30: version_flag
 *   bit 29: deleted_flag
 *   bit 28: min_rec_flag
 *   bits 27-24: n_owned (4 bits)
 *   bits 23-11: heap_no (13 bits)
 *   bits 10-1: n_fields (10 bits)
 *   bit 0: short_flag
 * Byte 4-5: next_record_offset (unsigned 16-bit, relative to record start)
 */
class RedundantRowParser : public IRowParser {
public:
    // Parse as RedundantHeader (preferred for REDUNDANT format)
    RedundantHeader parse_redundant_header(const uint8_t* data);

    // Legacy interface - maps to RecordHeader (COMPACT format)
    RecordHeader parse_record_header(const uint8_t* data) override;
    std::vector<RowValue> parse_record_data(
        const uint8_t* data,
        size_t data_size,
        const RecordHeader& header,
        const std::vector<catalog::ColumnInfo>& columns
    ) override;
    size_t get_record_size(const uint8_t* data, size_t max_size) override;
    bool is_record_deleted(const uint8_t* data) override;
    bool is_min_record(const uint8_t* data) override;
    bool is_max_record(const uint8_t* data) override;
};

// ==================== Compact 行格式解析器 ====================
/**
 * @brief CompactRowParser: Compact 行格式解析器
 * 
 * 记录格式（5字节头 + 可变长度字段）：
 * - 1 byte: info_bits (包含 n_fields 的高5位)
 * - 2 bytes: next_record_offset
 * - 2 bytes: heap_no
 * - 1 byte: n_fields (低4位)
 * - n bytes: variable_lengths (1-2 bytes each)
 * - n bytes: column_data
 */
class CompactRowParser : public IRowParser {
public:
    RecordHeader parse_record_header(const uint8_t* data) override;
    std::vector<RowValue> parse_record_data(
        const uint8_t* data,
        size_t data_size,
        const RecordHeader& header,
        const std::vector<catalog::ColumnInfo>& columns
    ) override;
    size_t get_record_size(const uint8_t* data, size_t max_size) override;
    bool is_record_deleted(const uint8_t* data) override;
    bool is_min_record(const uint8_t* data) override;
    bool is_max_record(const uint8_t* data) override;
};

// ==================== Dynamic 行格式解析器 ====================
/**
 * @brief DynamicRowParser: Dynamic 行格式解析器
 * 
 * 支持：
 * - 外部存储的 BLOB 指针
 * - 压缩页支持
 * - Instant DDL 元数据
 */
class DynamicRowParser : public IRowParser {
public:
    struct ExternalPointer {
        uint32_t space_id;
        uint32_t page_no;
        uint32_t offset;
        uint32_t len;
        bool is_compressed;
    };

    RecordHeader parse_record_header(const uint8_t* data) override;
    std::vector<RowValue> parse_record_data(
        const uint8_t* data,
        size_t data_size,
        const RecordHeader& header,
        const std::vector<catalog::ColumnInfo>& columns
    ) override;
    size_t get_record_size(const uint8_t* data, size_t max_size) override;
    bool is_record_deleted(const uint8_t* data) override;
    bool is_min_record(const uint8_t* data) override;
    bool is_max_record(const uint8_t* data) override;

    // 解析外部 BLOB 指针
    std::optional<ExternalPointer> parse_external_pointer(const uint8_t* data, size_t data_size);
};

// ==================== B+ 树遍历器 ====================
/**
 * @brief BTreeTraverser: B+ 树遍历器
 */
class BTreeTraverser {
public:
    struct TraversalStats {
        uint32_t total_pages;
        uint32_t leaf_pages;
        uint32_t internal_pages;
        uint32_t root_page;
        uint32_t max_level;
    };

    explicit BTreeTraverser(const uint8_t* data, size_t size);

    // 从指定页开始遍历
    std::vector<PageParseResult> traverse_from_page(uint32_t page_no);
    
    // 从根页开始遍历
    std::vector<PageParseResult> traverse_from_root();
    
    // 获取遍历统计
    TraversalStats stats() const { return stats_; }

private:
    const uint8_t* data_;
    size_t size_;
    TraversalStats stats_;
    
    // 递归遍历
    void traverse_recursive(
        uint32_t page_no,
        uint16_t level,
        std::vector<PageParseResult>& results
    );
};

// ==================== Undo Log 链条解析器 ====================
/**
 * @brief UndoLogChainParser: Undo Log 链条解析器
 */
class UndoLogChainParser {
public:
    struct UndoLogEntry {
        uint64_t trx_id;
        uint64_t roll_ptr;
        uint32_t page_no;
        uint16_t offset;
        uint8_t type;  // TRX_UNDO_INSERT/UPDATE/DELETE
        std::vector<uint8_t> data;
    };

    explicit UndoLogChainParser(const uint8_t* data, size_t size);

    // 从 Undo Log 页解析链条
    std::vector<UndoLogEntry> parse_chain(uint32_t first_page);
    
    // 获取所有 Undo Log 页
    std::vector<uint32_t> get_undo_log_pages();

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

// ==================== 增强的页解析器工厂 ====================
/**
 * @brief PageParserFactoryV3: 增强的页解析器工厂
 */
class PageParserFactoryV3 {
public:
    // 创建页解析器（自动探测）
    static std::unique_ptr<IPageParserStrategy> create_parser(
        const uint8_t* data, size_t size);
    
    // 创建行格式解析器
    static std::unique_ptr<IRowParser> create_row_parser(RowFormat format);
    
    // 识别行格式（增强版）
    static RowFormat detect_row_format_v3(const uint8_t* data, size_t size);

private:
    // 从 BTR header flags 识别行格式
    static RowFormat detect_from_btr_flags(const uint8_t* data, size_t size);
    
    // 从记录头识别行格式
    static RowFormat detect_from_record_header(const uint8_t* data, size_t size);
};

// ==================== 实用工具函数 ====================

/**
 * @brief 计算记录的实际大小（包括可变长度字段）
 */
size_t calculate_record_size(
    const uint8_t* data,
    size_t data_size,
    RowFormat format,
    uint16_t n_fields
);

/**
 * @brief 从记录中提取字段值
 */
std::vector<std::optional<std::string>> extract_field_values(
    const uint8_t* record_data,
    size_t record_len,
    RowFormat format,
    const std::vector<catalog::ColumnInfo>& columns
);

/**
 * @brief 生成建表语句（启发式）
 */
std::string generate_create_statement_heuristic(
    const std::vector<RowValue>& sample_rows,
    const std::vector<catalog::ColumnInfo>& inferred_columns
);

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_PARSER_V3_HPP
