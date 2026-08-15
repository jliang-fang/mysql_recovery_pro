/**
 * @file mysql_version_detector.cpp
 * @brief MySQL版本自动检测器实现
 */

#include "dbrecovery/common/mysql_version_detector.hpp"
#include "dbrecovery/common/endian_compat.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace dbrecovery {

// ==================== 版本特征表 ====================
const std::array<MySQLVersionDetector::VersionFeature, 6> MySQLVersionDetector::version_features_ = {{
    // MySQL 5.6 - 引入COMPACT格式，新校验和
    {MySQLVersion::MYSQL_56, "MySQL 5.6", 1024, 65536, false, false, false, false, true, 0},

    // MySQL 5.7 - 增强的页头，POST_ANTELOPE标志
    {MySQLVersion::MYSQL_57, "MySQL 5.7", 1024, 65536, false, false, false, false, true, 0},

    // MySQL 8.0 - LSN扩展，SDI页
    {MySQLVersion::MYSQL_80, "MySQL 8.0", 1024, 65536, true, true, false, false, true, 0x26F3},

    // MySQL 8.4 - 加密，临时页
    {MySQLVersion::MYSQL_84, "MySQL 8.4", 1024, 65536, true, true, true, true, true, 0x26F3},

    // MySQL 9.0 - 增强校验和
    {MySQLVersion::MYSQL_90, "MySQL 9.0", 1024, 65536, true, true, true, true, true, 0x26F3},
}};

// ==================== MySQLVersionDetector实现 ====================

MySQLVersion MySQLVersionDetector::detect(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 38) {
        return MySQLVersion::UNKNOWN;
    }

    if (check_page_type_range(page_data, page_size) <= 0) {
        return MySQLVersion::UNKNOWN;
    }

    const int lsn_score = check_lsn_extension(page_data, page_size);
    const int sdi_score = check_sdi_signature(page_data, page_size);
    const int checksum_score = check_checksum_type(page_data, page_size);
    const int fsp_score = check_fsp_flags(page_data, page_size);
    const int undo_score = check_undo_log_format(page_data, page_size);

    return decide_version(lsn_score, sdi_score, 0,
                          checksum_score, fsp_score, undo_score);
}

MySQLVersion MySQLVersionDetector::detect_from_multiple_pages(
    const std::vector<const uint8_t*>& pages, size_t page_size) {
    if (pages.empty()) {
        return MySQLVersion::UNKNOWN;
    }

    // 统计每个版本的票数（使用有序map保证确定性）
    std::map<MySQLVersion, int> votes;

    for (auto page : pages) {
        if (page) {
            MySQLVersion v = detect(page, page_size);
            if (v != MySQLVersion::UNKNOWN) {
                votes[v]++;
            }
        }
    }

    // 找到票数最多的版本
    // 并列时返回UNKNOWN——无法确定唯一版本
    // 如果对并列选择较小版本，可能造成5.6/8.0混合场景的误判
    MySQLVersion result = MySQLVersion::UNKNOWN;
    int max_votes = 0;
    bool tie = false;
    for (const auto& [version, count] : votes) {
        if (count > max_votes) {
            max_votes = count;
            result = version;
            tie = false;
        } else if (count == max_votes) {
            tie = true;
        }
    }

    if (tie) {
        return MySQLVersion::UNKNOWN;
    }

    return result;
}

int MySQLVersionDetector::get_confidence(MySQLVersion version,
                                          const uint8_t* page_data,
                                          size_t page_size) {
    if (!page_data || version == MySQLVersion::UNKNOWN ||
        detect(page_data, page_size) != version) {
        return 0;
    }

    int supported = 10;  // Valid InnoDB page type, required by detect().

    if (version == MySQLVersion::MYSQL_80) {
        if (check_sdi_signature(page_data, page_size) <= 0) {
            return 0;
        }
        supported += 90;
    }

    return supported;
}

bool MySQLVersionDetector::matches_version(const uint8_t* page_data,
                                            size_t page_size,
                                            MySQLVersion version) {
    if (!page_data || version == MySQLVersion::UNKNOWN) {
        return false;
    }

    if (check_page_type_range(page_data, page_size) <= 0) {
        return false;
    }

    const int sdi_score = check_sdi_signature(page_data, page_size);

    if (version == MySQLVersion::MYSQL_80) {
        return sdi_score > 0;
    }

    if (version == MySQLVersion::MYSQL_57) {
        return sdi_score <= 0 && check_fsp_flags(page_data, page_size) > 0;
    }

    return false;
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

    // FLUSH_LSN is corroborating evidence only. It must be paired with a
    // page-type-specific version marker before selecting a version.
    if (flush_lsn > 0) {
        return 1;
    }

    return 0;
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
            return 15;
        }
        return 0;
    }

    return -10;  // Not an SDI page type
}

int MySQLVersionDetector::check_page_version_field(const uint8_t* page_data, size_t page_size) {
    // Offset 38 belongs to the page header and overlaps PAGE_N_DIR_SLOTS on
    // layouts where it is not a version field. It is not server-version evidence.
    (void)page_data;
    (void)page_size;
    return 0;
}

int MySQLVersionDetector::check_checksum_type(const uint8_t* page_data, size_t page_size) {
    // A checksum value alone does not identify an algorithm or server version.
    (void)page_data;
    (void)page_size;
    return 0;
}

