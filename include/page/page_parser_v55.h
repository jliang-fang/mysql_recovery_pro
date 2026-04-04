#ifndef PAGE_PAGE_PARSER_V55_H
#define PAGE_PAGE_PARSER_V55_H

#include "page_parser.h"
#include <cstdint>
#include <memory>
#include <string>

namespace mysql_recovery {

// ==================== MySQL 5.5/5.1 Page Header Implementation ====================
/**
 * @brief MySQL 5.5/5.1 Page Header Implementation
 * @details Supports REDUNDANT row format only (COMPACT introduced in 5.0.3)
 * 
 * Key differences from 5.6/5.7:
 * - No PAGE_HEADER_VERSION field
 * - Different checksum algorithms (innodb_checksum_algorithm=innodb)
 * - Smaller page header in some cases
 * - Different BLOB pointer format
 */
class PageHeaderV55 : public PageHeaderBase {
private:
    static constexpr size_t HEADER_SIZE = 96;  // 5.5/5.1 Page Header total size
    static constexpr uint16_t DATA_START_OFFSET = 38;  // Same as 5.6/5.7

    std::unique_ptr<uint8_t[]> data_;
    size_t page_size_;
    MySQLVersion version_;

public:
    /**
     * @brief Constructor
     * @param page_data Page data pointer
     * @param page_size Page size (default 16KB)
     * @param version MySQL version (5.5 or 5.1)
     */
    PageHeaderV55(const uint8_t* page_data, size_t page_size = 16384, 
                  MySQLVersion version = MySQLVersion::MYSQL_56);

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

    // 加密支持 (MySQL 8.4+)
    bool is_encrypted() const override { return false; }
    uint32_t get_encryption_key_version() const override { return 0; }

    // 压缩支持
    bool is_compressed() const override { return false; }
    uint16_t get_zip_size() const override { return 0; }

    std::string to_string() const override;

    // 5.5/5.1 specific methods
    uint16_t get_header_version() const;
    uint32_t get_file_flush_lsn() const;

    // Checksum specific to 5.5/5.1
    uint32_t calculate_innodb_checksum() const;
    uint32_t calculate_none_checksum() const;
};

// ==================== MySQL 5.0/4.x Compatibility Layer ====================
/**
 * @brief MySQL 5.0/4.x Page Header Implementation
 * @details Very old format with minimal header
 * 
 * Key differences:
 * - Even smaller header (36 bytes instead of 38)
 * - No space ID in page header
 * - Different record format
 */
class PageHeaderV50 : public PageHeaderBase {
private:
    static constexpr size_t HEADER_SIZE = 96;
    static constexpr uint16_t DATA_START_OFFSET = 36;  // 2 bytes less than 5.5+

    std::unique_ptr<uint8_t[]> data_;
    size_t page_size_;
    MySQLVersion version_;

public:
    PageHeaderV50(const uint8_t* page_data, size_t page_size = 16384,
                  MySQLVersion version = MySQLVersion::MYSQL_56);

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
    uint32_t calculate_innodb_checksum() const;

    MySQLVersion get_version() const override { return version_; }
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
};

// ==================== Page Factory Extensions ====================
/**
 * @brief Extended PageFactory with full version support
 */
class PageFactoryExtended {
public:
    // SDI signature for MySQL 8.0+
    static constexpr uint8_t SDI_SIG[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};

    // Additional SDI signatures for older versions
    static constexpr uint8_t OLD_SDI_SIG[8] = {0x00, 'S', 'D', 'I', '0', '0', '0', '0'};

    /**
     * @brief Create PageHeader instance with full version detection
     */
    static std::unique_ptr<PageHeaderBase> create_page_header_extended(
        const uint8_t* page_data,
        size_t page_size = 16384,
        MySQLVersion* detected_version = nullptr
    );

    /**
     * @brief Detect MySQL version with more precise logic
     */
    static MySQLVersion detect_version_extended(
        const uint8_t* page_data,
        size_t page_size = 16384
    );

    /**
     * @brief Check if page is from very old MySQL (4.x/5.0)
     */
    static bool is_old_mysql_format(const uint8_t* page_data, size_t page_size);

    /**
     * @brief Get version string from enum
     */
    static std::string version_to_string(MySQLVersion version);
};

} // namespace mysql_recovery

#endif // PAGE_PAGE_PARSER_V55_H
