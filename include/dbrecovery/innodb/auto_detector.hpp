#ifndef DBRECOVERY_INNODB_AUTO_DETECTOR_HPP
#define DBRECOVERY_INNODB_AUTO_DETECTOR_HPP

#include "dbrecovery/innodb/page_types_v2.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace dbrecovery {
namespace innodb {

/**
 * @brief MySQL 文件自动探测器
 * @note 通过 Page 0/3 的特征码自动判定版本及行格式
 */
class AutoDetector {
public:
    struct DetectionResult {
        MySQLVersion version;
        RowFormat row_format;
        uint32_t page_size;
        uint32_t logical_page_size;
        uint32_t physical_page_size;
        bool has_sdi;
        bool is_compressed;
        uint16_t zip_size;
        
        // 推断的建表参数
        struct TableInfo {
            std::string table_name;
            std::string database_name;
            uint32_t n_cols;
            std::vector<std::string> col_names;
            std::vector<uint8_t> col_types;
        } table_info;
    };
    
    /**
     * @brief 从 Page 0 推测版本
     * @details 检查 FSP_HDR/XDES 页的特征
     */
    static MySQLVersion detect_version_from_page0(const uint8_t* page0, size_t available_bytes);
    
    /**
     * @brief 从 SDI 页推断版本
     */
    static MySQLVersion detect_version_from_sdi(const uint8_t* sdi_page);
    
    /**
     * @brief 从行格式特征推断
     */
    static RowFormat detect_row_format(const uint8_t* record, size_t len);
    
    /**
     * @brief 完整探测流程
     */
    static DetectionResult detect(const std::filesystem::path& ibd_path);
    
    /**
     * @brief 从文件头探测 (FIL_HDR)
     */
    static DetectionResult detect_from_file_header(const uint8_t* file_data, size_t size);
    
    /**
     * @brief 从记录头推断行格式
     */
    static RowFormat detect_row_format_from_header(const uint8_t* record_header);
    
    /**
     * @brief 从页类型推断版本
     */
    static MySQLVersion detect_version_from_page_type(uint16_t page_type);
    
    /**
     * @brief 从空间ID推断版本
     */
    static MySQLVersion detect_version_from_space_id(uint32_t space_id);
    
    /**
     * @brief 从页大小推断版本
     */
    static MySQLVersion detect_version_from_page_size(uint32_t page_size);
    
private:
    // Magic number for MySQL 5.6/5.7
    static constexpr uint32_t MAGIC_56_57 = 0x12345678;
    
    // Magic number for MySQL 8.0+
    static constexpr uint32_t MAGIC_80_PLUS = 0x80808080;
    
    // SDI 页特征
    static constexpr uint16_t SDI_PAGE_TYPE = 0x5441;
    
