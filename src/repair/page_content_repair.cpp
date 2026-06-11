#include "dbrecovery/repair/page_content_repair.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace dbrecovery {
namespace repair {

// ==================== PageContentRepairer ====================

PageContentRepairer::PageContentRepairer()
    : page_size_(16384) {
}

std::vector<PageContentCorruption> PageContentRepairer::detect_corruptions(const uint8_t* page_data, size_t page_size) {
    std::vector<PageContentCorruption> corruptions;

    if (!page_data || page_size < dbrecovery::innodb::FIL_HEADER_SIZE + 20) {
        return corruptions;
    }

    const uint8_t* page_header = page_data + dbrecovery::innodb::FIL_HEADER_SIZE;

    // 1. 检查 n_dir_slots
    uint16_t n_dir_slots = get_n_dir_slots(page_data);
    if (n_dir_slots == 0 || n_dir_slots > 1000) {
        PageContentCorruption c;
        c.type = PageContentCorruptionType::N_DIR_SLOTS_INVALID;
        c.offset = dbrecovery::innodb::FIL_HEADER_SIZE;
        c.length = 2;
        c.description = "n_dir_slots is invalid: " + std::to_string(n_dir_slots);
        c.confidence = 0.95f;
        c.repairable = true;
        corruptions.push_back(c);
    }

    // 2. 检查 heap_top
    uint16_t heap_top = get_heap_top(page_data);
    if (heap_top < 54 || heap_top > page_size - 8) {
        PageContentCorruption c;
        c.type = PageContentCorruptionType::HEAP_TOP_INVALID;
        c.offset = dbrecovery::innodb::FIL_HEADER_SIZE + 2;
        c.length = 2;
        c.description = "heap_top is invalid: " + std::to_string(heap_top);
        c.confidence = 0.90f;
        c.repairable = true;
        corruptions.push_back(c);
    }

    // 3. 检查 INFIMUM 记录 (offset 54)
    if (page_size >= 67) {
        uint8_t infimum_type = page_data[54] & 0x07;
        if (infimum_type != dbrecovery::innodb::REC_STATUS_INFIMUM) {
            PageContentCorruption c;
            c.type = PageContentCorruptionType::INFIMUM_CORRUPTED;
            c.offset = 54;
            c.length = 13;  // INFIMUM record size
            c.description = "INFIMUM record type is invalid: " + std::to_string((int)infimum_type);
            c.confidence = 0.95f;
            c.repairable = true;
            corruptions.push_back(c);
        }
    }

    // 4. 检查 SUPREMUM 记录 (offset 67)
    if (page_size >= 75) {
        uint8_t supremum_type = page_data[67] & 0x07;
        if (supremum_type != dbrecovery::innodb::REC_STATUS_SUPREMUM) {
            PageContentCorruption c;
            c.type = PageContentCorruptionType::SUPREMUM_CORRUPTED;
            c.offset = 67;
            c.length = 8;  // SUPREMUM record size
            c.description = "SUPREMUM record type is invalid: " + std::to_string((int)supremum_type);
            c.confidence = 0.95f;
            c.repairable = true;
            corruptions.push_back(c);
        }
    }

    // 5. 检查页目录槽
    if (n_dir_slots > 0 && n_dir_slots < 1000) {
        // 页目录位于页尾
        for (uint16_t i = 0; i < n_dir_slots; ++i) {
            auto slot_opt = read_dir_slot(page_data, i);
            if (!slot_opt || slot_opt->record_offset < 54 || slot_opt->record_offset >= page_size - n_dir_slots * 2) {
                PageContentCorruption c;
                c.type = PageContentCorruptionType::PAGE_DIRECTORY_CORRUPTED;
                c.offset = page_size - (i + 1) * 2;
                c.length = 2;
                c.description = "Page directory slot " + std::to_string(i) + " points to invalid offset";
                c.confidence = 0.85f;
                c.repairable = true;
                corruptions.push_back(c);
            }
        }
    }

    return corruptions;
}

size_t PageContentRepairer::repair(uint8_t* page_data, size_t page_size,
                                    const std::vector<PageContentCorruption>& corruptions) {
    size_t repaired = 0;

    for (const auto& c : corruptions) {
        bool success = false;

        switch (c.type) {
            case PageContentCorruptionType::N_DIR_SLOTS_INVALID:
                success = fix_n_dir_slots(page_data, page_size);
                break;

            case PageContentCorruptionType::HEAP_TOP_INVALID:
                success = fix_heap_top(page_data, page_size);
                break;

            case PageContentCorruptionType::INFIMUM_CORRUPTED:
                success = rebuild_infimum(page_data, page_size, 0);
                break;

            case PageContentCorruptionType::SUPREMUM_CORRUPTED:
                success = rebuild_supremum(page_data, page_size);
                break;

            case PageContentCorruptionType::PAGE_DIRECTORY_CORRUPTED:
                success = rebuild_page_directory(page_data, page_size);
                break;

            default:
                break;
        }

        if (success) {
            repaired++;
            repaired_count_++;
        }
    }

    return repaired;
}

bool PageContentRepairer::rebuild_page_directory(uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 100) {
        return false;
    }

