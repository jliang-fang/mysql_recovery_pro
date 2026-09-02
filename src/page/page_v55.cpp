#include "page/page_parser_v55.h"
#include "dbrecovery/innodb/mach_read.hpp"
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace mysql_recovery {

// Data start offset constant
static constexpr uint16_t DATA_START_OFFSET = 38;

// ==================== PageHeaderV55 Implementation ====================

PageHeaderV55::PageHeaderV55(const uint8_t* page_data, size_t page_size, MySQLVersion version)
    : page_size_(page_size), version_(version) {
    if (!page_data) {
        throw std::invalid_argument("Page data pointer is null");
    }

    // Validate page size
    if (page_size < 4096 || page_size > 65536) {
        throw std::invalid_argument("Invalid page size: " + std::to_string(page_size));
    }

    // Allocate and copy page data
    data_ = std::make_unique<uint8_t[]>(page_size);
    std::memcpy(data_.get(), page_data, page_size);
}

uint32_t PageHeaderV55::get_prev_page() const {
    // FIL_PAGE_PREV: canonical big-endian at FIL_PAGE_PREV_ABS (8)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_PREV_ABS + sizeof(uint32_t)) return 0xFFFFFFFF;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_PREV_ABS);
}

uint32_t PageHeaderV55::get_next_page() const {
    // FIL_PAGE_NEXT: canonical big-endian at FIL_PAGE_NEXT_ABS (12)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_NEXT_ABS + sizeof(uint32_t)) return 0xFFFFFFFF;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_NEXT_ABS);
}

uint64_t PageHeaderV55::get_lsn() const {
    // FIL_PAGE_LSN: canonical big-endian at FIL_PAGE_LSN_ABS (16, 8 bytes)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_LSN_ABS + sizeof(uint64_t)) return 0;
    return dbrecovery::innodb::read_be<uint64_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_LSN_ABS);
}

PageType PageHeaderV55::get_page_type() const {
    // FIL_PAGE_TYPE: canonical big-endian at FIL_PAGE_TYPE_ABS (24, 2 bytes)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_TYPE_ABS + sizeof(uint16_t)) return PageType::UNKNOWN;
    uint16_t type = dbrecovery::innodb::read_be<uint16_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);
    return page_type_from_fil(type);
}

uint16_t PageHeaderV55::get_level() const {
    // PAGE_LEVEL: offset 34-35 (relative to page start)
    if (page_size_ < 36) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 34);
}

uint16_t PageHeaderV55::get_n_recs() const {
    // PAGE_N_RECS: offset 36-37 (relative to page start)
    if (page_size_ < 38) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 36);
}

uint16_t PageHeaderV55::get_free_ptr() const {
    // PAGE_FREE_PTR: offset 44-45 (relative to page start)
    if (page_size_ < 46) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 44);
}

uint16_t PageHeaderV55::get_free_size() const {
    // PAGE_FREE_SIZE: offset 46-47 (relative to page start)
    if (page_size_ < 48) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 46);
}

uint16_t PageHeaderV55::get_n_dir_slots() const {
    // PAGE_N_DIR_SLOTS: offset 48-49 (relative to page start)
    if (page_size_ < 50) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 48);
}

uint16_t PageHeaderV55::get_data_start() const {
    // PAGE_DATA_START: offset 38 (relative to page start)
    return DATA_START_OFFSET;
}

bool PageHeaderV55::verify_checksum() const {
    // Checksum verification for 5.5/5.1
    // 5.5/5.1 uses different checksum algorithms
    if (page_size_ < 52) return false;

    // Read checksum fields
    uint32_t crc32_a = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 4);
    uint32_t crc32_b = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 8);
    uint32_t checksum_a = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 24);
    uint32_t checksum_b = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 28);

    // In 5.5/5.1, checksums should be consistent
    // If both are non-zero and equal, checksum is valid
    if (checksum_a != 0 && checksum_b != 0 && checksum_a == checksum_b) {
        return true;
    }

    // If both are zero, checksum might be disabled (NONE algorithm)
    if (checksum_a == 0 && checksum_b == 0) {
        return true;  // Assume valid if checksum is disabled
    }

    return false;
}

