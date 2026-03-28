#include "dbrecovery/repair/header_repairer.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <cstring>
#include <iostream>

namespace dbrecovery {
namespace repair {

bool HeaderRepairer::repair_page_header(uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 54) { // FIL_HEADER(38) + minimal PAGE_HEADER(16)
        last_error_ = "Invalid page size";
        return false;
    }

    // Step 1: 验证页头有效性
    if (!validate_page_header(page_data, page_size)) {
        // Step 2: 尝试重建页头
        if (!rebuild_page_header(page_data, page_size, 0, dbrecovery::innodb::FIL_PAGE_INDEX)) {
            return false;
        }
    }

    // Step 3: 修复校验和
    calculate_and_fix_checksum(page_data, page_size);

    return true;
}

uint32_t HeaderRepairer::calculate_and_fix_checksum(uint8_t* page_data, size_t page_size) {
    // 计算 CRC32C 校验和（排除校验和字段）
    std::vector<uint8_t> page_copy(page_size);
    std::memcpy(page_copy.data(), page_data, page_size);
    
    // 清零校验和字段（偏移 24-27）
    std::memset(page_copy.data() + 24, 0, 4);
    std::memset(page_copy.data() + 28, 0, 4);

    uint32_t computed_crc = dbrecovery::innodb::PageReader::compute_crc32c(page_copy);

    // 写入校验和字段（偏移 24-27）
    for (int i = 0; i < 4; ++i) {
        page_data[24 + i] = (computed_crc >> (i * 8)) & 0xFF;
    }

    return computed_crc;
}

bool HeaderRepairer::validate_page_header(const uint8_t* page_data, size_t page_size) const {
    if (!page_data || page_size < 54) {
        return false;
    }

    // 检查页类型
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data, 38 + 12);
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX &&
        page_type != dbrecovery::innodb::FIL_PAGE_SDI &&
        page_type != dbrecovery::innodb::FIL_PAGE_UNDO_LOG &&
        page_type != dbrecovery::innodb::FIL_PAGE_TYPE_BLOB) {
        return false;
    }

    // 检查页号
    uint32_t page_no = dbrecovery::innodb::read_le<uint32_t>(page_data, 20);
    if (page_no >= 0xFFFFFFF0) {
        return false;
    }

    // 检查校验和字段
    uint32_t checksum = dbrecovery::innodb::read_le<uint32_t>(page_data, 24);
    if (checksum == 0xFFFFFFFF) {
        // NONE 校验模式，有效
        return true;
    }

    return true;
}

std::optional<uint16_t> HeaderRepairer::extract_page_type(const uint8_t* page_data, size_t page_size) const {
    if (!page_data || page_size < 50) {
        return std::nullopt;
    }

    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data, 38 + 12);
    
    // 验证页类型是否有效
    if (page_type == dbrecovery::innodb::FIL_PAGE_INDEX ||
        page_type == dbrecovery::innodb::FIL_PAGE_SDI ||
        page_type == dbrecovery::innodb::FIL_PAGE_UNDO_LOG ||
        page_type == dbrecovery::innodb::FIL_PAGE_TYPE_BLOB ||
        page_type == dbrecovery::innodb::FIL_PAGE_TYPE_ALLOCATED) {
        return page_type;
    }

    return std::nullopt;
}

bool HeaderRepairer::rebuild_page_header(uint8_t* page_data, size_t page_size,
                                         uint32_t page_no, uint16_t page_type) {
    if (!page_data || page_size < 54) {
        return false;
    }

    // Step 1: 重建 FIL Header
    // prev_page (offset 0-3)
    dbrecovery::innodb::write_le<uint32_t>(page_data, 0, 0xFFFFFFFF);
    
    // next_page (offset 4-7)
    dbrecovery::innodb::write_le<uint32_t>(page_data, 4, 0xFFFFFFFF);
    
    // LSN (offset 8-15)
    dbrecovery::innodb::write_le<uint64_t>(page_data, 8, 0);
    
    // space_id (offset 16-19)
    dbrecovery::innodb::write_le<uint32_t>(page_data, 16, 0);
    
    // page_no (offset 20-23)
    dbrecovery::innodb::write_le<uint32_t>(page_data, 20, page_no);
    
    // checksum (offset 24-27) - will be fixed later
    dbrecovery::innodb::write_le<uint32_t>(page_data, 24, 0);
    dbrecovery::innodb::write_le<uint32_t>(page_data, 28, 0);
    
    // type (offset 38+12 = 50)
    dbrecovery::innodb::write_le<uint16_t>(page_data, 50, page_type);

    // Step 2: 重建 Page Header
    uint8_t* page_header = page_data + 38;
    
    // n_dir_slots (offset 0)
    dbrecovery::innodb::write_le<uint16_t>(page_header, 0, 2);
    
    // heap_top (offset 2)
    dbrecovery::innodb::write_le<uint16_t>(page_header, 2, 54 + 13 + 8); // INFIMUM(13) + dummy record(8)
    
    // n_heap (offset 4)
    dbrecovery::innodb::write_le<uint16_t>(page_header, 4, 2 | 0x8000); // INFIMUM + SUPREMUM
    
    // free_offset (offset 6)
    dbrecovery::innodb::write_le<uint16_t>(page_header, 6, 54);

    return true;
}

} // namespace repair
} // namespace dbrecovery
