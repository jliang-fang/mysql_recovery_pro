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

std::string parse_varchar_value(const uint8_t* data, size_t& pos, size_t max_len, bool is_nullable) {
    if (is_nullable && pos < max_len && data[pos] == 0xFF) {
        pos++;
        return "NULL";
    }
    
    // 边界检查：确保至少有一个字节用于长度前缀
    if (pos >= max_len) {
        return "";
    }
    
    // 读取长度前缀（1字节或2字节）
    uint8_t len_byte = data[pos];
    pos++;
    
    size_t str_len = len_byte;
    if (len_byte >= 0xFE) {
        // 2字节长度前缀
        if (pos + 1 < max_len) {
            str_len = static_cast<size_t>(data[pos]) | (static_cast<size_t>(data[pos + 1]) << 8);
            pos += 2;
        } else {
            return "";
        }
    }
    
    // 边界检查：确保有足够的数据用于字符串
    if (pos + str_len > max_len) {
        return "";
    }
    
    // 读取字符串数据
    std::string result;
    for (size_t i = 0; i < str_len && pos < max_len; ++i) {
        result += static_cast<char>(data[pos]);
        pos++;
    }
    
    return result;
}

std::string parse_int_value(const uint8_t* data, size_t& pos, size_t max_len, size_t field_size, bool is_signed, bool is_nullable) {
    if (is_nullable && pos < max_len && data[pos] == 0xFF) {
        pos++;
        return "NULL";
    }
    
    std::ostringstream oss;
    if (field_size == 1) {
        if (pos >= max_len) {
            return "";
        }
        uint8_t val = data[pos];
        pos++;
        if (is_signed) {
            oss << static_cast<int8_t>(val);
        } else {
            oss << static_cast<uint8_t>(val);
        }
    } else if (field_size == 2) {
        if (pos + 1 >= max_len) {
            return "";
        }
        uint16_t val = dbrecovery::innodb::read_le<uint16_t>(data, pos);
        pos += 2;
        if (is_signed) {
            oss << static_cast<int16_t>(val);
        } else {
            oss << val;
        }
    } else if (field_size == 4) {
        if (pos + 3 >= max_len) {
            return "";
        }
        uint32_t val = dbrecovery::innodb::read_le<uint32_t>(data, pos);
        pos += 4;
        if (is_signed) {
            oss << static_cast<int32_t>(val);
        } else {
            oss << val;
        }
    } else if (field_size == 8) {
        if (pos + 7 >= max_len) {
            return "";
        }
        uint64_t val = dbrecovery::innodb::read_le<uint64_t>(data, pos);
        pos += 8;
        if (is_signed) {
            oss << static_cast<int64_t>(val);
        } else {
            oss << val;
        }
    }
    
    return oss.str();
}

// ==================== BPlusTreeParser 实现 ====================

BPlusTreeParser::BPlusTreeParser(const std::vector<uint8_t>& page_data)
    : page_data_(page_data), 
      flags_(0), 
      page_level_(0), 
      n_records_(0),
      next_page_(0xFFFFFFFF), 
      prev_page_(0xFFFFFFFF), 
      node_type_(BTreeNodeType::INVALID) {
}

bool BPlusTreeParser::parse() noexcept {
    if (page_data_.size() < 38) {
        return false;
    }
    
    // ✅ 修复：使用正确的偏移计算页类型
    // FIL_PAGE_TYPE 在 page_defs.hpp 中定义为 12，但实际偏移是 38+12=50
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + dbrecovery::innodb::FIL_PAGE_TYPE);
    
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX && 
        page_type != dbrecovery::innodb::FIL_PAGE_SDI) {
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
    
    // ✅ 修复：使用正确的偏移计算
    // BTR_N_LEVELS = 38, flags 在 BTR_N_LEVELS + 2 = 40
    page_level_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + BTR_N_LEVELS);
    flags_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + BTR_N_LEVELS + 2);
    
    // 判断节点类型
    if (page_level_ == 0) {
        node_type_ = BTreeNodeType::LEAF;
    } else {
        node_type_ = BTreeNodeType::INTERNAL;
    }
    
    // 读取页链信息（使用正确的偏移）
    prev_page_ = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), 38 + 26);
    next_page_ = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), 38 + 30);
    
    return true;
}