    // 扫描所有用户记录
    std::vector<uint16_t> records = scan_user_records(page_data, page_size);
    if (records.empty()) {
        // 只有 INFIMUM 和 SUPREMUM
        records = {54, 67};  // INFIMUM at 54, SUPREMUM at 67
    }

    // 每个槽最多拥有 8 条记录
    std::vector<uint16_t> slots;
    for (size_t i = 0; i < records.size(); i += 8) {
        slots.push_back(records[i]);
    }

    // 确保包含 SUPREMUM
    if (slots.empty() || slots.back() != 67) {
        slots.push_back(67);
    }

    // 写入页目录槽 (从页尾向前)
    for (size_t i = 0; i < slots.size(); ++i) {
        write_dir_slot(page_data, i, slots[i]);
    }

    // 更新 n_dir_slots
    dbrecovery::innodb::write_le<uint16_t>(page_data, dbrecovery::innodb::FIL_HEADER_SIZE, slots.size());

    return true;
}

bool PageContentRepairer::rebuild_infimum(uint8_t* page_data, size_t page_size,
                                           uint16_t first_user_record_offset) {
    if (!page_data || page_size < 67) {
        return false;
    }

    // INFIMUM 记录结构 (COMPACT format):
    // offset 54: rec_type (2 bits) + heap_no (13 bits) + n_owned (4 bits)
    // offset 55-56: next_record_offset (相对偏移)
    // offset 57-62: 其他字段
    // 总共约 13 字节 (含 "infimum\0" 字符串)

    uint8_t* infimum = page_data + 54;

    // 清零
    std::memset(infimum, 0, 13);

    // 设置记录类型为 INFIMUM (2)
    infimum[0] = dbrecovery::innodb::REC_STATUS_INFIMUM;

    // 设置 heap_no = 0
    // heap_no 占 13 位，从 byte 0-2 编码
    // 格式: [extra_bytes] [n_owned:4bits] [heap_no:13bits] [rec_type:3bits]
    // 这里简化处理

    // 设置 next_record_offset 指向第一条用户记录或 SUPREMUM
    uint16_t next_offset = first_user_record_offset > 0 ? first_user_record_offset : 67;
    int16_t relative_offset = static_cast<int16_t>(next_offset - 54);
    dbrecovery::innodb::write_le<uint16_t>(infimum, 1, relative_offset);

    // 写入 "infimum\0" 标识 (offset 5-12)
    const char* infimum_str = "infimum";
    std::memcpy(infimum + 5, infimum_str, 8);

    return true;
}

bool PageContentRepairer::rebuild_supremum(uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 75) {
        return false;
    }

    // SUPREMUM 记录位于 offset 67
    // 结构类似 INFIMUM 但更小 (约 8 字节)

    uint8_t* supremum = page_data + 67;

    // 清零
    std::memset(supremum, 0, 8);

    // 设置记录类型为 SUPREMUM (3)
    supremum[0] = dbrecovery::innodb::REC_STATUS_SUPREMUM;

    // 设置 heap_no = 1
    // (heap_no 编码同 INFIMUM)

    // 设置 next_record_offset = 0 (表示链结束)
    dbrecovery::innodb::write_le<uint16_t>(supremum, 1, 0);

    // 写入 "supremum" 标识
    const char* supremum_str = "supremum";
    std::memcpy(supremum + 3, supremum_str, 8);

    return true;
}

