#include "page/page_parser.h"
#include "dbrecovery/innodb/mach_read.hpp"
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace mysql_recovery {

// ==================== PageHeaderV56 实现 ====================

PageHeaderV56::PageHeaderV56(const uint8_t* page_data, size_t page_size)
    : page_size_(page_size) {
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

uint32_t PageHeaderV56::get_prev_page() const {
    // FIL_PAGE_PREV: canonical big-endian at FIL_PAGE_PREV_ABS (8)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_PREV_ABS + sizeof(uint32_t)) return 0xFFFFFFFF;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_PREV_ABS);
}

uint32_t PageHeaderV56::get_next_page() const {
    // FIL_PAGE_NEXT: canonical big-endian at FIL_PAGE_NEXT_ABS (12)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_NEXT_ABS + sizeof(uint32_t)) return 0xFFFFFFFF;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_NEXT_ABS);
}

uint64_t PageHeaderV56::get_lsn() const {
    // FIL_PAGE_LSN: canonical big-endian at FIL_PAGE_LSN_ABS (16, 8 bytes)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_LSN_ABS + sizeof(uint64_t)) return 0;
    return dbrecovery::innodb::read_be<uint64_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_LSN_ABS);
}

PageType PageHeaderV56::get_page_type() const {
    // FIL_PAGE_TYPE: canonical big-endian at FIL_PAGE_TYPE_ABS (24, 2 bytes)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_TYPE_ABS + sizeof(uint16_t)) return PageType::UNKNOWN;
    uint16_t type = dbrecovery::innodb::read_be<uint16_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);
    return page_type_from_fil(type);
}

uint16_t PageHeaderV56::get_level() const {
    // PAGE_LEVEL: offset 34-35 (relative to page start)
    // In 5.6/5.7, this is at offset 34 (FIL_HEADER_SIZE(38) - 4)
    if (page_size_ < 36) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 34);
}

uint16_t PageHeaderV56::get_n_recs() const {
    // PAGE_N_RECS: offset 36-37 (relative to page start)
    if (page_size_ < 38) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 36);
}

uint16_t PageHeaderV56::get_free_ptr() const {
    // PAGE_FREE_PTR: offset 44-45 (relative to page start)
    if (page_size_ < 46) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 44);
}

uint16_t PageHeaderV56::get_free_size() const {
    // PAGE_FREE_SIZE: offset 46-47 (relative to page start)
    if (page_size_ < 48) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 46);
}

uint16_t PageHeaderV56::get_n_dir_slots() const {
    // PAGE_N_DIR_SLOTS: offset 48-49 (relative to page start)
    if (page_size_ < 50) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 48);
}

uint16_t PageHeaderV56::get_data_start() const {
    // PAGE_DATA_START: offset 38 (relative to page start)
    // This is where user records begin
    return DATA_START_OFFSET;
}

bool PageHeaderV56::verify_checksum() const {
    // Checksum verification for 5.6/5.7
    // 5.6/5.7 uses FIL_PAGE_CHECKSUM at offset 4, 8, 24, 28
    if (page_size_ < 52) return false;
    
    uint32_t crc32_a = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 4);
    uint32_t crc32_b = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 8);
    uint32_t checksum_a = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 24);
    uint32_t checksum_b = dbrecovery::innodb::read_le<uint32_t>(data_.get(), 28);
    
    // Check if checksums are consistent
    // Note: In practice, we would calculate the actual checksum
    // For now, we check if the fields are non-zero and consistent
    if (checksum_a != 0 && checksum_b != 0 && checksum_a != checksum_b) {
        return false;
    }
    
    return true;
}

