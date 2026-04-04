#ifndef PAGE_PAGE_PARSER_V84_H
#define PAGE_PAGE_PARSER_V84_H

#include "page_parser.h"
#include <cstdint>
#include <memory>
#include <string>

namespace mysql_recovery {

// ==================== MySQL 8.4/9.0 Page Header Implementation ====================
/**
 * @brief MySQL 8.4/9.0 Page Header Implementation
 * @details Supports newer InnoDB features introduced in MySQL 8.4+
 * 
 * Key differences from 8.0:
 * - New page types (e.g., TEMPORARY, ENCRYPTED)
 * - Enhanced checksum algorithms (CRC32C with hardware acceleration)
 * - Larger LSN values (64-bit with extended range)
 * - Improved compression support
 * - New SDI format with nested dictionary entries
 */
class PageHeaderV84 : public PageHeaderBase {
private:
    static constexpr size_t HEADER_SIZE = 128;  // 8.4+ Page Header total size
    static constexpr uint16_t DATA_START_OFFSET = 38;

    std::unique_ptr<uint8_t[]> data_;
    size_t page_size_;
    MySQLVersion version_;

    // Checksum verification helpers
    bool verify_crc32c() const;
    bool verify_stripe_checksum() const;
    bool verify_legacy_checksum() const;

public:
    /**
     * @brief Constructor
     * @param page_data Page data pointer
     * @param page_size Page size (default 16KB)
     * @param version MySQL version (8.4 or 9.0)
     */
    PageHeaderV84(const uint8_t* page_data, size_t page_size = 16384,
                  MySQLVersion version = MySQLVersion::MYSQL_84);

    // PageHeaderBase interface implementation
    uint32_t get_prev_page() const override;
    uint32_t get_next_page() const override;
    uint64_t get_lsn() const override;
    PageType get_page_type() const override;
    uint16_t get_level() const override;
    uint16_t get_n_recs() const override;
    uint16_t get_free_ptr() const override;
    uint16_t get_free_size() const override;
    uint16_t get_n_dir_slots() const override;
    uint16_t get_data_start() const override;

    bool verify_checksum() const override;
    uint32_t calculate_checksum() const override;

    MySQLVersion get_version() const override { return version_; }
    RowFormat get_row_format() const override;

    uint32_t get_space_id() const override;
    uint32_t get_page_offset() const override;
    uint16_t get_max_trx_id() const override;

    std::string to_string() const override;

    // 8.4/9.0 specific methods
    uint64_t get_extended_lsn() const;
    uint16_t get_header_version() const;
    bool is_sdi_page() const;
    bool is_compressed() const;
    uint16_t get_zip_size() const;
    bool is_encrypted() const;
    uint32_t get_encryption_key_version() const;
    bool is_temporary_page() const;
    uint32_t get_stripe_size() const;

    // Enhanced checksum methods
    uint32_t calculate_crc32c() const;
    uint32_t calculate_stripe_checksum() const;

    // Encryption support methods
    bool has_encryption_support() const { return true; }
    uint32_t get_encryption_algorithm() const;
};

// ==================== MySQL 8.0 Page Header Enhancement ====================
/**
 * @brief Enhanced MySQL 8.0 Page Header with 8.4+ compatibility
 * @details Provides fallback methods for newer features
 */
class PageHeaderV80Enhanced : public PageHeaderBase {
private:
    static constexpr size_t HEADER_SIZE = 120;
    static constexpr uint16_t DATA_START_OFFSET = 38;

    std::unique_ptr<uint8_t[]> data_;
    size_t page_size_;

public:
    PageHeaderV80Enhanced(const uint8_t* page_data, size_t page_size = 16384);

    // PageHeaderBase interface implementation
    uint32_t get_prev_page() const override;
    uint32_t get_next_page() const override;
    uint64_t get_lsn() const override;
    PageType get_page_type() const override;
    uint16_t get_level() const override;
    uint16_t get_n_recs() const override;
    uint16_t get_free_ptr() const override;
    uint16_t get_free_size() const override;
    uint16_t get_n_dir_slots() const override;
    uint16_t get_data_start() const override;

