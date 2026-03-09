#ifndef DBRECOVERY_INNODB_BTREE_HPP
#define DBRECOVERY_INNODB_BTREE_HPP

#include <cstdint>
#include <vector>
#include <memory>
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/catalog/schema.hpp"

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
     * @brief 解析记录数据
     * 
     * @param schema 表结构定义
     * @return std::vector<std::map<std::string, std::string>> 解析后的记录
     */
    std::vector<std::map<std::string, std::string>> parse_records(
        const catalog::TableSchema& schema) const;

private:
    std::vector<uint8_t> page_data_;
    BTreeNodeType node_type_ = BTreeNodeType::INVALID;
    uint32_t page_level_ = 0;
    uint32_t next_page_ = 0xFFFFFFFF;
    uint32_t prev_page_ = 0xFFFFFFFF;
    uint16_t n_records_ = 0;
    std::vector<uint16_t> record_offsets_;
    
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