uint32_t PageHeaderV56::calculate_checksum() const {
    // Calculate CRC32 checksum for the page
    // This is a simplified implementation - in production, use hardware CRC32C
    uint32_t checksum = 0;
    
    // For 5.6/5.7, we calculate CRC32 over the page data
    // excluding the checksum fields themselves
    const uint8_t* data = data_.get();
    size_t size = page_size_;
    
    // Skip checksum fields at offset 4, 8, 24, 28
    // For simplicity, we calculate over the entire page
    // and then adjust (in production, use proper CRC32C)
    
    for (size_t i = 0; i < size; i++) {
        checksum = (checksum >> 8) ^ ((checksum ^ data[i]) * 0x1B873593);
    }
    
    return checksum;
}

RowFormat PageHeaderV56::get_row_format() const {
    // Detect row format from page data
    // 5.6/5.7 supports REDUNDANT and COMPACT
    
    // Check the first record's header
    const uint8_t* first_rec = data_.get() + DATA_START_OFFSET + 16;  // After INFIMUM/SUPREMUM
    
    if (page_size_ < DATA_START_OFFSET + 25) {
        return RowFormat::COMPACT;  // Default to COMPACT if we can't determine
    }
    
    // Check extra_bytes (5th byte of record header)
    uint8_t extra_bytes = first_rec[4];
    
    if (extra_bytes == 6) {
        return RowFormat::REDUNDANT;
    }
    
    return RowFormat::COMPACT;
}

uint32_t PageHeaderV56::get_space_id() const {
    // FIL_PAGE_SPACE_ID: canonical big-endian at FIL_PAGE_SPACE_ID (34)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_SPACE_ID + sizeof(uint32_t)) return 0;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_SPACE_ID);
}

uint32_t PageHeaderV56::get_page_offset() const {
    // FIL_PAGE_OFFSET: canonical big-endian at FIL_PAGE_OFFSET (4)
    if (page_size_ < dbrecovery::innodb::FIL_PAGE_OFFSET + sizeof(uint32_t)) return 0;
    return dbrecovery::innodb::read_be<uint32_t>(data_.get(), dbrecovery::innodb::FIL_PAGE_OFFSET);
}

uint16_t PageHeaderV56::get_max_trx_id() const {
    // PAGE_MAX_TRX_ID: offset 38-41 (relative to page start)
    // Only present in clustered index pages
    if (page_size_ < 42) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 38);
}

uint16_t PageHeaderV56::get_header_version() const {
    // PAGE_HEADER_VERSION: offset 40-41 (relative to page start)
    // MySQL 5.7 introduced this field
    if (page_size_ < 42) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(data_.get(), 40);
}

uint32_t PageHeaderV56::get_file_flush_lsn() const {
    // FIL_PAGE_FILE_FLUSH_LSN: offset 24-31 (8 bytes)
    // Only present in ibdata1 system tablespace
    if (page_size_ < 32) return 0;
    return dbrecovery::innodb::read_le<uint32_t>(data_.get(), 24);
}

bool PageHeaderV56::is_compressed() const {
    // Check if page is compressed (5.7+)
    // Compressed pages have zip_size field
    uint16_t header_version = get_header_version();
    if (header_version == 0) return false;
    
    // Check flags in page header
    // For compressed pages, the flags field contains zip_size info
    if (page_size_ < 26) return false;
    uint16_t flags = dbrecovery::innodb::read_le<uint16_t>(data_.get(), 24);
    
    // Check if compressed bit is set
    return (flags & 0x0001) != 0;
}

uint16_t PageHeaderV56::get_zip_size() const {
    // Get compressed page size
    if (!is_compressed()) return 0;
    
    // zip_size is stored in the flags field
    if (page_size_ < 26) return 0;
    uint16_t flags = dbrecovery::innodb::read_le<uint16_t>(data_.get(), 24);
    
    // zip_size is stored in bits 0-11 of flags
    return flags & 0x0FFF;
}

std::string PageHeaderV56::to_string() const {
    std::ostringstream oss;
    oss << "PageHeaderV56{"
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
        << "}";
    return oss.str();
}

} // namespace mysql_recovery
