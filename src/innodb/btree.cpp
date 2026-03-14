#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <iostream>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace dbrecovery {
namespace innodb {

// 使用 page_defs.hpp 中定义的常量，避免重复定义
// #include "dbrecovery/innodb/page_defs.hpp" 已在文件开头包含

// ==================== 辅助函数：字段值解析 ====================

std::string parse_varchar_value(const uint8_t* data, size_t& pos, size_t max_len, bool is_nullable) {
    if (is_nullable && pos < max_len && data[pos] == 0xFF) {
        pos++;
        return "NULL";
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
    
    // 读取字符串数据
    std::string result;
    for (size_t i = 0; i < str_len && pos < max_len; ++i) {
        result += static_cast<char>(data[pos]);
        pos++;
    }
    
    return result;
}

std::string parse_int_value(const uint8_t* data, size_t& pos, size_t field_size, bool is_signed, bool is_nullable) {
    if (is_nullable && pos < max_len && data[pos] == 0xFF) {
        pos++;
        return "NULL";
    }
    
    std::ostringstream oss;
    if (field_size == 1) {
        uint8_t val = data[pos];
        pos++;
        if (is_signed) {
            oss << static_cast<int8_t>(val);
        } else {
            oss << static_cast<uint8_t>(val);
        }
    } else if (field_size == 2) {
        uint16_t val = dbrecovery::innodb::read_le<uint16_t>(data, pos);
        pos += 2;
        if (is_signed) {
            oss << static_cast<int16_t>(val);
        } else {
            oss << val;
        }
    } else if (field_size == 4) {
        uint32_t val = dbrecovery::innodb::read_le<uint32_t>(data, pos);
        pos += 4;
        if (is_signed) {
            oss << static_cast<int32_t>(val);
        } else {
            oss << val;
        }
    } else if (field_size == 8) {
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
    
    // 解析页类型（使用 page_defs.hpp 中定义的常量）
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dbrecovery::innodb::FIL_PAGE_TYPE);
    
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
            break;
        }
        
        uint16_t record_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), slot_pos);
        // ✅ 修复：添加更严格的边界检查
        if (record_offset >= 38 + 16 && record_offset < heap_top) {
            // 跳过INFIMUM(54)和SUPREMUM（需要动态获取）
            if (record_offset != 54) {
                // 检查是否为SUPREMUM
                uint16_t supremum_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 55);
                if (record_offset != supremum_offset) {
                    record_offsets_.push_back(record_offset);
                }
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

std::string BPlusTreeParser::parse_field_value(
    const catalog::ColumnInfo& col,
    size_t& data_pos,
    bool is_extern) const {
    
    // 检查是否为 NULL
    if (col.is_nullable && data_pos < page_data_.size() && page_data_[data_pos] == 0xFF) {
        data_pos++;
        return "NULL";
    }
    
    switch (col.mysql_type_id) {
        case mysql_types::TINY:
            return parse_int_value(page_data_.data(), data_pos, 1, col.is_signed, false);
            
        case mysql_types::SHORT:
            return parse_int_value(page_data_.data(), data_pos, 2, col.is_signed, false);
            
        case mysql_types::LONG:
            return parse_int_value(page_data_.data(), data_pos, 4, col.is_signed, false);
            
        case mysql_types::LONGLONG:
            return parse_int_value(page_data_.data(), data_pos, 8, col.is_signed, false);
            
        case mysql_types::INT24:
            // INT24 占用3字节，但读取时需要4字节
            if (col.is_nullable && data_pos < page_data_.size() && page_data_[data_pos] == 0xFF) {
                data_pos++;
                return "NULL";
            }
            if (data_pos + 3 > page_data_.size()) {
                return "";
            }
            uint32_t val24 = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), data_pos) & 0xFFFFFF;
            data_pos += 3;
            if (col.is_signed && (val24 & 0x800000)) {
                val24 |= 0xFF000000; // 符号扩展
            }
            return std::to_string(static_cast<int32_t>(val24));
            
        case mysql_types::FLOAT:
            if (col.is_nullable && data_pos < page_data_.size() && page_data_[data_pos] == 0xFF) {
                data_pos++;
                return "NULL";
            }
            if (data_pos + 4 > page_data_.size()) {
                return "";
            }
            // 简单转换为字符串（实际应使用 IEEE 754 解析）
            uint32_t float_val = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), data_pos);
            data_pos += 4;
            return std::to_string(*reinterpret_cast<float*>(&float_val));
            
        case mysql_types::DOUBLE:
            if (col.is_nullable && data_pos < page_data_.size() && page_data_[data_pos] == 0xFF) {
                data_pos++;
                return "NULL";
            }
            if (data_pos + 8 > page_data_.size()) {
                return "";
            }
            uint64_t double_val = dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), data_pos);
            data_pos += 8;
            return std::to_string(*reinterpret_cast<double*>(&double_val));
            
        case mysql_types::VARCHAR:
        case mysql_types::VAR_STRING: {
            // 对于 VARCHAR，需要读取长度前缀
            if (col.is_nullable && data_pos < page_data_.size() && page_data_[data_pos] == 0xFF) {
                data_pos++;
                return "NULL";
            }
            
            // 读取长度前缀（1字节或2字节）
            if (data_pos >= page_data_.size()) {
                return "";
            }
            
            uint8_t len_byte = page_data_[data_pos];
            data_pos++;
            
            size_t str_len = len_byte;
            if (len_byte >= 0xFE) {
                // 2字节长度前缀
                if (data_pos + 1 < page_data_.size()) {
                    str_len = static_cast<size_t>(page_data_[data_pos]) | 
                             (static_cast<size_t>(page_data_[data_pos + 1]) << 8);
                    data_pos += 2;
                } else {
                    return "";
                }
            }
            
            // 读取字符串数据
            std::string result;
            for (size_t i = 0; i < str_len && data_pos < page_data_.size(); ++i) {
                result += static_cast<char>(page_data_[data_pos]);
                data_pos++;
            }
            
            return result;
        }
        
        case mysql_types::DATE:
            if (col.is_nullable && data_pos < page_data_.size() && page_data_[data_pos] == 0xFF) {
                data_pos++;
                return "NULL";
            }
            if (data_pos + 3 > page_data_.size()) {
                return "";
            }
            // DATE 格式：[year_high][year_low][month][day]
            uint8_t year_high = page_data_[data_pos];
            uint8_t year_low = page_data_[data_pos + 1];
            uint8_t month = page_data_[data_pos + 2];
            uint8_t day = page_data_[data_pos + 3];
            data_pos += 4;
            
            int year = (year_high << 8) | year_low;
            return std::to_string(year) + "-" + 
                   std::to_string(month) + "-" + 
                   std::to_string(day);
        
        case mysql_types::DATETIME:
        case mysql_types::TIMESTAMP:
            if (col.is_nullable && data_pos < page_data_.size() && page_data_[data_pos] == 0xFF) {
                data_pos++;
                return "NULL";
            }
            if (data_pos + 5 > page_data_.size()) {
                return "";
            }
            // DATETIME 格式（MySQL 5.6+）：[year_high][year_low][month][day][hour][minute][second]
            uint8_t year_high = page_data_[data_pos];
            uint8_t year_low = page_data_[data_pos + 1];
            uint8_t month = page_data_[data_pos + 2];
            uint8_t day = page_data_[data_pos + 3];
            uint8_t hour = page_data_[data_pos + 4];
            uint8_t minute = page_data_[data_pos + 5];
            uint8_t second = page_data_[data_pos + 6];
            data_pos += 7;
            
            int year = (year_high << 8) | year_low;
            return std::to_string(year) + "-" + 
                   std::to_string(month) + "-" + 
                   std::to_string(day) + " " +
                   std::to_string(hour) + ":" +
                   std::to_string(minute) + ":" +
                   std::to_string(second);
        
        case mysql_types::DATETIME2:
        case mysql_types::TIMESTAMP2: {
            // DATETIME2 格式（MySQL 5.6+）
            if (col.is_nullable && data_pos < page_data_.size() && page_data_[data_pos] == 0xFF) {
                data_pos++;
                return "NULL";
            }
            
            int datetime_precision = col.datetime_precision;
            if (datetime_precision < 0) datetime_precision = 6; // 默认精度
            
            // 前5字节：packed date/time
            if (data_pos + 5 > page_data_.size()) {
                return "";
            }
            
            uint8_t year_low = page_data_[data_pos];
            uint8_t year_high = page_data_[data_pos + 1];
            uint8_t month_day = page_data_[data_pos + 2];
            uint8_t hour_minute = page_data_[data_pos + 3];
            uint8_t second_packed = page_data_[data_pos + 4];
            
            int year = (year_high << 8) | year_low;
            int month = month_day >> 4;
            int day = month_day & 0x0F;
            int hour = hour_minute & 0x1F;
            int minute = hour_minute >> 2;
            int second = second_packed >> 2;
            
            data_pos += 5;
            
            std::ostringstream oss;
            oss << year << "-" << month << "-" << day << " " 
                << hour << ":" << minute << ":" << second;
            
            // 小数部分（根据精度）
            if (datetime_precision > 0) {
                size_t frac_bytes = (datetime_precision + 1) / 2;
                if (data_pos + frac_bytes <= page_data_.size()) {
                    uint64_t frac_val = 0;
                    // ✅ 修复：MySQL DATETIME2 小数部分使用大端序存储（高位在前）
                    for (size_t i = 0; i < frac_bytes; ++i) {
                        frac_val = (frac_val << 8) | page_data_[data_pos + i];
                    }
                    
                    // 转换为微秒
                    double frac_sec = static_cast<double>(frac_val) / 
                                     std::pow(10, datetime_precision);
                    oss << std::fixed << std::setprecision(datetime_precision) << frac_sec;
                }
                data_pos += frac_bytes;
            }
            
            return oss.str();
        }
        
        case mysql_types::TIME:
        case mysql_types::TIME2: {
            if (col.is_nullable && data_pos < page_data_.size() && page_data_[data_pos] == 0xFF) {
                data_pos++;
                return "NULL";
            }
            
            // TIME 格式：[sign][hours][minutes][seconds]
            if (data_pos + 3 > page_data_.size()) {
                return "";
            }
            
            bool is_negative = (page_data_[data_pos] & 0x80) != 0;
            uint8_t hours = page_data_[data_pos + 1];
            uint8_t minutes = page_data_[data_pos + 2];
            uint8_t seconds = page_data_[data_pos + 3];
            data_pos += 4;
            
            std::ostringstream oss;
            if (is_negative) oss << "-";
            oss << hours << ":" << minutes << ":" << seconds;
            return oss.str();
        }
        
        case mysql_types::YEAR:
            if (col.is_nullable && data_pos < page_data_.size() && page_data_[data_pos] == 0xFF) {
                data_pos++;
                return "NULL";
            }
            if (data_pos >= page_data_.size()) {
                return "";
            }
            uint8_t year = page_data_[data_pos];
            data_pos++;
            // YEAR 格式：0=0000, 1-99=1901-2000, 100-999=2001-2999
            int actual_year = (year == 0) ? 0 : (year < 100) ? (1900 + year) : (year + 1900);
            return std::to_string(actual_year);
        
        case mysql_types::BIT:
            if (col.is_nullable && data_pos < page_data_.size() && page_data_[data_pos] == 0xFF) {
                data_pos++;
                return "NULL";
            }
            
            size_t bit_bytes = (col.column_length + 7) / 8;
            if (data_pos + bit_bytes > page_data_.size()) {
                return "";
            }
            
            uint64_t bit_val = 0;
            for (size_t i = 0; i < bit_bytes; ++i) {
                bit_val |= (static_cast<uint64_t>(page_data_[data_pos + i]) << (i * 8));
            }
            data_pos += bit_bytes;
            
            return std::to_string(bit_val);
        
        case mysql_types::JSON:
            // JSON 存储为 BLOB，这里简化处理
            if (col.is_nullable && data_pos < page_data_.size() && page_data_[data_pos] == 0xFF) {
                data_pos++;
                return "NULL";
            }
            
            // 简单读取前几个字节作为示例
            size_t json_len = std::min(col.column_length, static_cast<uint32_t>(100));
            if (data_pos + json_len > page_data_.size()) {
                json_len = page_data_.size() - data_pos;
            }
            
            std::string result;
            for (size_t i = 0; i < json_len; ++i) {
                char c = static_cast<char>(page_data_[data_pos]);
                if (c >= 32 && c <= 126) {
                    result += c;
                } else {
                    result += ".";
                }
                data_pos++;
            }
            
            return result + "...";
        
        default:
            // BLOB/TEXT 类型
            if (col.mysql_type_id >= mysql_types::TINY_BLOB && 
                col.mysql_type_id <= mysql_types::BLOB) {
                // 简单处理：返回指针信息
                std::ostringstream oss;
                oss << "BLOB_POINTER(space=" << page_data_[data_pos] 
                    << ",page=" << page_data_[data_pos + 1] << ")";
                data_pos += 4; // 跳过4字节指针
                return oss.str();
            }
            
            // 其他类型：返回十六进制表示
            std::ostringstream oss;
            oss << "0x";
            for (size_t i = 0; i < col.column_length && data_pos < page_data_.size(); ++i) {
                oss << std::hex << std::setfill('0') << std::setw(2) 
                    << static_cast<int>(page_data_[data_pos++]);
            }
            return oss.str();
    }
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
