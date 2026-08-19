/**
 * @file version_aware_recovery.hpp
 * @brief MySQL 5.6-9.0 版本感知恢复引擎
 *
 * 支持:
 * - MySQL 5.6: COMPACT 格式, 无 SDI
 * - MySQL 5.7: DYNAMIC 格式, Instant ADD COLUMN
 * - MySQL 8.0: SDI 页, 新校验和, 加密支持
 * - MySQL 8.4: 增强加密, 临时页
 * - MySQL 9.0: 最新 InnoDB 改进
 */

#ifndef DBRECOVERY_RECOVERY_VERSION_AWARE_RECOVERY_HPP
#define DBRECOVERY_RECOVERY_VERSION_AWARE_RECOVERY_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <string>
#include <memory>
#include <optional>
#include <functional>

#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/common/version_compat.hpp"

namespace dbrecovery {
namespace recovery {

// ==================== 版本特定配置 ====================

/**
 * @brief MySQL 版本特定配置
 */
struct VersionConfig {
    MySQLVersion version;
    std::string name;
    uint32_t default_page_size;
    bool supports_sdi;              // SDI 页支持 (MySQL 8.0+)
    bool supports_instant_ddl;      // Instant DDL 支持 (MySQL 5.7+)
    bool supports_encryption;       // 加密支持 (MySQL 8.0+)
    bool supports_temporary_pages;  // 临时页支持 (MySQL 8.4+)
    bool supports_new_checksum;     // 新校验和算法 (MySQL 8.0+)
    bool supports_invisible_columns;// 隐藏列支持 (MySQL 8.0+)
    bool supports_json_type;        // JSON 类型支持 (MySQL 5.7+)

    // 行格式支持
    bool supports_redundant;
    bool supports_compact;
    bool supports_dynamic;
    bool supports_compressed;

    // 页类型范围
    uint16_t min_page_type;
    uint16_t max_page_type;
};

// ==================== 版本配置表 ====================

/**
 * @brief 获取版本配置
 */
inline VersionConfig get_version_config(MySQLVersion version) {
    VersionConfig config{};

    switch (version) {
        case MySQLVersion::MYSQL_56:
            config = {
                MySQLVersion::MYSQL_56, "MySQL 5.6", 16384,
                false,  // supports_sdi
                false,  // supports_instant_ddl
                false,  // supports_encryption
                false,  // supports_temporary_pages
                false,  // supports_new_checksum
                false,  // supports_invisible_columns
                false,  // supports_json_type
                true,   // supports_redundant
                true,   // supports_compact
                false,  // supports_dynamic
                true,   // supports_compressed
                0x0000, 0x45BF  // page type range
            };
            break;

        case MySQLVersion::MYSQL_57:
            config = {
                MySQLVersion::MYSQL_57, "MySQL 5.7", 16384,
                false,  // supports_sdi
                true,   // supports_instant_ddl
                false,  // supports_encryption
                false,  // supports_temporary_pages
                false,  // supports_new_checksum
                false,  // supports_invisible_columns
                true,   // supports_json_type
                true,   // supports_redundant
                true,   // supports_compact
                true,   // supports_dynamic
                true,   // supports_compressed
                0x0000, 0x45BF
            };
            break;

        case MySQLVersion::MYSQL_80:
            config = {
                MySQLVersion::MYSQL_80, "MySQL 8.0", 16384,
                true,   // supports_sdi
                true,   // supports_instant_ddl
                true,   // supports_encryption
                false,  // supports_temporary_pages
                true,   // supports_new_checksum
                true,   // supports_invisible_columns
                true,   // supports_json_type
                true,   // supports_redundant
                true,   // supports_compact
                true,   // supports_dynamic
                true,   // supports_compressed
                0x0000, 0x45BF
            };
            break;

        case MySQLVersion::MYSQL_84:
            config = {
                MySQLVersion::MYSQL_84, "MySQL 8.4", 16384,
                true,   // supports_sdi
                true,   // supports_instant_ddl
                true,   // supports_encryption
                true,   // supports_temporary_pages
                true,   // supports_new_checksum
                true,   // supports_invisible_columns
                true,   // supports_json_type
                true,   // supports_redundant
                true,   // supports_compact
                true,   // supports_dynamic
                true,   // supports_compressed
                0x0000, 0x45BF
            };
            break;

        case MySQLVersion::MYSQL_90:
            config = {
                MySQLVersion::MYSQL_90, "MySQL 9.0", 16384,
                true,   // supports_sdi
                true,   // supports_instant_ddl
                true,   // supports_encryption
                true,   // supports_temporary_pages
                true,   // supports_new_checksum
                true,   // supports_invisible_columns
                true,   // supports_json_type
                true,   // supports_redundant
                true,   // supports_compact
                true,   // supports_dynamic
                true,   // supports_compressed
                0x0000, 0x45BF
            };
            break;

        default:
            config = {
                MySQLVersion::UNKNOWN, "Unknown", 0,
                false, false, false, false, false, false, false,
                false, false, false, false,
                0, 0
            };
            break;
    }

    return config;
}

// ==================== 版本感知恢复引擎 ====================

/**
 * @brief 版本感知恢复引擎
 *
 * 根据检测到的 MySQL 版本自动选择正确的解析策略
 */
class VersionAwareRecoveryEngine {
public:
    /**
     * @brief 构造函数
     */
    VersionAwareRecoveryEngine();