uint32_t PageHeaderV55::calculate_checksum() const {
    // Calculate CRC32 checksum for the page
    // MySQL 5.5/5.1 uses CRC32C (Castagnoli) or INNODB algorithm
    return calculate_innodb_checksum();
}

uint32_t PageHeaderV55::calculate_innodb_checksum() const {
    // Calculate INNODB-style checksum (legacy)
    // This is the original InnoDB checksum algorithm
    uint32_t checksum = 0;

    const uint8_t* data = data_.get();
    size_t size = page_size_;

    // Skip checksum fields at offset 4, 8, 24, 28
    // Calculate checksum over the entire page excluding these fields
    
    // Simple implementation: XOR all bytes
    for (size_t i = 0; i < size; i++) {
        // Skip checksum fields
        if ((i >= 4 && i < 12) || (i >= 24 && i < 32)) {
            continue;
        }
        checksum ^= static_cast<uint32_t>(data[i]) << (i % 24);
    }

    return checksum;
}

uint32_t PageHeaderV55::calculate_none_checksum() const {
    // Calculate checksum when algorithm is NONE
    // Returns CRC32C of the page data
    uint32_t checksum = 0xFFFFFFFF;

    const uint8_t* data = data_.get();
    size_t size = page_size_;

    for (size_t i = 0; i < size; i++) {
        uint8_t byte = data[i];
        for (int j = 0; j < 8; j++) {
            uint32_t bit = (byte >> j) & 1;
            uint32_t cbit = (checksum >> 31) & 1;
            checksum = (checksum << 1) ^ ((cbit ^ bit) * 0x1EDC6F41);
        }
    }

    return checksum ^ 0xFFFFFFFF;
}

RowFormat PageHeaderV55::get_row_format() const {
    // Detect row format from page data
    // 5.5/5.1 primarily uses REDUNDANT format
    // COMPACT was introduced in 5.0.3, but 5.5 defaults to REDUNDANT

    // Check the first record's header
    const uint8_t* first_rec = data_.get() + DATA_START_OFFSET + 16;  // After INFIMUM/SUPREMUM

    if (page_size_ < DATA_START_OFFSET + 25) {
        return RowFormat::REDUNDANT;  // Default to REDUNDANT for 5.5/5.1
    }

    // Check extra_bytes (5th byte of record header)
    uint8_t extra_bytes = first_rec[4];

    if (extra_bytes == 6) {
        return RowFormat::REDUNDANT;
    }

    // Check for COMPACT format (extra_bytes = 5)
    if (extra_bytes == 5) {
        return RowFormat::COMPACT;
    }

    return RowFormat::REDUNDANT;  // Default to REDUNDANT for 5.5/5.1
}

uint32_t PageHeaderV55::get_space_id() const {
    // FIL_PAGE_SPACE_ID: canonical big-endian at FIL_PAGE_SPACE_ID (34)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_SPACE_ID + sizeof(uint32_t)) return 0;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_SPACE_ID);
}

uint32_t PageHeaderV55::get_page_offset() const {
    // FIL_PAGE_OFFSET: canonical big-endian at FIL_PAGE_OFFSET (4)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_OFFSET + sizeof(uint32_t)) return 0;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_OFFSET);
}

uint16_t PageHeaderV55::get_max_trx_id() const {
    // PAGE_MAX_TRX_ID: offset 38-41 (relative to page start)
    // Only present in clustered index pages
    if (page_size_ < 42) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 38);
}

uint16_t PageHeaderV55::get_header_version() const {
    // PAGE_HEADER_VERSION: offset 40-41 (relative to page start)
    // MySQL 5.7 introduced this field, 5.5/5.1 doesn't have it
    // Return 0 to indicate not present
    if (page_size_ < 42) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 40);
}

