#ifndef DBRECOVERY_REPAIR_INDEX_CORRUPTION_REPAIR_HPP
#define DBRECOVERY_REPAIR_INDEX_CORRUPTION_REPAIR_HPP

#include <cstdint>
#include <vector>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <set>
#include <memory>
#include <chrono>

#include "dbrecovery/innodb/sec_index_btree_repair.hpp"

namespace dbrecovery {
namespace repair {

/**
 * @brief 索引损坏类型
 */
enum class IndexCorruptionType {
    NONE = 0,
    NODE_POINTER_INVALID,       // 节点指针失效
    ROOT_PAGE_NOT_FOUND,        // 根页未找到
    LEVEL_MISMATCH,             // 层级不匹配
    INDEX_ID_MISMATCH,          // 索引ID不匹配
    PAGE_CHAIN_ORPHAN,          // 孤儿页（不在任何链中）
    PAGE_CHAIN_LOOP,            // 页链循环
    NON_LEAF_NO_CHILDREN,       // 非叶节点没有子节点
    LEAF_HAS_CHILDREN,          // 叶节点有子节点
    INVALID_CHILD_POINTER,      // 无效的子节点指针
    PAGE_COUNT_MISMATCH,        // 页数与目录不匹配
};

/**
 * @brief 索引损坏详情
 */
struct IndexCorruption {
    IndexCorruptionType type;
    uint32_t page_no;               // 涉及的页号
    uint32_t related_page_no;       // 相关页号
    std::string description;
    float confidence;
    bool repairable;
};

/**
 * @brief B+Tree 页信息
 */
struct BTreePageInfo {
    uint32_t page_no;
    uint16_t page_type;
    uint8_t level;                  // 0 = leaf, > 0 = internal
    uint32_t index_id;
    uint32_t prev_page;
    uint32_t next_page;
    uint16_t n_records;
    uint16_t n_dir_slots;
    bool is_leaf;
    bool is_valid;
    std::vector<uint32_t> child_pages;  // 子节点页号（仅非叶节点）
};

/**
 * @brief 索引结构修复器
 *
 * 修复 B+Tree 索引结构损坏：
 * - 节点指针失效检测和修复
 * - 从损坏 INODE 检测根页
 * - 层级不匹配修复
 * - 索引ID不匹配检测和修复
 */
class IndexCorruptionRepairer {
public:
    IndexCorruptionRepairer();
    ~IndexCorruptionRepairer() = default;

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size) { page_size_ = page_size; }

    /**
     * @brief 分析 B+Tree 结构
     */
    std::vector<IndexCorruption> analyze(const uint8_t* file_data, size_t file_size);

    /**
     * @brief 修复索引结构损坏
     */
    size_t repair(uint8_t* file_data, size_t file_size,
                  const std::vector<IndexCorruption>& corruptions);

    /**
     * @brief 从 INODE 页检测根页
     *
     * INODE 页位于 page 2，包含根页信息
     */
    std::optional<uint32_t> detect_root_from_inode(const uint8_t* file_data, size_t file_size);

    /**
     * @brief 从页结构推断根页
     *
     * 当 INODE 损坏时，通过分析页层级推断根页
     */
    std::optional<uint32_t> infer_root_page(const uint8_t* file_data, size_t file_size);

    /**
     * @brief 修复节点指针
     *
     * @param file_data 文件数据
     * @param page_no 损坏的页号
     * @param expected_child 期望的子页号
     * @return true 修复成功
     */
    bool repair_node_pointer(uint8_t* file_data, uint32_t page_no, uint32_t expected_child);

    /**
     * @brief 修复层级不匹配
     *
     * 调整页的 level 字段
     */
    bool repair_level_mismatch(uint8_t* file_data, uint32_t page_no, uint8_t correct_level);

    /**
     * @brief 修复索引ID不匹配
     *
     * 统一页的索引ID
     */
    bool repair_index_id_mismatch(uint8_t* file_data, uint32_t page_no, uint32_t correct_index_id);

    /**
     * @brief 重建页链
     *
     * 修复断裂的 prev/next 指针
     */
    bool rebuild_page_chain(uint8_t* file_data, size_t file_size);

    /**
     * @brief 基于内容重建页链（适用于二级索引碎片化）
     *
     * 提取叶页的 min/max key，按 key range 排序后重连 prev/next 指针
     * @param file_data 文件数据
     * @param file_size 文件大小
     * @param index_id 目标索引 ID
     * @return true 如果至少重连了一个页
     */
    bool rebuild_page_chain_by_content(uint8_t* file_data, size_t file_size, uint64_t index_id);

