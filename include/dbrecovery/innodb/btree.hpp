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
    
    bool parse() noexcept;
    
    [[nodiscard]] BTreeNodeType node_type() const { return node_type_; }
    [[nodiscard]] uint32_t page_level() const { return page_level_; }
    [[nodiscard]] uint32_t next_page() const { return next_page_; }
    [[nodiscard]] uint32_t prev_page() const { return prev_page_; }
    [[nodiscard]] uint16_t n_records() const { return n_records_; }
    [[nodiscard]] const std::vector<uint16_t>& record_offsets() const { return record_offsets_; }
    
    /**
     * @brief 解析字段值（提取的辅助函数）
     * 
     * @param col 列定义
     * @param data_pos 数据位置引用（会被修改）
     * @param is_extern 是否为外部指针
     * @return std::string 解析后的字段值
     */
    std::string parse_field_value(
        const catalog::ColumnInfo& col,
        size_t& data_pos,
        bool is_extern) const;
    
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
            
            // 解析记录头（COMPACT格式固定5字节）
            if (offset + 5 > page_data_.size()) {
                continue;
            }
            
            uint8_t info_bits = page_data_[offset];
            uint16_t next_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data() + offset + 1, 0) & 0x3FFF;
            
            // 跳过已删除的记录
            if (info_bits & dbrecovery::innodb::REC_INFO_DELETED_FLAG) {
                continue;
            }
            
            // 解析字段数据
            std::map<std::string, std::string> record;
            size_t data_pos = offset + 5; // 跳过记录头
            
            for (const auto& pair : schema.columns) {
                const auto& col = pair.second;
                
                if (data_pos >= page_data_.size()) {
                    break;
                }
                
                std::string value = parse_field_value(col, data_pos, false);
                record[col.name] = value;
            }
            
            records.push_back(record);
        }
        
        return records;
    }
    
    // ✅ 新增：格式校验方法
    bool is_compact_format() const {
        return (btr_format_ & 0x80) == 0; // COMPACT 格式标志位为 0
    }
    
    // ✅ 新增：页健康状态检查
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
    };
    
    [[nodiscard]] PageHealth check_health() const;
    
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
    
    bool parse_header() noexcept;
    bool parse_infimum_supremum() noexcept;
    bool parse_user_records() noexcept;
};

// ==================== 实现部分 ====================
inline BPlusTreeParser::BPlusTreeParser(const std::vector<uint8_t>& page_data)
    : page_data_(page_data) {
}

inline bool BPlusTreeParser::parse() noexcept {
    if (page_data_.size() < 38 + 16) { // FIL header + minimal record space
        return false;
    }
    
    // 解析页头信息
    if (!parse_header()) {
        return false;
    }
    
    // 解析 INFIMUM 和 SUPREMUM 记录
    if (!parse_infimum_supremum()) {
        return false;
    }
    
    // 解析用户记录
    if (!parse_user_records()) {
        return false;
    }
    
    return true;
}

inline bool BPlusTreeParser::parse_header() noexcept {
    if (page_data_.size() < 38) {
        return false;
    }
    
    // 读取 FIL_PAGE_TYPE (offset 24 in page, but offset 12 in header)
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 24);
    
    if (page_type == FIL_PAGE_INDEX) {
        node_type_ = BTreeNodeType::LEAF;
        
        // 读取页级别（BTR_PAGE_LEVEL，offset 38+38=76）
        if (page_data_.size() >= 78) {
            page_level_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 38);
        }
        
        // 读取 BTR_N_DIR_SLOTS（offset 38+0）
        if (page_data_.size() >= 40) {
            n_records_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 0);
        }
        
        // 读取 BTR_FORMAT（flags 字段的 bit 7）
        if (page_data_.size() >= 44) {
            btr_format_ = page_data_[38 + 2]; // BTR_FORMAT_MASK at offset 40
        }
    } else {
        node_type_ = BTreeNodeType::INVALID;
        return false;
    }
    
    return true;
}

