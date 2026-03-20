#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <iostream>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace dbrecovery {
namespace innodb {

// ==================== 辅助函数：字段值解析 ====================

// ==================== BPlusTreeParser 实现 ====================

BPlusTreeParser::BPlusTreeParser(const std::vector<uint8_t>& page_data)
    : page_data_(page_data), 
      flags_(0xFFFF), // ✅ 修复：初始化为 0xFFFF 表示未解析
      page_level_(0xFFFFFFFF), // ✅ 修复：初始化为 0xFFFFFFFF 表示未解析
      n_records_(0),
      next_page_(0xFFFFFFFF), 
      prev_page_(0xFFFFFFFF), 
      node_type_(BTreeNodeType::INVALID) {
}

bool BPlusTreeParser::parse() noexcept(false) {
    if (page_data_.size() < 38 + 16) { // FIL header (38) + minimal page header space
        std::cerr << "[ERROR] BPlusTreeParser: Page too small" << std::endl;
        return false;
    }
    
    // ✅ 修复：使用正确的偏移计算页类型
    // FIL_PAGE_TYPE_OFFSET 在 page_defs.hpp 中定义为 12，绝对偏移 = 38 + 12 = 50
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + dbrecovery::innodb::FIL_PAGE_TYPE_OFFSET);
    
    // ✅ 新增：使用统一的页类型常量进行比较
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX && 
        page_type != dbrecovery::innodb::FIL_PAGE_SDI &&
        page_type != dbrecovery::innodb::FIL_PAGE_UNDO_LOG) {
        std::cerr << "[ERROR] BPlusTreeParser: Invalid page type " << page_type 
                  << " (" << dbrecovery::innodb::page_type_to_string(page_type) << ")" << std::endl;
        return false;
    }
    
    // 解析B+树头
    if (!parse_header()) {
        std::cerr << "[ERROR] BPlusTreeParser: Failed to parse B+ tree header" << std::endl;
        return false;
    }
    
    // 解析INFIMUM和SUPREMUM记录
    if (!parse_infimum_supremum()) {
        std::cerr << "[ERROR] BPlusTreeParser: Failed to parse infimum/supremum records" << std::endl;
        return false;
    }
    
    // ✅ 修复：核心逻辑，使用页目录解析用户记录，移除废弃的 fallback
    if (!parse_directory()) {
        std::cerr << "[ERROR] BPlusTreeParser: Failed to parse directory, cannot recover records reliably" << std::endl;
        return false;
    }
    
    // ✅ 新增：解析用户记录（从目录槽中获取偏移后）
    if (!parse_user_records()) {
        std::cerr << "[ERROR] BPlusTreeParser: Failed to parse user records" << std::endl;
        return false;
    }
    
    return true;
}

bool BPlusTreeParser::parse_header() noexcept {
    if (page_data_.size() < 38 + 24) { // 确保有足够空间读取到 PAGE_NEXT
        return false;
    }
    
    // ✅ 修复：使用正确的偏移计算（参考 page_defs.hpp）
    // FIL_HEADER_SIZE = 38, BTR_N_LEVELS_OFFSET = 4 (relative to B+ tree header)
    // So absolute offset for PAGE_LEVEL is 38 + 4 = 42
    // ✅ 新增：使用 uint16_t 读取以匹配 page_level_ 类型（实际 InnoDB 中为 2 字节）
    page_level_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 4);
    
    // Flags are at BTR_N_LEVELS_OFFSET + 2 = 6 relative to B+ tree header (BTR_SPACE_ID = 0, BTR_N_LEVELS = 4)
    flags_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 6);
    
    // ✅ 新增：验证页级别合理性（MySQL 8.0 B+树最大高度通常不超过5）
    if (page_level_ > 5) {
        std::cerr << "[WARN] BPlusTreeParser: Unreasonably high page level: " << page_level_ << std::endl;
    }
    
    // 判断节点类型
    if (page_level_ == 0) {
        node_type_ = BTreeNodeType::LEAF;
    } else {
        node_type_ = BTreeNodeType::INTERNAL;
    }
    
    // 读取页链信息（使用正确的偏移）
    // PAGE_PREV_OFFSET = 20, PAGE_NEXT_OFFSET = 26 (relative to B+ tree header)
    prev_page_ = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), 38 + 20);
    next_page_ = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), 38 + 26);
    
    // ✅ 新增：验证页链连续性（仅对非叶子节点有意义）
    if (node_type_ != BTreeNodeType::LEAF) {
        if (prev_page_ != 0xFFFFFFFF && next_page_ != 0xFFFFFFFF) {
            // 内部节点应有前驱和后继
        } else if (prev_page_ == 0xFFFFFFFF && next_page_ == 0xFFFFFFFF) {
            // 根节点可能没有前后页
        } else {
            std::cerr << "[WARN] BPlusTreeParser: Inconsistent page chain for internal node" << std::endl;
        }
    }
    
    return true;
}