std::vector<uint16_t> PageContentRepairer::recover_deleted_records(uint8_t* page_data, size_t page_size) {
    std::vector<uint16_t> recovered_offsets;

    if (!page_data || page_size < 100) {
        return recovered_offsets;
    }

    // PAGE_FREE 链头位于 PAGE_HEADER offset 6
    uint16_t free_head = get_free_list_head(page_data);

    if (free_head == 0 || free_head >= page_size - 20) {
        return recovered_offsets;
    }

    // 遍历 FREE 链
    uint16_t current = free_head;
    std::unordered_set<uint16_t> visited;  // 防止循环

    while (current != 0 && current < page_size - 10 && visited.find(current) == visited.end()) {
        visited.insert(current);
        recovered_offsets.push_back(current);

        // 读取下一个 FREE 记录的偏移
        // FREE 链通过记录头中的 next 指针连接
        int16_t next_relative = static_cast<int16_t>(
            dbrecovery::innodb::read_le<uint16_t>(page_data, current + 1));
        if (next_relative == 0) {
            break;
        }
        current = current + next_relative;
    }

    return recovered_offsets;
}

bool PageContentRepairer::fix_heap_top(uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 100) {
        return false;
    }

    // 扫描所有记录找到最大的记录结束位置
    std::vector<uint16_t> records = scan_user_records(page_data, page_size);

    uint16_t max_end = 67 + 8;  // 至少在 SUPREMUM 之后

    for (uint16_t offset : records) {
        if (offset > 54 && offset < page_size - 20) {
            // 简单估计记录长度 (实际需要解析记录头)
            // 假设每条记录至少 10 字节
            uint16_t end = offset + 10;
            if (end > max_end) {
                max_end = end;
            }
        }
    }

    // 更新 heap_top
    dbrecovery::innodb::write_le<uint16_t>(page_data, dbrecovery::innodb::FIL_HEADER_SIZE + 2, max_end);

    return true;
}

bool PageContentRepairer::fix_n_heap(uint8_t* page_data, size_t page_size, uint16_t actual_record_count) {
    if (!page_data) {
        return false;
    }

    // n_heap 包含 INFIMUM, SUPREMUM 和用户记录
    // 高位是标志位
    uint16_t n_heap = (actual_record_count + 2) | 0x8000;

    dbrecovery::innodb::write_le<uint16_t>(page_data, dbrecovery::innodb::FIL_HEADER_SIZE + 4, n_heap);

    return true;
}

bool PageContentRepairer::fix_n_dir_slots(uint8_t* page_data, size_t page_size) {
    if (!page_data) {
        return false;
    }

    // 计算合理的 n_dir_slots
    // 通常每个槽拥有 4-8 条记录
    std::vector<uint16_t> records = scan_user_records(page_data, page_size);
    uint16_t n_slots = std::max(2, static_cast<int>((records.size() + 7) / 8 + 1));

    dbrecovery::innodb::write_le<uint16_t>(page_data, dbrecovery::innodb::FIL_HEADER_SIZE, n_slots);

    return true;
}

bool PageContentRepairer::validate_page_structure(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 100) {
        return false;
    }

    auto corruptions = detect_corruptions(page_data, page_size);
    return corruptions.empty();
}

uint16_t PageContentRepairer::get_n_dir_slots(const uint8_t* page_data) const {
    return dbrecovery::innodb::read_le<uint16_t>(page_data, dbrecovery::innodb::FIL_HEADER_SIZE);
}

uint16_t PageContentRepairer::get_heap_top(const uint8_t* page_data) const {
    return dbrecovery::innodb::read_le<uint16_t>(page_data, dbrecovery::innodb::FIL_HEADER_SIZE + 2);
}

