#ifndef PAGE_PAGE_PARSER_FACTORY_H
#define PAGE_PAGE_PARSER_FACTORY_H

#include "page_parser.h"
#include "page_parser_v55.h"
#include "page_parser_v84.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <cstring>

// Include dbrecovery::innodb namespace for read_le functions
#include "dbrecovery/innodb/mach_read.hpp"

namespace mysql_recovery {

// ==================== Enhanced Page Factory ====================
/**
 * @brief Enhanced PageFactory with comprehensive version support
 * @details Provides automatic version detection and appropriate PageHeader creation
 *
 * Supported versions:
 * - MySQL 4.x/5.0 (legacy format)
 * - MySQL 5.1/5.5 (old checksum algorithms)
 * - MySQL 5.6 (introduced COMPACT format)
 * - MySQL 5.7 (enhanced page header)
 * - MySQL 8.0 (LSN extension, SDI pages)
 * - MySQL 8.4/9.0 (encryption, temporary pages, enhanced checksums)
 */
class PageFactoryEnhanced {
private:
    // Data start offset constants
    static constexpr uint16_t DATA_START_OFFSET = 38;

    // SDI signatures for different versions
    static constexpr uint8_t SDI_SIG_80[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};
    static constexpr uint8_t SDI_SIG_57[8] = {0x00, 'S', 'D', 'I', '0', '0', '0', '0'};
    static constexpr uint8_t SDI_SIG_LEGACY[8] = {0x00, 'S', 'D', 'I', '0', '0', '0', '1'};

    // Page size constants
    static constexpr size_t MIN_PAGE_SIZE = 4096;
    static constexpr size_t MAX_PAGE_SIZE = 65536;
    static constexpr size_t DEFAULT_PAGE_SIZE = 16384;

public:
    /**
     * @brief Create PageHeader instance with automatic version detection
     * @param page_data Page data pointer
     * @param page_size Page size (default 16KB)
     * @param detected_version Optional pointer to store detected version
     * @return PageHeaderBase unique pointer
     */
    static std::unique_ptr<PageHeaderBase> create_page_header(
        const uint8_t* page_data,
        size_t page_size = DEFAULT_PAGE_SIZE,
        MySQLVersion* detected_version = nullptr
    ) {
        if (!page_data) {
            throw std::invalid_argument("Page data pointer is null");
        }

        // Detect version
        MySQLVersion version = detect_version(page_data, page_size);

        if (detected_version) {
            *detected_version = version;
        }

        // Create appropriate PageHeader based on detected version
        switch (version) {
            case MySQLVersion::MYSQL_56:
                return std::make_unique<PageHeaderV56>(page_data, page_size);

            case MySQLVersion::MYSQL_57:
                return std::make_unique<PageHeaderV56>(page_data, page_size);

            case MySQLVersion::MYSQL_80:
                return std::make_unique<PageHeaderV80Enhanced>(page_data, page_size);

            case MySQLVersion::MYSQL_84:
                return std::make_unique<PageHeaderV84>(page_data, page_size, version);

            default:
                // Fallback to 5.7 compatible mode
                return std::make_unique<PageHeaderV56>(page_data, page_size);
        }
    }

    /**
     * @brief Create PageHeader for specific version
     * @param version Target MySQL version
     * @param page_data Page data pointer
     * @param page_size Page size
     * @return PageHeaderBase unique pointer
     */
    static std::unique_ptr<PageHeaderBase> create_page_header_for_version(
        MySQLVersion version,
        const uint8_t* page_data,
        size_t page_size = DEFAULT_PAGE_SIZE
    ) {
        if (!page_data) {
            throw std::invalid_argument("Page data pointer is null");
        }

        switch (version) {
            case MySQLVersion::MYSQL_56:
            case MySQLVersion::MYSQL_57:
                return std::make_unique<PageHeaderV56>(page_data, page_size);

            case MySQLVersion::MYSQL_80:
                return std::make_unique<PageHeaderV80Enhanced>(page_data, page_size);

            case MySQLVersion::MYSQL_84:
                return std::make_unique<PageHeaderV84>(page_data, page_size, version);

            default:
                throw std::invalid_argument("Unsupported MySQL version");
        }
    }

    /**
     * @brief Detect MySQL version from page data
     * @param page_data Page data pointer
     * @param page_size Page size
     * @return MySQLVersion detected version
     */
    static MySQLVersion detect_version(
        const uint8_t* page_data,
        size_t page_size = DEFAULT_PAGE_SIZE
    ) {
        if (!page_data || page_size < 38) {
            return MySQLVersion::UNKNOWN;
        }

        // Step 1: Check for SDI page (MySQL 8.0+)
        uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data, 16);
        if (page_type == static_cast<uint16_t>(PageType::SDI)) {
            return MySQLVersion::MYSQL_80;
        }

