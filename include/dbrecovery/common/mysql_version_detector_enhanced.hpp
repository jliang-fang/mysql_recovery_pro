/**
 * @file mysql_version_detector_enhanced.hpp
 * @brief MySQL版本自动检测器增强版 - 支持MySQL 5.0-10.0
 *
 * 本文件提供MySQL不同版本的高级自动检测功能，
 * 包含更详细的版本特征分析和更准确的版本判断。
 */

#pragma once

#include "mysql_version_detector.hpp"
#include <map>
#include <set>
#include <tuple>

namespace dbrecovery {

// ==================== 增强的版本特征 ====================
/**
 * @brief 增强的版本特征结构
 */
struct EnhancedVersionFeature {
    MySQLVersion version;
    std::string name;
    std::string codename;           // 代码名称
    std::string release_date;       // 发布日期
    uint32_t min_page_size;
    uint32_t max_page_size;
    uint32_t default_page_size;
    
    // 物理特性
    bool has_lsn_extension;         // 8字节LSN
    bool has_sdi;                   // 支持SDI页
    bool supports_encryption;       // 支持TDE
    bool supports_compression;      // 支持压缩
    bool supports_temporary_pages;  // 支持临时页
    bool supports_instant_ddl;      // 支持Instant DDL
    bool supports_online_ddl;       // 支持Online DDL
    
    // 校验和
    ChecksumType default_checksum;
    std::set<ChecksumType> supported_checksums;
    
    // 页类型
    std::set<PageType> supported_page_types;
    
    // 行格式
    RowFormat default_row_format;
    std::set<RowFormat> supported_row_formats;
    
    // 页大小支持
    std::set<uint32_t> supported_page_sizes;
    
    // 特殊特性
    uint16_t sdi_page_type;
    uint16_t temporary_page_type;
    
    // 版本依赖
    MySQLVersion successor;         // 后续版本
    MySQLVersion predecessor;       // 前序版本
};

// ==================== 增强的版本检测器 ====================
/**
 * @brief 增强的MySQL版本检测器
 * @details 提供更详细的版本信息和更准确的检测
 */
class EnhancedMySQLVersionDetector {
public:
    EnhancedMySQLVersionDetector();
    ~EnhancedMySQLVersionDetector() = default;

    /**
     * @brief 从页数据检测MySQL版本
     */
    MySQLVersion detect(const uint8_t* page_data, size_t page_size);

    /**
     * @brief 从页数据检测版本并返回详细信息
     */
    VersionDetectionResult detect_detailed(const uint8_t* page_data, size_t page_size);

    /**
     * @brief 从多个页数据检测版本（投票机制）
     */
    MySQLVersion detect_from_multiple_pages(const std::vector<const uint8_t*>& pages,
                                             size_t page_size);

    /**
     * @brief 获取版本信息
     */
    const EnhancedVersionFeature& get_version_info(MySQLVersion version) const;

    /**
     * @brief 获取所有支持的版本
     */
    std::vector<MySQLVersion> get_all_supported_versions() const;

    /**
     * @brief 获取版本范围
     */
    std::pair<MySQLVersion, MySQLVersion> get_version_range() const;

    /**
     * @brief 检查版本是否支持特定功能
     */
    bool version_supports_feature(MySQLVersion version, const std::string& feature) const;

    /**
     * @brief 获取版本的默认页大小
     */
    uint32_t get_default_page_size(MySQLVersion version) const;

    /**
     * @brief 获取版本支持的页大小列表
     */
    std::vector<uint32_t> get_supported_page_sizes(MySQLVersion version) const;

    /**
     * @brief 获取版本的默认校验和类型
     */
    ChecksumType get_default_checksum(MySQLVersion version) const;

    /**
     * @brief 获取版本支持的校验和类型列表
     */
    std::vector<ChecksumType> get_supported_checksums(MySQLVersion version) const;

    /**
     * @brief 获取版本的默认行格式
     */
    RowFormat get_default_row_format(MySQLVersion version) const;