uint16_t PageContentRepairer::get_n_heap(const uint8_t* page_data) const {
    return dbrecovery::innodb::read_le<uint16_t>(page_data, dbrecovery::innodb::FIL_HEADER_SIZE + 4);
}

uint16_t PageContentRepairer::get_free_list_head(const uint8_t* page_data) const {
    return dbrecovery::innodb::read_le<uint16_t>(page_data, dbrecovery::innodb::FIL_HEADER_SIZE + 6);
}

std::optional<PageDirectorySlot> PageContentRepairer::read_dir_slot(const uint8_t* page_data, uint16_t slot_idx) const {
    // 页目录位于页尾，每个槽 2 字节
    uint16_t slot_offset = page_size_ - (slot_idx + 1) * 2;

    if (slot_offset < 100) {
        return std::nullopt;
    }

    PageDirectorySlot slot;
    slot.slot_offset = slot_idx;
    slot.record_offset = dbrecovery::innodb::read_le<uint16_t>(page_data, slot_offset);
    slot.is_supremum = (slot_idx == 0);  // 第一个槽是 SUPREMUM

    return slot;
}

void PageContentRepairer::write_dir_slot(uint8_t* page_data, uint16_t slot_idx, uint16_t record_offset) {
    uint16_t slot_offset = page_size_ - (slot_idx + 1) * 2;
    dbrecovery::innodb::write_le<uint16_t>(page_data, slot_offset, record_offset);
}

std::vector<uint16_t> PageContentRepairer::scan_user_records(const uint8_t* page_data, size_t page_size) {
    std::vector<uint16_t> records;

    if (!page_data || page_size < 100) {
        return records;
    }

    // 从 INFIMUM 开始遍历记录链
    uint16_t current = 54;  // INFIMUM offset
    std::unordered_set<uint16_t> visited;

    while (current != 0 && current < page_size - 20) {
        if (visited.find(current) != visited.end()) {
            break;  // 检测到循环
        }
        visited.insert(current);

        // 读取记录类型
        uint8_t rec_type = page_data[current] & 0x07;

        if (rec_type == dbrecovery::innodb::REC_STATUS_SUPREMUM) {
            break;  // 到达链尾
        }

        // 如果是用户记录，添加到列表
        if (rec_type == dbrecovery::innodb::REC_STATUS_ORDINARY ||
            rec_type == dbrecovery::innodb::REC_STATUS_NODE_PTR) {
            records.push_back(current);
        }

        // 读取下一条记录的相对偏移
        int16_t next_relative = static_cast<int16_t>(
            dbrecovery::innodb::read_le<uint16_t>(page_data, current + 1));
        if (next_relative == 0) {
            break;
        }

        current = current + next_relative;
    }

    return records;
}

// ==================== PageContentHandler ====================

size_t PageContentHandler::analyze_and_repair(uint8_t* page_data, size_t page_size) {
    last_corruptions_ = repairer_.detect_corruptions(page_data, page_size);
    if (last_corruptions_.empty()) {
        return 0;
    }
    return repairer_.repair(page_data, page_size, last_corruptions_);
}

std::vector<PageContentCorruption> PageContentHandler::analyze(const uint8_t* page_data, size_t page_size) {
    last_corruptions_ = repairer_.detect_corruptions(page_data, page_size);
    return last_corruptions_;
}

