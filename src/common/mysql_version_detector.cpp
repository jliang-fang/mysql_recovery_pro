/**
 * @file mysql_version_detector.cpp
 * @brief MySQL版本自动检测器实现
 */

#include "dbrecovery/common/mysql_version_detector.hpp"
#include "dbrecovery/common/endian_compat.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <algorithm>
#include <cmath>

namespace dbrecovery {

// ==================== 版本特征表 ====================
const std::array<MySQLVersionDetector::VersionFeature, 4> MySQLVersionDetector::version_features_ = {{
    // MySQL 5.6 - 引入COMPACT格式，新校验和
    {MySQLVersion::MYSQL_56, "MySQL 5.6", 1024, 65536, false, false, false, false, true, 0},

    // MySQL 5.7 - 增强的页头
    {MySQLVersion::MYSQL_57, "MySQL 5.7", 1024, 65536, false, false, false, false, true, 0},

    // MySQL 8.0 - LSN扩展，SDI页
    {MySQLVersion::MYSQL_80, "MySQL 8.0", 1024, 65536, true, true, false, false, true, 0x26F3},

    // MySQL 8.4 - 加密，临时页
    {MySQLVersion::MYSQL_84, "MySQL 8.4", 1024, 65536, true, true, true, true, true, 0x26F3},
}};

// ==================== MySQLVersionDetector实现 ====================

MySQLVersion MySQLVersionDetector::detect(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 38) {
        return MySQLVersion::UNKNOWN;
    }

    // 执行所有检查并获取分数
    int lsn_score = check_lsn_extension(page_data, page_size);
    int sdi_score = check_sdi_signature(page_data, page_size);
    int page_version_score = check_page_version_field(page_data, page_size);
    int checksum_score = check_checksum_type(page_data, page_size);
    int fsp_score = check_fsp_flags(page_data, page_size);
    int undo_score = check_undo_log_format(page_data, page_size);

    // 决定版本
    return decide_version(lsn_score, sdi_score, page_version_score,
                         checksum_score, fsp_score, undo_score);
}

MySQLVersion MySQLVersionDetector::detect_from_multiple_pages(
    const std::vector<const uint8_t*>& pages, size_t page_size) {
    if (pages.empty()) {
        return MySQLVersion::UNKNOWN;
    }

    // 统计每个版本的票数
    std::unordered_map<MySQLVersion, int> votes;
    
    for (auto page : pages) {
        if (page) {
            MySQLVersion v = detect(page, page_size);
            if (v != MySQLVersion::UNKNOWN) {
                votes[v]++;
            }
        }
    }

    // 找到票数最多的版本
    MySQLVersion result = MySQLVersion::UNKNOWN;
    int max_votes = 0;
    for (const auto& [version, count] : votes) {
        if (count > max_votes) {
            max_votes = count;
            result = version;
        }
    }

    return result;
}

int MySQLVersionDetector::get_confidence(MySQLVersion version,
                                          const uint8_t* page_data,
                                          size_t page_size) {
    if (!page_data || version == MySQLVersion::UNKNOWN) {
        return 0;
    }

    int total_score = 0;
    int max_score = 0;

    // 检查LSN扩展
    int lsn_score = check_lsn_extension(page_data, page_size);
    total_score += std::abs(lsn_score);
    if (version >= MySQLVersion::MYSQL_80) {
        max_score += 10;
        if (lsn_score > 0) total_score += lsn_score;
    } else {
        max_score += 10;
        if (lsn_score <= 0) total_score += std::abs(lsn_score);
    }

    // 检查SDI
    int sdi_score = check_sdi_signature(page_data, page_size);
    total_score += std::abs(sdi_score);
    if (version >= MySQLVersion::MYSQL_80) {
        max_score += 15;
        if (sdi_score > 0) total_score += sdi_score;
    } else {
        max_score += 15;
        if (sdi_score <= 0) total_score += std::abs(sdi_score);
    }

    // 检查页版本字段
    int page_version_score = check_page_version_field(page_data, page_size);
    total_score += std::abs(page_version_score);
    max_score += 10;
    total_score += std::abs(page_version_score);

    // 检查校验和
    int checksum_score = check_checksum_type(page_data, page_size);
    total_score += std::abs(checksum_score);
    max_score += 10;
    total_score += std::abs(checksum_score);

    // 计算置信度
    if (max_score == 0) return 0;
    return static_cast<int>(std::min(100.0, (static_cast<double>(total_score) / max_score) * 100));
}