    /**
     * @brief 获取版本支持的行格式列表
     */
    std::vector<RowFormat> get_supported_row_formats(MySQLVersion version) const;

    /**
     * @brief 获取版本的代码名称
     */
    std::string get_version_codename(MySQLVersion version) const;

    /**
     * @brief 获取版本的发布日期
     */
    std::string get_version_release_date(MySQLVersion version) const;

    /**
     * @brief 从版本字符串解析版本
     */
    std::optional<MySQLVersion> parse_version_string(const std::string& version_str) const;

    /**
     * @brief 从版本号获取版本字符串
     */
    std::string version_to_string(MySQLVersion version) const;

    /**
     * @brief 比较两个版本
     * @return -1 if v1 < v2, 0 if v1 == v2, 1 if v1 > v2
     */
    int compare_versions(MySQLVersion v1, MySQLVersion v2) const;

    /**
     * @brief 获取版本系列
     */
    std::string get_version_series(MySQLVersion version) const;

    /**
     * @brief 检查版本是否在系列中
     */
    bool is_version_in_series(MySQLVersion version, const std::string& series) const;

private:
    // 版本特征表
    std::map<MySQLVersion, EnhancedVersionFeature> version_features_;

    // 初始化版本特征
    void init_version_features();

    // 特征检查
    int check_lsn_extension(const uint8_t* page_data, size_t page_size);
    int check_sdi_signature(const uint8_t* page_data, size_t page_size);
    int check_page_version_field(const uint8_t* page_data, size_t page_size);
    int check_checksum_type(const uint8_t* page_data, size_t page_size);
    int check_fsp_flags(const uint8_t* page_data, size_t page_size);
    int check_undo_log_format(const uint8_t* page_data, size_t page_size);
    int check_page_type_range(const uint8_t* page_data, size_t page_size);
    int check_row_format(const uint8_t* page_data, size_t page_size);
    int check_temporary_page(const uint8_t* page_data, size_t page_size);
    int check_encryption_support(const uint8_t* page_data, size_t page_size);

    // 版本判断
    MySQLVersion decide_version(const std::map<MySQLVersion, int>& scores);
    std::map<MySQLVersion, int> calculate_scores(const uint8_t* page_data, size_t page_size);
};

// ==================== 版本系列 ====================
/**
 * @brief 版本系列信息
 */
struct VersionSeries {
    std::string name;
    std::vector<MySQLVersion> versions;
    std::string description;
};

/**
 * @brief 版本系列管理器
 */
class VersionSeriesManager {
public:
    static VersionSeriesManager& instance();

    /**
     * @brief 获取版本系列
     */
    std::string get_series(MySQLVersion version) const;

    /**
     * @brief 获取系列的所有版本
     */
    std::vector<MySQLVersion> get_series_versions(const std::string& series) const;

    /**
     * @brief 获取所有系列
     */
    std::vector<VersionSeries> get_all_series() const;

    /**
     * @brief 检查版本是否在系列中
     */
    bool is_in_series(MySQLVersion version, const std::string& series) const;

private:
    VersionSeriesManager();
    ~VersionSeriesManager() = default;

    std::map<std::string, std::vector<MySQLVersion>> series_map_;
    std::map<MySQLVersion, std::string> version_series_map_;
};

// ==================== 版本兼容性矩阵 ====================
/**
 * @brief 版本兼容性矩阵
 */
class VersionCompatibilityMatrix {
public:
    static VersionCompatibilityMatrix& instance();

    /**
     * @brief 检查两个版本是否兼容
     */
    bool are_compatible(MySQLVersion v1, MySQLVersion v2) const;

    /**
     * @brief 获取版本的兼容版本列表
     */
    std::vector<MySQLVersion> get_compatible_versions(MySQLVersion version) const;

    /**
     * @brief 检查版本是否可以升级到目标版本
     */
    bool can_upgrade_to(MySQLVersion from, MySQLVersion to) const;

