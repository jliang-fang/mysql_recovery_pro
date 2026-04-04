/**
 * @file src/common/version_compat.cpp
 * @brief MySQL多版本兼容性实现
 * 
 * 支持MySQL 5.0-9.0的所有版本
 */

#include "dbrecovery/common/version_compat.hpp"
#include "dbrecovery/common/endian_compat.hpp"
#include "page/page_parser.h"
#include "page/page_parser_factory.h"
#include <cstring>
#include <stdexcept>

namespace dbrecovery {

// ==================== MySQLVersionInfo Implementation ====================

MySQLVersionInfo::MySQLVersionInfo(MySQLVersion v) : version(v) {
    switch (version) {
        case MySQLVersion::MYSQL_50:
            name = "MySQL 5.0/4.x";
            supported_row_formats = {RowFormat::REDUNDANT};
            max_page_size = 65536;
            default_page_size = 16384;
            supports_sdi = false;
            supports_encryption = false;
            supports_compression = false;
            break;
            
        case MySQLVersion::MYSQL_51:
            name = "MySQL 5.1";
            supported_row_formats = {RowFormat::REDUNDANT, RowFormat::COMPACT};
            max_page_size = 65536;
            default_page_size = 16384;
            supports_sdi = false;
            supports_encryption = false;
            supports_compression = true;
            break;

        case MySQLVersion::MYSQL_55:
            name = "MySQL 5.5";
            supported_row_formats = {RowFormat::REDUNDANT, RowFormat::COMPACT};
            max_page_size = 65536;
            default_page_size = 16384;
            supports_sdi = false;
            supports_encryption = false;
            supports_compression = true;
            break;

        case MySQLVersion::MYSQL_56:
            name = "MySQL 5.6";
            supported_row_formats = {RowFormat::REDUNDANT, RowFormat::COMPACT,
                          RowFormat::DYNAMIC, RowFormat::COMPRESSED};
            max_page_size = 65536;
            default_page_size = 16384;
            supports_sdi = false;
            supports_encryption = false;
            supports_compression = true;
            break;

        case MySQLVersion::MYSQL_57:
            name = "MySQL 5.7";
            supported_row_formats = {RowFormat::REDUNDANT, RowFormat::COMPACT,
                          RowFormat::DYNAMIC, RowFormat::COMPRESSED};
            max_page_size = 65536;
            default_page_size = 16384;
            supports_sdi = false;
            supports_encryption = false;
            supports_compression = true;
            break;

        case MySQLVersion::MYSQL_80:
            name = "MySQL 8.0";
            supported_row_formats = {RowFormat::REDUNDANT, RowFormat::COMPACT,
                          RowFormat::DYNAMIC, RowFormat::COMPRESSED};
            max_page_size = 65536;
            default_page_size = 16384;
            supports_sdi = true;
            supports_encryption = true;
            supports_compression = true;
            break;

        case MySQLVersion::MYSQL_81:
            name = "MySQL 8.1";
            supported_row_formats = {RowFormat::REDUNDANT, RowFormat::COMPACT,
                          RowFormat::DYNAMIC, RowFormat::COMPRESSED};
            max_page_size = 65536;
            default_page_size = 16384;
            supports_sdi = true;
            supports_encryption = true;
            supports_compression = true;
            break;

        case MySQLVersion::MYSQL_82:
            name = "MySQL 8.2";
            supported_row_formats = {RowFormat::REDUNDANT, RowFormat::COMPACT,
                          RowFormat::DYNAMIC, RowFormat::COMPRESSED};
            max_page_size = 65536;
            default_page_size = 16384;
            supports_sdi = true;
            supports_encryption = true;
            supports_compression = true;
            break;

        case MySQLVersion::MYSQL_83:
            name = "MySQL 8.3";
            supported_row_formats = {RowFormat::REDUNDANT, RowFormat::COMPACT,
                          RowFormat::DYNAMIC, RowFormat::COMPRESSED};
            max_page_size = 65536;
            default_page_size = 16384;
            supports_sdi = true;
            supports_encryption = true;
            supports_compression = true;
            break;

        case MySQLVersion::MYSQL_84:
            name = "MySQL 8.4";
            supported_row_formats = {RowFormat::REDUNDANT, RowFormat::COMPACT,
                          RowFormat::DYNAMIC, RowFormat::COMPRESSED};
            max_page_size = 65536;
            default_page_size = 16384;
            supports_sdi = true;
            supports_encryption = true;
            supports_compression = true;
            break;

        case MySQLVersion::MYSQL_90:
            name = "MySQL 9.0";
            supported_row_formats = {RowFormat::REDUNDANT, RowFormat::COMPACT,
                          RowFormat::DYNAMIC, RowFormat::COMPRESSED};
            max_page_size = 65536;
            default_page_size = 16384;
            supports_sdi = true;
            supports_encryption = true;
            supports_compression = true;
            break;

        default:
            name = "Unknown";
            max_page_size = 16384;
            default_page_size = 16384;
            break;
    }
}

// ==================== RowFormatInfo Implementation ====================

RowFormatInfo::RowFormatInfo(RowFormat f) : format(f) {
    switch (format) {
        case RowFormat::REDUNDANT:
            name = "REDUNDANT";
            record_header_size = 6;
            max_row_size = 8060;
            supports_external_storage = false;
            supports_compression = false;
            break;
            
        case RowFormat::COMPACT:
            name = "COMPACT";
            record_header_size = 5;
            max_row_size = 8060;
            supports_external_storage = false;
            supports_compression = false;
            break;
            
        case RowFormat::DYNAMIC:
            name = "DYNAMIC";
            record_header_size = 5;
            max_row_size = 65536;
            supports_external_storage = true;
            supports_compression = false;
            break;
            
        case RowFormat::COMPRESSED:
            name = "COMPRESSED";
            record_header_size = 5;
            max_row_size = 8060;
            supports_external_storage = true;
            supports_compression = true;
            break;
            
        default:
            name = "UNKNOWN";
            record_header_size = 0;
            max_row_size = 0;
            break;
    }
}

// ==================== PageTypeInfo Implementation ====================

PageTypeInfo::PageTypeInfo(PageType t) : type(t) {
    switch (type) {
        case PageType::INDEX:
            name = "FIL_PAGE_INDEX";
            value = 9969;
            description = "B+Tree index page";
            break;

        case PageType::SDI:
            name = "FIL_PAGE_SDI";
            value = 9971;
            description = "Serialized Dictionary Index page (MySQL 8.0+)";
            break;

        case PageType::UNDO_LOG:
            name = "FIL_PAGE_UNDO_LOG";
            value = 9970;
            description = "Undo log page";
            break;

        case PageType::ALLOCATED:
            name = "FIL_PAGE_TYPE_ALLOCATED";
            value = 0;
            description = "Freshly allocated page";
            break;

        case PageType::BLOB:
            name = "FIL_PAGE_BLOB";
            value = 9972;
            description = "BLOB page";
            break;

        case PageType::COMPRESSED:
            name = "FIL_PAGE_COMPRESSED";
            value = 9973;
            description = "Compressed page";
            break;

        default:
            name = "UNKNOWN";
            value = 0;
            description = "Unknown page type";
            break;
    }
}

// ==================== ChecksumTypeInfo Implementation ====================

ChecksumTypeInfo::ChecksumTypeInfo(ChecksumType t) : type(t) {
    switch (type) {
        case ChecksumType::CRC32:
            name = "CRC32";
            algorithm = "IEEE CRC32";
            description = "Standard CRC32 (MySQL 5.6+)";
            break;

        case ChecksumType::INNODB:
            name = "INNODB";
            algorithm = "InnoDB proprietary";
            description = "Original InnoDB checksum (pre-5.6)";
            break;

        case ChecksumType::BLOCK_CHECKSUM:
            name = "BLOCK_CHECKSUM";
            algorithm = "CRC32 + XOR";
            description = "Block checksum (MySQL 8.0+)";
            break;

        default:
            name = "UNKNOWN";
            algorithm = "Unknown";
            description = "Unknown checksum type";
            break;
    }
}

// ==================== VersionCompatibilityFactory Implementation ====================

VersionCompatibilityFactory& VersionCompatibilityFactory::instance() {
    static VersionCompatibilityFactory factory;
    // Initialize the factory
    factory.init_version_info();
    return factory;
}

void VersionCompatibilityFactory::init_version_info() {
    // Initialize version info map
    for (int v = static_cast<int>(MySQLVersion::MYSQL_50);
         v <= static_cast<int>(MySQLVersion::MYSQL_90); v++) {
        version_info_[static_cast<MySQLVersion>(v)] = MySQLVersionInfo(static_cast<MySQLVersion>(v));
    }

    // Initialize row format info map
    for (int f = static_cast<int>(RowFormat::REDUNDANT);
         f <= static_cast<int>(RowFormat::COMPRESSED); f++) {
        row_format_info_[static_cast<RowFormat>(f)] = RowFormatInfo(static_cast<RowFormat>(f));
    }

    // Initialize page type info map
    for (int t = static_cast<int>(PageType::INDEX);
         t <= static_cast<int>(PageType::COMPRESSED); t++) {
        page_type_info_[static_cast<PageType>(t)] = PageTypeInfo(static_cast<PageType>(t));
    }

    // Initialize checksum type info map
    for (int c = static_cast<int>(ChecksumType::CRC32);
         c <= static_cast<int>(ChecksumType::BLOCK_CHECKSUM); c++) {
        checksum_type_info_[static_cast<ChecksumType>(c)] = ChecksumTypeInfo(static_cast<ChecksumType>(c));
    }

    // Initialize supported versions list
    for (int v = static_cast<int>(MySQLVersion::MYSQL_50);
         v <= static_cast<int>(MySQLVersion::MYSQL_90); v++) {
        supported_versions_.push_back(static_cast<MySQLVersion>(v));
    }

    // Initialize supported row formats list
    supported_row_formats_ = {
        RowFormat::REDUNDANT, RowFormat::COMPACT, RowFormat::DYNAMIC, RowFormat::COMPRESSED
    };

    // Initialize supported page types list
    supported_page_types_ = {
        PageType::INDEX, PageType::SDI, PageType::UNDO_LOG, PageType::ALLOCATED,
        PageType::BLOB, PageType::COMPRESSED
    };

    // Initialize supported checksum types list
    supported_checksum_types_ = {
        ChecksumType::CRC32, ChecksumType::INNODB, ChecksumType::BLOCK_CHECKSUM
    };
}

MySQLVersion VersionCompatibilityFactory::detect_version(const uint8_t* page_data, size_t page_size) const {
    return dbrecovery::detect_mysql_version(page_data, page_size);
}

RowFormat VersionCompatibilityFactory::detect_row_format(const uint8_t* page_data,
                                MySQLVersion version,
                                size_t page_size) const {
    // Simple heuristic row format detection based on version
    switch (version) {
        case MySQLVersion::MYSQL_50:
        case MySQLVersion::MYSQL_51:
        case MySQLVersion::MYSQL_55:
            return RowFormat::REDUNDANT;
        case MySQLVersion::MYSQL_56:
        case MySQLVersion::MYSQL_57:
            return RowFormat::COMPACT;
        case MySQLVersion::MYSQL_80:
        case MySQLVersion::MYSQL_81:
        case MySQLVersion::MYSQL_82:
        case MySQLVersion::MYSQL_83:
        case MySQLVersion::MYSQL_84:
        case MySQLVersion::MYSQL_90:
            return RowFormat::DYNAMIC;
        default:
            return RowFormat::UNKNOWN;
    }
}

bool VersionCompatibilityFactory::supports_feature(MySQLVersion version, const std::string& feature) const {
    auto it = version_info_.find(version);
    if (it == version_info_.end()) {
        return false;
    }
    const auto& info = it->second;
    if (feature == "SDI") {
        return info.supports_sdi;
    } else if (feature == "encryption") {
        return info.supports_encryption;
    } else if (feature == "compression") {
        return info.supports_compression;
    }
    return false;
}

std::string VersionCompatibilityFactory::version_to_string(MySQLVersion version) const {
    auto it = version_info_.find(version);
    if (it != version_info_.end()) {
        return it->second.name;
    }
    return "Unknown";
}

std::pair<MySQLVersion, MySQLVersion> VersionCompatibilityFactory::get_supported_version_range() const {
    return {MySQLVersion::MYSQL_50, MySQLVersion::MYSQL_90};
}

bool VersionCompatibilityFactory::is_version_supported(MySQLVersion version) const {
    return version_info_.find(version) != version_info_.end();
}

uint32_t VersionCompatibilityFactory::get_default_page_size(MySQLVersion version) const {
    auto it = version_info_.find(version);
    if (it != version_info_.end()) {
        return it->second.default_page_size;
    }
    return 16384;
}

std::vector<uint32_t> VersionCompatibilityFactory::get_supported_page_sizes(MySQLVersion version) const {
    std::vector<uint32_t> sizes;
    auto it = version_info_.find(version);
    if (it != version_info_.end()) {
        sizes.push_back(it->second.min_page_size);
        if (it->second.max_page_size > it->second.min_page_size) {
            sizes.push_back(it->second.max_page_size);
        }
    }
    return sizes;
}

const MySQLVersionInfo& VersionCompatibilityFactory::get_version_info(MySQLVersion version) const {
    auto it = version_info_.find(version);
    if (it != version_info_.end()) {
        return it->second;
    }
    throw std::runtime_error("Unknown MySQL version");
}

const RowFormatInfo& VersionCompatibilityFactory::get_row_format_info(RowFormat format) const {
    auto it = row_format_info_.find(format);
    if (it != row_format_info_.end()) {
        return it->second;
    }
    throw std::runtime_error("Unknown row format");
}

const PageTypeInfo& VersionCompatibilityFactory::get_page_type_info(PageType type) const {
    auto it = page_type_info_.find(type);
    if (it != page_type_info_.end()) {
        return it->second;
    }
    throw std::runtime_error("Unknown page type");
}

const ChecksumTypeInfo& VersionCompatibilityFactory::get_checksum_type_info(ChecksumType type) const {
    auto it = checksum_type_info_.find(type);
    if (it != checksum_type_info_.end()) {
        return it->second;
    }
    throw std::runtime_error("Unknown checksum type");
}

std::vector<MySQLVersion> VersionCompatibilityFactory::get_supported_versions() const {
    std::vector<MySQLVersion> result;
    for (const auto& pair : version_info_) {
        result.push_back(pair.first);
    }
    return result;
}

std::vector<RowFormat> VersionCompatibilityFactory::get_supported_row_formats() const {
    return supported_row_formats_;
}

std::vector<PageType> VersionCompatibilityFactory::get_supported_page_types() const {
    return supported_page_types_;
}

std::vector<ChecksumType> VersionCompatibilityFactory::get_supported_checksum_types() const {
    return supported_checksum_types_;
}

// Global instance
static VersionCompatibilityFactory g_version_factory;

const VersionCompatibilityFactory& getVersionCompatibilityFactory() {
    return g_version_factory;
}

// ==================== Version Detection ====================

MySQLVersion detectVersion(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 38) {
        return MySQLVersion::UNKNOWN;
    }