    // 默认页大小
    static constexpr uint32_t DEFAULT_PAGE_SIZE = 16384;
};

// ==================== 实现 ====================

inline MySQLVersion AutoDetector::detect_version_from_page0(const uint8_t* page0,
                                                            size_t available_bytes) {
    constexpr size_t kPage0FieldsEnd = FIL_PAGE_SPACE_ID + sizeof(uint32_t);
    if (!page0 || available_bytes < kPage0FieldsEnd) {
        return MySQLVersion::UNKNOWN;
    }
    
    // 读取页类型 (FIL header, big-endian, offset 24)
    uint16_t page_type = read_be<uint16_t>(page0, FIL_PAGE_TYPE_ABS);

    // Only SDI page types provide version-specific evidence. Generic index and
    // FSP metadata are shared across supported server versions.
    if (is_sdi_page_type(page_type)) {
        return MySQLVersion::MYSQL_80;
    }

    return MySQLVersion::UNKNOWN;
}

inline MySQLVersion AutoDetector::detect_version_from_sdi(const uint8_t* sdi_page) {
    if (!sdi_page) {
        return MySQLVersion::UNKNOWN;
    }
    
    // SDI page types are MySQL 8.0+ evidence.
    uint16_t page_type = read_be<uint16_t>(sdi_page, FIL_PAGE_TYPE_ABS);
    if (is_sdi_page_type(page_type)) {
        return MySQLVersion::MYSQL_80;
    }
    
    return MySQLVersion::UNKNOWN;
}

inline RowFormat AutoDetector::detect_row_format(const uint8_t* record, size_t len) {
    if (!record || len < 5) {
        return RowFormat::COMPACT;
    }
    
    // 检查记录头
    uint8_t info_bits = record[0];
    uint8_t extra_bytes = record[4];
    
    // Redundant: extra_bytes = 6
    if (extra_bytes == 6) {
        return RowFormat::REDUNDANT;
    }
    
    // COMPACT/DYNAMIC: extra_bytes = 5
    // 通过检查是否有 external 指针区分
    if (len > 25 && (info_bits & 0x20) != 0) {
        // 有 external 指针，可能是 DYNAMIC
        return RowFormat::DYNAMIC;
    }
    
    return RowFormat::COMPACT;
}

inline RowFormat AutoDetector::detect_row_format_from_header(const uint8_t* record_header) {
    if (!record_header) {
        return RowFormat::COMPACT;
    }
    
    // 检查 extra_bytes (第5字节)
    if (record_header[4] == 6) {
        return RowFormat::REDUNDANT;
    }
    
    return RowFormat::COMPACT;
}

inline MySQLVersion AutoDetector::detect_version_from_page_type(uint16_t page_type) {
    return is_sdi_page_type(page_type) ? MySQLVersion::MYSQL_80
                                       : MySQLVersion::UNKNOWN;
}

inline MySQLVersion AutoDetector::detect_version_from_space_id(uint32_t space_id) {
    (void)space_id;
    return MySQLVersion::UNKNOWN;
}

inline MySQLVersion AutoDetector::detect_version_from_page_size(uint32_t page_size) {
    (void)page_size;
    return MySQLVersion::UNKNOWN;
}

inline AutoDetector::DetectionResult AutoDetector::detect(const std::filesystem::path& ibd_path) {
    DetectionResult result{};
    result.version = MySQLVersion::UNKNOWN;
    result.row_format = RowFormat::COMPACT;
    result.page_size = DEFAULT_PAGE_SIZE;
    result.logical_page_size = DEFAULT_PAGE_SIZE;
    result.physical_page_size = DEFAULT_PAGE_SIZE;
    result.has_sdi = false;
    result.is_compressed = false;
    result.zip_size = 0;
    
    // 读取文件
    std::ifstream file(ibd_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return result;
    }
    
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> buffer(std::min(size_t(16384), file_size), 0);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    return detect_from_file_header(buffer.data(), static_cast<size_t>(file.gcount()));
}

inline AutoDetector::DetectionResult AutoDetector::detect_from_file_header(const uint8_t* file_data, size_t size) {
    DetectionResult result{};
    result.version = MySQLVersion::UNKNOWN;
    result.row_format = RowFormat::COMPACT;
    result.page_size = DEFAULT_PAGE_SIZE;
    result.logical_page_size = DEFAULT_PAGE_SIZE;
    result.physical_page_size = DEFAULT_PAGE_SIZE;
    result.has_sdi = false;
    result.is_compressed = false;
    result.zip_size = 0;
    
    if (!file_data || size < FSP_SPACE_FLAGS_OFFSET + sizeof(uint32_t)) {
        return result;
    }

    // 读取 Page 0 的关键字段 (FIL header fields are big-endian)
    uint16_t page_type = read_be<uint16_t>(file_data, FIL_PAGE_TYPE_ABS);
    const auto compression = parse_fsp_compression_metadata_detailed(
        file_data, size, DEFAULT_PAGE_SIZE);
    
    // Infer a version only from explicit version-specific page evidence.
    result.version = detect_version_from_page_type(page_type);
    result.has_sdi = is_sdi_page_type(page_type);

    if (compression) {
        result.logical_page_size = static_cast<uint32_t>(compression.metadata->logical_page_size);
        result.physical_page_size = static_cast<uint32_t>(compression.metadata->physical_page_size);
        result.page_size = result.logical_page_size;
        result.is_compressed = compression.metadata->declares_compression;
        result.zip_size = static_cast<uint16_t>(compression.metadata->physical_page_size);
    }
    
    // 推断行格式 (从 Page 0 的记录)
    if (size > 100) {
        // Page 0 通常有 FSEG_DIR 记录
        result.row_format = detect_row_format_from_header(file_data + 38);
    }
    
    return result;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_AUTO_DETECTOR_HPP
