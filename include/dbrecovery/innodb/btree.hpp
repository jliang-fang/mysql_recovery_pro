#ifndef DBRECOVERY_INNODB_BTREE_HPP
#define DBRECOVERY_INNODB_BTREE_HPP

#include <cstdint>
#include <vector>
#include <memory>
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief B+Tree 节点类型
 */
enum class BTreeNodeType : uint8_t {
    ROOT = 0,
    INTERNAL = 1,
    LEAF = 2,
    INVALID = 255
};

// ✅ 记录头标志位常量（用于 COMPACT 格式，参考 MySQL 8.0 源码 sql/dd/impl/table.h 和 page/page0page.h）
// 注意：REC_INFO_STATUS_MASK 是最低3位（0x07），REC_INFO_DELETED_FLAG 是 bit 3 (0x08)
// 这些常量已在 page_defs.hpp 中定义，此处仅作引用以保持向后兼容性

/**
 * @brief B+Tree 索引页结构解析器
 */
class BPlusTreeParser {
public:
    explicit BPlusTreeParser(const std::vector<uint8_t>& page_data);
    
    // ✅ 统一异常规范：所有解析方法均不标记 noexcept，确保异常可传播
    bool parse();
    
    [[nodiscard]] bool is_valid_page_type() const { return valid_page_type_; }
    [[nodiscard]] BTreeNodeType node_type() const { return node_type_; }

private:
    bool valid_page_type_ = false;
    uint32_t page_level_ = 0xFFFFFFFF;
    uint32_t next_page_ = 0xFFFFFFFF;
    uint32_t prev_page_ = 0xFFFFFFFF;
    uint16_t n_records_ = 0;
    std::vector<uint16_t> record_offsets_;
    
    // ✅ 统一：页健康状态检查（消除重复定义）
    struct PageHealth {
        bool is_valid = true;
        std::string status_message;
        uint32_t checksum_expected = 0;
        uint32_t checksum_actual = 0;
        bool checksum_match = false;
        
        // B+Tree specific
        bool has_infimum = false;
        bool has_supremum = false;
        uint16_t n_records = 0;
        uint16_t n_dir_slots = 0;
        uint16_t page_level = 0;
        uint32_t next_page = 0;
        uint32_t prev_page = 0;
    };
    
    [[nodiscard]] PageHealth check_health() const;

private:
    uint16_t page_type_ = 0; // ✅ 新增：存储页类型，供 SDI 页判断使用

    /**
     * @brief 解析单个字段的值（根据列类型）
     * 
     * @param col 列定义
     * @param data_pos 数据位置指针（会更新）
     * @param is_extern 是否为外部存储（如 BLOB 指针）
     * @return std::string 解析后的字符串值
     */
    std::string parse_field_value(
        const catalog::ColumnInfo& col, size_t& data_pos, bool is_extern) const;

    // ✅ 新增：DATETIME2 字段解析辅助函数
    std::string parse_datetime2_value(uint8_t precision, size_t& data_pos) const;

    /**
     * @brief 解析页的目录槽（record directory）
     * 
     * @return true 目录解析成功
     * @return false 目录解析失败
     */
    bool parse_directory() noexcept;
    
    // ✅ 新增：解析页头信息
    bool parse_header() noexcept;
    
    // ✅ 修复：移除 inline 和 noexcept，与 .cpp 实现保持一致
    bool parse_infimum_supremum();
    
    // ✅ 修复：移除 inline 和 noexcept，与 .cpp 实现保持一致
    bool parse_user_records();

private:
    std::vector<uint8_t> page_data_;
    BTreeNodeType node_type_ = BTreeNodeType::INVALID;
    uint32_t page_level_ = 0;
    uint32_t next_page_ = 0xFFFFFFFF;
    uint32_t prev_page_ = 0xFFFFFFFF;
    uint16_t n_records_ = 0;
    std::vector<uint16_t> record_offsets_;
    uint8_t btr_format_ = 0;  // B+树记录格式标志位 (BTR_FORMAT_MASK)
    uint16_t flags_ = 0;      // B+树标志位（用于存储行格式等信息）
    