bool MySQLVersionDetector::matches_version(const uint8_t* page_data,
                                            size_t page_size,
                                            MySQLVersion version) {
    if (!page_data || version == MySQLVersion::UNKNOWN) {
        return false;
    }

    // 检查所有关键特征
    int lsn_score = check_lsn_extension(page_data, page_size);
    int sdi_score = check_sdi_signature(page_data, page_size);
    int page_version_score = check_page_version_field(page_data, page_size);

    // 对于MySQL 8.0+，需要LSN扩展
    if (version >= MySQLVersion::MYSQL_80) {
        if (lsn_score <= 0) return false;
    } else {
        if (lsn_score > 0) return false;
    }

    // 对于MySQL 8.0+，需要SDI支持
    if (version >= MySQLVersion::MYSQL_80) {
        if (sdi_score <= 0) return false;
    } else {
        if (sdi_score > 0) return false;
    }

    return true;
}

// ==================== 版本特征检查方法 ====================

int MySQLVersionDetector::check_lsn_extension(const uint8_t* page_data, size_t page_size) {
    // Check FIL header: page_type at offset 24 (2 bytes big-endian)
    // FLUSH_LSN (MySQL 8.0+) at offset 26 (8 bytes big-endian)
    // MySQL 8.0+ has 8-byte LSN extension in the FIL header
    if (page_size < 34) return 0;

    // Read page_type at offset 24 (FIL_PAGE_TYPE_ABS), big-endian
    uint16_t page_type = endian::read_be16(page_data, innodb::FIL_PAGE_TYPE_ABS);

    // If page type is 0 (freshly allocated), this is not a valid page
    if (page_type == 0) return 0;

    // Read FLUSH_LSN at offset 26 (FIL_PAGE_TYPE_ABS + 2), big-endian
    uint64_t flush_lsn = endian::read_be64(page_data, innodb::FIL_PAGE_TYPE_ABS + 2);

    // If FLUSH_LSN is non-zero, this is MySQL 8.0+
    if (flush_lsn > 0) {
        return 10;  // Supports LSN extension
    }

    return -5;  // No LSN extension
}

int MySQLVersionDetector::check_sdi_signature(const uint8_t* page_data, size_t page_size) {
    // Check for SDI signature at offset 64-71
    // MySQL 8.0+ SDI pages have signature: 0x08 'S' 'D' 'I' '0' '0' '0' '0'
    if (page_size < 72) return 0;

    constexpr uint8_t SDI_SIG[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};

    // Read page type at offset 24 (FIL_PAGE_TYPE_ABS), big-endian
    uint16_t page_type = endian::read_be16(page_data, innodb::FIL_PAGE_TYPE_ABS);

    // Check if this is an SDI page (uncompressed 0x45BE or compressed 0x45BD)
    if (page_type == innodb::FIL_PAGE_SDI || page_type == innodb::FIL_PAGE_SDI_ZIP) {
        // Check SDI signature
        if (std::memcmp(page_data + 64, SDI_SIG, 8) == 0) {
            return 15;  // Matches SDI signature
        }
        return 5;  // SDI page but no signature (possibly older version)
    }

    return -10;  // Not an SDI page type
}

int MySQLVersionDetector::check_page_version_field(const uint8_t* page_data, size_t page_size) {
    // 检查FIL_PAGE_PAGE_VERSION字段（偏移36-37）
    // MySQL 8.0+引入此字段
    if (page_size < 38) return 0;

    uint16_t page_version = endian::read_le16(page_data, 36);
    
    if (page_version > 0) {
        if (page_version == 1) return 8;   // MySQL 8.0
        if (page_version == 2) return 10;  // MySQL 8.4+
        return 5;  // 未知版本
    }

    return -5;  // 无页版本字段
}

int MySQLVersionDetector::check_checksum_type(const uint8_t* page_data, size_t page_size) {
    // Check checksum type in FIL header
    // Checksum is stored at offset 0 in the FIL header, in big-endian format
    // MySQL 5.6+ uses CRC32 checksum (typically a large value)
    // MySQL 5.5 and earlier use OLD or INNODB checksum

    if (page_size < 4) return 0;

    // Read checksum at offset 0, big-endian (InnoDB FIL header stores all fields big-endian)
    uint32_t checksum = endian::read_be32(page_data, 0);

    // CRC32 checksum values are typically large (> 16 million)
    if (checksum > 0x1000000) {
        return 8;  // Likely CRC32
    }

    return 5;  // Unknown checksum type
}