    /**
     * @brief 提取指定索引的 key ranges
     * @param file_data 文件数据
     * @param file_size 文件大小
     * @param index_id 目标索引 ID
     * @return key ranges 列表
     */
    std::vector<innodb::PageKeyRange> extract_key_ranges(uint8_t* file_data, size_t file_size, uint64_t index_id);

    /**
     * @brief 检测孤儿页
     *
     * 找出不在任何链中的页
     */
    std::vector<uint32_t> detect_orphan_pages(const uint8_t* file_data, size_t file_size);

    /**
     * @brief 检测页链循环
     */
    std::vector<std::vector<uint32_t>> detect_page_chain_loops(const uint8_t* file_data, size_t file_size);

    /**
     * @brief 获取页信息
     */
    std::optional<BTreePageInfo> get_page_info(const uint8_t* file_data, size_t file_size, uint32_t page_no) const;

    /**
     * @brief 获取所有页信息
     */
    std::map<uint32_t, BTreePageInfo> get_all_page_info(const uint8_t* file_data, size_t file_size) const;

    /**
     * @brief 验证 B+Tree 结构完整性
     */
    bool validate_btree_structure(const uint8_t* file_data, size_t file_size);

    /**
     * @brief 获取最后错误
     */
    const std::string& get_last_error() const { return last_error_; }

    /**
     * @brief 获取修复统计
     */
    size_t get_repaired_count() const { return repaired_count_; }

    /**
     * @brief 清空统计
     */
    void clear_stats() { repaired_count_ = 0; }

private:
    size_t page_size_ = 16384;
    std::string last_error_;
    size_t repaired_count_ = 0;

    /**
     * @brief 解析 B+Tree 页头
     */
    BTreePageInfo parse_page_header(const uint8_t* page_data) const;

    /**
     * @brief 提取子节点指针（非叶节点）
     */
    std::vector<uint32_t> extract_child_pointers(const uint8_t* page_data, size_t page_size) const;

    /**
     * @brief 找到同一层级的页链
     */
    std::vector<std::vector<uint32_t>> find_level_chains(const uint8_t* file_data, size_t file_size) const;
};

/**
 * @brief INODE 页修复器
 *
 * 专门处理 INODE 页（page 2）损坏
 */
class InodePageRepairer {
public:
    InodePageRepairer();
    ~InodePageRepairer() = default;

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size) { page_size_ = page_size; }

    /**
     * @brief 检测 INODE 页损坏
     */
    std::vector<IndexCorruption> detect_corruptions(const uint8_t* page_data, size_t page_size);

    /**
     * @brief 修复 INODE 页
     */
    bool repair(uint8_t* page_data, size_t page_size);

    /**
     * @brief 从 INODE 提取根页号
     */
    std::optional<uint32_t> extract_root_page(const uint8_t* page_data, size_t page_size) const;

    /**
     * @brief 写入根页信息到 INODE
     */
    bool write_root_page(uint8_t* page_data, size_t page_size, uint32_t root_page_no);

    /**
     * @brief 重建 INODE 页
     *
     * 当 INODE 完全损坏时，从扫描结果重建
     */
    bool rebuild_inode(uint8_t* page_data, size_t page_size,
                       const std::vector<uint32_t>& root_pages);

    /**
     * @brief 获取最后错误
     */
    const std::string& get_last_error() const { return last_error_; }

private:
    size_t page_size_ = 16384;
    std::string last_error_;

    // INODE 常量
    static constexpr size_t INODE_PAGE_NO = 2;
    static constexpr size_t INODE_OFFSET = 38;  // FIL_HEADER 之后
    static constexpr size_t INODE_ENTRY_SIZE = 12;
};

/**
 * @brief 索引结构处理器 (检测+修复一站式)
 */
class IndexStructureHandler {
public:
    IndexStructureHandler() = default;

    /**
     * @brief 分析并修复索引结构
     */
    size_t analyze_and_repair(uint8_t* file_data, size_t file_size);

    /**
     * @brief 仅分析
     */
    std::vector<IndexCorruption> analyze(const uint8_t* file_data, size_t file_size);

    /**
     * @brief 获取修复器
     */
    IndexCorruptionRepairer& get_repairer() { return repairer_; }

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size) {
        page_size_ = page_size;
        repairer_.set_page_size(page_size);
        inode_repairer_.set_page_size(page_size);
    }

    /**
     * @brief 获取报告
     */
    std::string get_report() const;

private:
    size_t page_size_ = 16384;
    IndexCorruptionRepairer repairer_;
    InodePageRepairer inode_repairer_;
    std::vector<IndexCorruption> last_corruptions_;
};

} // namespace repair
} // namespace dbrecovery

#endif // DBRECOVERY_REPAIR_INDEX_CORRUPTION_REPAIR_HPP
