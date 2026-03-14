#include "dbrecovery/innodb/btree.hpp"
#include <iostream>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// ==================== BPlusTreeParser 实现 ====================

BPlusTreeParser::BPlusTreeParser(const std::vector<uint8_t>& page_data)
    : page_data_(page_data), flags_(0), page_level_(0), n_records_(0),
      next_page_(0xFFFFFFFF), prev_page_(0xFFFFFFFF), node_type_(BTreeNodeType::LEAF) {
}

bool BPlusTreeParser::parse() noexcept {
    if (page_data_.size() < 38) {
        return false;
    }
    
    // 解析页类型
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), FIL_PAGE_TYPE);
    
    if (page_type != FIL_PAGE_INDEX && page_type != FIL_PAGE_SDI) {
        return false;
    }
    
    // 解析B+树头
    if (!parse_header()) {
        return false;
    }
    
    // 解析INFIMUM和SUPREMUM记录
    if (!parse_infimum_supremum()) {
        return false;
    }
    
    // 解析用户记录
    parse_user_records();
    
    return true;
}

bool BPlusTreeParser::parse_header() noexcept {
    if (page_data_.size() < 50) {
        return false;
    }
    
    // 读取B+树头信息
    page_level_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), BTR_N_LEVELS);
    flags_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), BTR_N_LEVELS + 2);
    
    // 判断节点类型
    if (page_level_ == 0) {
        node_type_ = BTreeNodeType::LEAF;
    } else {
        node_type_ = BTreeNodeType::INTERNAL;
    }
    
    // 读取页链信息
    prev_page_ = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), 26);
    next_page_ = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), 30);
    
    return true;
}

bool BPlusTreeParser::parse_infimum_supremum() noexcept {
    if (page_data_.size() < 100) {
        return false;
    }
    
    // INFIMUM 记录偏移（固定为54）
    uint16_t infimum_offset = 54;
    if (infimum_offset + 13 > page_data_.size()) {
        return false;
    }
    
    // SUPREMUM 记录偏移
    uint16_t supremum_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), infimum_offset + 1);
    if (supremum_offset == 0 || supremum_offset >= page_data_.size()) {
        return false;
    }
    
    // 记录偏移列表
    record_offsets_.push_back(infimum_offset);
    record_offsets_.push_back(supremum_offset);
    
    return true;
}

bool BPlusTreeParser::parse_user_records() noexcept {
    if (page_data_.size() < 100) {
        return false;
    }
    
    // 解析页目录槽
    uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), PAGE_N_DIR_SLOTS + 38);
    if (n_dir_slots < 2) { // 至少有INFIMUM和SUPREMUM
        return false;
    }
    
    // 解析每个记录偏移
    uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), PAGE_HEAP_TOP + 38);
    n_records_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), PAGE_N_HEAP + 38) & 0x7FFF;
    
    // 从页目录中解析记录偏移
    uint16_t dir_slot_offset = 38 + 36; // PAGE_DIRECTORY 开始位置
    for (uint16_t i = 0; i < n_dir_slots; ++i) {
        uint16_t slot_pos = dir_slot_offset + i * 2;
        if (slot_pos + 2 > page_data_.size()) {
            break;
        }
        
        uint16_t record_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), slot_pos);
        if (record_offset > 0 && record_offset < heap_top) {
            // 跳过INFIMUM和SUPREMUM
            if (record_offset != 54 && record_offset != dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 55)) {
                record_offsets_.push_back(record_offset);
            }
        }
    }
    
    return true;
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
    
    // 解析页类型
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), FIL_PAGE_TYPE);
    if (page_type != FIL_PAGE_INDEX && page_type != FIL_PAGE_SDI) {
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
    }
    
    // 解析页目录
    uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), PAGE_N_DIR_SLOTS + 38);
    health.n_dir_slots = n_dir_slots;
    
    // 解析堆大小
    uint16_t n_heap = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), PAGE_N_HEAP + 38) & 0x7FFF;
    health.n_records = n_heap - 2; // 减去INFIMUM和SUPREMUM
    
    return health;
}

uint64_t BPlusTreeParser::get_sdi_table_id() const {
    if (page_data_.size() < 58) {
        return 0;
    }
    
    // SDI页的表ID在偏移38+20处
    return dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), 58);
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
    
    // 解析记录（这里需要表结构，暂时返回）
    records_ = parser.parse_records(catalog::TableSchema());
    
    return true;
}

} // namespace innodb
} // namespace dbrecovery