        // Step 2: Check Page Header Version (MySQL 8.0+) - before SDI signature check
        // This allows us to distinguish 8.4+ (header_version >= 2) from 8.0 (header_version == 1)
        if (page_size >= 42) {
            uint16_t header_version = dbrecovery::innodb::read_le<uint16_t>(page_data, 40);
            if (header_version >= 2) {
                // MySQL 8.4+ has header_version >= 2
                return MySQLVersion::MYSQL_84;
            }
            if (header_version > 0) {
                return MySQLVersion::MYSQL_80;
            }
        }

        // Step 3: Check SDI Signature (8 bytes at offset 64)
        if (page_size >= 72) {
            if (std::memcmp(page_data + 64, SDI_SIG_80, 8) == 0) {
                return MySQLVersion::MYSQL_80;
            }
            if (std::memcmp(page_data + 64, SDI_SIG_57, 8) == 0) {
                return MySQLVersion::MYSQL_57;
            }
        }

        // Step 4: Check FIL_PAGE_FILE_FLUSH_LSN (8 bytes in 8.0+)
        if (page_size >= 32) {
            uint64_t flush_lsn = dbrecovery::innodb::read_le<uint64_t>(page_data, 24);
            if (flush_lsn > 0xFFFFFFFF) {
                return MySQLVersion::MYSQL_80;
            }
        }

        // Step 5: Check PAGE_MAX_TRX_ID (5.6/5.7 specific)
        if (page_size >= 42) {
            uint32_t max_trx_id = dbrecovery::innodb::read_le<uint32_t>(page_data, 38);
            // For 5.6/5.7, max_trx_id is non-zero and not 0xFFFFFFFF
            if (max_trx_id != 0 && max_trx_id != 0xFFFFFFFF) {
                // Could be 5.6 or 5.7, check for additional fields
                if (page_size >= 50) {
                    uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data, 48);
                    if (n_dir_slots > 0) {
                        return MySQLVersion::MYSQL_57;
                    }
                }
                return MySQLVersion::MYSQL_56;
            }
        }

        // Step 6: Check space ID
        if (page_size >= 22) {
            uint32_t space_id = dbrecovery::innodb::read_le<uint32_t>(page_data, 18);
            if (space_id > 0 && page_type == static_cast<uint16_t>(PageType::INDEX)) {
                return MySQLVersion::MYSQL_80;
            }
        }

        // Step 7: Check page size field (compressed pages)
        if (page_size >= 64) {
            uint16_t page_size_field = dbrecovery::innodb::read_le<uint16_t>(page_data, 60);
            if (page_size_field != 0 && (page_size_field & 0x0FFF) < DEFAULT_PAGE_SIZE) {
                return MySQLVersion::MYSQL_80;
            }
        }

