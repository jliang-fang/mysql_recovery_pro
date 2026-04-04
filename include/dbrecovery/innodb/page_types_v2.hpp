#ifndef DBRECOVERY_INNODB_PAGE_TYPES_V2_HPP
#define DBRECOVERY_INNODB_PAGE_TYPES_V2_HPP

/**
 * @file page_types_v2.hpp
 * @brief MySQL 5.6-9.0 全版本兼容页类型定义
 *
 * 版本历史:
 * - v1.0: 初始版本，支持 MySQL 5.6-9.0
 * - v2.0: 增强 SDI 支持，添加 Instant DDL 类型
 */

#include <cstdint>
#include <string>
#include <optional>
#include <vector>
#include <map>
#include <memory>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== MySQL 版本枚举 ====================
enum class MySQLVersion : uint8_t {
    UNKNOWN = 0,
    MYSQL_41 = 41,      // MySQL 4.1.x (早期 Redundant 格式)
    MYSQL_50 = 50,      // MySQL 5.0.x (引入 COMPACT 格式)
    MYSQL_51 = 51,      // MySQL 5.1.x
    MYSQL_55 = 55,      // MySQL 5.5.x
    MYSQL_56 = 56,      // MySQL 5.6.x
    MYSQL_57 = 57,      // MySQL 5.7.x
    MYSQL_80 = 80,      // MySQL 8.0.x
    MYSQL_81 = 81,      // MySQL 8.1.x
    MYSQL_82 = 82,      // MySQL 8.2.x
    MYSQL_83 = 83,      // MySQL 8.3.x
    MYSQL_84 = 84,      // MySQL 8.4.x
    MYSQL_90 = 90,      // MySQL 9.0.x (预览版)
};

// ==================== 行格式枚举 ====================
enum class RowFormat : uint8_t {
    UNKNOWN = 0,
    REDUNDANT = 1,      // MySQL 4.1-5.0 (旧格式)
    COMPACT = 2,        // MySQL 5.0+ (紧凑格式)
    DYNAMIC = 3,        // MySQL 5.7+ (动态行格式)
    COMPRESSED = 4,     // MySQL 5.7+ (压缩格式)
};

// ==================== 页校验和类型 ====================
enum class ChecksumType : uint8_t {
    NONE = 0,           // 无校验和
    CRC32 = 1,          // CRC32 (默认)
    INNODB = 2,         // InnoDB 专用校验和
    FNV1A = 3,          // FNV-1a 校验和
    BLOCK = 4,          // Block 校验和
};

// ==================== 页状态枚举 ====================
enum class PageState : uint8_t {
    ALLOCATED = 0,      // 新分配的页
    INDEX = 1,          // B+树索引页
    UNDO_LOG = 2,       // Undo log 页
    SYSTEM = 3,         // 系统页
    TRX_SYSTEM = 4,     // 事务系统页
    FILE_SPACE_HDR = 5, // 文件空间头页
    BLOB = 6,           // BLOB 页
    FULLTEXT = 7,       // 全文索引页
};

// ==================== 页类型字符串化 ====================
inline std::string page_state_to_string(PageState state) {
    switch (state) {
        case PageState::ALLOCATED: return "ALLOCATED";
        case PageState::INDEX: return "INDEX";
        case PageState::UNDO_LOG: return "UNDO_LOG";
        case PageState::SYSTEM: return "SYSTEM";
        case PageState::TRX_SYSTEM: return "TRX_SYSTEM";
        case PageState::FILE_SPACE_HDR: return "FILE_SPACE_HDR";
        case PageState::BLOB: return "BLOB";
        case PageState::FULLTEXT: return "FULLTEXT";
        default: return "UNKNOWN";
    }
}

inline std::string checksum_type_to_string(ChecksumType type) {
    switch (type) {
        case ChecksumType::NONE: return "NONE";
        case ChecksumType::CRC32: return "CRC32";
        case ChecksumType::INNODB: return "INNODB";
        case ChecksumType::FNV1A: return "FNV1A";
        case ChecksumType::BLOCK: return "BLOCK";
        default: return "UNKNOWN";
    }
}

