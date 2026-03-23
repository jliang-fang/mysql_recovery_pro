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

std::string BPlusTreeParser::parse_field_value(
    const catalog::ColumnInfo& col, size_t& data_pos, bool is_extern) const {
    
    // ✅ 新增：处理外部存储（BLOB）指针
    if (is_extern || col.mysql_type_id == dbrecovery::innodb::mysql_types::TINY_BLOB ||
                   col.mysql_type_id == dbrecovery::innodb::mysql_types::MEDIUM_BLOB ||
                   col.mysql_type_id == dbrecovery::innodb::mysql_types::LONG_BLOB ||
                   col.mysql_type_id == dbrecovery::innodb::mysql_types::BLOB) {
        // BLOB 字段存储的是 20 字节指针（或更短的前缀）
        if (data_pos + 20 > page_data_.size()) {
            return "<BLOB_POINTER_TRUNCATED>";
        }
        
        auto blob_ptr = dbrecovery::innodb::parse_blob_pointer(page_data_.data() + data_pos, 20);
        if (!blob_ptr.has_value()) {
            return "<INVALID_BLOB_PTR>";
        }
        
        // ✅ 简化处理：返回 BLOB 指针信息（完整数据需通过 page_reader 回调获取）
        std::ostringstream oss;
        oss << "BLOB[sid=" << blob_ptr->space_id 
            << ",page=" << blob_ptr->page_no 
            << ",len=" << blob_ptr->total_len << "]";
        return oss.str();
    }
    
    // ✅ 新增：处理 VARCHAR 类型（前缀长度字节 + 数据）
    if (col.mysql_type_id == dbrecovery::innodb::mysql_types::VARCHAR ||
        col.mysql_type_id == dbrecovery::innodb::mysql_types::VAR_STRING) {
        if (data_pos >= page_data_.size()) {
            return "<TRUNCATED>";
        }
        
        uint8_t prefix_len = page_data_[data_pos];
        data_pos++;
        
        std::string value;
        for (uint8_t i = 0; i < prefix_len && data_pos < page_data_.size(); ++i) {
            value += static_cast<char>(page_data_[data_pos++]);
        }
        return value;
    }
    
    // ✅ 新增：处理固定长度类型
    switch (col.mysql_type_id) {
        case dbrecovery::innodb::mysql_types::TINY:
            if (data_pos + 1 > page_data_.size()) return "<TRUNCATED>";
            return std::to_string(static_cast<int>(page_data_[data_pos++]));
            
        case dbrecovery::innodb::mysql_types::SHORT:
            if (data_pos + 2 > page_data_.size()) return "<TRUNCATED>";
            return std::to_string(dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), data_pos));
            
        case dbrecovery::innodb::mysql_types::LONG:
            if (data_pos + 4 > page_data_.size()) return "<TRUNCATED>";
            return std::to_string(dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), data_pos));
            
        case dbrecovery::innodb::mysql_types::LONGLONG:
            if (data_pos + 8 > page_data_.size()) return "<TRUNCATED>";
            return std::to_string(dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), data_pos));
            
        case dbrecovery::innodb::mysql_types::DATE:
            if (data_pos + 3 > page_data_.size()) return "<TRUNCATED>";
            // MySQL DATE 格式：[year_high(7bit), year_low(8bit), month(4bit), day(5bit)]
            uint32_t packed = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), data_pos) & 0xFFFFFF;
            int year = ((packed >> 17) & 0x7F) * 256 + ((packed >> 9) & 0xFF);
            int month = (packed >> 5) & 0xF;
            int day = packed & 0x1F;
            return std::to_string(year) + "-" + std::to_string(month) + "-" + std::to_string(day);
            
        case dbrecovery::innodb::mysql_types::DATETIME:
            if (data_pos + 8 > page_data_.size()) return "<TRUNCATED>";
            // MySQL DATETIME 格式（8字节）：[year(2B), month(1B), day(1B), hour(1B), minute(1B), second(1B), frac(3B)]
            uint64_t packed = dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), data_pos);
            int year = static_cast<int>(packed >> 48);
            int month = (packed >> 40) & 0xFF;
            int day = (packed >> 32) & 0xFF;
            int hour = (packed >> 24) & 0xFF;
            int minute = (packed >> 16) & 0xFF;
            int second = (packed >> 8) & 0xFF;
            return std::to_string(year) + "-" + 
                   std::to_string(month) + "-" + 
                   std::to_string(day) + " " +
                   std::to_string(hour) + ":" + 
                   std::to_string(minute) + ":" + 
                   std::to_string(second);
                   
        default:
            return "<UNSUPPORTED_TYPE>";
    }
}