    /**
     * @brief 析构函数
     */
    ~VersionAwareRecoveryEngine();

    /**
     * @brief 设置输入文件
     */
    void set_input_file(const std::string& path);

    /**
     * @brief 设置输出目录
     */
    void set_output_dir(const std::string& dir);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 设置强制版本（跳过自动检测）
     */
    void set_force_version(MySQLVersion version);

    /**
     * @brief 设置页大小
     */
    void set_page_size(uint32_t size);

    /**
     * @brief 设置恢复模式
     */
    enum class RecoveryMode {
        TRUNCATE,   // TRUNCATE TABLE 恢复
        DROP,       // DROP TABLE 恢复
        RMRF,       // rm -rf 恢复
        NORMAL      // 正常恢复
    };
    void set_recovery_mode(RecoveryMode mode);

    /**
     * @brief 执行恢复
     * @return 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover();

    /**
     * @brief 导出为 CSV
     */
    bool export_csv(const std::string& path);

    /**
     * @brief 导出为 SQL
     */
    bool export_sql(const std::string& path);

    /**
     * @brief 获取检测到的版本
     */
    MySQLVersion get_detected_version() const { return detected_version_; }

    /**
     * @brief 获取版本配置
     */
    VersionConfig get_config() const { return get_version_config(detected_version_); }

    /**
     * @brief 获取恢复统计
     */
    struct RecoveryStats {
        uint32_t total_pages;
        uint32_t processed_pages;
        uint32_t recovered_records;
        uint32_t failed_pages;
        MySQLVersion detected_version;
        double detection_time_ms;
        double recovery_time_ms;
        std::string version_detection_method;
    };
    const RecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 从页数据检测版本（公开方法）
     */
    MySQLVersion detect_version(const uint8_t* data, size_t size);

private:
    std::string input_file_;
    std::string output_dir_;
    catalog::TableSchema schema_;
    MySQLVersion force_version_;
    MySQLVersion detected_version_;
    uint32_t page_size_;
    RecoveryMode recovery_mode_;

    std::vector<std::map<std::string, std::string>> records_;
    RecoveryStats stats_;

    // 版本检测（内部方法）
    MySQLVersion detect_version_internal(const uint8_t* data, size_t size);
    MySQLVersion detect_version_from_sdi(const uint8_t* data, size_t size);
    MySQLVersion detect_version_from_lsn(const uint8_t* data, size_t size);
    MySQLVersion detect_version_from_page_header(const uint8_t* data, size_t size);
    MySQLVersion detect_version_from_checksum(const uint8_t* data, size_t size);

    // 版本特定解析
    std::vector<std::map<std::string, std::string>> parse_mysql56(
        const uint8_t* data, size_t size);
    std::vector<std::map<std::string, std::string>> parse_mysql57(
        const uint8_t* data, size_t size);
    std::vector<std::map<std::string, std::string>> parse_mysql80(
        const uint8_t* data, size_t size);
    std::vector<std::map<std::string, std::string>> parse_mysql84(
        const uint8_t* data, size_t size);
    std::vector<std::map<std::string, std::string>> parse_mysql90(
        const uint8_t* data, size_t size);