// ==================== 页解析策略接口 ====================
/**
 * @brief IPageParserStrategy: 页解析策略接口
 *
 * 所有版本特定的页解析器必须实现此接口
 */
class IPageParserStrategy {
public:
    virtual ~IPageParserStrategy() = default;

    // 解析页头信息
    virtual bool parse_header(const uint8_t* data, size_t size) = 0;

    // 解析记录
    virtual std::vector<Record> parse_records(const uint8_t* data, size_t size) = 0;

    // 获取页类型
    virtual uint16_t get_page_type() const = 0;

    // 获取行格式
    virtual RowFormat get_row_format() const = 0;

    // 获取 MySQL 版本
    virtual MySQLVersion get_mysql_version() const = 0;

    // 验证页健康状态
    virtual bool validate_health() const = 0;

    // 获取页级别（B+树高度）
    virtual uint16_t get_page_level() const = 0;

    // 解析 SDI 元数据
    virtual std::optional<catalog::TableSchema> parse_sdi_metadata(
        const uint8_t* data, size_t size) = 0;

    // 获取页链信息
    virtual std::pair<uint32_t, uint32_t> get_page_chain() const = 0;
};

// ==================== 页解析器工厂 ====================
/**
 * @brief PageParserFactory: 页解析器工厂
 *
 * 根据页数据自动识别 MySQL 版本和行格式，创建对应的解析器
 */
class PageParserFactory {
public:
    // 创建页解析器
    static std::unique_ptr<IPageParserStrategy> create_parser(
        const uint8_t* data, size_t size);

    // 识别 MySQL 版本
    static MySQLVersion detect_mysql_version(const uint8_t* data, size_t size);

    // 识别行格式
    static RowFormat detect_row_format(const uint8_t* data, size_t size);

    // 从页头识别版本（基于页类型和特征码）
    static MySQLVersion detect_version_from_header(const uint8_t* data, size_t size);

    // 从 SDI 页识别版本
    static MySQLVersion detect_version_from_sdi(const uint8_t* data, size_t size);
};

// ==================== 通用页解析器（模板方法模式） ====================
/**
 * @brief GenericPageParser: 通用页解析器（实现策略接口）
 *
 * 提供基础的页解析功能，子类可覆盖特定方法
 */
class GenericPageParser : public IPageParserStrategy {
public:
    explicit GenericPageParser(MySQLVersion version, RowFormat format);

    bool parse_header(const uint8_t* data, size_t size) override;
    std::vector<Record> parse_records(const uint8_t* data, size_t size) override;
    uint16_t get_page_type() const override { return page_type_; }
    RowFormat get_row_format() const override { return row_format_; }
    MySQLVersion get_mysql_version() const override { return mysql_version_; }
    bool validate_health() const override;
    uint16_t get_page_level() const override { return page_level_; }
    std::optional<catalog::TableSchema> parse_sdi_metadata(
        const uint8_t* data, size_t size) override;
    std::pair<uint32_t, uint32_t> get_page_chain() const override;

protected:
    // 子类可覆盖的解析方法
    virtual bool parse_fil_header(const uint8_t* data, size_t size);
    virtual bool parse_page_header(const uint8_t* data, size_t size);
    virtual bool parse_btr_header(const uint8_t* data, size_t size);
    virtual std::vector<uint16_t> parse_record_offsets(const uint8_t* data, size_t size);
    virtual Record parse_record(const uint8_t* data, size_t offset, size_t size);

    // 辅助方法
    uint16_t read_uint16_le(const uint8_t* data, size_t offset) const;
    uint32_t read_uint32_le(const uint8_t* data, size_t offset) const;
    uint64_t read_uint64_le(const uint8_t* data, size_t offset) const;

protected:
    MySQLVersion mysql_version_;
    RowFormat row_format_;
    uint16_t page_type_;
    uint16_t page_level_;
    uint32_t page_offset_;
    uint32_t prev_page_;
    uint32_t next_page_;
    uint16_t n_dir_slots_;
    uint16_t heap_top_;
    uint16_t n_heap_;
    uint16_t free_offset_;