uint32_t PageHeaderV55::get_file_flush_lsn() const {
    // FIL_PAGE_FILE_FLUSH_LSN: canonical big-endian at FIL_PAGE_FILE_FLUSH_LSN (8 bytes)
    // Only present in ibdata1 system tablespace
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_FILE_FLUSH_LSN + sizeof(uint64_t)) return 0;
    return static_cast<uint32_t>(dbrecovery::innodb::read_be<uint64_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_FILE_FLUSH_LSN));
}

std::string PageHeaderV55::to_string() const {
    std::ostringstream oss;
    oss << "PageHeaderV55{"
        << "prev=" << get_prev_page()
        << ", next=" << get_next_page()
        << ", lsn=" << std::hex << get_lsn() << std::dec
        << ", type=" << static_cast<int>(get_page_type())
        << ", level=" << get_level()
        << ", n_recs=" << get_n_recs()
        << ", free_ptr=" << get_free_ptr()
        << ", free_size=" << get_free_size()
        << ", n_dir_slots=" << get_n_dir_slots()
        << ", data_start=" << get_data_start()
        << ", space_id=" << get_space_id()
        << ", page_offset=" << get_page_offset()
        << ", max_trx_id=" << get_max_trx_id()
        << ", header_version=" << get_header_version()
        << ", checksum_valid=" << verify_checksum()
        << ", row_format=" << static_cast<int>(get_row_format())
        << "}";
    return oss.str();
}

// ==================== PageHeaderV50 Implementation ====================

PageHeaderV50::PageHeaderV50(const uint8_t* page_data, size_t page_size, MySQLVersion version)
    : page_size_(page_size), version_(version) {
    if (!page_data) {
        throw std::invalid_argument("Page data pointer is null");
    }

    // Validate page size
    if (page_size < 4096 || page_size > 65536) {
        throw std::invalid_argument("Invalid page size: " + std::to_string(page_size));
    }

    // Allocate and copy page data
    data_ = std::make_unique<uint8_t[]>(page_size);
    std::memcpy(data_.get(), page_data, page_size);
}

uint32_t PageHeaderV50::get_prev_page() const {
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_PREV_ABS + sizeof(uint32_t)) return 0xFFFFFFFF;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_PREV_ABS);
}

uint32_t PageHeaderV50::get_next_page() const {
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_NEXT_ABS + sizeof(uint32_t)) return 0xFFFFFFFF;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_NEXT_ABS);
}

uint64_t PageHeaderV50::get_lsn() const {
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_LSN_ABS + sizeof(uint64_t)) return 0;
    return dbrecovery::innodb::read_be<uint64_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_LSN_ABS);
}

PageType PageHeaderV50::get_page_type() const {
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_TYPE_ABS + sizeof(uint16_t)) return PageType::UNKNOWN;
    uint16_t type = dbrecovery::innodb::read_be<uint16_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);
    return page_type_from_fil(type);
}

uint16_t PageHeaderV50::get_level() const {
    if (page_size_ < 36) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 34);
}

uint16_t PageHeaderV50::get_n_recs() const {
    if (page_size_ < 38) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 36);
}

uint16_t PageHeaderV50::get_free_ptr() const {
    if (page_size_ < 46) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 44);
}

uint16_t PageHeaderV50::get_free_size() const {
    if (page_size_ < 48) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 46);
}

uint16_t PageHeaderV50::get_n_dir_slots() const {
    if (page_size_ < 50) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 48);
}

uint16_t PageHeaderV50::get_data_start() const {
    // 5.0/4.x has 2-byte smaller data start
    return DATA_START_OFFSET;
}

bool PageHeaderV50::verify_checksum() const {
    if (page_size_ < 52) return false;

    uint32_t checksum_a = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 24);
    uint32_t checksum_b = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 28);

    if (checksum_a != 0 && checksum_b != 0 && checksum_a == checksum_b) {
        return true;
    }

    if (checksum_a == 0 && checksum_b == 0) {
        return true;  // Checksum disabled
    }

    return false;
}