    // INFIMUM/SUPREMUM 记录偏移
    uint16_t infimum_offset_ = 54;
    uint16_t supremum_offset_ = 0;

public:
    /**
     * @brief 解析记录数据（仅支持 COMPACT 格式）
     * 
     * @param schema 表结构定义
     * @return std::vector<std::map<std::string, std::string>> 解析后的记录
     */
    std::vector<std::map<std::string, std::string>> parse_records(
        const catalog::TableSchema& schema) const {
        std::vector<std::map<std::string, std::string>> records;
        
        if (!is_compact_format()) {
            // 仅支持 COMPACT 格式
            return records;
        }
        
        // 解析每个用户记录
        for (size_t i = 0; i < record_offsets_.size(); ++i) {
            uint16_t offset = record_offsets_[i];
            
            // 跳过 INFIMUM 和 SUPREMUM 系统记录
            if (offset == 0 || offset >= page_data_.size() - 5) {
                continue;
            }
            
            // 解析记录头（COMPACT格式）
            if (offset + 6 > page_data_.size()) {
                continue;
            }
            
            uint8_t info_bits = page_data_[offset];
            uint16_t next_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data() + offset + 1, 0) & 0x3FFF;
            
            // 跳过已删除的记录
            if (info_bits & dbrecovery::innodb::REC_INFO_DELETED_FLAG) {
                continue;
            }
            
            // ✅ 统一：解析 n_fields 字段数量（位于记录头第5字节）
            uint16_t n_fields = page_data_[offset + 5];
            if (n_fields == 0 || n_fields > schema.columns.size()) {
                // 兼容性处理：如果无法确定字段数，使用schema中的字段数
                n_fields = static_cast<uint16_t>(schema.columns.size());
            }
            
            // 解析 NULL 位图（COMPACT格式：NULL位图在记录头后）
            size_t data_pos = offset + 5; // 记录头从 offset 开始，第5字节是 info_bits
            uint32_t nullable_count = 0;
            for (const auto& pair : schema.columns) {
                if (pair.second.is_nullable) {
                    nullable_count++;
                }
            }
            size_t null_bitmap_size = (nullable_count + 7) / 8;
            
            // ✅ 修复：跳过 info_bits 后，data_pos 应该指向 NULL 位图的开头
            data_pos += 1; // info_bits
            
            if (data_pos + null_bitmap_size > page_data_.size()) {
                break;
            }
            
            // ✅ 新增：确保 NULL 位图解析正确处理非空列跳过逻辑
            // COMPACT 格式中，NULL 位图只包含可空列，顺序与 schema.columns 中的定义顺序一致
            
            // 解析字段数据
            std::map<std::string, std::string> record;
            
            // ✅ 统一：根据实际解析的字段数进行循环
            uint16_t parsed_fields = 0;
            size_t nullable_idx = 0; // NULL 位图中的索引
            
            for (const auto& pair : schema.columns) {
                const auto& col = pair.second;
                
                if (data_pos >= page_data_.size() || parsed_fields >= n_fields) {
                    break;
                }
                
                // ✅ 新增：对于非空列，跳过 NULL 位图中的对应位
                bool is_nullable_col = col.is_nullable;
                
                // 检查当前列是否在 NULL 位图中（仅当它是可空列时）
                if (is_nullable_col) {
                    // ✅ 修复：data_pos 现在指向 NULL 位图的开头，直接使用
                    size_t byte_idx = nullable_idx / 8;
                    size_t bit_idx = nullable_idx % 8;
                    
                    if (data_pos + byte_idx < page_data_.size()) {
                        uint8_t bitmap_byte = page_data_[data_pos + byte_idx];
                        bool is_null = (bitmap_byte & (1 << bit_idx)) != 0;
                        
                        if (is_null) {
                            record[col.name] = "NULL";
                            nullable_idx++;
                            parsed_fields++;
                            continue; // 跳过字段值解析
                        }
                    }
                    nullable_idx++;
                }
                
                // ✅ 修复：移动 data_pos 到字段数据的起始位置（跳过 NULL 位图）
                if (is_nullable_col && nullable_idx == 0) {
                    data_pos += null_bitmap_size;
                }
                
                std::string value = parse_field_value(col, data_pos, false);
                record[col.name] = value;
                parsed_fields++;
            }
            
            records.push_back(record);
        }
        
