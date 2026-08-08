#ifndef DBRECOVERY_INNODB_AUTO_DETECTOR_HPP
#define DBRECOVERY_INNODB_AUTO_DETECTOR_HPP

#include "dbrecovery/innodb/page_types_v2.hpp"
#include <filesystem>
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
    static MySQLVersion detect_version_from_page0(const uint8_t* page0);
    
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
    static MySQLVersion detect_version_from_space_id(space_id_t space_id);
    
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

inline MySQLVersion AutoDetector::detect_version_from_page0(const uint8_t* page0) {
    if (!page0) {
        return MySQLVersion::UNKNOWN;
    }
    
    // 读取页类型 (FIL header, big-endian, offset 24)
    uint16_t page_type = read_be<uint16_t>(page0, FIL_PAGE_TYPE_ABS);

    // 读取空间ID (FIL header, big-endian, offset 34)
    uint32_t space_id = read_be<uint32_t>(page0, FIL_PAGE_SPACE_ID);

    // FSP header starts at offset 38 (FIL_HEADER_SIZE); FSP_SPACE_FLAGS is at +4
    // (absolute offset 42), big-endian. ZIP_SSIZE in bits 1-4.
    uint32_t space_flags = read_be<uint32_t>(page0, 38 + 4);
    uint16_t zip_ssize = static_cast<uint16_t>((space_flags >> 1) & 0x0F);
    
    // 判断版本
    if (page_type == SDI_PAGE_TYPE || page_type == 0x5a5a) {
        return MySQLVersion::VERSION_8_0;
    }
    
    if (space_id > 0 && page_type == static_cast<uint16_t>(PageType::INDEX)) {
        return MySQLVersion::VERSION_8_0;
    }
    
    if (zip_ssize > 0) {
        // 压缩页通常是 8.0+ 的特性
        return MySQLVersion::VERSION_8_0;
    }
    
    // 检查 FSEG_DIR 类型 (5.6/5.7 特有)
    if (page_type == 0x45ba) {  // FSEG_DIR
        return MySQLVersion::VERSION_5_7;
    }
    
    // 默认返回 5.7
    return MySQLVersion::VERSION_5_7;
}

inline MySQLVersion AutoDetector::detect_version_from_sdi(const uint8_t* sdi_page) {
    if (!sdi_page) {
        return MySQLVersion::UNKNOWN;
    }
    
    // SDI 页是 MySQL 8.0+ 的特性 — FIL header is big-endian, page_type at offset 24
    uint16_t page_type = read_be<uint16_t>(sdi_page, 24);
    if (page_type == SDI_PAGE_TYPE || page_type == 0x5a5a) {
        return MySQLVersion::VERSION_8_0;
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
    switch (page_type) {
        case 0x5441:  // SDI
        case 0x5a5a:  // SDI2
            return MySQLVersion::VERSION_8_0;
        case 0x45ba:  // FSEG_DIR
            return MySQLVersion::VERSION_5_7;
        default:
            return MySQLVersion::VERSION_5_7;
    }
}

inline MySQLVersion AutoDetector::detect_version_from_space_id(space_id_t space_id) {
    // 8.0+ 空间ID通常非零
    if (space_id > 0) {
        return MySQLVersion::VERSION_8_0;
    }
    return MySQLVersion::VERSION_5_7;
}

inline MySQLVersion AutoDetector::detect_version_from_page_size(uint32_t page_size) {
    if (page_size != DEFAULT_PAGE_SIZE) {
        // 非默认页大小通常是 8.0+ 的压缩页
        return MySQLVersion::VERSION_8_0;
    }
    return MySQLVersion::VERSION_5_7;
}

inline AutoDetector::DetectionResult AutoDetector::detect(const std::filesystem::path& ibd_path) {
    DetectionResult result{};
    result.version = MySQLVersion::UNKNOWN;
    result.row_format = RowFormat::COMPACT;
    result.page_size = DEFAULT_PAGE_SIZE;
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
    file.read(buffer.data(), buffer.size());
    
    return detect_from_file_header(buffer.data(), buffer.size());
}

inline AutoDetector::DetectionResult AutoDetector::detect_from_file_header(const uint8_t* file_data, size_t size) {
    DetectionResult result{};
    result.version = MySQLVersion::UNKNOWN;
    result.row_format = RowFormat::COMPACT;
    result.page_size = DEFAULT_PAGE_SIZE;
    result.has_sdi = false;
    result.is_compressed = false;
    result.zip_size = 0;
    
    if (!file_data || size < 42) {
        return result;
    }
    
    // 读取 Page 0 的关键字段 (FIL header fields are big-endian)
    uint16_t page_type = read_be<uint16_t>(file_data, FIL_PAGE_TYPE_ABS);
    uint32_t space_id = read_be<uint32_t>(file_data, FIL_PAGE_SPACE_ID);
    // FSP_SPACE_FLAGS at absolute offset 42 (FIL_HEADER_SIZE + 4), big-endian.
    // ZIP_SSIZE in bits 1-4.
    uint32_t space_flags = read_be<uint32_t>(file_data, 38 + 4);
    uint16_t page_size_field = static_cast<uint16_t>((space_flags >> 1) & 0x0F);
    
    // 推断版本
    result.version = detect_version_from_page_type(page_type);
    if (result.version == MySQLVersion::UNKNOWN) {
        result.version = detect_version_from_space_id(space_id);
    }
    
    // 推断页大小
    if (page_size_field > 0) {
        // ZIP_SSIZE: 1=1024, 2=2048, 3=4096, 4=8192, 5=16384
        static constexpr uint16_t zip_sizes[] = {0, 1024, 2048, 4096, 8192, 16384};
        uint16_t zsize = (page_size_field < 6) ? zip_sizes[page_size_field] : 0;
        if (zsize > 0 && zsize < DEFAULT_PAGE_SIZE) {
            result.is_compressed = true;
            result.zip_size = zsize;
            result.page_size = zsize;
        }
    }
    
    // 推断行格式 (从 Page 0 的记录)
    if (size > 100) {
        // Page 0 通常有 FSEG_DIR 记录
        result.row_format = detect_row_format_from_header(file_data + 38);
    }
    
    // 检查是否有 SDI 页
    if (result.version >= MySQLVersion::VERSION_8_0) {
        result.has_sdi = true;
    }
    
    return result;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_AUTO_DETECTOR_HPP