    /**
     * @brief 检查版本是否可以降级到目标版本
     */
    bool can_downgrade_to(MySQLVersion from, MySQLVersion to) const;

    /**
     * @brief 获取升级路径
     */
    std::vector<MySQLVersion> get_upgrade_path(MySQLVersion from, MySQLVersion to) const;

    /**
     * @brief 获取降级路径
     */
    std::vector<MySQLVersion> get_downgrade_path(MySQLVersion from, MySQLVersion to) const;

private:
    VersionCompatibilityMatrix();
    ~VersionCompatibilityMatrix() = default;

    // 兼容性矩阵
    std::map<std::pair<MySQLVersion, MySQLVersion>, bool> compatibility_matrix_;
};

// ==================== 版本检测工厂增强 ====================
/**
 * @brief 增强的版本检测工厂
 */
class EnhancedVersionDetectionFactory {
public:
    static EnhancedVersionDetectionFactory& instance();

    /**
     * @brief 从页数据检测版本
     */
    MySQLVersion detect_version(const uint8_t* page_data, size_t page_size);

    /**
     * @brief 从页数据检测版本（详细结果）
     */
    VersionDetectionResult detect_version_detailed(const uint8_t* page_data, size_t page_size);

    /**
     * @brief 从多个页检测版本
     */
    MySQLVersion detect_version_from_pages(const std::vector<const uint8_t*>& pages,
                                            size_t page_size);

    /**
     * @brief 获取版本信息
     */
    const EnhancedVersionFeature& get_version_info(MySQLVersion version);

    /**
     * @brief 获取版本系列管理器
     */
    VersionSeriesManager& get_series_manager();

    /**
     * @brief 获取版本兼容性矩阵
     */
    VersionCompatibilityMatrix& get_compatibility_matrix();

    /**
     * @brief 获取所有支持的版本
     */
    std::vector<MySQLVersion> get_all_supported_versions();

    /**
     * @brief 获取版本范围
     */
    std::pair<MySQLVersion, MySQLVersion> get_version_range();

private:
    EnhancedVersionDetectionFactory();
    ~EnhancedVersionDetectionFactory() = default;

    EnhancedMySQLVersionDetector detector_;
    VersionSeriesManager series_manager_;
    VersionCompatibilityMatrix compatibility_matrix_;
};

// ==================== 便捷函数 ====================
/**
 * @brief 快速版本检测（单页）
 */
inline MySQLVersion quick_detect_version_enhanced(const uint8_t* page_data, size_t page_size) {
    return EnhancedVersionDetectionFactory::instance().detect_version(page_data, page_size);
}

/**
 * @brief 详细版本检测
 */
inline VersionDetectionResult detailed_detect_version_enhanced(const uint8_t* page_data, size_t page_size) {
    return EnhancedVersionDetectionFactory::instance().detect_version_detailed(page_data, page_size);
}

/**
 * @brief 从多个页检测版本
 */
inline MySQLVersion detect_version_from_multiple_pages_enhanced(const std::vector<const uint8_t*>& pages,
                                                                 size_t page_size) {
    return EnhancedVersionDetectionFactory::instance().detect_version_from_pages(pages, page_size);
}

/**
 * @brief 获取版本信息
 */
inline const EnhancedVersionFeature& get_version_info_enhanced(MySQLVersion version) {
    return EnhancedVersionDetectionFactory::instance().get_version_info(version);
}

/**
 * @brief 获取版本系列
 */
inline std::string get_version_series_enhanced(MySQLVersion version) {
    return EnhancedVersionDetectionFactory::instance().get_series_manager().get_series(version);
}

/**
 * @brief 检查版本是否支持特定功能
 */
inline bool version_supports_feature_enhanced(MySQLVersion version, const std::string& feature) {
    return EnhancedVersionDetectionFactory::instance().get_version_info(version).supports_instant_ddl;
}

} // namespace dbrecovery