    // Simple version detection based on page header
    // For a complete implementation, use mysql_recovery::PageFactory
    uint16_t page_type = dbrecovery::endian::read_le16(page_data, 16);
    
    // Check for SDI page (MySQL 8.0+)
    if (page_type == 9971) {
        return MySQLVersion::MYSQL_80;
    }
    
    // Check for header_version field (MySQL 8.0+)
    if (page_size >= 42) {
        uint16_t header_version = dbrecovery::endian::read_le16(page_data, 40);
        if (header_version >= 2) {
            return MySQLVersion::MYSQL_84;
        }
        if (header_version > 0) {
            return MySQLVersion::MYSQL_80;
        }
    }
    
    // Check for FIL_PAGE_FILE_FLUSH_LSN (8 bytes in 8.0+)
    if (page_size >= 32) {
        uint64_t flush_lsn = dbrecovery::endian::read_le64(page_data, 24);
        if (flush_lsn > 0xFFFFFFFF) {
            return MySQLVersion::MYSQL_80;
        }
    }
    
    // Check for PAGE_MAX_TRX_ID (5.6/5.7 specific)
    if (page_size >= 42) {
        uint32_t max_trx_id = dbrecovery::endian::read_le32(page_data, 38);
        if (max_trx_id != 0 && max_trx_id != 0xFFFFFFFF) {
            if (page_size >= 50) {
                uint16_t n_dir_slots = dbrecovery::endian::read_le16(page_data, 48);
                if (n_dir_slots > 0) {
                    return MySQLVersion::MYSQL_57;
                }
            }
            return MySQLVersion::MYSQL_56;
        }
    }
    
