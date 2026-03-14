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
