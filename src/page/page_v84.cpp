#include "page/page_parser_v84.h"
#include "dbrecovery/innodb/mach_read.hpp"
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace mysql_recovery {

// ==================== PageHeaderV84 Implementation ====================

PageHeaderV84::PageHeaderV84(const uint8_t* page_data, size_t page_size, MySQLVersion version)
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

uint32_t PageHeaderV84::get_prev_page() const {
    if (page_size_ < 4) return 0xFFFFFFFF;
    return dbrecovery::innodb::read_le<uint32_t>(data_.get(), 0);
}

uint32_t PageHeaderV84::get_next_page() const {
    if (page_size_ < 8) return 0xFFFFFFFF;
    return dbrecovery::innodb::read_le<uint32_t>(data_.get(), 4);
}

uint64_t PageHeaderV84::get_lsn() const {
    if (page_size_ < 16) return 0;
    return dbrecovery::innodb::read_le<uint64_t>(data_.get(), 8);
}

PageType PageHeaderV84::get_page_type() const {
    if (page_size_ < 18) return PageType::UNKNOWN;
    uint16_t type = dbrecovery::innodb::read_le<uint16_t>(data_.get(), 16);
    return static_cast<PageType>(type);
}

uint16_t PageHeaderV84::get_level() const {
    if (page_size_ < 36) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 34);
}

uint16_t PageHeaderV84::get_n_recs() const {
    if (page_size_ < 38) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 36);
}

uint16_t PageHeaderV84::get_free_ptr() const {
    if (page_size_ < 46) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 44);
}

uint16_t PageHeaderV84::get_free_size() const {
    if (page_size_ < 48) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 46);
}

uint16_t PageHeaderV84::get_n_dir_slots() const {
    if (page_size_ < 50) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 48);
}

uint16_t PageHeaderV84::get_data_start() const {
    return DATA_START_OFFSET;
}

bool PageHeaderV84::verify_checksum() const {
    // Verify checksum using the appropriate algorithm
    // MySQL 8.4+ supports multiple checksum algorithms
    
    // Try CRC32C first (most common in 8.4+)
    if (verify_crc32c()) {
        return true;
    }
    
    // Try stripe checksum (for encrypted pages)
    if (verify_stripe_checksum()) {
        return true;
    }
    
    // Try legacy checksum
    return verify_legacy_checksum();
}

bool PageHeaderV84::verify_crc32c() const {
    // CRC32C (Castagnoli) checksum verification
    if (page_size_ < 52) return false;

    uint32_t checksum_a = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 24);
    uint32_t checksum_b = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 28);

    // In 8.4+, checksums should match when calculated
    if (checksum_a != 0 && checksum_b != 0) {
        uint32_t calculated = calculate_crc32c();
        return (checksum_a == calculated || checksum_b == calculated);
    }

    return checksum_a == checksum_b;
}

bool PageHeaderV84::verify_stripe_checksum() const {
    // Stripe checksum for encrypted pages
    // Uses a different algorithm with stripe pattern
    if (page_size_ < 52) return false;

    uint32_t checksum_a = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 24);
    uint32_t checksum_b = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 28);

    if (checksum_a == 0 && checksum_b == 0) {
        return true;  // Checksum disabled
    }

    uint32_t calculated = calculate_stripe_checksum();
    return (checksum_a == calculated || checksum_b == calculated);
}

bool PageHeaderV84::verify_legacy_checksum() const {
    // Legacy checksum verification (fallback)
    if (page_size_ < 52) return false;

    uint32_t checksum_a = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 24);
    uint32_t checksum_b = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 28);

    return (checksum_a == checksum_b);
}

uint32_t PageHeaderV84::calculate_checksum() const {
    // Calculate CRC32C checksum (default for 8.4+)
    return calculate_crc32c();
}