    // Check for 5.5/5.1 specific patterns
    if (page_size >= 38) {
        uint16_t data_start = dbrecovery::endian::read_le16(page_data, 38);
        if (data_start == 0 || data_start == 38 || data_start == 0xFFFF) {
            return MySQLVersion::MYSQL_55;
        }
    }
    
    // Default to 5.6
    return MySQLVersion::MYSQL_56;
}

std::string versionToString(MySQLVersion version) {
    try {
        return getVersionCompatibilityFactory().get_version_info(version).name;
    } catch (...) {
        return "Unknown";
    }
}

bool isVersionSupported(MySQLVersion version) {
    try {
        getVersionCompatibilityFactory().get_version_info(version);
        return true;
    } catch (...) {
        return false;
    }
}

// ==================== Row Format Detection ====================

RowFormat detectRowFormat(const uint8_t* page_data, MySQLVersion version, size_t page_size) {
    if (!page_data) {
        return RowFormat::UNKNOWN;
    }

    // Simple heuristic row format detection based on version
    // For a complete implementation, use mysql_recovery::PageFactory
    switch (version) {
        case MySQLVersion::MYSQL_50:
        case MySQLVersion::MYSQL_51:
        case MySQLVersion::MYSQL_55:
            return RowFormat::REDUNDANT;
        case MySQLVersion::MYSQL_56:
        case MySQLVersion::MYSQL_57:
            return RowFormat::COMPACT;
        case MySQLVersion::MYSQL_80:
        case MySQLVersion::MYSQL_81:
        case MySQLVersion::MYSQL_82:
        case MySQLVersion::MYSQL_83:
        case MySQLVersion::MYSQL_84:
        case MySQLVersion::MYSQL_90:
            return RowFormat::DYNAMIC;
        default:
            return RowFormat::UNKNOWN;
    }
}