        // Default to 5.6
        return MySQLVersion::MYSQL_56;
    }

    /**
     * @brief Detect row format from page data
     * @param page_data Page data pointer
     * @param version MySQL version
     * @param page_size Page size
     * @return RowFormat detected row format
     */
    static RowFormat detect_row_format(
        const uint8_t* page_data,
        MySQLVersion version,
        size_t page_size = DEFAULT_PAGE_SIZE
    ) {
        if (!page_data) {
            return RowFormat::UNKNOWN;
        }

        switch (version) {
            case MySQLVersion::MYSQL_56:
            case MySQLVersion::MYSQL_57:
                return detect_row_format_v56_57(page_data, page_size);

            case MySQLVersion::MYSQL_80:
            case MySQLVersion::MYSQL_84:
                return detect_row_format_v8x(page_data, page_size);

            default:
                return RowFormat::UNKNOWN;
        }
    }

    /**
     * @brief Check if page is from very old MySQL (4.x/5.0)
     * @param page_data Page data pointer
     * @param page_size Page size
     * @return true if old format
     */
    static bool is_old_mysql_format(const uint8_t* page_data, size_t page_size) {
        if (!page_data || page_size < 36) {
            return false;
        }

        // Check for 5.0/4.x specific patterns
        if (page_size >= 40) {
            uint16_t data_start_36 = dbrecovery::innodb::read_le<uint16_t>(page_data, 36);
            uint16_t data_start_38 = dbrecovery::innodb::read_le<uint16_t>(page_data, 38);

            // If data_start at 36 is reasonable but 38 is not, likely old format
            if (data_start_36 >= 36 && data_start_36 < 100 && data_start_38 > 100) {
                return true;
            }
        }

        // Check for old-style record headers
        if (page_size >= 60) {
            const uint8_t* first_rec = page_data + 54;
            if (first_rec[0] == 0x80 || first_rec[0] == 0x81) {
                return true;
            }
        }

        return false;
    }

    /**
     * @brief Get version string from enum
     * @param version MySQL version
     * @return Version string
     */
    static std::string version_to_string(MySQLVersion version) {
        switch (version) {
            case MySQLVersion::MYSQL_56:
                return "MySQL 5.6";
            case MySQLVersion::MYSQL_57:
                return "MySQL 5.7";
            case MySQLVersion::MYSQL_80:
                return "MySQL 8.0";
            case MySQLVersion::MYSQL_84:
                return "MySQL 8.4";
            default:
                return "Unknown";
        }
    }

    /**
     * @brief Get page type string
     * @param page_type Page type value
     * @return Page type string
     */
    static std::string page_type_to_string(uint16_t page_type) {
        switch (page_type) {
            case static_cast<uint16_t>(PageType::INDEX):
                return "FIL_PAGE_INDEX";
            case static_cast<uint16_t>(PageType::SDI):
                return "FIL_PAGE_SDI";
            case static_cast<uint16_t>(PageType::UNDO_LOG):
                return "FIL_PAGE_UNDO_LOG";
            case static_cast<uint16_t>(PageType::ALLOCATED):
                return "FIL_PAGE_TYPE_ALLOCATED";
            case static_cast<uint16_t>(PageType::BLOB):
                return "FIL_PAGE_BLOB";
            case static_cast<uint16_t>(PageType::COMPRESSED):
                return "FIL_PAGE_COMPRESSED";
            default:
                return "UNKNOWN";
        }
    }

    /**
     * @brief Validate page header checksum
     * @param page_data Page data pointer
     * @param page_size Page size
     * @param version MySQL version (optional, auto-detected if not provided)
     * @return true if checksum is valid
     */
    static bool validate_checksum(
        const uint8_t* page_data,
        size_t page_size = DEFAULT_PAGE_SIZE,
        MySQLVersion version = MySQLVersion::UNKNOWN
    ) {
        if (!page_data || page_size < 52) {
            return false;
        }

        // Auto-detect version if not provided
        if (version == MySQLVersion::UNKNOWN) {
            version = detect_version(page_data, page_size);
        }

        // Create temporary PageHeader to validate checksum
        auto header = create_page_header_for_version(version, page_data, page_size);
        return header->verify_checksum();
    }

    /**
     * @brief Calculate page checksum
     * @param page_data Page data pointer
     * @param page_size Page size
     * @param version MySQL version (optional, auto-detected if not provided)
     * @return Calculated checksum value
     */
    static uint32_t calculate_checksum(
        const uint8_t* page_data,
        size_t page_size = DEFAULT_PAGE_SIZE,
        MySQLVersion version = MySQLVersion::UNKNOWN
    ) {
        if (!page_data) {
            return 0;
        }

        // Auto-detect version if not provided
        if (version == MySQLVersion::UNKNOWN) {
            version = detect_version(page_data, page_size);
        }

        // Create temporary PageHeader to calculate checksum
        auto header = create_page_header_for_version(version, page_data, page_size);
        return header->calculate_checksum();
    }