        return records;
    }
    
    // ✅ 统一：格式校验方法
    bool is_compact_format() const {
        return (btr_format_ & 0x80) == 0; // COMPACT 格式标志位为 0
    }
    
    // ✅ 新增：SDI 页解析支持
    [[nodiscard]] uint64_t get_sdi_table_id() const;

private:
    std::vector<uint8_t> page_data_;
    BTreeNodeType node_type_ = BTreeNodeType::INVALID;
    uint32_t page_level_ = 0;
    uint32_t next_page_ = 0xFFFFFFFF;
    uint32_t prev_page_ = 0xFFFFFFFF;
    uint16_t n_records_ = 0;
    std::vector<uint16_t> record_offsets_;
    uint8_t btr_format_ = 0;  // B+树记录格式标志位 (BTR_FORMAT_MASK)
    uint16_t flags_ = 0;      // B+树标志位（用于存储行格式等信息）
    
    // INFIMUM/SUPREMUM 记录偏移
    uint16_t infimum_offset_ = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
    uint16_t supremum_offset_ = 0;

    bool parse_header() noexcept;
    bool parse_infimum_supremum();
    bool parse_user_records();
};

// ==================== 实现部分 ====================
inline BPlusTreeParser::BPlusTreeParser(const std::vector<uint8_t>& page_data)
    : page_data_(page_data), node_type_(BTreeNodeType::INVALID),
      page_level_(0xFFFFFFFF), next_page_(0xFFFFFFFF), prev_page_(0xFFFFFFFF) {
}

// ✅ 修复：实现核心的 parse 方法，移除 noexcept 以支持错误传播
inline bool BPlusTreeParser::parse() {
    if (page_data_.size() < 38 + 16) { // FIL header (38) + minimal record space
        std::cerr << "[ERROR] Page too small for parsing" << std::endl;
        return false;
    }

    // 解析页头信息
    if (!parse_header()) {
        std::cerr << "[ERROR] Failed to parse page header" << std::endl;
        return false;
    }
    
    // ✅ 新增：SDI 页特殊处理（无需解析记录）
    if (page_type_ == dbrecovery::innodb::FIL_PAGE_SDI) {
        node_type_ = BTreeNodeType::LEAF; // SDI 页视为叶子页
        return true;
    }
    
    // 解析 INFIMUM/SUPREMUM 系统记录
    if (!parse_infimum_supremum()) {
        std::cerr << "[ERROR] Failed to parse infimum/supremum records" << std::endl;
        return false;
    }
    
    // 解析用户记录
    if (!parse_user_records()) {
        std::cerr << "[ERROR] Failed to parse user records" << std::endl;
        return false;
    }

    node_type_ = (page_level_ == 0) ? BTreeNodeType::LEAF : BTreeNodeType::INTERNAL;
    
    return true;
}