    // 启发式解析（用于极端恢复）
    std::vector<std::map<std::string, std::string>> heuristic_parse(
        const uint8_t* data, size_t size, MySQLVersion version);

    // SDI 解析
    std::optional<catalog::TableSchema> parse_sdi(const uint8_t* data, size_t size);
};

// ==================== 版本特定解析器 ====================

/**
 * @brief MySQL 5.6 解析器
 */
class MySQL56Parser {
public:
    explicit MySQL56Parser(const uint8_t* data, size_t size);

    std::vector<std::map<std::string, std::string>> parse_compact();
    std::vector<std::map<std::string, std::string>> parse_redundant();

private:
    const uint8_t* data_;
    size_t size_;

    // COMPACT 格式解析
    std::vector<uint16_t> parse_variable_lengths(const uint8_t* record, size_t& offset);
    std::vector<uint8_t> parse_null_bitmap(const uint8_t* record, size_t n_fields);
};

/**
 * @brief MySQL 5.7 解析器
 */
class MySQL57Parser {
public:
    explicit MySQL57Parser(const uint8_t* data, size_t size);

    std::vector<std::map<std::string, std::string>> parse_dynamic();
    std::vector<std::map<std::string, std::string>> parse_compact();

    // Instant DDL 支持
    bool has_instant_columns() const { return has_instant_columns_; }
    uint16_t get_instant_column_count() const { return instant_column_count_; }

private:
    const uint8_t* data_;
    size_t size_;
    bool has_instant_columns_;
    uint16_t instant_column_count_;
};

/**
 * @brief MySQL 8.0 解析器
 */
class MySQL80Parser {
public:
    explicit MySQL80Parser(const uint8_t* data, size_t size);

    std::vector<std::map<std::string, std::string>> parse_dynamic();
    std::vector<std::map<std::string, std::string>> parse_compressed();

    // SDI 解析
    std::optional<catalog::TableSchema> parse_sdi();

    /// Structured parse with provenance metadata (schema_hash, schema_source, trust_level).
    std::optional<catalog::TableSchema> parse_sdi_structured();

    // 加密支持
    bool is_encrypted() const { return is_encrypted_; }

private:
    const uint8_t* data_;
    size_t size_;
    bool is_encrypted_;
};

/**
 * @brief MySQL 8.4 解析器
 */
class MySQL84Parser : public MySQL80Parser {
public:
    explicit MySQL84Parser(const uint8_t* data, size_t size);

    // 临时页支持
    bool is_temporary() const { return is_temporary_; }

private:
    bool is_temporary_;
};

/**
 * @brief MySQL 9.0 解析器
 */
class MySQL90Parser : public MySQL84Parser {
public:
    explicit MySQL90Parser(const uint8_t* data, size_t size);

    // 最新特性支持
    std::vector<std::map<std::string, std::string>> parse_with_metadata();
};

// ==================== 版本检测工具 ====================

/**
 * @brief 版本检测工具类
 */
class VersionDetector {
public:
    /**
     * @brief 从文件检测版本
     */
    static MySQLVersion detect_from_file(const std::string& path, uint32_t page_size = 16384);

    /**
     * @brief 从页数据检测版本
     */
    static MySQLVersion detect_from_page(const uint8_t* data, size_t size);

    /**
     * @brief 获取版本字符串
     */
    static std::string version_to_string(MySQLVersion version);

    /**
     * @brief 检查版本是否支持 SDI
     */
    static bool supports_sdi(MySQLVersion version);

    /**
     * @brief 检查版本是否支持加密
     */
    static bool supports_encryption(MySQLVersion version);

    /**
     * @brief 检查版本是否支持 Instant DDL
     */
    static bool supports_instant_ddl(MySQLVersion version);

    /**
     * @brief 获取版本的默认行格式
     */
    RowFormat get_default_row_format(MySQLVersion version);
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_VERSION_AWARE_RECOVERY_HPP