std::string rowFormatToString(RowFormat format) {
    switch (format) {
        case RowFormat::UNKNOWN:
            return "UNKNOWN";
        case RowFormat::REDUNDANT:
            return "REDUNDANT";
        case RowFormat::COMPACT:
            return "COMPACT";
        case RowFormat::DYNAMIC:
            return "DYNAMIC";
        case RowFormat::COMPRESSED:
            return "COMPRESSED";
        case RowFormat::PAGE_COMPRESSED:
            return "PAGE_COMPRESSED";
        default:
            return "UNKNOWN";
    }
}

// ==================== Page Type Detection ====================

PageType detectPageType(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 18) {
        return PageType::UNKNOWN;
    }

    uint16_t page_type = dbrecovery::endian::read_le16(page_data, 16);

    switch (page_type) {
        case 9969:  // FIL_PAGE_INDEX
            return PageType::INDEX;
        case 9971:  // FIL_PAGE_SDI
            return PageType::SDI;
        case 9970:  // FIL_PAGE_UNDO_LOG
            return PageType::UNDO_LOG;
        case 9972:  // FIL_PAGE_BLOB
            return PageType::BLOB;
        case 9973:  // FIL_PAGE_COMPRESSED
            return PageType::COMPRESSED;
        default:
            return PageType::UNKNOWN;
    }
}