std::string PageContentHandler::get_report() const {
    std::ostringstream oss;
    oss << "Page Content Corruption Report\n";
    oss << "================================\n\n";

    if (last_corruptions_.empty()) {
        oss << "No page content corruption detected.\n";
        return oss.str();
    }

    oss << "Detected " << last_corruptions_.size() << " corruption(s):\n\n";

    for (size_t i = 0; i < last_corruptions_.size(); ++i) {
        const auto& c = last_corruptions_[i];
        oss << (i + 1) << ". ";

        switch (c.type) {
            case PageContentCorruptionType::PAGE_DIRECTORY_CORRUPTED:
                oss << "Page Directory Corrupted";
                break;
            case PageContentCorruptionType::INFIMUM_CORRUPTED:
                oss << "INFIMUM Record Corrupted";
                break;
            case PageContentCorruptionType::SUPREMUM_CORRUPTED:
                oss << "SUPREMUM Record Corrupted";
                break;
            case PageContentCorruptionType::RECORD_HEADER_CORRUPTED:
                oss << "Record Header Corrupted";
                break;
            case PageContentCorruptionType::HEAP_TOP_INVALID:
                oss << "Heap Top Invalid";
                break;
            case PageContentCorruptionType::N_HEAP_INVALID:
                oss << "N_Heap Invalid";
                break;
            case PageContentCorruptionType::N_DIR_SLOTS_INVALID:
                oss << "N_Dir_Slots Invalid";
                break;
            default:
                oss << "Unknown";
                break;
        }

        oss << "\n   Offset: " << c.offset << "\n";
        oss << "   Confidence: " << std::fixed << std::setprecision(1) << (c.confidence * 100) << "%\n";
        oss << "   Repairable: " << (c.repairable ? "Yes" : "No") << "\n";
        oss << "   Description: " << c.description << "\n\n";
    }

    oss << "Repair Statistics:\n";
    oss << "  Repaired: " << repairer_.get_repaired_count() << "\n";

    return oss.str();
}

// ==================== BatchPageContentRepairer ====================

BatchPageContentRepairer::BatchPageContentRepairer()
    : page_size_(16384) {
    stats_ = {};
}

size_t BatchPageContentRepairer::repair_all_pages(uint8_t* file_data, size_t file_size) {
    if (!file_data || file_size < page_size_) {
        return 0;
    }

    size_t page_count = file_size / page_size_;
    stats_.total_pages = page_count;
    stats_.corruptions_detected = 0;
    stats_.corruptions_repaired = 0;
    stats_.pages_with_corruption = 0;
    stats_.pages_repaired = 0;

    page_repairer_.set_page_size(page_size_);

    for (size_t i = 0; i < page_count; ++i) {
        uint8_t* page = file_data + i * page_size_;

        auto corruptions = page_repairer_.detect_corruptions(page, page_size_);

        if (!corruptions.empty()) {
            stats_.pages_with_corruption++;
            stats_.corruptions_detected += corruptions.size();

            size_t repaired = page_repairer_.repair(page, page_size_, corruptions);
            stats_.corruptions_repaired += repaired;

            if (repaired > 0) {
                stats_.pages_repaired++;
            }
        }
    }

    return stats_.pages_repaired;
}

std::vector<std::pair<uint32_t, PageContentCorruption>> BatchPageContentRepairer::analyze_all_pages(
    const uint8_t* file_data, size_t file_size) {

    std::vector<std::pair<uint32_t, PageContentCorruption>> all_corruptions;

    if (!file_data || file_size < page_size_) {
        return all_corruptions;
    }

    size_t page_count = file_size / page_size_;
    page_repairer_.set_page_size(page_size_);

    for (size_t i = 0; i < page_count; ++i) {
        const uint8_t* page = file_data + i * page_size_;
        auto corruptions = page_repairer_.detect_corruptions(page, page_size_);

        for (const auto& c : corruptions) {
            all_corruptions.push_back({static_cast<uint32_t>(i), c});
        }
    }

    return all_corruptions;
}

std::string BatchPageContentRepairer::get_report() const {
    std::ostringstream oss;
    oss << "Batch Page Content Repair Report\n";
    oss << "=================================\n\n";

    oss << "Total pages: " << stats_.total_pages << "\n";
    oss << "Pages with corruption: " << stats_.pages_with_corruption << "\n";
    oss << "Pages repaired: " << stats_.pages_repaired << "\n";
    oss << "Corruptions detected: " << stats_.corruptions_detected << "\n";
    oss << "Corruptions repaired: " << stats_.corruptions_repaired << "\n";

    if (stats_.total_pages > 0) {
        float repair_rate = static_cast<float>(stats_.pages_repaired) / stats_.total_pages * 100;
        oss << "Repair rate: " << std::fixed << std::setprecision(1) << repair_rate << "%\n";
    }

    return oss.str();
}

} // namespace repair
} // namespace dbrecovery