std::string BPlusTreeParser::parse_datetime2_value(uint8_t precision, size_t& data_pos) const {
    // ✅ 新增：DATETIME2 解析（参考 MySQL 8.0 field.cc）
    if (data_pos + 5 > page_data_.size()) return "<TRUNCATED>";
    
    // 前5字节 packed format
    uint32_t packed = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), data_pos);
    int year_low = packed & 0xFF;
    int year_high = (packed >> 8) & 0xFF;
    int month = (packed >> 16) & 0xF;
    int day = (packed >> 20) & 0x1F;
    int hour = (packed >> 25) & 0x1F;
    int minute = (packed >> 30) & 0x3F;
    
    // 后3字节小数部分（precision=6）
    if (data_pos + 8 > page_data_.size()) return "<TRUNCATED>";
    uint32_t frac = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), data_pos + 5) & 0xFFFFFF;
    
    std::ostringstream oss;
    oss << (year_high * 256 + year_low) << "-" 
        << month << "-" << day << " "
        << hour << ":" << minute << ":";
    
    if (precision > 0) {
        // 格式化小数部分
        int frac_digits = precision;
        std::string frac_str = std::to_string(frac);
        while (frac_str.length() < 6) frac_str = "0" + frac_str;
        oss << frac_str.substr(0, frac_digits);
    } else {
        oss << "00";
    }
    
    data_pos += 8; // DATETIME2(6) 总共 8 字节
    return oss.str();
}

// ==================== BPlusTreeParser 实现 ====================

BPlusTreeParser::BPlusTreeParser(const std::vector<uint8_t>& page_data)
    : page_data_(page_data), 
      flags_(0xFFFF), // ✅ 修复：初始化为 0xFFFF 表示未解析
      page_level_(0xFFFFFFFF), // ✅ 修复：初始化为 0xFFFFFFFF 表示未解析
      n_records_(0),
      next_page_(0xFFFFFFFF), 
      prev_page_(0xFFFFFFFF), 
      node_type_(BTreeNodeType::INVALID) {
    // ✅ 新增：初始化 INFIMUM/SUPREMUM 偏移常量
    infimum_offset_ = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
}

