#include "page/page_parser.h"
#include "dbrecovery/innodb/mach_read.hpp"
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace mysql_recovery {

// ==================== PageHeaderV80 实现 ====================

PageHeaderV80::PageHeaderV80(
    const uint8_t* page_data,
    size_t page_size,
    MySQLVersion version
) : page_size_(page_size), version_(version) {
    if (!page_data) {
        throw std::invalid_argument("Page data pointer is null");
    }
    
    // 验证页大小
    if (page_size < 4096 || page_size > 65536) {
        throw std::invalid_argument("Invalid page size: " + std::to_string(page_size));
    }
    
    // 分配并拷贝页数据
    data_ = std::make_unique<uint8_t[]>(page_size);
    std::memcpy(data_.get(), page_data, page_size);
}

uint32_t PageHeaderV80::get_prev_page() const {
    // FIL_PAGE_PREV: canonical big-endian at FIL_PAGE_PREV_ABS (8)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_PREV_ABS + sizeof(uint32_t)) return 0xFFFFFFFF;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_PREV_ABS);
}

uint32_t PageHeaderV80::get_next_page() const {
    // FIL_PAGE_NEXT: canonical big-endian at FIL_PAGE_NEXT_ABS (12)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_NEXT_ABS + sizeof(uint32_t)) return 0xFFFFFFFF;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_NEXT_ABS);
}

uint64_t PageHeaderV80::get_lsn() const {
    // FIL_PAGE_LSN: canonical big-endian at FIL_PAGE_LSN_ABS (16, 8 bytes)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_LSN_ABS + sizeof(uint64_t)) return 0;
    return dbrecovery::innodb::read_be<uint64_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_LSN_ABS);
}

PageType PageHeaderV80::get_page_type() const {
    // FIL_PAGE_TYPE: canonical big-endian at FIL_PAGE_TYPE_ABS (24, 2 bytes)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_TYPE_ABS + sizeof(uint16_t)) return PageType::UNKNOWN;
    uint16_t type = dbrecovery::innodb::read_be<uint16_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);
    return page_type_from_fil(type);
}

uint16_t PageHeaderV80::get_level() const {
    // PAGE_LEVEL: offset 34-35 (relative to page start)
    if (page_size_ < 36) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 34);
}

uint16_t PageHeaderV80::get_n_recs() const {
    // PAGE_N_RECS: offset 36-37 (relative to page start)
    if (page_size_ < 38) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 36);
}

uint16_t PageHeaderV80::get_free_ptr() const {
    // PAGE_FREE_PTR: offset 44-45 (relative to page start)
    if (page_size_ < 46) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 44);
}

uint16_t PageHeaderV80::get_free_size() const {
    // PAGE_FREE_SIZE: offset 46-47 (relative to page start)
    if (page_size_ < 48) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 46);
}

uint16_t PageHeaderV80::get_n_dir_slots() const {
    // PAGE_N_DIR_SLOTS: offset 48-49 (relative to page start)
    if (page_size_ < 50) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 48);
}

uint16_t PageHeaderV80::get_data_start() const {
    // PAGE_DATA_START: offset 38 (relative to page start)
    return DATA_START_OFFSET;
}

bool PageHeaderV80::verify_checksum() const {
    // Checksum verification for 8.0+
    // 8.0+ uses stricter checksum validation
    if (page_size_ < 52) return false;
    
    uint32_t checksum_a = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 24);
    uint32_t checksum_b = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 28);
    
    // In 8.0+, checksum fields should be consistent
    if (checksum_a != 0 && checksum_b != 0 && checksum_a != checksum_b) {
        return false;
    }
    
    return true;
}

uint32_t PageHeaderV80::calculate_checksum() const {
    // Calculate CRC32C checksum for the page
    // MySQL 8.0+ uses CRC32C (Castagnoli)
    uint32_t checksum = 0xFFFFFFFF;  // Initial value for CRC32C
    
    const uint8_t* data = data_.get();
    size_t size = page_size_;
    
    // CRC32C calculation (simplified)
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

RowFormat PageHeaderV80::get_row_format() const {
    // Detect row format from page data
    // 8.0+ supports COMPACT, DYNAMIC, and COMPRESSED
    
    // Check the first record's header
    const uint8_t* first_rec = data_.get() + DATA_START_OFFSET + 16;
    
    if (page_size_ < DATA_START_OFFSET + 25) {
        return RowFormat::DYNAMIC;  // Default to DYNAMIC for 8.0+
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

uint32_t PageHeaderV80::get_space_id() const {
    // FIL_PAGE_SPACE_ID: canonical big-endian at FIL_PAGE_SPACE_ID (34)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_SPACE_ID + sizeof(uint32_t)) return 0;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_SPACE_ID);
}

uint32_t PageHeaderV80::get_page_offset() const {
    // FIL_PAGE_OFFSET: canonical big-endian at FIL_PAGE_OFFSET (4)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_OFFSET + sizeof(uint32_t)) return 0;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_OFFSET);
}

uint16_t PageHeaderV80::get_max_trx_id() const {
    // In 8.0+, max_trx_id is moved to TRX_SYS page
    // Return 0 for non-clustered index pages
    return 0;
}

uint64_t PageHeaderV80::get_file_flush_lsn() const {
    // FIL_PAGE_FILE_FLUSH_LSN: offset 24-31 (8 bytes)
    // In 8.0+, this is extended to 8 bytes
    if (page_size_ < 32) return 0;
    return dbrecovery::innodb::read_le<uint64_t>(data_.get(), 24);
}

uint16_t PageHeaderV80::get_header_version() const {
    // PAGE_HEADER_VERSION: offset 40-41 (relative to page start)
    // MySQL 8.0 introduced this field
    if (page_size_ < 42) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 40);
}

bool PageHeaderV80::is_sdi_page() const {
    // Check if this is an SDI page
    PageType type = get_page_type();
    return type == PageType::SDI;
}

bool PageHeaderV80::is_compressed() const {
    // Check if page is compressed (8.0+)
    if (page_size_ < 26) return false;
    uint16_t flags = dbrecovery::innodb::read_le<uint16_t>(data_.get(), 24);
    
    // Check if compressed bit is set (bit 0)
    return (flags & 0x0001) != 0;
}

uint16_t PageHeaderV80::get_zip_size() const {
    // Get compressed page size
    if (!is_compressed()) return 0;
    
    if (page_size_ < 26) return 0;
    uint16_t flags = dbrecovery::innodb::read_le<uint16_t>(data_.get(), 24);
    
    // zip_size is stored in bits 0-11 of flags
    return flags & 0x0FFF;
}

std::string PageHeaderV80::to_string() const {
    std::ostringstream oss;
    oss << "PageHeaderV80{"
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
        << ", file_flush_lsn=" << std::hex << get_file_flush_lsn() << std::dec
        << ", header_version=" << get_header_version()
        << ", is_sdi=" << (is_sdi_page() ? "true" : "false")
        << ", is_compressed=" << (is_compressed() ? "true" : "false")
        << ", zip_size=" << get_zip_size()
        << ", checksum_valid=" << verify_checksum()
        << "}";
    return oss.str();
}

} // namespace mysql_recovery