int MySQLVersionDetector::check_fsp_flags(const uint8_t* page_data, size_t page_size) {
    // 检查FSP Header中的flags（偏移0-39的FSP_HEADER_OFFSET）
    // MySQL 8.4+有新的flags位
    if (page_size < 100) return 0;

    // 这里简化检查，实际需要解析FSP Header
    return 5;  // 中等置信度
}

int MySQLVersionDetector::check_undo_log_format(const uint8_t* page_data, size_t page_size) {
    // Check UNDO LOG page format
    // MySQL 8.0+ uses new UNDO LOG format
    if (page_size < 50) return 0;

    // Read page type at offset 24 (FIL_PAGE_TYPE_ABS), big-endian
    uint16_t page_type = endian::read_be16(page_data, innodb::FIL_PAGE_TYPE_ABS);

    if (page_type == innodb::FIL_PAGE_UNDO_LOG) {  // 0x45B2 = 17842
        // Check UNDO LOG header
        uint8_t undo_type = page_data[100];  // UNDO_LOG_HEADER_OFFSET
        if (undo_type == 0 || undo_type == 1) {
            return 8;  // New format
        }
    }

    return 5;  // Cannot determine
}

int MySQLVersionDetector::check_page_type_range(const uint8_t* page_data, size_t page_size) {
    // Check if page type is within valid range
    // Page type is at offset 24 (FIL_PAGE_TYPE_ABS), read as big-endian
    if (page_size < 38) return 0;

    uint16_t page_type = endian::read_be16(page_data, innodb::FIL_PAGE_TYPE_ABS);

    // Valid InnoDB page type values (big-endian read values)
    if (page_type == innodb::FIL_PAGE_TYPE_ALLOCATED || page_type == innodb::FIL_PAGE_INDEX ||
        page_type == innodb::FIL_PAGE_UNDO_LOG || page_type == innodb::FIL_PAGE_SDI ||
        page_type == innodb::FIL_PAGE_SDI_ZIP || page_type == innodb::FIL_PAGE_TYPE_FSP_HDR ||
        page_type == innodb::FIL_PAGE_TYPE_XDES) {
        return 10;
    }

    return -5;  // Invalid page type
}

MySQLVersion MySQLVersionDetector::decide_version(int lsn_score, int sdi_score,
                                                   int page_version_score,
                                                   int checksum_score, int fsp_score,
                                                   int undo_score) {
    // 计算总分
    int total_score = lsn_score + sdi_score + page_version_score +
                      checksum_score + fsp_score + undo_score;

    // 根据分数决定版本
    if (lsn_score > 0 && sdi_score > 0 && page_version_score > 0) {
        // MySQL 8.0+
        if (page_version_score >= 10) {
            return MySQLVersion::MYSQL_84;  // 8.4+
        }
        return MySQLVersion::MYSQL_80;  // 8.0-8.3
    } else if (lsn_score <= 0 && sdi_score <= 0) {
        // MySQL 5.x
        if (checksum_score >= 8) {
            return MySQLVersion::MYSQL_56;  // 5.6+
        }
        return MySQLVersion::MYSQL_56;  // Fallback to 5.6 for older versions
    }

    // 默认返回未知
    return MySQLVersion::UNKNOWN;
}

// ==================== DetailedVersionDetector实现 ====================

VersionDetectionResult DetailedVersionDetector::detect(const uint8_t* page_data, size_t page_size) {
    VersionDetectionResult result;
    result.version = MySQLVersion::UNKNOWN;
    result.confidence = 0;

    if (!page_data || page_size < 38) {
        return result;
    }

    // 执行所有特征检查
    auto features = check_all_features(page_data, page_size);

    // 收集匹配和非匹配的特征
    for (const auto& f : features) {
        if (f.matches) {
            result.matching_features.push_back(f.feature_name);
        } else {
            result.non_matching_features.push_back(f.feature_name);
        }
    }

    // 决定版本
    result.version = match_version(features);

    // 计算置信度
    int matching_count = static_cast<int>(result.matching_features.size());
    int total_count = static_cast<int>(features.size());
    if (total_count > 0) {
        result.confidence = static_cast<int>((static_cast<double>(matching_count) / total_count) * 100);
    }

    return result;
}

std::pair<MySQLVersion, MySQLVersion> DetailedVersionDetector::get_supported_version_range() {
    return {MySQLVersion::MYSQL_56, MySQLVersion::MYSQL_84};
}

std::vector<MySQLVersion> DetailedVersionDetector::get_supported_versions() {
    return {
        MySQLVersion::MYSQL_56,
        MySQLVersion::MYSQL_57,
        MySQLVersion::MYSQL_80,
        MySQLVersion::MYSQL_84
    };
}