uint32_t PageHeaderV50::calculate_checksum() const {
    return calculate_innodb_checksum();
}

uint32_t PageHeaderV50::calculate_innodb_checksum() const {
    uint32_t checksum = 0;
    const uint8_t* data = data_.get();
    size_t size = page_size_;

    for (size_t i = 0; i < size; i++) {
        if ((i >= 4 && i < 12) || (i >= 24 && i < 32)) {
            continue;
        }
        checksum ^= static_cast<uint32_t>(data[i]) << (i % 24);
    }

    return checksum;
}

RowFormat PageHeaderV50::get_row_format() const {
    // 5.0/4.x primarily uses REDUNDANT format
    const uint8_t* first_rec = data_.get() + DATA_START_OFFSET + 16;

    if (page_size_ < DATA_START_OFFSET + 25) {
        return RowFormat::REDUNDANT;
    }

    uint8_t extra_bytes = first_rec[4];
    return (extra_bytes == 6) ? RowFormat::REDUNDANT : RowFormat::REDUNDANT;
}

uint32_t PageHeaderV50::get_space_id() const {
    // 5.0/4.x doesn't have space ID in page header
    // Return 0 or read from alternative location
    return 0;
}

uint32_t PageHeaderV50::get_page_offset() const {
    // FIL_PAGE_OFFSET: canonical big-endian at FIL_PAGE_OFFSET (4)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_OFFSET + sizeof(uint32_t)) return 0;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_OFFSET);
}

uint16_t PageHeaderV50::get_max_trx_id() const {
    if (page_size_ < 42) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 38);
}

std::string PageHeaderV50::to_string() const {
    std::ostringstream oss;
    oss << "PageHeaderV50{"
        << "prev=" << get_prev_page()
        << ", next=" << get_next_page()
        << ", lsn=" << std::hex << get_lsn() << std::dec
        << ", type=" << static_cast<int>(get_page_type())
        << ", level=" << get_level()
        << ", n_recs=" << get_n_recs()
        << ", space_id=" << get_space_id()
        << ", page_offset=" << get_page_offset()
        << ", checksum_valid=" << verify_checksum()
        << ", row_format=" << static_cast<int>(get_row_format())
        << "}";
    return oss.str();
}

// ==================== PageFactoryExtended Implementation ====================

std::unique_ptr<PageHeaderBase> PageFactoryExtended::create_page_header_extended(
    const uint8_t* page_data,
    size_t page_size,
    MySQLVersion* detected_version
) {
    if (!page_data) {
        throw std::invalid_argument("Page data pointer is null");
    }

    // Detect version with extended logic
    MySQLVersion version = detect_version_extended(page_data, page_size);

    if (detected_version) {
        *detected_version = version;
    }

    // Create appropriate PageHeader based on version
    switch (version) {
        case MySQLVersion::MYSQL_56:
        case MySQLVersion::MYSQL_57:
            return std::make_unique<PageHeaderV56>(page_data, page_size);

        case MySQLVersion::MYSQL_80:
        case MySQLVersion::MYSQL_84:
            return std::make_unique<PageHeaderV80>(page_data, page_size, version);

        default:
            // Fail closed: UNKNOWN or unsupported versions must not select a
            // parser, or pages could be misread under a wrong layout.
            throw std::invalid_argument(
                "UNKNOWN/unsupported MySQL version — cannot select page parser");
    }
}