    // 页健康状态
    bool header_parsed_;
    bool records_parsed_;
    bool health_valid_;
};

// ==================== MySQL 5.6/5.7 页解析器 ====================
/**
 * @brief MySQL56PageParser: MySQL 5.6/5.7 页解析器
 *
 * 特征：
 * - 支持 REDUNDANT/COMPACT 行格式
 * - 使用 ibdata1 系统字典
 * - 无 SDI（元数据存储在系统表中）
 */
class MySQL56PageParser : public GenericPageParser {
public:
    explicit MySQL56PageParser(RowFormat format);

    bool parse_header(const uint8_t* data, size_t size) override;
    std::vector<Record> parse_records(const uint8_t* data, size_t size) override;
    std::optional<catalog::TableSchema> parse_sdi_metadata(
        const uint8_t* data, size_t size) override;
};

// ==================== MySQL 8.0 页解析器 ====================
/**
 * @brief MySQL80PageParser: MySQL 8.0 页解析器
 *
 * 特征：
 * - 支持 REDUNDANT/COMPACT/DYNAMIC/COMPRESSED 行格式
 * - 使用独立的 mysql.ibd 系统字典
 * - 支持 SDI（Serialized Dictionary Info）
 */
class MySQL80PageParser : public GenericPageParser {
public:
    explicit MySQL80PageParser(RowFormat format);

    bool parse_header(const uint8_t* data, size_t size) override;
    std::vector<Record> parse_records(const uint8_t* data, size_t size) override;
    std::optional<catalog::TableSchema> parse_sdi_metadata(
        const uint8_t* data, size_t size) override;

    // MySQL 8.0 特有：Instant DDL 支持
    bool parse_instant_ddl_metadata(const uint8_t* data, size_t size);
    uint64_t get_metadata_version() const { return metadata_version_; }

private:
    uint64_t metadata_version_;
};

// ==================== MySQL 8.4+ 页解析器 ====================
/**
 * @brief MySQL84PageParser: MySQL 8.4+ 页解析器
 *
 * 特征：
 * - 支持所有 MySQL 8.0 行格式
 * - 增强的 SDI 支持
 * - 更好的压缩支持
 */
class MySQL84PageParser : public MySQL80PageParser {
public:
    explicit MySQL84PageParser(RowFormat format);

    bool parse_header(const uint8_t* data, size_t size) override;
    std::optional<catalog::TableSchema> parse_sdi_metadata(
        const uint8_t* data, size_t size) override;
};

// ==================== MySQL 9.0 页解析器 ====================
/**
 * @brief MySQL90PageParser: MySQL 9.0 页解析器
 *
 * 特征：
 * - 支持所有 MySQL 8.x 行格式
 * - 预留新特性支持
 */
class MySQL90PageParser : public MySQL84PageParser {
public:
    explicit MySQL90PageParser(RowFormat format);

    bool parse_header(const uint8_t* data, size_t size) override;
};

// ==================== 页解析结果 ====================
/**
 * @brief PageParseResult: 页解析结果
 */
struct PageParseResult {
    bool success;
    std::string error_message;
    uint16_t page_type;
    uint16_t page_level;
    RowFormat row_format;
    MySQLVersion version;
    uint32_t page_offset;
    uint32_t prev_page;
    uint32_t next_page;
    std::vector<Record> records;
    std::optional<catalog::TableSchema> schema;
    bool checksum_valid;
    uint32_t checksum_stored;
    uint32_t checksum_computed;
};

// ==================== 页扫描器 ====================
/**
 * @brief PageScanner: 页扫描器
 *
 * 用于扫描文件或磁盘，识别 InnoDB 页
 */
class PageScanner {
public:
    explicit PageScanner(size_t page_size = 16384);

    // 扫描文件中的所有页
    std::vector<PageParseResult> scan_file(const std::string& file_path);

    // 扫描内存中的页
    std::vector<PageParseResult> scan_buffer(const uint8_t* data, size_t size);