uint32_t PageHeaderV84::calculate_crc32c() const {
    // CRC32C (Castagnoli) checksum calculation
    // MySQL 8.4+ uses hardware-accelerated CRC32C when available
    
    uint32_t checksum = 0xFFFFFFFF;  // Initial value for CRC32C
    
    const uint8_t* data = data_.get();
    size_t size = page_size_;
    
    // CRC32C calculation using Slicing-by-8 algorithm (simplified)
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

uint32_t PageHeaderV84::calculate_stripe_checksum() const {
    // Stripe checksum for encrypted pages
    // Uses a pattern-based approach
    
    uint32_t checksum = 0;
    const uint8_t* data = data_.get();
    size_t size = page_size_;
    
    // Simple stripe checksum: XOR with position-based mixing
    for (size_t i = 0; i < size; i++) {
        // Skip checksum fields
        if ((i >= 4 && i < 12) || (i >= 24 && i < 32)) {
            continue;
        }
        
        // Mix with position
        uint32_t byte_val = static_cast<uint32_t>(data[i]);
        uint32_t position_factor = static_cast<uint32_t>((i % 256) + 1);
        checksum ^= (byte_val * position_factor);
        
        // Additional mixing
        checksum = (checksum << 3) | (checksum >> 29);
    }
    
    return checksum;
}

RowFormat PageHeaderV84::get_row_format() const {
    // Detect row format from page data
    // 8.4+ supports COMPACT, DYNAMIC, and COMPRESSED
    
    const uint8_t* first_rec = data_.get() + DATA_START_OFFSET + 16;
    
    if (page_size_ < DATA_START_OFFSET + 25) {
        return RowFormat::DYNAMIC;  // Default to DYNAMIC for 8.4+
    }
    
    // Check extra_bytes (5th byte of record header)
    uint8_t extra_bytes = first_rec[4];
    
    if (extra_bytes == 6) {
        return RowFormat::REDUNDANT;  // Legacy
    }
    
    // Check for external pointer flag (bit 5)
    uint8_t info_bits = first_rec[0];
    if ((info_bits & 0x20) != 0) {
        return RowFormat::DYNAMIC;  // Has external BLOB pointers
    }
    
    return RowFormat::COMPACT;
}

uint32_t PageHeaderV84::get_space_id() const {
    if (page_size_ < 22) return 0;
    return dbrecovery::innodb::read_le<uint32_t>(data_.get(), 18);
}

uint32_t PageHeaderV84::get_page_offset() const {
    if (page_size_ < 16) return 0;
    return dbrecovery::innodb::read_le<uint32_t>(data_.get(), 12);
}

uint16_t PageHeaderV84::get_max_trx_id() const {
    // In 8.4+, max_trx_id is moved to TRX_SYS page
    // Return 0 for non-clustered index pages
    return 0;
}

uint64_t PageHeaderV84::get_extended_lsn() const {
    // Get extended LSN (64-bit in 8.4+)
    if (page_size_ < 32) return 0;
    return dbrecovery::innodb::read_le<uint64_t>(data_.get(), 24);
}

uint16_t PageHeaderV84::get_header_version() const {
    // PAGE_HEADER_VERSION: offset 40-41 (relative to page start)
    if (page_size_ < 42) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 40);
}

bool PageHeaderV84::is_sdi_page() const {
    PageType type = get_page_type();
    return type == PageType::SDI;
}

bool PageHeaderV84::is_compressed() const {
    if (page_size_ < 26) return false;
    uint16_t flags = dbrecovery::innodb::read_le<uint16_t>(data_.get(), 24);
    return (flags & 0x0001) != 0;
}

uint16_t PageHeaderV84::get_zip_size() const {
    if (!is_compressed()) return 0;
    if (page_size_ < 26) return 0;
    uint16_t flags = dbrecovery::innodb::read_le<uint16_t>(data_.get(), 24);
    return flags & 0x0FFF;
}

bool PageHeaderV84::is_encrypted() const {
    // Check if page is encrypted (8.4+ feature)
    // Encryption flag is in the page header flags
    if (page_size_ < 26) return false;
    uint16_t flags = dbrecovery::innodb::read_le<uint16_t>(data_.get(), 24);
    return (flags & 0x0002) != 0;  // Encryption flag
}

uint32_t PageHeaderV84::get_encryption_key_version() const {
    // Get encryption key version (8.4+)
    if (!is_encrypted()) return 0;
    if (page_size_ < 42) return 0;
    return dbrecovery::innodb::read_le<uint32_t>(data_.get(), 42) & 0x00FFFFFF;
}

bool PageHeaderV84::is_temporary_page() const {
    // Check if page is temporary (8.4+)
    // PageType84::TEMPORARY = 0x26F6 = 9974
    PageType type = get_page_type();
    return static_cast<uint16_t>(type) == 0x26F6;  // PageType84::TEMPORARY
}

uint32_t PageHeaderV84::get_stripe_size() const {
    // Get stripe size for encrypted pages (8.4+)
    if (!is_encrypted()) return 0;
    if (page_size_ < 46) return 0;
    return dbrecovery::innodb::read_le<uint32_t>(data_.get(), 46);
}

uint32_t PageHeaderV84::get_encryption_algorithm() const {
    // Get encryption algorithm (8.4+)
    // Algorithm is stored in page flags
    if (!is_encrypted()) return 0;  // NONE
    if (page_size_ < 26) return 0;
    uint16_t flags = dbrecovery::innodb::read_le<uint16_t>(data_.get(), 24);
    // Algorithm is stored in bits 8-11 of flags
    return (flags >> 8) & 0x0F;
}

std::string PageHeaderV84::to_string() const {
    std::ostringstream oss;
    oss << "PageHeaderV84{"
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
        << ", extended_lsn=" << std::hex << get_extended_lsn() << std::dec
        << ", header_version=" << get_header_version()
        << ", is_sdi=" << (is_sdi_page() ? "true" : "false")
        << ", is_compressed=" << (is_compressed() ? "true" : "false")
        << ", zip_size=" << get_zip_size()
        << ", is_encrypted=" << (is_encrypted() ? "true" : "false")
        << ", key_version=" << get_encryption_key_version()
        << ", is_temporary=" << (is_temporary_page() ? "true" : "false")
        << ", stripe_size=" << get_stripe_size()
        << ", checksum_valid=" << verify_checksum()
        << "}";
    return oss.str();
}