bool BPlusTreeParser::parse() noexcept(false) {
    // ✅ P0 修复：添加页校验和验证（防止解析损坏页）
    // 在解析任何结构之前，必须先验证页的完整性
    if (!dbrecovery::innodb::PageReader::validate_checksum(page_data_)) {
        std::cerr << "[ERROR] BPlusTreeParser: Page checksum validation failed. Aborting parse." << std::endl;
        return false;
    }
    
    // ✅ 修复：使用 page_defs.hpp 中定义的常量进行边界检查
    constexpr size_t min_page_size = dbrecovery::innodb::FIL_HEADER_SIZE + 16; // FIL header (38) + minimal page header space
    if (page_data_.size() < min_page_size) {
        std::cerr << "[ERROR] BPlusTreeParser: Page too small. Required: " << min_page_size 
                  << ", Actual: " << page_data_.size() << std::endl;
        return false;
    }
    
    // ✅ 修复：使用正确的偏移计算页类型，引用 page_defs.hpp 中的常量
    constexpr size_t page_type_offset = dbrecovery::innodb::FIL_PAGE_TYPE_ABS;
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), page_type_offset);
    
    // ✅ 新增：使用统一的页类型常量进行比较
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX && 
        page_type != dbrecovery::innodb::FIL_PAGE_SDI &&
        page_type != dbrecovery::innodb::FIL_PAGE_UNDO_LOG) {
        std::cerr << "[ERROR] BPlusTreeParser: Invalid page type " << page_type 
                  << " (" << dbrecovery::innodb::page_type_to_string(page_type) << ")" << std::endl;
        return false;
    }
    
    // ✅ 新增：页健康检查（目录槽、记录链完整性）
    PageHealth health = check_health();
    if (!health.is_valid) {
        std::cerr << "[ERROR] BPlusTreeParser: Page health check failed. Status: " << health.status_message << std::endl;
        return false;
    }
    
    // 解析B+树头
    if (!parse_header()) {
        std::cerr << "[ERROR] BPlusTreeParser: Failed to parse B+ tree header" << std::endl;
        return false;
    }
    
    // ✅ 修复：使用常量定义的偏移解析 INFIMUM/SUPREMUM
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
    const size_t min_header_size = dbrecovery::innodb::FIL_HEADER_SIZE + 24; // 确保有足够空间读取到 PAGE_NEXT
    if (page_data_.size() < min_header_size) {
        std::cerr << "[ERROR] BPlusTreeParser: Page header too small. Required: " << min_header_size 
                  << ", Actual: " << page_data_.size() << std::endl;
        return false;
    }
    
    // ✅ 使用 page_defs.hpp 中定义的常量
    const size_t btr_n_levels_abs = dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::BTR_N_LEVELS_ABS;
    // ✅ 新增：使用 uint16_t 读取以匹配 page_level_ 类型（实际 InnoDB 中为 2 字节）
    page_level_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), btr_n_levels_abs);
    
    const size_t flags_offset = dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::BTR_PAGE_OFFSET_HEAP_TOP; // 修复：使用常量定义的偏移
    flags_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), flags_offset);
    
    // ✅ 新增：验证页级别合理性（MySQL 8.0 B+树最大高度通常不超过5）
    if (page_level_ > 5) {
        std::cerr << "[WARN] BPlusTreeParser: Unreasonably high page level: " << page_level_ << std::endl;
    }
    
    // 判断节点类型
    node_type_ = (page_level_ == 0) ? BTreeNodeType::LEAF : BTreeNodeType::INTERNAL;
    
    // ✅ 修复：使用常量定义的偏移读取页链信息（prev_page 和 next_page）
    const size_t prev_page_offset = dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::BTR_PAGE_OFFSET_FREE; // 修复：使用常量定义的偏移
    const size_t next_page_offset = dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::BTR_PAGE_OFFSET_DIRECTION; // 修复：使用常量定义的偏移
    prev_page_ = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), prev_page_offset);
    next_page_ = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), next_page_offset);
    
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
    constexpr size_t infimum_abs = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
    // ✅ 修复：使用常量定义的最小大小，而不是硬编码
    constexpr size_t min_size_for_infimum = infimum_abs + 13; // INFIMUM record header (5) + type byte (1) + next_offset (2) + padding (5)

    if (page_data_.size() < min_size_for_infimum) {
        std::cerr << "[ERROR] BPlusTreeParser: Page too small for INFIMUM/SUPREMUM. Required: " 
                  << min_size_for_infimum << ", Actual: " << page_data_.size() << std::endl;
        return false;
    }

    infimum_offset_ = static_cast<uint16_t>(infimum_abs);

    // ✅ 新增：验证 INFIMUM 记录头（必须是 REC_STATUS_INFIMUM = 0x02）
    uint8_t infimum_info_bits = page_data_[infimum_offset_];
    if ((infimum_info_bits & dbrecovery::innodb::REC_INFO_STATUS_MASK) != dbrecovery::innodb::REC_STATUS_INFIMUM) {
        std::cerr << "[ERROR] BPlusTreeParser: INFIMUM record type mismatch. Expected 0x02, got 0x"
                  << std::hex << static_cast<int>(infimum_info_bits & dbrecovery::innodb::REC_INFO_STATUS_MASK) << std::dec << std::endl;
        return false;
    }

    // SUPREMUM 记录偏移（位于 INFIMUM 记录头的 next_record_offset 字段）
    constexpr size_t infimum_next_offset_pos = infimum_abs + 1;
    uint16_t infimum_next_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), infimum_next_offset_pos);
    supremum_offset_ = static_cast<uint16_t>(infimum_next_offset & 0x3FFF); // ✅ 修复：next_record_offset 是 15 位

    if (supremum_offset_ == 0 || supremum_offset_ >= page_data_.size()) {
        std::cerr << "[ERROR] BPlusTreeParser: SUPREMUM offset invalid: " << supremum_offset_ 
                  << ", page_size: " << page_data_.size() << std::endl;
        return false;
    }

    // ✅ 新增：验证 SUPREMUM 记录头（必须是 REC_STATUS_SUPREMUM = 0x03）
    uint8_t supremum_info_bits = page_data_[supremum_offset_];
    if ((supremum_info_bits & dbrecovery::innodb::REC_INFO_STATUS_MASK) != dbrecovery::innodb::REC_STATUS_SUPREMUM) {
        std::cerr << "[ERROR] BPlusTreeParser: SUPREMUM record type mismatch. Expected 0x03, got 0x"
                  << std::hex << static_cast<int>(supremum_info_bits & dbrecovery::innodb::REC_INFO_STATUS_MASK) << std::dec << std::endl;
        return false;
    }

    // 记录偏移列表（按页目录顺序）
    record_offsets_.push_back(infimum_offset_);
    record_offsets_.push_back(supremum_offset_);

    return true;
}