    bool verify_checksum() const override;
    uint32_t calculate_checksum() const override;

    MySQLVersion get_version() const override { return MySQLVersion::MYSQL_80; }
    RowFormat get_row_format() const override;

    uint32_t get_space_id() const override;
    uint32_t get_page_offset() const override;
    uint16_t get_max_trx_id() const override;

    // 加密支持 (MySQL 8.4+)
    bool is_encrypted() const override { return false; }
    uint32_t get_encryption_key_version() const override { return 0; }

    // 压缩支持
    bool is_compressed() const override { return false; }
    uint16_t get_zip_size() const override { return 0; }

    std::string to_string() const override;

    // Enhanced methods
    uint64_t get_extended_lsn() const;
    bool is_temporary_page() const;
};

// ==================== Encryption Algorithm Enum ====================
/**
 * @brief Encryption algorithm types
 */
enum class EncryptionAlgorithm {
    NONE = 0,
    AES = 1,            // AES-128/192/256
    AES_CBC = 2,        // AES-CBC mode
    AES_ECB = 3,        // AES-ECB mode
    AES_CFB = 4,        // AES-CFB mode
    AES_OFB = 5,        // AES-OFB mode
};

/**
 * @brief Get encryption algorithm string
 */
inline std::string encryption_algorithm_to_string(EncryptionAlgorithm algo) {
    switch (algo) {
        case EncryptionAlgorithm::NONE: return "NONE";
        case EncryptionAlgorithm::AES: return "AES";
        case EncryptionAlgorithm::AES_CBC: return "AES-CBC";
        case EncryptionAlgorithm::AES_ECB: return "AES-ECB";
        case EncryptionAlgorithm::AES_CFB: return "AES-CFB";
        case EncryptionAlgorithm::AES_OFB: return "AES-OFB";
        default: return "UNKNOWN";
    }
}

// ==================== New Page Types for 8.4+ ====================
enum class PageType84 : uint16_t {
    UNKNOWN = 0,
    INDEX = 9969,           // 0x26F1 - B+Tree index page
    SDI = 9971,             // 0x26F3 - SDI metadata page
    UNDO_LOG = 9970,        // 0x26F2 - Undo log page
    TEMPORARY = 9974,       // 0x26F6 - Temporary table page (8.4+)
    ENCRYPTED = 9975,       // 0x26F7 - Encrypted page (8.4+)
    BLOB = 9972,            // 0x26F4 - BLOB page
    COMPRESSED = 9973,      // 0x26F5 - Compressed page
    FSP_HDR = 0x45BF,       // File space header
    XDES = 0x45BE,          // Extent descriptor
};

// Helper function to convert PageType84 to string
inline std::string page_type_84_to_string(uint16_t type) {
    switch (type) {
        case static_cast<uint16_t>(PageType84::INDEX):
            return "FIL_PAGE_INDEX";
        case static_cast<uint16_t>(PageType84::SDI):
            return "FIL_PAGE_SDI";
        case static_cast<uint16_t>(PageType84::UNDO_LOG):
            return "FIL_PAGE_UNDO_LOG";
        case static_cast<uint16_t>(PageType84::TEMPORARY):
            return "FIL_PAGE_TEMPORARY";
        case static_cast<uint16_t>(PageType84::ENCRYPTED):
            return "FIL_PAGE_ENCRYPTED";
        case static_cast<uint16_t>(PageType84::BLOB):
            return "FIL_PAGE_BLOB";
        case static_cast<uint16_t>(PageType84::COMPRESSED):
            return "FIL_PAGE_COMPRESSED";
        case static_cast<uint16_t>(PageType84::FSP_HDR):
            return "FIL_PAGE_FSP_HDR";
        case static_cast<uint16_t>(PageType84::XDES):
            return "FIL_PAGE_XDES";
        default:
            return "UNKNOWN";
    }
}

} // namespace mysql_recovery

#endif // PAGE_PAGE_PARSER_V84_H
