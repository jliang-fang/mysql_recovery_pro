/**
 * @file mysql_version_detector.hpp
 * @brief MySQL版本自动检测器 - 支持MySQL 5.0-9.0
 *
 * 本文件提供MySQL不同版本的自动检测功能，
 * 通过分析页数据中的特征字段来确定MySQL版本。
 */

#pragma once

#include "version_compat.hpp"
#include <cstdint>
#include <vector>
#include <optional>
#include <array>

namespace dbrecovery {

// ==================== 版本检测器 ====================
/**
 * @brief MySQL版本自动检测器
 * @details 通过分析InnoDB页数据中的多个特征字段来确定MySQL版本
 */
class MySQLVersionDetector {
public:
    /**
     * @brief 从页数据检测MySQL版本
     * @param page_data 页数据指针
     * @param page_size 页大小
     * @return 检测到的MySQL版本，如果无法检测则返回UNKNOWN
     */
    static MySQLVersion detect(const uint8_t* page_data, size_t page_size);

    /**
     * @brief 从多个页数据中检测MySQL版本（投票机制）
     * @param pages 页数据向量
     * @return 检测到的MySQL版本
     */
    static MySQLVersion detect_from_multiple_pages(const std::vector<const uint8_t*>& pages,
                                                    size_t page_size);

    /**
     * @brief 获取版本检测的置信度
     * @param version 检测到的版本
     * @param page_data 页数据
     * @param page_size 页大小
     * @return 置信度（0-100）
     */
    static int get_confidence(MySQLVersion version, const uint8_t* page_data, size_t page_size);

    /**
     * @brief 检查页数据是否符合指定版本的特征
     * @param page_data 页数据
     * @param page_size 页大小
     * @param version 待检查的版本
     * @return 是否匹配
     */
    static bool matches_version(const uint8_t* page_data, size_t page_size, MySQLVersion version);

    // 版本检测方法（公有，供DetailedVersionDetector使用）
    static int check_lsn_extension(const uint8_t* page_data, size_t page_size);
    static int check_sdi_signature(const uint8_t* page_data, size_t page_size);
    static int check_page_version_field(const uint8_t* page_data, size_t page_size);
    static int check_checksum_type(const uint8_t* page_data, size_t page_size);
    static int check_fsp_flags(const uint8_t* page_data, size_t page_size);
    static int check_undo_log_format(const uint8_t* page_data, size_t page_size);
    static int check_page_type_range(const uint8_t* page_data, size_t page_size);

    // 版本判断逻辑
    static MySQLVersion decide_version(int lsn_score, int sdi_score, int page_version_score,
                                       int checksum_score, int fsp_score, int undo_score);

private:
    // 版本特征结构
    struct VersionFeature {
        MySQLVersion version;
        std::string name;
        uint32_t min_page_size;
        uint32_t max_page_size;
        bool has_lsn_extension;        // 8字节LSN
        bool has_sdi;                  // 支持SDI页
        bool has_encryption;           // 支持加密
        bool has_temporary_pages;      // 支持临时页
        bool has_new_checksum;         // 新校验和
        uint16_t sdi_page_type;        // SDI页类型值
    };

    // 版本特征表
    static const std::array<VersionFeature, 11> version_features_;
};

// ==================== 版本检测辅助类 ====================
/**
 * @brief 版本检测结果
 */
struct VersionDetectionResult {
    MySQLVersion version;
    int confidence;
    std::vector<std::string> matching_features;
    std::vector<std::string> non_matching_features;
};

/**
 * @brief 详细的版本检测器
 */
class DetailedVersionDetector {
public:
    /**
     * @brief 进行详细版本检测
     */
    VersionDetectionResult detect(const uint8_t* page_data, size_t page_size);

    /**
     * @brief 获取版本检测器的版本范围
     */
    static std::pair<MySQLVersion, MySQLVersion> get_supported_version_range();

    /**
     * @brief 获取所有支持的版本列表
     */
    static std::vector<MySQLVersion> get_supported_versions();

private:
    // 特征检查结果
    struct FeatureCheckResult {
        std::string feature_name;
        bool matches;
        int score;
        std::string description;
    };

    // 执行所有特征检查
    std::vector<FeatureCheckResult> check_all_features(const uint8_t* page_data, size_t page_size);

    // 版本匹配逻辑
    MySQLVersion match_version(const std::vector<FeatureCheckResult>& results);
};

// ==================== 版本检测工厂 ====================
/**
 * @brief 版本检测工厂
 */
class VersionDetectionFactory {
public:
    static VersionDetectionFactory& instance();

    /**
     * @brief 从页数据检测版本
     */
    MySQLVersion detect_version(const uint8_t* page_data, size_t page_size);

    /**
     * @brief 从页数据检测版本（详细结果）
     */
    VersionDetectionResult detect_version_detailed(const uint8_t* page_data, size_t page_size);

    /**
     * @brief 从多个页数据检测版本
     */
    MySQLVersion detect_version_from_pages(const std::vector<const uint8_t*>& pages,
                                            size_t page_size);

    /**
     * @brief 获取版本检测器
     */
    MySQLVersionDetector& get_detector();

    /**
     * @brief 获取详细检测器
     */
    DetailedVersionDetector& get_detailed_detector();

private:
    VersionDetectionFactory();
    ~VersionDetectionFactory() = default;
    VersionDetectionFactory(const VersionDetectionFactory&) = delete;
    VersionDetectionFactory& operator=(const VersionDetectionFactory&) = delete;

    MySQLVersionDetector detector_;
    DetailedVersionDetector detailed_detector_;
};

// ==================== 便捷函数 ====================
/**
 * @brief 快速版本检测（单页）
 */
inline MySQLVersion quick_detect_version(const uint8_t* page_data, size_t page_size) {
    return VersionDetectionFactory::instance().detect_version(page_data, page_size);
}

/**
 * @brief 详细版本检测
 */
inline VersionDetectionResult detailed_detect_version(const uint8_t* page_data, size_t page_size) {
    return VersionDetectionFactory::instance().detect_version_detailed(page_data, page_size);
}

/**
 * @brief 从多个页检测版本
 */
inline MySQLVersion detect_version_from_multiple_pages(const std::vector<const uint8_t*>& pages,
                                                        size_t page_size) {
    return VersionDetectionFactory::instance().detect_version_from_pages(pages, page_size);
}

} // namespace dbrecovery
