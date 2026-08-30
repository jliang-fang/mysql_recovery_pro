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
const std::array<MySQLVersionDetector::VersionFeature, 5> MySQLVersionDetector::version_features_ = {{
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
    auto result = detect_pages_with_evidence(pages, page_size);
    return result.version;
}

MultiPageDetectionResult MySQLVersionDetector::detect_pages_with_evidence(
    const std::vector<const uint8_t*>& pages, size_t page_size) {
    MultiPageDetectionResult result;
    result.total_pages = pages.size();

    if (pages.empty()) {
        result.evidence = MultiPageEvidence::INSUFFICIENT;
        result.rejection_reasons.push_back("No pages provided");
        return result;
    }

    // Phase 1: Check every page for strong evidence (SDI signature).
    // SDI on any typed and signed SDI page is the only authoritative
    // automatic marker for the MySQL 8.0+ family.
    for (size_t i = 0; i < pages.size(); ++i) {
        if (!pages[i]) {
            result.unknown_pages++;
            continue;
        }

        int sdi_score = check_sdi_signature(pages[i], page_size);
        if (sdi_score > 0) {
            result.strong_evidence_pages++;
            result.strong_evidence_details.push_back(
                "Page " + std::to_string(i) + ": SDI signature found");
        } else {
            // Check for corroborating evidence (FSP flags, LSN, undo)
            int fsp_score = check_fsp_flags(pages[i], page_size);
            int lsn_score = check_lsn_extension(pages[i], page_size);
            int undo_score = check_undo_log_format(pages[i], page_size);

            if (fsp_score > 0 || lsn_score > 0 || undo_score > 0) {
                result.corroborating_pages++;
                std::string details = "Page " + std::to_string(i) + ":";
                if (fsp_score > 0) details += " FSP_flags";
                if (lsn_score > 0) details += " LSN_ext";
                if (undo_score > 0) details += " undo";
                result.corroborating_details.push_back(details);
            } else {
                result.unknown_pages++;
            }
        }
    }

    // Phase 2: Decide version based on evidence.
    // Only SDI is strong enough to uniquely identify MySQL 8.0+ family.
    if (result.strong_evidence_pages > 0) {
        result.version = MySQLVersion::MYSQL_80;
        result.evidence = MultiPageEvidence::STRONG;
        return result;
    }

    // Corroborating evidence alone (FSP flags, LSN extension) provides
    // only a lower bound (5.7+), not a unique version. 8.0+ FSP_HDR pages
    // also set POST_ANTELOPE/ATOMIC_BLOBS, so returning MYSQL_57 would be
    // a false positive. Only UNKNOWN is safe.
    if (result.corroborating_pages > 0) {
        result.evidence = MultiPageEvidence::INSUFFICIENT;
        result.rejection_reasons.push_back(
            "Corroborating evidence found (FSP flags, LSN, undo) but no SDI. "
            "FSP flags alone only provide a 5.7+ lower bound, not a unique version.");
        return result;
    }

    result.evidence = MultiPageEvidence::INSUFFICIENT;
    result.rejection_reasons.push_back(
        "No version-reliable markers found on any page. "
        "SDI required for MySQL 8.0+ family detection.");
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

    // MYSQL_57 can no longer be confirmed by FSP flags alone: 8.0+ FSP_HDR
    // pages also set POST_ANTELOPE/ATOMIC_BLOBS. Only positive SDI-absence in
    // combination with the caller's explicit version context can match 5.7.
    if (version == MySQLVersion::MYSQL_57) {
        return false;
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
    // read_be32 at FSP_SPACE_FLAGS_OFFSET (42) requires 46 bytes; the old
    // < 42 check allowed out-of-bounds reads on 42..45 byte buffers.
    if (page_size < innodb::FSP_SPACE_FLAGS_OFFSET + sizeof(uint32_t)) return 0;

    uint16_t page_type = endian::read_be16(page_data, innodb::FIL_PAGE_TYPE_ABS);
    if (page_type != innodb::FIL_PAGE_TYPE_FSP_HDR) {
        return 0;  // 非FSP页，无法读取空间标志
    }

    // FSP Header (Page 0, offset 38): FSP_SPACE_ID at +0, FSP_SPACE_FLAGS at +4
    uint32_t flags = endian::read_be32(page_data, innodb::FSP_SPACE_FLAGS_OFFSET);  // offset 42

    // POST_ANTELOPE位（bit 0）：MySQL 5.7+ 设置，5.6及更早版本清除
    // ATOMIC_BLOBS位（bit 5）：MySQL 5.7+ 设置

    int score = 0;
    if (flags & innodb::FSP_FLAGS_POST_ANTELOPE) {
        score += 8;  // MySQL 5.7+ 特征
    }
    if (flags & innodb::FSP_FLAGS_ATOMIC_BLOBS) {
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

    return 0;  // Not an undo log page or cannot determine
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
        page_type == innodb::FIL_PAGE_TYPE_XDES || page_type == innodb::FIL_PAGE_COMPRESSED ||
        page_type == innodb::FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
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
    (void)fsp_score;

    if (sdi_score > 0) {
        return MySQLVersion::MYSQL_80;
    }

    // FSP flags alone only provide a lower bound (5.7+), not a unique version.
    // 8.0+ FSP_HDR pages also set POST_ANTELOPE and ATOMIC_BLOBS so returning
    // MYSQL_57 here would be a false positive. Callers must use SDI evidence
    // or explicit operator override.
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

    // FSP flags alone only provide a lower bound (5.7+), not a unique version.
    // 8.0+ FSP_HDR pages also set POST_ANTELOPE and ATOMIC_BLOBS so returning
    // MYSQL_57 here would be a false positive. SDI evidence is required for
    // 8.0+ family; FSP-only evidence is insufficient.
    (void)has_fsp_flags;
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

MultiPageDetectionResult VersionDetectionFactory::detect_version_from_pages_with_evidence(
    const std::vector<const uint8_t*>& pages, size_t page_size) {
    return detector_.detect_pages_with_evidence(pages, page_size);
}

MySQLVersionDetector& VersionDetectionFactory::get_detector() {
    return detector_;
}

DetailedVersionDetector& VersionDetectionFactory::get_detailed_detector() {
    return detailed_detector_;
}

} // namespace dbrecovery