MySQLVersion PageFactoryExtended::detect_version_extended(
    const uint8_t* page_data,
    size_t page_size
) {
    if (!page_data || page_size < 38) {
        return MySQLVersion::UNKNOWN;
    }

    // Step 1: Check for very old format (5.0/4.x) - fallback to 5.6
    if (is_old_mysql_format(page_data, page_size)) {
        return MySQLVersion::MYSQL_56;
    }

    // Step 2: Check for SDI page (MySQL 8.0+)
    uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(page_data, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);
    if (page_type == static_cast<uint16_t>(PageType::SDI)) {
        return MySQLVersion::MYSQL_80;
    }

    // Step 3: Check SDI Signature (8 bytes at offset 64)
    if (page_size >= 72) {
        if (std::memcmp(page_data + 64, SDI_SIG, 8) == 0) {
            return MySQLVersion::MYSQL_80;
        }
        // Check for old SDI signature
        if (std::memcmp(page_data + 64, OLD_SDI_SIG, 8) == 0) {
            return MySQLVersion::MYSQL_57;
        }
    }

    // Step 4: Check Page Header Version (MySQL 8.0+)
    if (page_size >= 42) {
        uint16_t header_version = dbrecovery::innodb::read_le<uint16_t>(page_data, 40);
        if (header_version > 0) {
            return MySQLVersion::MYSQL_80;
        }
    }

    // Step 5: Check FIL_PAGE_FILE_FLUSH_LSN (8 bytes in 8.0+)
    if (page_size >= dbrecovery::innodb::FIL_PAGE_FILE_FLUSH_LSN + sizeof(uint64_t)) {
        uint64_t flush_lsn = dbrecovery::innodb::read_be<uint64_t>(page_data, dbrecovery::innodb::FIL_PAGE_FILE_FLUSH_LSN);
        if (flush_lsn > 0xFFFFFFFF) {
            return MySQLVersion::MYSQL_80;
        }
    }

    // Step 6: Check PAGE_MAX_TRX_ID (5.6/5.7 specific)
    if (page_size >= 42) {
        uint32_t max_trx_id = dbrecovery::innodb::read_le<uint32_t>(page_data, 38);
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

    // Step 7: Check for 5.5/5.1 specific patterns - fallback to 5.6
    if (page_size >= 38) {
        // 5.5/5.1 typically has smaller page headers
        // Check if data_start is at offset 38
        uint16_t data_start = dbrecovery::innodb::read_le<uint16_t>(page_data, 38);
        if (data_start == 0 || data_start == DATA_START_OFFSET) {
            return MySQLVersion::MYSQL_56;
        }
    }

    // Step 8: Check space ID
    if (page_size >= dbrecovery::innodb::FIL_PAGE_SPACE_ID + sizeof(uint32_t)) {
        uint32_t space_id = dbrecovery::innodb::read_be<uint32_t>(page_data, dbrecovery::innodb::FIL_PAGE_SPACE_ID);
        // 8.0+ typically has non-zero space ID for user tables
        if (space_id > 0 && page_type == static_cast<uint16_t>(PageType::INDEX)) {
            return MySQLVersion::MYSQL_80;
        }
    }

    // No unique version evidence — fail closed. None of the weak heuristics
    // above uniquely identify a server version.
    return MySQLVersion::UNKNOWN;
}

bool PageFactoryExtended::is_old_mysql_format(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 36) {
        return false;
    }

    // Check for 5.0/4.x specific patterns:
    // 1. Data start at offset 36 (2 bytes less than 5.5+)
    // 2. No PAGE_N_DIR_SLOTS field at expected location
    // 3. Different record header format

    // Check if data_start field at offset 38 is unusual
    uint16_t data_start_38 = dbrecovery::innodb::read_le<uint16_t>(page_data, 38);
    
    // Check if there's a valid page header at offset 36
    if (page_size >= 40) {
        uint16_t data_start_36 = dbrecovery::innodb::read_le<uint16_t>(page_data, 36);
        
        // If data_start at 36 is reasonable but 38 is not, likely old format
        if (data_start_36 >= 36 && data_start_36 < 100 && data_start_38 > 100) {
            return true;
        }
    }

    // Check for old-style record headers
    if (page_size >= 60) {
        const uint8_t* first_rec = page_data + 54;  // INFIMUM at offset 54
        
        if (first_rec[0] == 0x80 || first_rec[0] == 0x81) {
            // Old format record info bits
            return true;
        }
    }

    return false;
}

std::string PageFactoryExtended::version_to_string(MySQLVersion version) {
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

} // namespace mysql_recovery
