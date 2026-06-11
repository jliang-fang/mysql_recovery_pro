#include "dbrecovery/repair/header_repairer.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <cstring>
#include <iostream>

namespace dbrecovery {
namespace repair {

bool HeaderRepairer::repair_page_header(uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 74) { // FIL_HEADER(38) + PAGE_HEADER(36)
        last_error_ = "Invalid page size";
        return false;
    }

    // Step 1: 检查页头是否已经有效
    if (validate_page_header(page_data, page_size)) {
        // 页面已经有效，不需要修复
        return false;  // Return false to indicate no repair was needed
    }

    // Step 2: 检查页面类型字节
    uint16_t current_type = dbrecovery::innodb::read_be<uint16_t>(page_data, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

    // Check if this looks like it might have been an INDEX page
    // Read index_id from PAGE_HEADER (offset 70-73, little-endian)
    // Note: index_id in PAGE_HEADER is stored after FIL_HEADER(38) + BTR_HEADER(8) = 46 bytes
    // Actually, index_id is at offset 38 + 6 = 44 in the page (FIL_HEADER + BTR_IND_PAGE_OFFSET)
    // But let's also check offset 70 which may be another location
    uint32_t index_id_at_44 = page_data[44] | (page_data[45] << 8) | (page_data[46] << 16) | (page_data[47] << 24);
    uint32_t index_id_at_70 = page_data[70] | (page_data[71] << 8) | (page_data[72] << 16) | (page_data[73] << 24);

    // Read n_recs from PAGE_HEADER (offset 54-55) - but this is often corrupted
    uint16_t n_recs = page_data[54] | (page_data[55] << 8);

    // Heuristic: check if index_id looks reasonable
    // InnoDB index_ids are typically > 0 and can be very large (up to billions)
    // A valid index_id should not be 0 or 0xFFFFFFFF
    bool likely_index_page = (index_id_at_44 > 0 && index_id_at_44 != 0xFFFFFFFF) ||
                            (index_id_at_70 > 0 && index_id_at_70 != 0xFFFFFFFF);

    // Also check if n_recs is somewhat reasonable (< 10000 per page for most cases)
    if (n_recs > 0 && n_recs < 10000) {
        likely_index_page = true;
    }

    // Step 3: 重建页头
    uint16_t target_type;

    // Determine the target page type
    if (current_type == dbrecovery::innodb::FIL_PAGE_INDEX ||
        current_type == dbrecovery::innodb::FIL_PAGE_SDI ||
        current_type == dbrecovery::innodb::FIL_PAGE_TYPE_BLOB ||
        current_type == dbrecovery::innodb::FIL_PAGE_UNDO_LOG ||
        current_type == dbrecovery::innodb::FIL_PAGE_TYPE_ALLOCATED) {
        // Page type is valid, only need to fix checksum
        target_type = current_type;
    } else if (likely_index_page) {
        // Page type is corrupted but looks like INDEX page
        target_type = dbrecovery::innodb::FIL_PAGE_INDEX;
    } else if (current_type == 0xFFFF || current_type == 0x0000) {
        // Completely corrupted, assume INDEX if index_id is set
        target_type = likely_index_page ? dbrecovery::innodb::FIL_PAGE_INDEX : 0;
    } else {
        // Unknown page type, skip
        return false;
    }

    // Only repair if we have a valid target type
    if (target_type == 0) {
        return false;
    }

    // Rebuild the FIL header with correct type
    // First, preserve some important values from the corrupted page
    // Note: InnoDB stores page_no, prev_page, next_page in BIG-ENDIAN format
    uint32_t page_no = dbrecovery::innodb::read_be<uint32_t>(page_data, 4);
    uint32_t prev_page = dbrecovery::innodb::read_be<uint32_t>(page_data, 8);
    uint32_t next_page = dbrecovery::innodb::read_be<uint32_t>(page_data, 12);

    // If page_no, prev_page or next_page look corrupted, set to defaults
    if (page_no >= 0xFFFFFFF0) page_no = 0;
    if (prev_page >= 0xFFFFFFF0) prev_page = 0xFFFFFFFF;
    if (next_page >= 0xFFFFFFF0) next_page = 0xFFFFFFFF;

    if (!rebuild_page_header(page_data, page_size, page_no, target_type)) {
        return false;
    }

    // Restore prev/next page pointers
    dbrecovery::innodb::write_be<uint32_t>(page_data, 8, prev_page);
    dbrecovery::innodb::write_be<uint32_t>(page_data, 12, next_page);

    // Step 4: 修复校验和
    calculate_and_fix_checksum(page_data, page_size);

    return true;
}

uint32_t HeaderRepairer::calculate_and_fix_checksum(uint8_t* page_data, size_t page_size) {
    // 计算 CRC32C 校验和（排除校验和字段）
    std::vector<uint8_t> page_copy(page_size);
    std::memcpy(page_copy.data(), page_data, page_size);

    // 清零校验和字段（偏移 0-3） - checksum is at offset 0, not 24!
    std::memset(page_copy.data() + 0, 0, 4);

    uint32_t computed_crc = dbrecovery::innodb::PageReader::compute_crc32c(page_copy);

    // 写入校验和字段（偏移 0-3）
    for (int i = 0; i < 4; ++i) {
        page_data[0 + i] = (computed_crc >> (i * 8)) & 0xFF;
    }

    return computed_crc;
}

bool HeaderRepairer::validate_page_header(const uint8_t* page_data, size_t page_size) const {
    if (!page_data || page_size < 54) {
        return false;
    }

    // 检查页类型 - correct offset is FIL_PAGE_TYPE_ABS = 24
    uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(page_data, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);
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

    // Use correct offset FIL_PAGE_TYPE_ABS = 24
    uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(page_data, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

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
    // checksum (offset 0-3)
    dbrecovery::innodb::write_be<uint32_t>(page_data, 0, 0);
    
    // page_number (offset 4-7)
    dbrecovery::innodb::write_be<uint32_t>(page_data, 4, page_no);
    
    // FIL_PAGE_PREV (offset 8-11)
    dbrecovery::innodb::write_be<uint32_t>(page_data, 8, 0xFFFFFFFF);
    
    // FIL_PAGE_NEXT (offset 12-15)
    dbrecovery::innodb::write_be<uint32_t>(page_data, 12, 0xFFFFFFFF);
    
    // LSN (offset 16-23)
    dbrecovery::innodb::write_be<uint64_t>(page_data, 16, 0);
    
    // type (offset FIL_PAGE_TYPE_ABS = 24)
    dbrecovery::innodb::write_be<uint16_t>(page_data, dbrecovery::innodb::FIL_PAGE_TYPE_ABS, page_type);
    
    // flush_lsn (offset 26-33)
    dbrecovery::innodb::write_be<uint64_t>(page_data, 26, 0);
    
    // space_id (offset 34-37)
    dbrecovery::innodb::write_be<uint32_t>(page_data, 34, 0);

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

size_t HeaderRepairer::repair_file_pages(uint8_t* file_data, size_t file_size, size_t page_size) {
    if (!file_data || file_size < page_size) {
        last_error_ = "Invalid file data or size";
        return 0;
    }

    size_t page_count = file_size / page_size;
    size_t repaired_count = 0;

    for (size_t i = 0; i < page_count; ++i) {
        uint8_t* page_data = file_data + i * page_size;

        // Check if page header is valid before attempting repair
        if (validate_page_header(page_data, page_size)) {
            // Page is already valid, no need to repair
            continue;
        }

        // Page header is corrupted, try to repair it
        if (repair_page_header(page_data, page_size)) {
            repaired_count++;
        }
    }

    std::cout << "[INFO] Repaired " << repaired_count << " / " << page_count << " pages" << std::endl;
    return repaired_count;
}

} // namespace repair
} // namespace dbrecovery