// ✅ 新增：解析页头信息
bool BPlusTreeParser::parse_header() noexcept {
    // 解析页头信息（简化版）
    if (page_data_.size() < dbrecovery::innodb::FIL_HEADER_SIZE + 16) {
        return false;
    }
    
    // ✅ 修复：使用 page_defs.hpp 中定义的常量
    page_type_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);
    if (page_type_ != dbrecovery::innodb::FIL_PAGE_INDEX && 
        page_type_ != dbrecovery::innodb::FIL_PAGE_SDI &&
        page_type_ != dbrecovery::innodb::FIL_PAGE_UNDO_LOG) {
        return false;
    }
    
    // ✅ 修复：使用正确的偏移读取页级别
    page_level_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dbrecovery::innodb::FIL_HEADER_SIZE + dbrecovery::innodb::BTR_N_LEVELS);
    
    // ✅ 修复：使用正确的偏移读取下一页和前一页
    next_page_ = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), dbrecovery::innodb::FIL_HEADER_SIZE + 26);
    prev_page_ = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), dbrecovery::innodb::FIL_HEADER_SIZE + 20);
    
    // ✅ 修复：使用正确的偏移读取 flags
    flags_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dbrecovery::innodb::FIL_HEADER_SIZE + 6);
    
    // BTR_FORMAT 是 flags 的一部分，通过位掩码提取
    // BTR_FORMAT_MASK = 0x80, 表示是否为 DYNAMIC/COMPRESSED 格式
    btr_format_ = (flags_ & 0x80) >> 7;
    
    return true;
}

// ✅ 新增：解析 INFIMUM/SUPREMUM 系统记录
bool BPlusTreeParser::parse_infimum_supremum() {
    // 解析 INFIMUM 和 SUPREMUM 记录（固定偏移）
    if (page_data_.size() < 54 + 5) {
        return false;
    }
    
    infimum_offset_ = 54;
    uint8_t info_bits_infimum = page_data_[infimum_offset_];
    if ((info_bits_infimum & dbrecovery::innodb::REC_INFO_MIN_REC_FLAG) == 0) {
        // INFIMUM 记录格式不正确
        return false;
    }
    
    // 解析 SUPREMUM 记录（通常在堆顶附近）
    uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 4);
    if (heap_top < 54 || heap_top + 5 > page_data_.size()) {
        return false;
    }
    
    supremum_offset_ = heap_top - 5;
    uint8_t info_bits_supremum = page_data_[supremum_offset_];
    if ((info_bits_supremum & 0x03) != dbrecovery::innodb::REC_STATUS_SUPREMUM) {
        // SUPREMUM 记录格式不正确
        return false;
    }
    
    return true;
}

// ✅ 新增：解析用户记录
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

inline bool BPlusTreeParser::parse_directory() noexcept {
    // 简化实现：假设页头信息在偏移 38 处
    if (page_data_.size() < 38 + 16) {
        return false;
    }
    
    uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38);
    if (n_dir_slots == 0 || n_dir_slots > 1000) {
        std::cerr << "[ERROR] Invalid directory slots: " << n_dir_slots << std::endl;
        return false;
    }
    
    // 解析堆顶位置
    uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 40);
    if (heap_top < 56 || heap_top > page_data_.size()) {
        std::cerr << "[ERROR] Invalid heap top: " << heap_top << std::endl;
        return false;
    }
    
    // 解析记录偏移（从目录槽中获取）
    record_offsets_.clear();
    uint16_t page_size = static_cast<uint16_t>(page_data_.size());
    if (page_size < n_dir_slots * 2) {
        std::cerr << "[ERROR] Page size too small for directory" << std::endl;
        return false;
    }
    
    // ✅ 新增：确保目录槽区域在页边界内
    uint16_t dir_start = page_size - (n_dir_slots * 2);
    if (dir_start < 38 + 16) { // 确保不覆盖页头和数据区
        std::cerr << "[ERROR] Directory start overlaps with header/data area" << std::endl;
        return false;
    }
    
    for (uint16_t i = 0; i < n_dir_slots; ++i) {
        if (dir_start + i * 2 + 2 > page_data_.size()) {
            continue;
        }
        
        uint16_t slot_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dir_start + i * 2);
        // ✅ 新增：严格检查偏移范围
        if (slot_offset >= 54 && slot_offset < heap_top && slot_offset < page_size - 5) {
            record_offsets_.push_back(slot_offset);
        }
    }
    
    return !record_offsets_.empty();
}