private:
    // Row format detection helpers
    static RowFormat detect_row_format_v5x(const uint8_t* page_data, size_t page_size) {
        const uint8_t* first_rec = page_data + DATA_START_OFFSET + 16;
        if (page_size < DATA_START_OFFSET + 25) {
            return RowFormat::REDUNDANT;
        }
        uint8_t extra_bytes = first_rec[4];
        return (extra_bytes == 6) ? RowFormat::REDUNDANT : RowFormat::REDUNDANT;
    }

    static RowFormat detect_row_format_v56_57(const uint8_t* page_data, size_t page_size) {
        const uint8_t* first_rec = page_data + DATA_START_OFFSET + 16;
        if (page_size < DATA_START_OFFSET + 25) {
            return RowFormat::COMPACT;
        }
        uint8_t extra_bytes = first_rec[4];
        if (extra_bytes == 6) {
            return RowFormat::REDUNDANT;
        }
        uint8_t info_bits = first_rec[0];
        if ((info_bits & 0x20) != 0) {
            return RowFormat::DYNAMIC;
        }
        return RowFormat::COMPACT;
    }

    static RowFormat detect_row_format_v8x(const uint8_t* page_data, size_t page_size) {
        const uint8_t* first_rec = page_data + DATA_START_OFFSET + 16;
        if (page_size < DATA_START_OFFSET + 25) {
            return RowFormat::DYNAMIC;
        }
        uint8_t extra_bytes = first_rec[4];
        if (extra_bytes == 6) {
            return RowFormat::REDUNDANT;
        }
        uint8_t info_bits = first_rec[0];
        if ((info_bits & 0x20) != 0) {
            return RowFormat::DYNAMIC;
        }
        return RowFormat::COMPACT;
    }

    /**
     * @brief Enhanced row format detection with more accurate logic
     * @param page_data Page data pointer
     * @param version MySQL version
     * @param page_size Page size
     * @return RowFormat detected row format
     */
    static RowFormat detect_row_format_enhanced(
        const uint8_t* page_data,
        MySQLVersion version,
        size_t page_size = DEFAULT_PAGE_SIZE
    ) {
        if (!page_data || page_size < DATA_START_OFFSET + 25) {
            return RowFormat::UNKNOWN;
        }

        const uint8_t* first_rec = page_data + DATA_START_OFFSET + 16;

        // Check extra_bytes first (most reliable indicator)
        uint8_t extra_bytes = first_rec[4];

        // 6 bytes = Redundant format (all versions)
        if (extra_bytes == 6) {
            return RowFormat::REDUNDANT;
        }

        // 5 bytes = Compact/Dynamic format
        if (extra_bytes == 5) {
            uint8_t info_bits = first_rec[0];

            // Check for external pointer flag (bit 5)
            // This is the most reliable indicator for Dynamic format
            if ((info_bits & 0x20) != 0) {
                return RowFormat::DYNAMIC;
            }

            // Check for external length flag (bit 4) - also indicates Dynamic
            if ((info_bits & 0x10) != 0) {
                return RowFormat::DYNAMIC;
            }

            // Check record length field
            uint16_t rec_length = (info_bits >> 3) & 0x1F;
            if (rec_length > 0) {
                return RowFormat::COMPACT;
            }

            return RowFormat::COMPACT;
        }

        // Fallback based on version
        switch (version) {
            case MySQLVersion::MYSQL_56:
            case MySQLVersion::MYSQL_57:
                return RowFormat::COMPACT;

            case MySQLVersion::MYSQL_80:
            case MySQLVersion::MYSQL_84:
                return RowFormat::DYNAMIC;

            default:
                return RowFormat::UNKNOWN;
        }
    }

    /**
     * @brief Detect compressed page (zip_size)
     * @param page_data Page data pointer
     * @param version MySQL version
     * @param page_size Page size
     * @return true if page is compressed
     */
    static bool is_compressed_page(
        const uint8_t* page_data,
        MySQLVersion version,
        size_t page_size = DEFAULT_PAGE_SIZE
    ) {
        if (!page_data || page_size < 26) {
            return false;
        }

        // Check page flags at offset 24
        uint16_t flags = dbrecovery::innodb::read_le<uint16_t>(page_data, 24);

        // Bit 0 indicates compressed page
        if ((flags & 0x0001) != 0) {
            return true;
        }

        return false;
    }

    /**
     * @brief Get zip_size for compressed pages
     * @param page_data Page data pointer
     * @param version MySQL version
     * @param page_size Page size
     * @return zip_size value (0 if not compressed)
     */
    static uint16_t get_zip_size(
        const uint8_t* page_data,
        MySQLVersion version,
        size_t page_size = DEFAULT_PAGE_SIZE
    ) {
        if (!is_compressed_page(page_data, version, page_size)) {
            return 0;
        }

        if (page_size < 26) {
            return 0;
        }

        // zip_size is stored in bits 0-11 of flags
        uint16_t flags = dbrecovery::innodb::read_le<uint16_t>(page_data, 24);
        return flags & 0x0FFF;
    }
};

// ==================== Version Detection Helper Functions ====================

/**
 * @brief Detect MySQL version from file
 * @param page_data Page data pointer
 * @param page_size Page size
 * @return MySQLVersion detected version
 */
inline MySQLVersion detect_mysql_version(
    const uint8_t* page_data,
    size_t page_size = 16384
) {
    return PageFactoryEnhanced::detect_version(page_data, page_size);
}

/**
 * @brief Get version string from page data
 * @param page_data Page data pointer
 * @param page_size Page size
 * @return Version string
 */
inline std::string get_mysql_version_string(
    const uint8_t* page_data,
    size_t page_size = 16384
) {
    MySQLVersion version = detect_mysql_version(page_data, page_size);
    return PageFactoryEnhanced::version_to_string(version);
}

/**
 * @brief Check if page is compatible with current recovery engine
 * @param page_data Page data pointer
 * @param page_size Page size
 * @param min_version Minimum supported version
 * @return true if compatible
 */
inline bool is_page_compatible(
    const uint8_t* page_data,
    size_t page_size = 16384,
    MySQLVersion min_version = MySQLVersion::MYSQL_56
) {
    MySQLVersion version = detect_mysql_version(page_data, page_size);
    return version >= min_version;
}

} // namespace mysql_recovery

#endif // PAGE_PAGE_PARSER_FACTORY_H