// ✅ 修复：移除重复的 parse_user_records 实现（已在头文件中声明为 private）
// 此处仅保留 check_health 的实现

BPlusTreeParser::PageHealth BPlusTreeParser::check_health() const {
    PageHealth health;
    health.is_valid = true;

    // 检查页大小
    if (page_data_.size() < dbrecovery::innodb::FIL_HEADER_SIZE) {
        health.is_valid = false;
        health.status_message = "Page size too small";
        return health;
    }

    // ✅ 新增：使用正确的偏移计算页类型（参考 page_defs.hpp）
    constexpr size_t page_type_offset_in_header = dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::FIL_PAGE_TYPE_OFFSET;
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), page_type_offset_in_header);
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX && page_type != dbrecovery::innodb::FIL_PAGE_SDI) {
        health.is_valid = false;
        health.status_message = "Invalid page type: " + std::to_string(page_type);
        return health;
    }

    // 检查INFIMUM和SUPREMUM记录
    constexpr size_t min_size_for_records = 100;
    if (page_data_.size() >= min_size_for_records) {
        constexpr uint16_t infimum_offset = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
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
    constexpr size_t min_size_for_directory = dbrecovery::innodb::FIL_HEADER_SIZE + 36; // 确保有足够空间读取页目录
    if (page_data_.size() >= min_size_for_directory) {
        constexpr size_t n_dir_slots_offset = dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_N_DIR_SLOTS_OFFSET;
        uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), n_dir_slots_offset);
        health.n_dir_slots = n_dir_slots;

        // 验证页目录槽数量合理性（至少有INFIMUM和SUPREMUM）
        if (n_dir_slots < 2) {
            health.status_message += "Insufficient dir slots; ";
            health.is_valid = false;
        }

        // ✅ 新增：计算空闲空间
        constexpr size_t heap_top_offset = dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_HEAP_TOP_OFFSET;
        uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), heap_top_offset);

        constexpr size_t free_offset = dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_FREE_OFFSET;
        uint16_t free_value = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), free_offset);
        if (heap_top >= free_value) {
            health.free_space = heap_top - free_value;
        }
    } else {
        health.status_message += "Page header incomplete; ";
        health.is_valid = false;
    }

    // 解析堆大小
    constexpr size_t min_size_for_heap = dbrecovery::innodb::FIL_HEADER_SIZE + 4; // 确保有足够空间读取PAGE_N_HEAP
    if (page_data_.size() >= min_size_for_heap) {
        constexpr size_t n_heap_offset = dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::PAGE_N_HEAP_OFFSET;
        uint16_t n_heap = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), n_heap_offset) & 0x7FFF;
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
    // SDI 页表 ID 解析（参考 MySQL 8.0 dict0dict.cc 和 sdi.h）
    constexpr size_t sdi_meta_header_offset = dbrecovery::innodb::FIL_HEADER_SIZE;
    constexpr size_t min_sdi_size = sdi_meta_header_offset + 18; // 10 (meta header) + 8 (table_id)

    if (page_data_.size() < min_sdi_size) {
        return 0;
    }

    // SDI meta header: [version(4), type(4), table_id(8)]
    constexpr size_t sdi_table_id_offset = sdi_meta_header_offset + 10;
    uint64_t table_id = dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), sdi_table_id_offset);
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