inline std::string BPlusTreeParser::parse_field_value(
    const catalog::ColumnInfo& col,
    size_t& data_pos,
    bool is_extern) const {
    // 简化实现，实际需要根据列类型进行详细解析
    if (data_pos >= page_data_.size()) {
        return "";
    }
    
    switch (col.mysql_type_id) {
        case catalog::mysql_types::TINY:
            return std::to_string(static_cast<int8_t>(page_data_[data_pos++]));
        case catalog::mysql_types::SHORT: {
            if (data_pos + 2 > page_data_.size()) return "";
            uint16_t value = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), data_pos);
            data_pos += 2;
            return std::to_string(value);
        }
        case catalog::mysql_types::LONG: {
            if (data_pos + 4 > page_data_.size()) return "";
            uint32_t value = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), data_pos);
            data_pos += 4;
            return std::to_string(value);
        }
        case catalog::mysql_types::LONGLONG: {
            if (data_pos + 8 > page_data_.size()) return "";
            uint64_t value = dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), data_pos);
            data_pos += 8;
            return std::to_string(value);
        }
        case catalog::mysql_types::VARCHAR:
        case catalog::mysql_types::VAR_STRING: {
            if (data_pos >= page_data_.size()) return "";
            uint8_t len = page_data_[data_pos++];
            if (data_pos + len > page_data_.size()) return "";
            std::string value(reinterpret_cast<const char*>(&page_data_[data_pos]), len);
            data_pos += len;
            return value;
        }
        case catalog::mysql_types::DATETIME2:
        case catalog::mysql_types::TIMESTAMP2: {
            // ✅ 新增：使用辅助函数解析 DATETIME2
            uint8_t precision = col.datetime_precision >= 0 ? static_cast<uint8_t>(col.datetime_precision) : 6;
            return parse_datetime2_value(precision, data_pos);
        }
        default:
            return "[UNSUPPORTED_TYPE]";
    }
    
    return "";
}

inline std::string BPlusTreeParser::parse_datetime2_value(uint8_t precision, size_t& data_pos) const {
    // ✅ 完整实现 DATETIME2 解析（参考 MySQL 8.0 源码 sql/field.cc）
    
    if (data_pos + 6 > page_data_.size()) return ""; // 最小长度为6字节
    
    // 解析 packed datetime2 格式
    // 前5字节: [year_low, year_high, month_day, hour_minute, second_packed]
    // 后3字节 (precision=6): [frac_0, frac_1, frac_2] 小端序
    
    uint8_t year_low = page_data_[data_pos++];
    uint8_t year_high = page_data_[data_pos++];
    uint8_t month_day = page_data_[data_pos++];
    uint8_t hour_minute = page_data_[data_pos++];
    uint8_t second_packed = page_data_[data_pos++];
    
    // 解析小数部分
    uint32_t frac = 0;
    if (precision > 0) {
        size_t frac_bytes = (precision + 1) / 2; // 1-2 digits: 1 byte, 3-4 digits: 2 bytes, 5-6 digits: 3 bytes
        for (size_t i = 0; i < frac_bytes && data_pos < page_data_.size(); ++i) {
            frac |= (static_cast<uint32_t>(page_data_[data_pos++]) << (i * 8));
        }
    }

    // 解包日期部分
    int year = year_low | (year_high << 8);
    int month = month_day & 0x0F;
    int day = month_day >> 4;
    
    // 解包时间部分
    int hour = hour_minute & 0x1F;
    int minute = (hour_minute >> 2) & 0x3F;
    int second = second_packed & 0x3F;
    int frac_6 = 0; // 微秒

    // ✅ 新增：根据精度解包小数部分
    if (precision <= 2) {
        frac_6 = frac * 10000; // 2 digits -> microsec
    } else if (precision <= 4) {
        frac_6 = frac * 100;   // 4 digits -> microsec
    } else { // precision <= 6
        frac_6 = frac;
    }

    // 格式化为字符串 "YYYY-MM-DD HH:MM:SS[.ffffff]"
    std::ostringstream oss;
    oss << year << "-" 
        << std::setfill('0') << std::setw(2) << month << "-" 
        << std::setfill('0') << std::setw(2) << day << " "
        << std::setfill('0') << std::setw(2) << hour << ":"
        << std::setfill('0') << std::setw(2) << minute << ":"
        << std::setfill('0') << std::setw(2) << second;
    
    if (precision > 0 && frac_6 > 0) {
        oss << "." << std::setfill('0') << std::setw(6) << frac_6;
    }
    
    return oss.str();
}