    // 识别 InnoDB 页
    static bool is_innodb_page(const uint8_t* data, size_t size);

    // 获取页大小
    size_t page_size() const { return page_size_; }

private:
    size_t page_size_;
};

// ==================== 页链重建器 ====================
/**
 * @brief PageChainReconstructor: 页链重建器
 *
 * 用于重建断裂的 B+ 树页链
 */
class PageChainReconstructor {
public:
    explicit PageChainReconstructor();

    // 添加页
    void add_page(uint32_t page_no, const std::vector<uint8_t>& data);

    // 重建页链
    bool reconstruct();

    // 获取重建后的页链
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages() const { return pages_; }

    // 获取页链统计
    struct ChainStats {
        uint32_t total_pages;
        uint32_t valid_chain;
        uint32_t broken_chain;
        uint32_t circular_reference;
    };
    ChainStats stats() const { return stats_; }

private:
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pages_;
    std::map<uint32_t, std::vector<uint8_t>> page_map_;
    ChainStats stats_;
};

// ==================== 页健康检查器 ====================
/**
 * @brief PageHealthChecker: 页健康检查器
 */
class PageHealthChecker {
public:
    struct HealthInfo {
        bool is_valid;
        std::string status_message;
        uint32_t checksum_expected;
        uint32_t checksum_actual;
        bool checksum_match;
        bool has_infimum;
        bool has_supremum;
        uint16_t n_records;
        uint16_t n_dir_slots;
        uint16_t page_level;
        uint32_t next_page;
        uint32_t prev_page;
        uint16_t free_space;
        ChecksumType checksum_type;  // 校验和类型
        uint32_t page_size;          // 页大小
        PageState page_state;        // 页状态
    };

    // 检查页健康状态
    static HealthInfo check(const uint8_t* data, size_t size);

    // 计算 CRC32 校验和
    static uint32_t compute_crc32(const uint8_t* data, size_t size);

    // 计算 InnoDB 校验和
    static uint32_t compute_innodb_checksum(const uint8_t* data, size_t size);

    // 验证页校验和
    static bool validate_checksum(const uint8_t* data, size_t size, 
                                   uint32_t* stored, uint32_t* computed,
                                   ChecksumType* type = nullptr);

    // 验证 INFIMUM/SUPREMUM 记录
    static bool validate_infimum_supremum(const uint8_t* data, size_t size);

    // 验证页目录
    static bool validate_directory(const uint8_t* data, size_t size);

    // 获取页状态
    static PageState get_page_state(uint16_t page_type);
};

// ==================== 页类型识别器 ====================
/**
 * @brief PageTypeDetector: 页类型识别器
 */
class PageTypeDetector {
public:
    // 从页数据识别页类型
    static uint16_t detect_page_type(const uint8_t* data, size_t size);

    // 从页类型识别版本
    static MySQLVersion detect_version(uint16_t page_type, const uint8_t* data, size_t size);

    // 页类型字符串化
    static std::string page_type_to_string(uint16_t type);

    // 从页数据识别行格式
    static RowFormat detect_row_format(const uint8_t* data, size_t size);

    // 从页数据识别所有特征
    static std::map<std::string, std::string> detect_all_features(const uint8_t* data, size_t size);
};

// ==================== 工具函数 ====================

/**
 * @brief 从页数据中提取页级别
 */
inline uint16_t get_page_level(const uint8_t* data) {
    return read_le<uint16_t>(data, FIL_HEADER_SIZE + 4);
}

/**
 * @brief 从页数据中提取页号
 */
inline uint32_t get_page_offset(const uint8_t* data) {
    return read_le<uint32_t>(data, 20);
}

/**
 * @brief 从页数据中提取页链信息
 */
inline std::pair<uint32_t, uint32_t> get_page_chain(const uint8_t* data) {
    uint32_t prev = read_le<uint32_t>(data, 24);
    uint32_t next = read_le<uint32_t>(data, 28);
    return {prev, next};
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_TYPES_V2_HPP