int MySQLVersionDetector::check_fsp_flags(const uint8_t* page_data, size_t page_size) {
    // 检查FSP Header中的FSP_SPACE_FLAGS（page 0，offset 38起，4字节big-endian）
    // flags位定义（参考 fil0fil.h FSP_SPACE_FLAGS bit布局）：
    //   bit 0  : POST_ANTELOPE （MySQL 5.7+，COMPACT+/ANTELOPE格式）
    //   bit 1  : ZIP_SSIZE（压缩页大小）
    //   bit 5  : ATOMIC_BLOBS（MySQL 5.7+，原子BLOB指针）
    //   bit 6  : PAGE_COMPRESSION
    // 仅对FSP_HDR页（page 0）有效；非FSP页无法判断，返回0（不确定）
    if (page_size < 42) return 0;

    uint16_t page_type = endian::read_be16(page_data, innodb::FIL_PAGE_TYPE_ABS);
    if (page_type != innodb::FIL_PAGE_TYPE_FSP_HDR) {
        return 0;  // 非FSP页，无法读取空间标志
    }

    // FSP Header (Page 0, offset 38): FSP_SPACE_ID at +0, FSP_SPACE_FLAGS at +4
    uint32_t flags = endian::read_be32(page_data, innodb::FIL_HEADER_SIZE + 4);  // offset 42

    // POST_ANTELOPE位（bit 0）：MySQL 5.7+ 设置，5.6及更早版本清除
    constexpr uint32_t FSP_FLAGS_POST_ANTELOPE = 0x1;
    // ATOMIC_BLOBS位（bit 5）：MySQL 5.7+ 设置
    constexpr uint32_t FSP_FLAGS_ATOMIC_BLOBS  = 0x20;

    int score = 0;
    if (flags & FSP_FLAGS_POST_ANTELOPE) {
        score += 8;  // MySQL 5.7+ 特征
    }
    if (flags & FSP_FLAGS_ATOMIC_BLOBS) {
        score += 4;  // MySQL 5.7+ 原子BLOB
    }

    if (score > 0) return score;
    return -5;  // 无MODERN特征（可能是5.6或更早）
}

int MySQLVersionDetector::check_undo_log_format(const uint8_t* page_data, size_t page_size) {
    // Check UNDO LOG page format
    // MySQL 8.0+ uses new UNDO LOG format
    if (page_size < 50) return 0;

    // Read page type at offset 24 (FIL_PAGE_TYPE_ABS), big-endian
    uint16_t page_type = endian::read_be16(page_data, innodb::FIL_PAGE_TYPE_ABS);

    if (page_type == innodb::FIL_PAGE_UNDO_LOG) {  // 0x45B2 = 17842
        // Check TRX_UNDO_PAGE_TYPE at offset FIL_PAGE_DATA (38)
        // TRX_UNDO_PAGE_TYPE: 2 bytes, values: 1=TRX_UNDO_INSERT, 2=TRX_UNDO_UPDATE
        // In MySQL 5.6+, the UNDO page header is always at FIL_PAGE_DATA
        uint16_t undo_type = endian::read_be16(page_data, innodb::FIL_HEADER_SIZE);
        if (undo_type == 1 || undo_type == 2) {
            return 8;  // Valid undo log page
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
    // FLUSH_LSN, checksum values, undo layout, and page-header bytes do not
    // uniquely identify a server version. Only page-type-scoped markers select
    // an automatic compatibility family.
    (void)lsn_score;
    (void)page_version_score;
    (void)checksum_score;
    (void)undo_score;

    if (sdi_score > 0) {
        return MySQLVersion::MYSQL_80;
    }

    if (fsp_score > 0) {
        return MySQLVersion::MYSQL_57;
    }

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
    return {MySQLVersion::MYSQL_56, MySQLVersion::MYSQL_90};
}

std::vector<MySQLVersion> DetailedVersionDetector::get_supported_versions() {
    return {
        MySQLVersion::MYSQL_56,
        MySQLVersion::MYSQL_57,
        MySQLVersion::MYSQL_80,
        MySQLVersion::MYSQL_84,
        MySQLVersion::MYSQL_90
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
        r.feature_name = "Page Header Version Bytes";
        int score = MySQLVersionDetector::check_page_version_field(page_data, page_size);
        r.matches = (score > 0);
        r.score = std::abs(score);
        r.description = "Not used for server-version detection";
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

    // 检查FSP格式标志
    {
        FeatureCheckResult r;
        r.feature_name = "FSP Format Flags";
        int score = MySQLVersionDetector::check_fsp_flags(page_data, page_size);
        r.matches = (score > 0);
        r.score = std::abs(score);
        r.description = r.matches ? "Has modern FSP format flags" : "No modern FSP format flags";
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
    bool has_sdi = false;
    bool has_fsp_flags = false;
    bool has_valid_page_type = false;

    for (const auto& r : results) {
        if (r.feature_name == "SDI Support") {
            has_sdi = r.matches;
        } else if (r.feature_name == "FSP Format Flags") {
            has_fsp_flags = r.matches;
        } else if (r.feature_name == "Valid Page Type") {
            has_valid_page_type = r.matches;
        }
    }

    if (!has_valid_page_type) {
        return MySQLVersion::UNKNOWN;
    }

    if (has_sdi) {
        return MySQLVersion::MYSQL_80;
    }

    if (has_fsp_flags) {
        return MySQLVersion::MYSQL_57;
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