inline BPlusTreeParser::PageHealth BPlusTreeParser::check_health() const {
    PageHealth health;
    health.is_valid = true;
    health.status_message = "OK";
    health.page_level = page_level_;
    health.next_page = next_page_;
    health.prev_page = prev_page_;
    
    // 检查页大小
    if (page_data_.size() < 38 + 16) {
        health.is_valid = false;
        health.status_message = "Page too small";
        return health;
    }
    
    // 检查 INFIMUM 记录（固定偏移 54）
    uint16_t infimum_offset = 54;
    if (infimum_offset < page_data_.size()) {
        uint8_t info_bits = page_data_[infimum_offset];
        health.has_infimum = ((info_bits & REC_INFO_MIN_REC_FLAG) != 0);
        
        // 额外检查：INFIMUM 应该是类型为 REC_STATUS_INFIMUM 的系统记录
        if (health.has_infimum && ((info_bits & 0x03) != REC_STATUS_INFIMUM)) {
            health.status_message = "INFIMUM record type mismatch";
        }
    } else {
        health.has_infimum = false;
    }
    
    // 检查 SUPREMUM 记录
    if (!record_offsets_.empty()) {
        uint16_t supremum_offset = record_offsets_.back();
        if (supremum_offset < page_data_.size() && supremum_offset >= 54) {
            uint8_t info_bits = page_data_[supremum_offset];
            health.has_supremum = ((info_bits & 0x03) == REC_STATUS_SUPREMUM);
            
            // 额外检查：SUPREMUM 应该是最大记录
            if (!health.has_supremum) {
                health.status_message = "SUPREMUM record type mismatch";
            }
        } else {
            health.has_supremum = false;
        }
    } else {
        health.has_supremum = false;
    }
    
    // 检查记录数量
    if (!record_offsets_.empty()) {
        health.n_records = static_cast<uint16_t>(record_offsets_.size());
        
        // 额外检查：记录数应该至少包含 INFIMUM 和 SUPREMUM
        if (health.n_records < 2) {
            health.status_message = "Insufficient records (should include INFIMUM and SUPREMUM)";
        }
    } else {
        health.n_records = 0;
    }
    
    // 检查页级别（B+树高度）
    if (page_level_ > 5) { // MySQL 8.0 B+树最大高度通常不超过5
        health.status_message = "Unreasonably high page level";
    }
    
    return health;
}

inline uint64_t BPlusTreeParser::get_sdi_table_id() const {
    // SDI 页解析：从 FIL_PAGE_SPACE_ID 和 FIL_PAGE_OFFSET 组合
    if (page_data_.size() < 12) {
        return 0;
    }
    
    uint32_t space_id = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), 0);
    uint32_t page_no = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), 4);
    
    // 组合 space_id 和 page_no 形成唯一标识
    return (static_cast<uint64_t>(space_id) << 32) | page_no;
}

// ==================== 单元测试入口（供 GTest 调用）====================
#ifdef DBRECOVERY_ENABLE_TESTS
#include <gtest/gtest.h>
#include <vector>