bool BPlusTreeParser::parse_infimum_supremum() {
    if (page_data_.size() < 100) {
        return false;
    }
    
    // INFIMUM 记录偏移（固定为54，参考 page_defs.hpp）
    infimum_offset_ = 54;
    if (infimum_offset_ + 13 > page_data_.size()) {
        std::cerr << "[ERROR] BPlusTreeParser: INFIMUM record out of bounds" << std::endl;
        return false;
    }
    
    // ✅ 新增：验证 INFIMUM 记录头（必须是 REC_STATUS_INFIMUM = 0x02）
    uint8_t infimum_info_bits = page_data_[infimum_offset_];
    if ((infimum_info_bits & 0x03) != REC_STATUS_INFIMUM) {
        std::cerr << "[ERROR] BPlusTreeParser: INFIMUM record type mismatch. Expected 0x02, got 0x"
                  << std::hex << static_cast<int>(infimum_info_bits & 0x03) << std::dec << std::endl;
        return false;
    }
    
    // SUPREMUM 记录偏移（位于 INFIMUM 记录头的 next_record_offset 字段）
    uint16_t infimum_next_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), infimum_offset_ + 1);
    supremum_offset_ = infimum_next_offset & 0x3FFF; // ✅ 修复：next_record_offset 是 15 位
    
    if (supremum_offset_ == 0 || supremum_offset_ >= page_data_.size()) {
        std::cerr << "[ERROR] BPlusTreeParser: SUPREMUM offset invalid: " << supremum_offset_ << std::endl;
        return false;
    }
    
    // ✅ 新增：验证 SUPREMUM 记录头（必须是 REC_STATUS_SUPREMUM = 0x03）
    uint8_t supremum_info_bits = page_data_[supremum_offset_];
    if ((supremum_info_bits & 0x03) != REC_STATUS_SUPREMUM) {
        std::cerr << "[ERROR] BPlusTreeParser: SUPREMUM record type mismatch. Expected 0x03, got 0x"
                  << std::hex << static_cast<int>(supremum_info_bits & 0x03) << std::dec << std::endl;
        return false;
    }
    
    // 记录偏移列表（按页目录顺序）
    record_offsets_.push_back(infimum_offset_);
    record_offsets_.push_back(supremum_offset_);
    
    return true;
}

bool BPlusTreeParser::parse_user_records() {
    // 解析用户记录（从 INFIMUM 之后开始）
    if (page_data_.size() < infimum_offset_ + 5) {
        return false;
    }
    
    uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 4);
    size_t current_offset = infimum_offset_;
    
    while (current_offset < heap_top - 5) {
        // 检查是否为 SUPREMUM
        if (current_offset == supremum_offset_) {
            break;
        }
        
        uint8_t info_bits = page_data_[current_offset];
        uint16_t next_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data() + current_offset + 1, 0) & 0x3FFF;
        
        if (next_offset <= current_offset || next_offset >= page_data_.size()) {
            // 下一个偏移无效
            break;
        }
        
        record_offsets_.push_back(static_cast<uint16_t>(current_offset));
        current_offset = next_offset;
    }
    
    return !record_offsets_.empty();
}

