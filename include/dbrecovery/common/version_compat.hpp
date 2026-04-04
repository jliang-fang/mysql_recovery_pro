/**
 * @file version_compat.hpp
 * @brief MySQL版本兼容性层 - 支持MySQL 5.0-9.0
 * 
 * 本文件提供MySQL不同版本之间的兼容性抽象层，
 * 自动检测版本并选择合适的解析策略。
 */

#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <unordered_map>
#include <optional>
#include <vector>
#include <functional>

namespace dbrecovery {

// ==================== MySQL版本枚举 ====================
enum class MySQLVersion {
    UNKNOWN = 0,
    MYSQL_50 = 50,      // MySQL 5.0/4.x (legacy format)
    MYSQL_51 = 51,      // MySQL 5.1 (old checksum)
    MYSQL_55 = 55,      // MySQL 5.5 (old checksum)
    MYSQL_56 = 56,      // MySQL 5.6 (introduced COMPACT format)
    MYSQL_57 = 57,      // MySQL 5.7 (enhanced page header)
    MYSQL_80 = 80,      // MySQL 8.0 (LSN extension, SDI pages)
    MYSQL_81 = 81,      // MySQL 8.1
    MYSQL_82 = 82,      // MySQL 8.2
    MYSQL_83 = 83,      // MySQL 8.3
    MYSQL_84 = 84,      // MySQL 8.4 (encryption, temporary pages)
    MYSQL_90 = 90,      // MySQL 9.0 (enhanced checksums)
};

// ==================== 行格式枚举 ====================
enum class RowFormat {
    UNKNOWN = 0,
    REDUNDANT = 1,      // MySQL 5.0-5.6
    COMPACT = 2,        // MySQL 5.6+
    DYNAMIC = 3,        // MySQL 5.7+
    COMPRESSED = 4,     // MySQL 5.1+ (compressed)
    PAGE_COMPRESSED = 5,// MySQL 5.1-5.6 (deprecated)
};

// ==================== 页类型枚举 ====================
enum class PageType {
    UNKNOWN = 0,
    INDEX = 9969,       // 0x26F1 - B+Tree index page
    SYSTEM = 9984,      // 0x2718 - System page
    UNDO_LOG = 9970,    // 0x26F2 - Undo log page
    ALLOCATED = 0,      // Freshly allocated page (same as UNKNOWN)
    FSP_HDR = 0x45BF,   // File space header page
    XDES = 0x45BE,      // Extent descriptor page
    SDI = 9971,         // 0x26F3 - SDI Index page (MySQL 8.0+)
    BLOB = 9972,        // 0x26F4 - BLOB page
    COMPRESSED = 9973,  // 0x26F5 - Compressed page
    TEMPORARY = 0x26F6, // MySQL 8.4+ temporary page
};

// ==================== 校验和类型 ====================
enum class ChecksumType {
    NONE = 0,           // No checksum
    CRC32 = 1,          // CRC32 (MySQL 5.6+ default)
    INNODB = 2,         // InnoDB checksum (old)
    FSP_CHECKSUM = 3,   // FSP checksum
    NEW = 4,            // New checksum algorithm
    OLD = 5,            // Old checksum algorithm
    BLOCK_CHECKSUM = 6, // Block checksum (MySQL 8.0+)
};

// ==================== 版本信息结构 ====================
struct MySQLVersionInfo {
    MySQLVersion version;
    std::string name;
    std::string release_date;
    uint32_t min_page_size;
    uint32_t max_page_size;
    uint32_t default_page_size;
    bool supports_sdi;
    bool supports_encryption;
    bool supports_compression;
    bool supports_temporary_pages;
    std::vector<RowFormat> supported_row_formats;

    // 构造函数
    MySQLVersionInfo() = default;
    MySQLVersionInfo(MySQLVersion v);
};

// ==================== 行格式信息结构 ====================
struct RowFormatInfo {
    RowFormat format;
    std::string name;
    uint16_t record_header_size;
    uint32_t max_row_size;
    bool supports_external_storage;
    bool supports_compression;

    // 构造函数
    RowFormatInfo() = default;
    RowFormatInfo(RowFormat f);
};

// ==================== 页类型信息结构 ====================
struct PageTypeInfo {
    PageType type;
    std::string name;
    uint16_t value;
    std::string description;

    // 构造函数
    PageTypeInfo() = default;
    PageTypeInfo(PageType t);
};

// ==================== 校验和类型信息结构 ====================
struct ChecksumTypeInfo {
    ChecksumType type;
    std::string name;
    std::string algorithm;
    std::string description;

    // 构造函数
    ChecksumTypeInfo() = default;
    ChecksumTypeInfo(ChecksumType t);
};

// ==================== 版本兼容性工厂 ====================
/**
 * @brief MySQL版本兼容性工厂
 * @details 提供自动版本检测和版本特定功能的访问
 */
class VersionCompatibilityFactory {
public:
    // 单例访问
    static VersionCompatibilityFactory& instance();

    /**
     * @brief 从页数据检测MySQL版本
     */
    MySQLVersion detect_version(const uint8_t* page_data, size_t page_size) const;

    /**
     * @brief 从页数据检测行格式
     */
    RowFormat detect_row_format(const uint8_t* page_data, 
                                MySQLVersion version,
                                size_t page_size) const;

    /**
     * @brief 获取版本信息
     */
    const MySQLVersionInfo& get_version_info(MySQLVersion version) const;

    /**
     * @brief 检查版本是否支持特定功能
     */
    bool supports_feature(MySQLVersion version, const std::string& feature) const;