bool BPlusTreeParser::parse_infimum_supremum() noexcept {
    if (page_data_.size() < 100) {
        return false;
    }
    
    // INFIMUM 记录偏移（固定为54）
    infimum_offset_ = 54;
    if (infimum_offset_ + 13 > page_data_.size()) {
        return false;
    }
    
    // SUPREMUM 记录偏移
    supremum_offset_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), infimum_offset_ + 1);
    if (supremum_offset_ == 0 || supremum_offset_ >= page_data_.size()) {
        return false;
    }
    
    // 记录偏移列表
    record_offsets_.push_back(infimum_offset_);
    record_offsets_.push_back(supremum_offset_);
    
    return true;
}

bool BPlusTreeParser::parse_user_records() noexcept {
    if (page_data_.size() < 100) {
        return false;
    }
    
    // 解析页目录槽
    uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dbrecovery::innodb::PAGE_N_DIR_SLOTS + 38);
    if (n_dir_slots < 2) { // 至少有INFIMUM和SUPREMUM
        return false;
    }
    
    // 解析每个记录偏移
    uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dbrecovery::innodb::PAGE_HEAP_TOP + 38);
    n_records_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dbrecovery::innodb::PAGE_N_HEAP + 38) & 0x7FFF;
    
    // 从页目录中解析记录偏移
    uint16_t dir_slot_offset = 38 + 36; // PAGE_DIRECTORY 开始位置
    for (uint16_t i = 0; i < n_dir_slots; ++i) {
        uint16_t slot_pos = dir_slot_offset + i * 2;
        if (slot_pos + 2 > page_data_.size()) {
            std::cerr << "[WARN] BPlusTreeParser: directory slot position out of bounds" << std::endl;
            break;
        }
        
        uint16_t record_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), slot_pos);
        // ✅ 增强：添加更严格的边界检查和验证
        if (record_offset < 38 + 16 || record_offset >= page_data_.size()) {
            std::cerr << "[WARN] BPlusTreeParser: invalid record offset=" << record_offset 
                      << " (min=38+16, max=" << page_data_.size() << ")" << std::endl;
            continue;
        }
        
        if (record_offset >= heap_top) {
            std::cerr << "[WARN] BPlusTreeParser: record offset=" << record_offset 
                      << " exceeds heap_top=" << heap_top << std::endl;
            continue;
        }
        
        // 跳过INFIMUM和SUPREMUM（使用已存储的值）
        if (record_offset != infimum_offset_ && record_offset != supremum_offset_) {
            record_offsets_.push_back(record_offset);
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
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dbrecovery::innodb::FIL_PAGE_TYPE);
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
        uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dbrecovery::innodb::PAGE_N_DIR_SLOTS + 38);
        health.n_dir_slots = n_dir_slots;
        
        // 验证页目录槽数量合理性（至少有INFIMUM和SUPREMUM）
        if (n_dir_slots < 2) {
            health.status_message += "Insufficient dir slots; ";
            health.is_valid = false;
        }
        
        // ✅ 新增：计算空闲空间
        uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dbrecovery::innodb::PAGE_HEAP_TOP + 38);
        uint16_t free_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dbrecovery::innodb::PAGE_FREE + 38);
        if (heap_top >= free_offset) {
            health.free_space = heap_top - free_offset;
        }
    } else {
        health.status_message += "Page header incomplete; ";
        health.is_valid = false;
    }
    
    // 解析堆大小
    if (page_data_.size() >= 38 + 4) { // 确保有足够空间读取PAGE_N_HEAP
        uint16_t n_heap = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dbrecovery::innodb::PAGE_N_HEAP + 38) & 0x7FFF;
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

// 已删除重复的 parse_field_value 实现

uint64_t BPlusTreeParser::get_sdi_table_id() const {
    if (page_data_.size() < 58) {
        return 0;
    }
    
    // SDI页的表ID在偏移38+20处
    uint64_t table_id = dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), 38 + 20);
    return table_id;
}

// ✅ 新增：CRC32C 校验和计算（供页健康检查使用）
// 注意：此函数已移动到 page_reader.hpp，此处删除重复定义

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