BPlusTreeParser::PageHealth BPlusTreeParser::check_health() const {
    PageHealth health;
    health.is_valid = true;
    
    // 检查页大小
    if (page_data_.size() < 38) {
        health.is_valid = false;
        health.status_message = "Page size too small";
        return health;
    }
    
    // ✅ 新增：使用正确的偏移计算页类型（参考 page_defs.hpp）
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + dbrecovery::innodb::FIL_PAGE_TYPE_OFFSET);
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX && page_type != dbrecovery::innodb::FIL_PAGE_SDI) {
        health.is_valid = false;
        health.status_message = "Invalid page type: " + std::to_string(page_type);
        return health;
    }
    
    // 检查INFIMUM和SUPREMUM记录
    if (page_data_.size() >= 100) {
        uint16_t infimum_offset = 54;
        if (infimum_offset + 13 <= page_data_.size()) {
            health.has_infimum = true;
            
            uint16_t supremum_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), infimum_offset + 1);
            if (supremum_offset > 0 && supremum_offset < page_data_.size()) {
                health.has_supremum = true;
            }
        }
    } else {
        // 页太小，无法包含完整的记录
        health.status_message = "Page too small for records";
    }
    
    // 解析页目录
    if (page_data_.size() >= 38 + 36) { // 确保有足够空间读取页目录
        uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 0); // PAGE_N_DIR_SLOTS
        health.n_dir_slots = n_dir_slots;
        
        // 验证页目录槽数量合理性（至少有INFIMUM和SUPREMUM）
        if (n_dir_slots < 2) {
            health.status_message += "Insufficient dir slots; ";
            health.is_valid = false;
        }
        
        // ✅ 新增：计算空闲空间
        uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 2); // PAGE_HEAP_TOP
        uint16_t free_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 6); // PAGE_FREE
        if (heap_top >= free_offset) {
            health.free_space = heap_top - free_offset;
        }
    } else {
        health.status_message += "Page header incomplete; ";
        health.is_valid = false;
    }
    
    // 解析堆大小
    if (page_data_.size() >= 38 + 4) { // 确保有足够空间读取PAGE_N_HEAP
        uint16_t n_heap = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 4) & 0x7FFF;
        health.n_records = n_heap - 2; // 减去INFIMUM和SUPREMUM
        
        // 验证堆大小合理性
        if (n_heap < 2 || n_heap > 16384) {
            health.status_message += "Invalid heap size: " + std::to_string(n_heap);
            health.is_valid = false;
        }
    } else {
        health.status_message += "Cannot read heap size; ";
        health.is_valid = false;
    }
    
    return health;
}

uint64_t BPlusTreeParser::get_sdi_table_id() const {
    if (page_data_.size() < 58) {
        return 0;
    }
    
    // SDI页的表ID在偏移38+20处
    uint64_t table_id = dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), 38 + 20);
    return table_id;
}

// ==================== BPlusTreeReconstructor 实现 ====================

BPlusTreeReconstructor::BPlusTreeReconstructor(uint32_t root_page_no)
    : root_page_no_(root_page_no) {
}

void BPlusTreeReconstructor::add_page(uint32_t page_no, std::vector<uint8_t> page_data) {
    pages_[page_no] = std::move(page_data);
}

bool BPlusTreeReconstructor::reconstruct() noexcept {
    if (pages_.empty()) {
        return false;
    }
    
    // 从根页开始遍历
    auto it = pages_.find(root_page_no_);
    if (it == pages_.end()) {
        return false;
    }
    
    BPlusTreeParser parser(it->second);
    if (!parser.parse()) {
        return false;
    }
    
    // TODO: 实现完整的记录解析逻辑
    // 这里需要表结构信息来正确解析字段类型和长度
    // 暂时返回空记录列表
    records_.clear();
    
    return true;
}

} // namespace innodb
} // namespace dbrecovery