std::string pageTypeToString(PageType type) {
    switch (type) {
        case PageType::INDEX:
            return "INDEX";
        case PageType::SYSTEM:
            return "SYSTEM";
        case PageType::UNDO_LOG:
            return "UNDO_LOG";
        case PageType::ALLOCATED:
            return "ALLOCATED";
        case PageType::FSP_HDR:
            return "FSP_HDR";
        case PageType::XDES:
            return "XDES";
        case PageType::SDI:
            return "SDI";
        case PageType::BLOB:
            return "BLOB";
        case PageType::COMPRESSED:
            return "COMPRESSED";
        case PageType::TEMPORARY:
            return "TEMPORARY";
        default:
            return "UNKNOWN";
    }
}

// ==================== Checksum Type Detection ====================

ChecksumType detectChecksumType(const uint8_t* page_data, MySQLVersion version, size_t page_size) {
    if (!page_data || page_size < 4) {
        return ChecksumType::NONE;
    }

    // MySQL 5.6+ uses CRC32
    if (version >= MySQLVersion::MYSQL_56) {
        return ChecksumType::CRC32;
    }

    // MySQL 8.0+ uses BLOCK_CHECKSUM
    if (version >= MySQLVersion::MYSQL_80) {
        return ChecksumType::BLOCK_CHECKSUM;
    }

    // Older versions use INNODB
    return ChecksumType::INNODB;
}

std::string checksumTypeToString(ChecksumType type) {
    switch (type) {
        case ChecksumType::NONE:
            return "NONE";
        case ChecksumType::CRC32:
            return "CRC32";
        case ChecksumType::INNODB:
            return "INNODB";
        case ChecksumType::FSP_CHECKSUM:
            return "FSP_CHECKSUM";
        case ChecksumType::NEW:
            return "NEW";
        case ChecksumType::OLD:
            return "OLD";
        case ChecksumType::BLOCK_CHECKSUM:
            return "BLOCK_CHECKSUM";
        default:
            return "UNKNOWN";
    }
}

} // namespace dbrecovery