    /**
     * @brief 获取版本字符串
     */
    std::string version_to_string(MySQLVersion version) const;

    /**
     * @brief 获取版本范围（支持的最小和最大版本）
     */
    std::pair<MySQLVersion, MySQLVersion> get_supported_version_range() const;

    /**
     * @brief 检查版本是否在支持范围内
     */
    bool is_version_supported(MySQLVersion version) const;

    /**
     * @brief 获取默认页大小
     */
    uint32_t get_default_page_size(MySQLVersion version) const;

    /**
     * @brief 获取页大小列表（支持的页大小）
     */
    std::vector<uint32_t> get_supported_page_sizes(MySQLVersion version) const;

    /**
     * @brief 获取支持的校验和类型
     */
    std::vector<ChecksumType> get_supported_checksum_types() const;

    /**
     * @brief 获取支持的页类型
     */
    std::vector<PageType> get_supported_page_types() const;

    /**
     * @brief 获取支持的行格式
     */
    std::vector<RowFormat> get_supported_row_formats() const;

    /**
     * @brief 获取支持的版本
     */
    std::vector<MySQLVersion> get_supported_versions() const;

    /**
     * @brief 获取行格式信息
     */
    const RowFormatInfo& get_row_format_info(RowFormat format) const;

    /**
     * @brief 获取页类型信息
     */
    const PageTypeInfo& get_page_type_info(PageType type) const;

    /**
     * @brief 获取校验和类型信息
     */
    const ChecksumTypeInfo& get_checksum_type_info(ChecksumType type) const;

    /**
     * @brief 析构函数
     */
    ~VersionCompatibilityFactory() = default;

    /**
     * @brief 默认构造函数
     */
    VersionCompatibilityFactory() = default;

    // 初始化版本信息
    void init_version_info();

    // 版本信息映射
    std::unordered_map<MySQLVersion, MySQLVersionInfo> version_info_;

    // 行格式信息映射
    std::unordered_map<RowFormat, RowFormatInfo> row_format_info_;

    // 页类型信息映射
    std::unordered_map<PageType, PageTypeInfo> page_type_info_;

    // 校验和类型信息映射
    std::unordered_map<ChecksumType, ChecksumTypeInfo> checksum_type_info_;

    // 默认页大小映射
    std::unordered_map<MySQLVersion, uint32_t> default_page_sizes_;

    // 支持的页大小列表
    std::unordered_map<MySQLVersion, std::vector<uint32_t>> supported_page_sizes_;

    // 支持的校验和类型列表
    std::vector<ChecksumType> supported_checksum_types_;

    // 支持的页类型列表
    std::vector<PageType> supported_page_types_;

    // 支持的行格式列表
    std::vector<RowFormat> supported_row_formats_;

    // 支持的版本列表
    std::vector<MySQLVersion> supported_versions_;
};

// ==================== 版本检测辅助函数 ====================
/**
 * @brief 从页数据检测MySQL版本（便捷函数）
 */
inline MySQLVersion detect_mysql_version(const uint8_t* page_data, size_t page_size) {
    return VersionCompatibilityFactory::instance().detect_version(page_data, page_size);
}

/**
 * @brief 获取版本字符串（便捷函数）
 */
inline std::string get_mysql_version_string(MySQLVersion version) {
    return VersionCompatibilityFactory::instance().version_to_string(version);
}

/**
 * @brief 检查版本是否支持SDI
 */
inline bool version_supports_sdi(MySQLVersion version) {
    return VersionCompatibilityFactory::instance().supports_feature(version, "SDI");
}

/**
 * @brief 检查版本是否支持加密
 */
inline bool version_supports_encryption(MySQLVersion version) {
    return VersionCompatibilityFactory::instance().supports_feature(version, "encryption");
}

/**
 * @brief 检查版本是否支持压缩
 */
inline bool version_supports_compression(MySQLVersion version) {
    return VersionCompatibilityFactory::instance().supports_feature(version, "compression");
}

// ==================== 页大小辅助函数 ====================
/**
 * @brief 获取页大小（从页数据）
 */
uint32_t get_page_size_from_data(const uint8_t* page_data, size_t page_size);

/**
 * @brief 获取页大小（从页类型）
 */
uint32_t get_page_size_from_type(PageType page_type, MySQLVersion version);

/**
 * @brief 标准化页大小
 */
uint32_t normalize_page_size(uint32_t page_size, MySQLVersion version);

// ==================== 行格式辅助函数 ====================
/**
 * @brief 获取行格式字符串
 */
std::string row_format_to_string(RowFormat format);

/**
 * @brief 从字符串解析行格式
 */
std::optional<RowFormat> string_to_row_format(const std::string& str);

/**
 * @brief 获取行格式的默认页大小
 */
uint32_t get_default_page_size_for_row_format(RowFormat format);

// ==================== 页类型辅助函数 ====================
/**
 * @brief 获取页类型字符串
 */
std::string page_type_to_string(PageType page_type);

/**
 * @brief 从十六进制值解析页类型
 */
std::optional<PageType> string_to_page_type(uint16_t page_type_value);

// ==================== 校验和辅助函数 ====================
/**
 * @brief 获取版本的默认校验和类型
 */
ChecksumType get_default_checksum_type(MySQLVersion version);

/**
 * @brief 获取校验和类型字符串
 */
std::string checksum_type_to_string(ChecksumType type);

/**
 * @brief 从字符串解析校验和类型
 */
std::optional<ChecksumType> string_to_checksum_type(const std::string& str);

} // namespace dbrecovery