inline bool BPlusTreeParser::parse_infimum_supremum() noexcept {
    if (page_data_.size() < 56) { // INFIMUM record at offset 54, minimal size 13
        return false;
    }
    
    // INFIMUM 记录在偏移 54 处（PAGE_DATA_START = 38 + 16）
    uint16_t infimum_offset = 54;
    if (infimum_offset >= page_data_.size()) {
        return false;
    }
    
    // 检查 INFIMUM 记录标志
    uint8_t info_bits = page_data_[infimum_offset];
    if ((info_bits & REC_INFO_MIN_REC_FLAG) == 0) {
        // 可能格式不正确，但继续尝试解析
    }
    
    return true;
}

inline bool BPlusTreeParser::parse_user_records() noexcept {
    // 解析记录目录（directory slots）
    uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 0);
    if (n_dir_slots == 0 || n_dir_slots > 1000) {
        return false;
    }
    
    // 解析堆顶位置
    uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 2);
    if (heap_top < 56 || heap_top > page_data_.size()) {
        return false;
    }
    
    // 解析记录偏移（从目录槽中获取）
    record_offsets_.clear();
    
    // 目录槽位于页尾部，每个槽占2字节
    uint16_t dir_start = (page_data_.size() / 16) * 16 - n_dir_slots * 2;
    if (dir_start < 38 + 16) {
        return false;
    }
    
    for (uint16_t i = 0; i < n_dir_slots; ++i) {
        uint16_t slot_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dir_start + i * 2);
        if (slot_offset >= 38 && slot_offset < heap_top) {
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
        case catalog::mysql_types::SHORT:
            if (data_pos + 2 > page_data_.size()) return "";
            return std::to_string(dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), data_pos));
            data_pos += 2;
            break;
        case catalog::mysql_types::LONG:
            if (data_pos + 4 > page_data_.size()) return "";
            return std::to_string(dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), data_pos));
            data_pos += 4;
            break;
        case catalog::mysql_types::LONGLONG:
            if (data_pos + 8 > page_data_.size()) return "";
            return std::to_string(dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), data_pos));
            data_pos += 8;
            break;
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
            // MySQL 8.0 DATETIME2 编码：前5字节 packed format，后3字节小数部分
            if (data_pos + 8 > page_data_.size()) return "";
            
            uint8_t year_low = page_data_[data_pos++];
            uint8_t year_high = page_data_[data_pos++];
            uint8_t month_day = page_data_[data_pos++];
            uint8_t hour_minute = page_data_[data_pos++];
            uint8_t second_packed = page_data_[data_pos++];
            
            int year = year_low | (year_high << 8);
            int month = month_day & 0x0F;
            int day = month_day >> 4;
            int hour = hour_minute & 0x1F;
            int minute = (hour_minute >> 2) & 0x3F;
            int second = second_packed >> 2;
            
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
                    year, month, day, hour, minute, second);
            data_pos += 3; // skip fractional part
            return std::string(buffer);
        }
        default:
            return "[UNSUPPORTED_TYPE]";
    }
    
    return "";
}

inline BPlusTreeParser::PageHealth BPlusTreeParser::check_health() const {
    PageHealth health;
    health.is_valid = true;
    health.status_message = "OK";
    
    // 检查页大小
    if (page_data_.size() < 38 + 16) {
        health.is_valid = false;
        health.status_message = "Page too small";
        return health;
    }
    
    // 检查 INFIMUM 记录
    uint16_t infimum_offset = 54;
    if (infimum_offset < page_data_.size()) {
        uint8_t info_bits = page_data_[infimum_offset];
        health.has_infimum = (info_bits & REC_INFO_MIN_REC_FLAG) != 0;
    }
    
    // 检查 SUPREMUM 记录
    if (!record_offsets_.empty()) {
        uint16_t supremum_offset = record_offsets_.back();
        if (supremum_offset < page_data_.size()) {
            uint8_t info_bits = page_data_[supremum_offset];
            health.has_supremum = ((info_bits & 0x03) == REC_STATUS_SUPREMUM);
        }
    }
    
    // 检查记录数量
    if (!record_offsets_.empty()) {
        health.n_records = static_cast<uint16_t>(record_offsets_.size());
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
