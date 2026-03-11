#include "dbrecovery/innodb/page_parser.hpp"
#include <cstring>
#include <stdexcept>
#include <iostream>

namespace dbrecovery {
namespace innodb {

PageParser::PageParser(const uint8_t* buffer, size_t size)
    : buffer_(buffer), size_(size), page_type_(0), flags_(0), page_level_(0),
      fil_prev_(0xFFFFFFFF), fil_next_(0xFFFFFFFF), page_offset_(0) {
    // ✅ 添加页大小校验：InnoDB 页大小通常为 4KB/8KB/16KB/32KB/64KB
    if (!buffer_ || size_ < UNIV_PAGE_SIZE_MIN) {
        throw std::runtime_error("Invalid page buffer: too small (less than " + std::to_string(UNIV_PAGE_SIZE_MIN) + " bytes)");
    }
    
    if (size_ > UNIV_PAGE_SIZE_MAX) {
        throw std::runtime_error("Invalid page buffer: too large (exceeds " + std::to_string(UNIV_PAGE_SIZE_MAX) + " bytes)");
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
    
    // ✅ 添加：校验页大小范围
    if (size_ < UNIV_PAGE_SIZE_MIN || size_ > UNIV_PAGE_SIZE_MAX) {
        std::cerr << "[WARN] Invalid page size: " << size_ << "\n";
        return false;
    }
    
    fil_prev_ = dbrecovery::innodb::read_le<uint32_t>(buffer_, 0);
    fil_next_ = dbrecovery::innodb::read_le<uint32_t>(buffer_, 4);
    // LSN (8 bytes)
    uint64_t fil_lsn = dbrecovery::innodb::read_le<uint64_t>(buffer_, 8);
    page_type_ = dbrecovery::innodb::read_le<uint16_t>(buffer_, 12); // FIL_PAGE_TYPE offset
    uint32_t space_id = dbrecovery::innodb::read_le<uint32_t>(buffer_, 16);
    page_offset_ = dbrecovery::innodb::read_le<uint32_t>(buffer_, 20);

    // ✅ Step 1.1: 校验页校验和（FIL_PAGE_SPACE_CHECKSUM at offset 4, 8, 24, 28）
    bool checksum_valid = true;
    if (size_ >= 52) {
        uint32_t crc32_a = dbrecovery::innodb::read_le<uint32_t>(buffer_, 4);
        uint32_t crc32_b = dbrecovery::innodb::read_le<uint32_t>(buffer_, 8);
        uint32_t checksum_a = dbrecovery::innodb::read_le<uint32_t>(buffer_, 24);
        uint32_t checksum_b = dbrecovery::innodb::read_le<uint32_t>(buffer_, 28);
        
        // 简化校验：检查 checksum 字段是否一致
        if (checksum_a != checksum_b && checksum_a != 0 && checksum_b != 0) {
            std::cerr << "[WARN] Checksum mismatch: " << checksum_a << " vs " << checksum_b << "\n";
            checksum_valid = false;
        }
    }

    // ✅ Step 2: 校验页类型（支持索引页、SDI页和Undo日志页）
    if (page_type_ != FIL_PAGE_INDEX && 
        page_type_ != FIL_PAGE_UNDO_LOG && 
        page_type_ != FIL_PAGE_SDI &&
        page_type_ != FIL_PAGE_TYPE_ALLOCATED) {
        std::cerr << "[WARN] Unsupported page type: 0x" << std::hex << page_type_ << std::dec << "\n";
        return false; // 非有效页类型，跳过
    }

    // ✅ Step 3: 解析 PAGE_HEADER (offset 38)
    const uint8_t* header = buffer_ + 38;
    if (size_ < 38 + 14) { // PAGE_HEADER 至少14字节
        return false;
    }
    
    header_.n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(header, 0);
    header_.heap_top    = dbrecovery::innodb::read_le<uint16_t>(header, 2);
    header_.n_heap      = dbrecovery::innodb::read_le<uint16_t>(header, 4) & 0x7FFF; // 最高位为 REC_INFO_NEW_CHUNK_FLAG
    header_.free_offset = dbrecovery::innodb::read_le<uint16_t>(header, 6);

    // 解析 BTR_HEADER (PAGE_HEADER 后的8字节)
    uint16_t n_owned = dbrecovery::innodb::read_le<uint16_t>(header, 8);
    uint32_t max_insert = dbrecovery::innodb::read_le<uint16_t>(header, 10);
    uint16_t free_space = dbrecovery::innodb::read_le<uint16_t>(header, 12);

    // ✅ Step 4: 解析 flags (offset 24 in FIL_HEADER)
    if (size_ < 26) {
        return false;
    }
    flags_ = dbrecovery::innodb::read_le<uint16_t>(buffer_, 24);
    page_level_ = (flags_ >> 12) & 0x0F; // 实际仅占 4 bits

    if (!checksum_valid) {
        std::cerr << "[WARN] Page " << page_offset_ << " checksum validation failed\n";
        return false;
    }

    return true;
}

bool PageParser::validate_page_chain() const {
    // ✅ 校验页链表闭环（B+ Tree 要求）
    // 对于 B+ 树叶子节点，需要验证 prev/next 指针形成双向链表
    // 对于根节点或内部节点，prev/next 可能为 0xFFFFFFFF
    
    bool is_valid = true;
    
    // 检查页偏移是否在合理范围内
    if (page_offset_ >= 0xFFFFFFF0) {
        std::cerr << "[WARN] Invalid page offset: " << page_offset_ << "\n";
        return false;
    }
    
    // 对于非根节点（有前后页），验证链表连续性
    if (fil_prev_ != 0xFFFFFFFF && fil_next_ != 0xFFFFFFFF) {
        // 检查 prev/next 是否指向有效范围
        if (fil_prev_ >= page_offset_) {
            std::cerr << "[WARN] Invalid prev page: " << fil_prev_ 
                      << " >= current page: " << page_offset_ << "\n";
            is_valid = false;
        }
        
        if (fil_next_ <= page_offset_) {
            std::cerr << "[WARN] Invalid next page: " << fil_next_ 
                      << " <= current page: " << page_offset_ << "\n";
            is_valid = false;
        }
    } else if (fil_prev_ != 0xFFFFFFFF && fil_next_ == 0xFFFFFFFF) {
        // 只有 prev 没有 next，可能是最后一个叶子节点
        // 这种情况在 B+ 树中是合法的
    } else if (fil_prev_ == 0xFFFFFFFF && fil_next_ != 0xFFFFFFFF) {
        // 只有 next 没有 prev，可能是第一个叶子节点
        // 这种情况在 B+ 树中也是合法的
    }
    
    return is_valid;
}

std::vector<Record> PageParser::parse_records() const {
    std::vector<Record> records;

    if (!buffer_ || header_.n_heap == 0 || header_.heap_top < 38 + 16) {
        return records; // 空页或无效堆
    }

    // 定位记录起始点（INFIMUM 记录）
    const uint8_t* rec = buffer_ + 38 + 16;

    bool found_infimum = false;
    bool found_supremum = false;
    std::set<uint16_t> visited_offsets;

    while (true) {
        if (rec >= buffer_ + header_.heap_top || rec + 5 > buffer_ + size_) break;

        uint16_t rec_offset = static_cast<uint16_t>(rec - buffer_);
        
        if (visited_offsets.find(rec_offset) != visited_offsets.end()) {
            std::cerr << "[WARN] Circular reference detected at offset " << rec_offset << "\n";
            break;
        }
        visited_offsets.insert(rec_offset);
        
        uint8_t info_bits = *rec;
        bool is_deleted = (info_bits & REC_INFO_DELETED_FLAG) != 0;

        if (rec + 1 >= buffer_ + size_) break;
        uint16_t next_rec_offset = dbrecovery::innodb::read_le<uint16_t>(rec, 1);
        
        // ✅ 修复：边界检查更严格
        if (next_rec_offset < 38 + 16 || next_rec_offset >= header_.heap_top) {
            std::cerr << "[WARN] Invalid next_record offset " << next_rec_offset 
                      << " at record " << rec_offset << "\n";
            break;
        }
        
        // ✅ 修复：SUPREMUM 标记是 next_rec_offset == 0xFFFF，但也要检查 info_bits
        if (next_rec_offset == 0xFFFF) {
            found_supremum = true;
            break;
        }

        if (rec_offset == 38 + 16) {
            found_infimum = true;
        }

        Record rec_info;
        rec_info.offset = rec_offset;
        rec_info.info_bits = info_bits;
        rec_info.next_offset = next_rec_offset;
        rec_info.is_deleted = is_deleted;

        // 解析字段数量（COMPACT格式）
        if (rec + 5 <= buffer_ + size_) {
            uint8_t n_fields_high = (info_bits & REC_INFO_BITS_MASK);
            uint8_t n_fields_low = (rec[1] >> 4) & 0x0F;
            rec_info.n_fields = static_cast<uint16_t>((n_fields_high << 4) | n_fields_low);
        }

        records.push_back(rec_info);

        rec = buffer_ + next_rec_offset;
    }

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