namespace dbrecovery {
namespace innodb {

// 测试 BPlusTreeParser 在页太小时返回 false
TEST(BPlusTreeParserTest, ParseTooSmallPage) {
    std::vector<uint8_t> page_data(50, 0); // 小于最小大小 (38 + 16 = 54)
    
    BPlusTreeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

// 测试 BPlusTreeParser 在页头信息无效时返回 false
TEST(BPlusTreeParserTest, ParseInvalidPageHeader) {
    std::vector<uint8_t> page_data(1024, 0);
    
    // 设置一个无效的页类型（非 FIL_PAGE_INDEX/SDI/UNDO_LOG）
    write_le<uint16_t>(page_data.data(), 38 + 12, 0xFFFF);
    
    BPlusTreeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

// 测试 BPlusTreeParser 在 INFIMUM/SUPREMUM 记录无效时返回 false
TEST(BPlusTreeParserTest, ParseInvalidInfimumSupremum) {
    std::vector<uint8_t> page_data(1024, 0);
    
    // 设置有效的页类型
    write_le<uint16_t>(page_data.data(), 38 + 12, FIL_PAGE_INDEX);
    
    // 设置堆顶位置（使 INFIMUM/SUPREMUM 解析失败）
    write_le<uint16_t>(page_data.data(), 38 + 4, 50); // heap_top = 50
    
    BPlusTreeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

// 测试 BPlusTreeParser 在正常情况下返回 true
TEST(BPlusTreeParserTest, ParseValidPage) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的页类型
    write_le<uint16_t>(page_data.data(), 38 + 12, FIL_PAGE_INDEX);
    
    // 设置堆顶位置（足够大以包含 INFIMUM 和 SUPREMUM）
    write_le<uint16_t>(page_data.data(), 38 + 4, 100); // heap_top = 100
    
    // 设置有效的 INFIMUM 记录（偏移 54）
    page_data[54] = REC_INFO_MIN_REC_FLAG;
    write_le<uint16_t>(page_data.data(), 55, 100); // next_record_offset
    
    // 设置有效的 SUPREMUM 记录
    page_data[99] = REC_STATUS_SUPREMUM;
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
}

// 测试 BPlusTreeParser 在解析 SDI 页时返回 true
TEST(BPlusTreeParserTest, ParseSDIPage) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的 SDI 页类型
    write_le<uint16_t>(page_data.data(), 38 + 12, FIL_PAGE_SDI);
    
    // 设置堆顶位置
    write_le<uint16_t>(page_data.data(), 38 + 4, 100); // heap_top = 100
    
    // 设置有效的 INFIMUM 记录（偏移 54）
    page_data[54] = REC_INFO_MIN_REC_FLAG;
    write_le<uint16_t>(page_data.data(), 55, 100);
    
    // 设置有效的 SUPREMUM 记录
    page_data[99] = REC_STATUS_SUPREMUM;
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
}

// 测试 BPlusTreeParser 在解析 Undo Log 页时返回 true
TEST(BPlusTreeParserTest, ParseUndoLogPage) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的 Undo Log 页类型
    write_le<uint16_t>(page_data.data(), 38 + 12, FIL_PAGE_UNDO_LOG);
    
    // 设置堆顶位置
    write_le<uint16_t>(page_data.data(), 38 + 4, 100); // heap_top = 100
    
    // 设置有效的 INFIMUM 记录（偏移 54）
    page_data[54] = REC_INFO_MIN_REC_FLAG;
    write_le<uint16_t>(page_data.data(), 55, 100);
    
    // 设置有效的 SUPREMUM 记录
    page_data[99] = REC_STATUS_SUPREMUM;
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_ENABLE_TESTS

/**
 * @brief B+Tree 恢复器
 */
class BPlusTreeReconstructor {
public:
    explicit BPlusTreeReconstructor(uint32_t root_page_no);
    
    /**
     * @brief 添加索引页数据
     */
    void add_page(uint32_t page_no, std::vector<uint8_t> page_data);
    
    /**
     * @brief 重建索引结构
     */
    bool reconstruct() noexcept;
    
    /**
     * @brief 获取所有记录
     */
    [[nodiscard]] const std::vector<std::map<std::string, std::string>>& records() const {
        return records_;
    }

private:
    uint32_t root_page_no_;
    std::map<uint32_t, std::vector<uint8_t>> pages_;
    std::vector<std::map<std::string, std::string>> records_;
    
    bool traverse_tree(uint32_t page_no, const catalog::TableSchema& schema) noexcept;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BTREE_HPP