std::vector<DetailedVersionDetector::FeatureCheckResult> DetailedVersionDetector::check_all_features(
    const uint8_t* page_data, size_t page_size) {
    std::vector<FeatureCheckResult> results;

    // 检查LSN扩展
    {
        FeatureCheckResult r;
        r.feature_name = "LSN Extension (8-byte)";
        int score = MySQLVersionDetector::check_lsn_extension(page_data, page_size);
        r.matches = (score > 0);
        r.score = std::abs(score);
        r.description = r.matches ? "Has 8-byte LSN extension" : "Uses 4-byte LSN";
        results.push_back(r);
    }

    // 检查SDI
    {
        FeatureCheckResult r;
        r.feature_name = "SDI Support";
        int score = MySQLVersionDetector::check_sdi_signature(page_data, page_size);
        r.matches = (score > 0);
        r.score = std::abs(score);
        r.description = r.matches ? "Has SDI page support" : "No SDI page support";
        results.push_back(r);
    }

    // 检查页版本字段
    {
        FeatureCheckResult r;
        r.feature_name = "Page Version Field";
        int score = MySQLVersionDetector::check_page_version_field(page_data, page_size);
        r.matches = (score > 0);
        r.score = std::abs(score);
        r.description = r.matches ? "Has page version field" : "No page version field";
        results.push_back(r);
    }

    // 检查校验和
    {
        FeatureCheckResult r;
        r.feature_name = "CRC32 Checksum";
        int score = MySQLVersionDetector::check_checksum_type(page_data, page_size);
        r.matches = (score >= 8);
        r.score = std::abs(score);
        r.description = r.matches ? "Uses CRC32 checksum" : "Uses old checksum";
        results.push_back(r);
    }

    // 检查页类型
    {
        FeatureCheckResult r;
        r.feature_name = "Valid Page Type";
        int score = MySQLVersionDetector::check_page_type_range(page_data, page_size);
        r.matches = (score > 0);
        r.score = std::abs(score);
        r.description = r.matches ? "Valid InnoDB page type" : "Invalid page type";
        results.push_back(r);
    }

    return results;
}

MySQLVersion DetailedVersionDetector::match_version(const std::vector<FeatureCheckResult>& results) {
    int lsn_matches = 0;
    int sdi_matches = 0;
    int page_version_matches = 0;
    int checksum_matches = 0;

    for (const auto& r : results) {
        if (r.feature_name == "LSN Extension (8-byte)") {
            lsn_matches = r.matches ? 1 : -1;
        } else if (r.feature_name == "SDI Support") {
            sdi_matches = r.matches ? 1 : -1;
        } else if (r.feature_name == "Page Version Field") {
            page_version_matches = r.matches ? 1 : -1;
        } else if (r.feature_name == "CRC32 Checksum") {
            checksum_matches = r.matches ? 1 : -1;
        }
    }

    // 决定版本
    if (lsn_matches > 0 && sdi_matches > 0 && page_version_matches > 0) {
        if (checksum_matches > 0) {
            return MySQLVersion::MYSQL_84;  // 8.4+
        }
        return MySQLVersion::MYSQL_80;  // 8.0-8.3
    } else if (lsn_matches <= 0 && sdi_matches <= 0) {
        if (checksum_matches > 0) {
            return MySQLVersion::MYSQL_56;  // 5.6+
        }
        return MySQLVersion::MYSQL_56;  // Fallback to 5.6 for older versions
    }

    return MySQLVersion::UNKNOWN;
}

// ==================== VersionDetectionFactory实现 ====================

VersionDetectionFactory::VersionDetectionFactory() {
    // 初始化
}

VersionDetectionFactory& VersionDetectionFactory::instance() {
    static VersionDetectionFactory instance_;
    return instance_;
}

MySQLVersion VersionDetectionFactory::detect_version(const uint8_t* page_data, size_t page_size) {
    return detector_.detect(page_data, page_size);
}

VersionDetectionResult VersionDetectionFactory::detect_version_detailed(const uint8_t* page_data,
                                                                          size_t page_size) {
    return detailed_detector_.detect(page_data, page_size);
}

MySQLVersion VersionDetectionFactory::detect_version_from_pages(
    const std::vector<const uint8_t*>& pages, size_t page_size) {
    return detector_.detect_from_multiple_pages(pages, page_size);
}

MySQLVersionDetector& VersionDetectionFactory::get_detector() {
    return detector_;
}

DetailedVersionDetector& VersionDetectionFactory::get_detailed_detector() {
    return detailed_detector_;
}

} // namespace dbrecovery