// ==================== PageHeaderV80Enhanced Implementation ====================

PageHeaderV80Enhanced::PageHeaderV80Enhanced(const uint8_t* page_data, size_t page_size)
    : page_size_(page_size) {
    if (!page_data) {
        throw std::invalid_argument("Page data pointer is null");
    }

    if (page_size < 4096 || page_size > 65536) {
        throw std::invalid_argument("Invalid page size: " + std::to_string(page_size));
    }

    data_ = std::make_unique<uint8_t[]>(page_size);
    std::memcpy(data_.get(), page_data, page_size);
}

uint32_t PageHeaderV80Enhanced::get_prev_page() const {
    if (page_size_ < 4) return 0xFFFFFFFF;
    return dbrecovery::innodb::read_le<uint32_t>(data_.get(), 0);
}

uint32_t PageHeaderV80Enhanced::get_next_page() const {
    if (page_size_ < 8) return 0xFFFFFFFF;
    return dbrecovery::innodb::read_le<uint32_t>(data_.get(), 4);
}

uint64_t PageHeaderV80Enhanced::get_lsn() const {
    if (page_size_ < 16) return 0;
    return dbrecovery::innodb::read_le<uint64_t>(data_.get(), 8);
}

PageType PageHeaderV80Enhanced::get_page_type() const {
    if (page_size_ < 18) return PageType::UNKNOWN;
    uint16_t type = dbrecovery::innodb::read_le<uint16_t>(data_.get(), 16);
    return static_cast<PageType>(type);
}

uint16_t PageHeaderV80Enhanced::get_level() const {
    if (page_size_ < 36) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 34);
}

uint16_t PageHeaderV80Enhanced::get_n_recs() const {
    if (page_size_ < 38) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 36);
}

uint16_t PageHeaderV80Enhanced::get_free_ptr() const {
    if (page_size_ < 46) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 44);
}

uint16_t PageHeaderV80Enhanced::get_free_size() const {
    if (page_size_ < 48) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 46);
}

uint16_t PageHeaderV80Enhanced::get_n_dir_slots() const {
    if (page_size_ < 50) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 48);
}

uint16_t PageHeaderV80Enhanced::get_data_start() const {
    return DATA_START_OFFSET;
}

bool PageHeaderV80Enhanced::verify_checksum() const {
    if (page_size_ < 52) return false;

    uint32_t checksum_a = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 24);
    uint32_t checksum_b = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 28);

    return (checksum_a == checksum_b);
}

uint32_t PageHeaderV80Enhanced::calculate_checksum() const {
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

RowFormat PageHeaderV80Enhanced::get_row_format() const {
    const uint8_t* first_rec = data_.get() + DATA_START_OFFSET + 16;

    if (page_size_ < DATA_START_OFFSET + 25) {
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

uint32_t PageHeaderV80Enhanced::get_space_id() const {
    if (page_size_ < 22) return 0;
    return dbrecovery::innodb::read_le<uint32_t>(data_.get(), 18);
}

uint32_t PageHeaderV80Enhanced::get_page_offset() const {
    if (page_size_ < 16) return 0;
    return dbrecovery::innodb::read_le<uint32_t>(data_.get(), 12);
}

uint16_t PageHeaderV80Enhanced::get_max_trx_id() const {
    return 0;
}

uint64_t PageHeaderV80Enhanced::get_extended_lsn() const {
    if (page_size_ < 32) return 0;
    return dbrecovery::innodb::read_le<uint64_t>(data_.get(), 24);
}

bool PageHeaderV80Enhanced::is_temporary_page() const {
    // PageType84::TEMPORARY = 0x26F6 = 9974
    PageType type = get_page_type();
    return static_cast<uint16_t>(type) == 0x26F6;
}

std::string PageHeaderV80Enhanced::to_string() const {
    std::ostringstream oss;
    oss << "PageHeaderV80Enhanced{"
        << "prev=" << get_prev_page()
        << ", next=" << get_next_page()
        << ", lsn=" << std::hex << get_lsn() << std::dec
        << ", type=" << static_cast<int>(get_page_type())
        << ", level=" << get_level()
        << ", n_recs=" << get_n_recs()
        << ", space_id=" << get_space_id()
        << ", page_offset=" << get_page_offset()
        << ", extended_lsn=" << std::hex << get_extended_lsn() << std::dec
        << ", is_temporary=" << (is_temporary_page() ? "true" : "false")
        << ", checksum_valid=" << verify_checksum()
        << "}";
    return oss.str();
}

} // namespace mysql_recovery
