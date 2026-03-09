#include "dbrecovery/innodb/page_parser.hpp"
#include <cstring>
#include <stdexcept>
#include <iostream>

namespace dbrecovery {
namespace innodb {

PageParser::PageParser(const uint8_t* buffer, size_t size)
    : buffer_(buffer), size_(size), page_type_(0), flags_(0), page_level_(0) {
    if (!buffer_ || size_ < 38 + 16) { // 至少包含 FIL_HEADER + PAGE_HEADER 基础部分
        throw std::runtime_error("Invalid page buffer: too small");
    }
}

const uint8_t* PageParser::data() const {
    return buffer_;
}

size_t PageParser::size() const {
    return size_;
}

bool PageParser::parse_header() {
    // ✅ Step 1: 解析 FIL_HEADER (offset 0–37)
    if (size_ < 38) {
        return false;
    }
    
    uint32_t fil_prev = read_le<uint32_t>(buffer_ + 0);
    uint32_t fil_next = read_le<uint32_t>(buffer_ + 4);
    // LSN (8 bytes)
    uint64_t fil_lsn = read_le<uint64_t>(buffer_ + 8);
    page_type_ = read_le<uint16_t>(buffer_ + 12); // 注意：FIL_PAGE_TYPE 是 uint16_t！
    uint32_t space_id = read_le<uint32_t>(buffer_ + 16);
    uint32_t page_offset = read_le<uint32_t>(buffer_ + 20);

    // ✅ Step 2: 校验页类型（仅支持索引页）
    if (page_type_ != FIL_PAGE_INDEX && page_type_ != FIL_PAGE_UNDO_LOG) {
        return false; // 非 B+ 树页，跳过
    }

    // ✅ Step 3: 解析 PAGE_HEADER (offset 38)
    const uint8_t* header = buffer_ + 38;
    if (size_ < 38 + 14) { // PAGE_HEADER 至少14字节
        return false;
    }
    
    header_.n_dir_slots = read_le<uint16_t>(header + 0);
    header_.heap_top    = read_le<uint16_t>(header + 2);
    header_.n_heap      = read_le<uint16_t>(header + 4) & 0x7FFF; // 最高位为 REC_INFO_NEW_CHUNK_FLAG
    header_.free_offset = read_le<uint16_t>(header + 6);

    // 解析 BTR_HEADER (PAGE_HEADER 后的8字节)
    uint16_t n_owned = read_le<uint16_t>(header + 8);
    uint32_t max_insert = read_le<uint16_t>(header + 10);
    uint16_t free_space = read_le<uint16_t>(header + 12);

    // ✅ Step 4: 解析 flags (offset 24 in FIL_HEADER)
    if (size_ < 26) {
        return false;
    }
    flags_ = read_le<uint16_t>(buffer_ + 24);
    page_level_ = (flags_ >> 12) & 0x0F; // 实际仅占 4 bits

    return true;
}

bool PageParser::validate_page_chain() const {
    // ✅ 校验页链表闭环（B+ Tree 要求）
    // 注意：实际闭环需跨页校验，此处仅验证当前页的 prev/next 是否指向有效范围
    if (fil_prev_ != 0xFFFFFFFF && fil_next_ != 0xFFFFFFFF) {
        // 非首尾页：prev < current < next（逻辑顺序）
        if (fil_prev_ >= page_offset_ || fil_next_ <= page_offset_) {
            std::cerr << "[WARN] Page chain broken: prev=" << fil_prev_
                      << ", current=" << page_offset_
                      << ", next=" << fil_next_ << "\n";
            return false;
        }
    } else if (fil_prev_ != 0xFFFFFFFF) {
        // 尾页：prev < current
        if (fil_prev_ >= page_offset_) {
            std::cerr << "[WARN] Tail page chain broken: prev=" << fil_prev_
                      << ", current=" << page_offset_ << "\n";
            return false;
        }
    } else if (fil_next_ != 0xFFFFFFFF) {
        // 首页：current < next
        if (page_offset_ >= fil_next_) {
            std::cerr << "[WARN] Head page chain broken: current=" << page_offset_
                      << ", next=" << fil_next_ << "\n";
            return false;
        }
    }
    return true;
}

std::vector<Record> PageParser::parse_records() const {
    std::vector<Record> records;

    if (!buffer_ || header_.n_heap == 0 || header_.heap_top < 38 + 16) {
        return records; // 空页或无效堆
    }

    // ✅ Step 1: 定位记录起始点（INFIMUM 记录）
    const uint8_t* rec = buffer_ + 38 + 16; // PAGE_HEADER 后第一个记录位置

    bool found_infimum = false;
    bool found_supremum = false;

    // ✅ Step 2: 遍历记录链表
    while (true) {
        // 检查是否超出堆顶或页边界
        if (rec >= buffer_ + header_.heap_top || rec + 5 > buffer_ + size_) break;

        uint16_t rec_offset = static_cast<uint16_t>(rec - buffer_);
        uint8_t info_bits = *rec;
        bool is_deleted = (info_bits & REC_INFO_DELETED_FLAG) != 0;

        // 提取 next_record 偏移（相对页起始）
        if (rec + 1 >= buffer_ + size_) break;
        uint16_t next_rec_offset = read_le<uint16_t>(rec + 1);
        
        // ✅ 检查是否为 SUPREMUM 标记
        if (next_rec_offset == 0xFFFF) {
            found_supremum = true;
            break; // 结束遍历
        }

        // ✅ 检查是否为 INFIMUM 记录（固定偏移）
        if (rec_offset == 38 + 16) {
            found_infimum = true;
        }

        Record rec_info;
        rec_info.offset = rec_offset;
        rec_info.info_bits = info_bits;
        rec_info.next_offset = next_rec_offset;
        rec_info.is_deleted = is_deleted;

        // 解析记录头中的字段数量（COMPACT格式）
        if (rec + 5 <= buffer_ + size_) {
            uint8_t n_fields_high = info_bits & REC_INFO_BITS_MASK;
            uint8_t n_fields_low = (rec[1] >> 4) & 0x3F;
            rec_info.n_fields = (n_fields_high << 6) | n_fields_low;
        }

        records.push_back(rec_info);

        // 移动到下一条记录
        rec = buffer_ + next_rec_offset;
    }

    // ✅ Step 3: 校验 infimum/supremum 完整性
    if (!found_infimum) {
        std::cerr << "[WARN] Missing INFIMUM record in page " << page_offset_ << "\n";
    }
    if (!found_supremum) {
        std::cerr << "[WARN] Missing SUPREMUM record in page " << page_offset_ << "\n";
    }

    return records;
}

} // namespace innodb
} // namespace dbrecovery
