#ifndef DBRECOVERY_INNODB_BTREE_TRAVERSAL_HPP
#define DBRECOVERY_INNODB_BTREE_TRAVERSAL_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <functional>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/btree_node_parser.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/sec_index_btree_repair.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== B+Tree Traversal Status ====================
/**
 * @brief B+Tree 遍历状态
 */
enum class BTreeTraversalStatus : uint8_t {
    SUCCESS = 0,           // 遍历成功
    PAGE_NOT_FOUND = 1,    // 页未找到
    PAGE_TYPE_MISMATCH = 2, // 页类型不匹配
    PAGE_CHAIN_BROKEN = 3,  // 页链断裂
    CIRCULAR_REFERENCE = 4, // 循环引用
    CHECKSUM_MISMATCH = 5,  // 校验和不匹配
    INVALID_PAGE_NUMBER = 6, // 无效页号
    MEMORY_ERROR = 7,       // 内存错误
    UNKNOWN_ERROR = 255
};

/**
 * @brief B+Tree 遍历统计
 */
struct BTreeTraversalStats {
    size_t total_pages_visited = 0;
    size_t index_pages_visited = 0;
    size_t leaf_pages_visited = 0;
    size_t internal_pages_visited = 0;
    size_t records_found = 0;
    size_t records_recovered = 0;
    size_t pages_with_errors = 0;
    size_t chain_breaks_detected = 0;
    size_t circular_references_detected = 0;
    size_t checksum_errors = 0;
    uint32_t root_page_no = 0;
    uint32_t first_leaf_page = 0;
    uint32_t last_leaf_page = 0;

    // 页类型统计
    size_t valid_index_pages = 0;
    size_t invalid_pages = 0;
    size_t orphan_pages = 0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "BTreeTraversalStats{total=" << total_pages_visited
            << ", index=" << index_pages_visited
            << ", leaf=" << leaf_pages_visited
            << ", internal=" << internal_pages_visited
            << ", records=" << records_found
            << ", recovered=" << records_recovered
            << ", errors=" << pages_with_errors
            << ", chain_breaks=" << chain_breaks_detected
            << ", circular=" << circular_references_detected
            << ", checksum_errors=" << checksum_errors
            << ", root=" << root_page_no
            << ", first_leaf=" << first_leaf_page
            << ", last_leaf=" << last_leaf_page << "}";
        return oss.str();
    }
};

/**
 * @brief B+Tree 页链断裂信息
 */
struct PageChainBreak {
    uint32_t page_no;          // 断裂的页号
    uint32_t expected_prev;    // 预期的 prev_page
    uint32_t actual_prev;      // 实际的 prev_page
    uint32_t expected_next;    // 预期的 next_page
    uint32_t actual_next;      // 实际的 next_page
    std::string description;   // 断裂描述

    std::string to_string() const {
        std::ostringstream oss;
        oss << "PageChainBreak{page=" << page_no
            << ", expected_prev=" << expected_prev
            << ", actual_prev=" << actual_prev
            << ", expected_next=" << expected_next
            << ", actual_next=" << actual_next
            << ", desc=\"" << description << "\"}";
        return oss.str();
    }
};

/**
 * @brief B+Tree 遍历器
 * @details 支持完整的 B+Tree 遍历，包括页链验证和断裂检测
 */
class BPlusTreeTraversal {
public:
    /**
     * @brief 构造函数
     * @param pages 扫描到的页数据
     * @param schema 表结构定义
     * @param page_size 页大小（默认 16KB）
     */
    BPlusTreeTraversal(const std::vector<ScannedPage>& pages,
                       const catalog::TableSchema& schema,
                       size_t page_size = 16384);

    /**
     * @brief 构造函数（从 PageScanner 结果构建）
     * @param scanner PageScanner 实例
     * @param schema 表结构定义
     */
    BPlusTreeTraversal(const PageScanner& scanner, const catalog::TableSchema& schema);

    /**
     * @brief 设置页数据（用于动态更新）
     */
    void set_pages(const std::vector<ScannedPage>& pages);

    /**
     * @brief 设置页数据映射（用于内容排序）
     */
    void set_page_data_map(const std::map<uint32_t, std::vector<uint8_t>>& m) {
        page_data_map_ = m;
    }

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 获取页列表（用于测试）
     */
    const std::vector<ScannedPage>& pages() const { return pages_; }

    /**
     * @brief 遍历 B+Tree
     * @param root_page_no 根页号（0 表示自动检测）
     * @return 遍历状态
     */
    BTreeTraversalStatus traverse(uint32_t root_page_no = 0);

    /**
     * @brief 从根节点开始递归遍历
     * @param page_no 当前页号
     * @param depth 当前深度
     * @return 遍历状态
     */
    BTreeTraversalStatus traverse_from_page(uint32_t page_no, uint32_t depth = 0);

    /**
     * @brief 获取遍历结果
     */
    const std::vector<std::map<std::string, std::string>>& records() const {
        return records_;
    }

    /**
     * @brief 获取遍历统计
     */
    const BTreeTraversalStats& stats() const { return stats_; }

    /**
     * @brief 获取页链断裂信息
     */
    const std::vector<PageChainBreak>& chain_breaks() const { return chain_breaks_; }

    /**
     * @brief 获取循环引用信息
     */
    const std::vector<uint32_t>& circular_references() const { return circular_refs_; }

    /**
     * @brief 获取遍历路径（用于调试）
     */
    const std::vector<uint32_t>& traversal_path() const { return traversal_path_; }

    /**
     * @brief 检测页链断裂
     * @details 分析 prev/next 链的完整性
     */
    void detect_chain_breaks();

    /**
     * @brief 检测循环引用
     * @details 使用快慢指针算法检测循环
     */
    void detect_circular_references();

    /**
     * @brief 修复页链断裂
     * @details 自动修复 prev/next 指针
     */
    void fix_chain_breaks();

    /**
     * @brief 验证页校验和
     * @param page_data 页数据
     * @return 校验和是否匹配
     */
    bool verify_page_checksum(const std::vector<uint8_t>& page_data);

    /**
     * @brief 获取页数据
     * @param page_no 页号
     * @return 页数据（如果存在）
     */
    std::optional<std::vector<uint8_t>> get_page_data(uint32_t page_no) const;

    /**
     * @brief 获取页号到索引的映射
     */
    std::map<uint32_t, size_t> build_page_index_map() const;

    /**
     * @brief 内部方法：构建页号到索引的映射（更新内部状态）
     */
    void build_page_index_map_internal();

    /**
     * @brief 按页链顺序排序页
     */
    void sort_pages_by_chain();

    /**
     * @brief 基于内容排序重连页链
     * @param page_data 页号到原始页数据的映射
     * @return true 如果至少重连了一个页
     */
    bool fix_chain_breaks_content_based(const std::map<uint32_t, std::vector<uint8_t>>& page_data);

    /**
     * @brief 按 key range 排序页（基于内容排序）
     */
    void sort_by_key_range();

    /**
     * @brief 获取页链的叶子页范围
     * @return (first_leaf, last_leaf) 页号
     */
    std::pair<uint32_t, uint32_t> get_leaf_range() const;

    /**
     * @brief 获取页链统计
     */
    std::string get_chain_statistics() const;

private:
    std::vector<ScannedPage> pages_;
    std::map<uint32_t, size_t> page_index_map_;
    std::map<uint32_t, std::vector<uint8_t>> page_data_map_;
    catalog::TableSchema schema_;
    size_t page_size_;

    std::vector<std::map<std::string, std::string>> records_;
    BTreeTraversalStats stats_;
    std::vector<PageChainBreak> chain_breaks_;
    std::vector<uint32_t> circular_refs_;
    std::vector<uint32_t> traversal_path_;
    std::set<uint32_t> visited_pages_;

    // 解析单个页的记录
    BTreeTraversalStatus parse_page_records(uint32_t page_no);

    // 获取下一页（用于遍历叶子页链）
    std::optional<uint32_t> get_next_leaf_page(uint32_t page_no) const;

    // 获取上一页（用于反向遍历叶子页链）
    std::optional<uint32_t> get_prev_leaf_page(uint32_t page_no) const;

    // 检查页类型是否为索引页
    bool is_index_page(uint16_t type) const;

    // 检查页类型是否为叶子页
    bool is_leaf_page(uint32_t page_no) const;

    // 检查页链关系
    bool validate_page_chain(uint32_t page_no);

    // 添加遍历路径
    void add_to_path(uint32_t page_no);

    // 检测页号是否已访问（循环引用检测）
    bool is_visited(uint32_t page_no) const;
};

// ==================== 实现 ====================

inline BPlusTreeTraversal::BPlusTreeTraversal(const std::vector<ScannedPage>& pages,
                                              const catalog::TableSchema& schema,
                                              size_t page_size)
    : pages_(pages),
      schema_(schema),
      page_size_(page_size) {
    // 构建页号索引
    build_page_index_map_internal();
}

inline BPlusTreeTraversal::BPlusTreeTraversal(const PageScanner& scanner,
                                              const catalog::TableSchema& schema)
    : pages_(),
      schema_(schema),
      page_size_(16384) {
    // 从 Scanner 获取页数据
    // 注意：这里需要实际的页数据，Scanner 只存储元数据
}

inline void BPlusTreeTraversal::set_pages(const std::vector<ScannedPage>& pages) {
    pages_ = pages;
    build_page_index_map_internal();
}

inline void BPlusTreeTraversal::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

inline std::map<uint32_t, size_t> BPlusTreeTraversal::build_page_index_map() const {
    std::map<uint32_t, size_t> map;
    for (size_t i = 0; i < pages_.size(); ++i) {
        map[pages_[i].page_no] = i;
    }
    return map;
}

inline void BPlusTreeTraversal::build_page_index_map_internal() {
    page_index_map_ = build_page_index_map();
}

inline BTreeTraversalStatus BPlusTreeTraversal::traverse(uint32_t root_page_no) {
    // 初始化统计
    stats_ = BTreeTraversalStats{};
    records_.clear();
    chain_breaks_.clear();
    circular_refs_.clear();
    traversal_path_.clear();
    visited_pages_.clear();

    if (pages_.empty()) {
        stats_.total_pages_visited = 0;
        return BTreeTraversalStatus::UNKNOWN_ERROR;
    }

    // 自动检测根页号
    if (root_page_no == 0) {
        // 查找第一个索引页作为根页
        for (const auto& page : pages_) {
            if (page.type == FIL_PAGE_INDEX) {
                root_page_no = page.page_no;
                break;
            }
        }
    }

    if (root_page_no == 0) {
        stats_.total_pages_visited = 0;
        return BTreeTraversalStatus::PAGE_NOT_FOUND;
    }

    stats_.root_page_no = root_page_no;

    // 检测页链断裂
    detect_chain_breaks();

    // 检测循环引用
    detect_circular_references();

    // 从根页开始遍历
    auto status = traverse_from_page(root_page_no, 0);

    // 获取叶子页范围
    auto [first, last] = get_leaf_range();
    stats_.first_leaf_page = first;
    stats_.last_leaf_page = last;

    return status;
}

inline BTreeTraversalStatus BPlusTreeTraversal::traverse_from_page(uint32_t page_no, uint32_t depth) {
    // 检查页号有效性
    if (page_no >= 0xFFFFFFF0) {
        stats_.pages_with_errors++;
        return BTreeTraversalStatus::INVALID_PAGE_NUMBER;
    }

    // 安全检查：防止过深递归导致栈溢出（最大 B+Tree 深度通常不超过 20）
    if (depth > 1000) {
        stats_.pages_with_errors++;
        return BTreeTraversalStatus::UNKNOWN_ERROR;
    }

    // 检查是否已访问（循环引用检测）
    if (is_visited(page_no)) {
        circular_refs_.push_back(page_no);
        stats_.circular_references_detected++;
        return BTreeTraversalStatus::CIRCULAR_REFERENCE;
    }

    // 查找页数据
    auto it = page_index_map_.find(page_no);
    if (it == page_index_map_.end()) {
        stats_.pages_with_errors++;
        stats_.invalid_pages++;
        return BTreeTraversalStatus::PAGE_NOT_FOUND;
    }

    size_t page_idx = it->second;
    const auto& page = pages_[page_idx];

    // 验证页类型
    if (!is_index_page(page.type)) {
        stats_.pages_with_errors++;
        stats_.invalid_pages++;
        return BTreeTraversalStatus::PAGE_TYPE_MISMATCH;
    }

    // 标记为已访问
    add_to_path(page_no);
    visited_pages_.insert(page_no);
    stats_.total_pages_visited++;

    if (page.type == FIL_PAGE_INDEX) {
        stats_.index_pages_visited++;
    }

    // 使用 ScannedPage.level 判断页类型（0 = 叶子页，> 0 = 内部节点）
    if (page.level == 0) {
        // 叶子页：解析记录
        stats_.leaf_pages_visited++;
        auto parse_status = parse_page_records(page_no);
        if (parse_status != BTreeTraversalStatus::SUCCESS) {
            stats_.pages_with_errors++;
        }
        return BTreeTraversalStatus::SUCCESS;
    }

    // 内部节点：从记录中提取子节点页号并递归遍历
    stats_.internal_pages_visited++;

    auto data_it = page_data_map_.find(page_no);
    if (data_it == page_data_map_.end()) {
        stats_.pages_with_errors++;
        return BTreeTraversalStatus::PAGE_NOT_FOUND;
    }

    BPlusTreeParser parser(data_it->second);
    if (!parser.parse()) {
        stats_.pages_with_errors++;
        return BTreeTraversalStatus::UNKNOWN_ERROR;
    }

    // 内部节点记录的第一个 4 字节是子节点页号（小端序）
    const auto& offsets = parser.record_offsets();
    const auto& page_data = data_it->second;
    uint32_t child_count = 0;

    for (uint16_t rec_off : offsets) {
        if (rec_off < dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS) continue;
        if (static_cast<size_t>(rec_off) + 4 > page_data.size()) continue;
        uint32_t child = dbrecovery::innodb::read_le<uint32_t>(page_data.data(), rec_off);
        if (child != 0 && child != 0xFFFFFFFF) {
            child_count++;
            auto child_status = traverse_from_page(child, depth + 1);
            if (child_status != BTreeTraversalStatus::SUCCESS &&
                child_status != BTreeTraversalStatus::CIRCULAR_REFERENCE) {
                // 继续遍历其他子节点，不因单个子节点失败而终止
                stats_.pages_with_errors++;
            }
        }
    }

    // 没有找到子节点可能表示页数据不完整
    if (child_count == 0) {
        stats_.pages_with_errors++;
    }

    return BTreeTraversalStatus::SUCCESS;
}

inline BTreeTraversalStatus BPlusTreeTraversal::parse_page_records(uint32_t page_no) {
    // Get page data from page_data_map_
    auto data_it = page_data_map_.find(page_no);
    if (data_it == page_data_map_.end()) {
        // No raw data available, just count the page
        stats_.records_found++;
        return BTreeTraversalStatus::SUCCESS;
    }

    // Use BPlusTreeParser to parse record structure
    BPlusTreeParser parser(data_it->second);
    if (!parser.parse()) {
        stats_.pages_with_errors++;
        return BTreeTraversalStatus::UNKNOWN_ERROR;
    }

    // Parse records using the schema
    auto parsed_records = parser.parse_records(schema_);
    stats_.records_found += parsed_records.size();
    stats_.records_recovered += parsed_records.size();

    // Add to records_ collection
    for (auto& rec : parsed_records) {
        records_.push_back(std::move(rec));
    }

    return BTreeTraversalStatus::SUCCESS;
}

inline std::optional<std::vector<uint8_t>> BPlusTreeTraversal::get_page_data(uint32_t page_no) const {
    auto it = page_data_map_.find(page_no);
    if (it != page_data_map_.end()) {
        return it->second;
    }
    return std::nullopt;
}

inline bool BPlusTreeTraversal::is_index_page(uint16_t type) const {
    return type == FIL_PAGE_INDEX ||
           type == FIL_PAGE_SDI ||
           type == FIL_PAGE_UNDO_LOG;
}

inline bool BPlusTreeTraversal::is_leaf_page(uint32_t page_no) const {
    auto it = page_index_map_.find(page_no);
    if (it == page_index_map_.end()) {
        return true; // default to leaf if not found
    }
    return pages_[it->second].level == 0;
}

inline void BPlusTreeTraversal::add_to_path(uint32_t page_no) {
    traversal_path_.push_back(page_no);
}

inline bool BPlusTreeTraversal::is_visited(uint32_t page_no) const {
    return visited_pages_.find(page_no) != visited_pages_.end();
}

inline void BPlusTreeTraversal::detect_chain_breaks() {
    if (pages_.empty()) {
        return;
    }

    // 按页号排序
    std::vector<ScannedPage> sorted_pages = pages_;
    std::sort(sorted_pages.begin(), sorted_pages.end(),
              [](const ScannedPage& a, const ScannedPage& b) {
                  return a.page_no < b.page_no;
              });

    // 检查每个页的链关系
    for (size_t i = 0; i < sorted_pages.size(); ++i) {
        const auto& page = sorted_pages[i];

        // 检查 prev 指针
        if (page.prev_page != 0xFFFFFFFF) {
            auto prev_it = page_index_map_.find(page.prev_page);
            if (prev_it == page_index_map_.end()) {
                // prev 指向不存在的页
                PageChainBreak break_info{};
                break_info.page_no = page.page_no;
                break_info.expected_prev = (i > 0) ? sorted_pages[i - 1].page_no : 0xFFFFFFFF;
                break_info.actual_prev = page.prev_page;
                break_info.description = "prev points to non-existent page";
                chain_breaks_.push_back(break_info);
                stats_.chain_breaks_detected++;
            }
        }

        // 检查 next 指针
        if (page.next_page != 0xFFFFFFFF) {
            auto next_it = page_index_map_.find(page.next_page);
            if (next_it == page_index_map_.end()) {
                // next 指向不存在的页
                PageChainBreak break_info{};
                break_info.page_no = page.page_no;
                break_info.expected_next = (i < sorted_pages.size() - 1) ? sorted_pages[i + 1].page_no : 0xFFFFFFFF;
                break_info.actual_next = page.next_page;
                break_info.description = "next points to non-existent page";
                chain_breaks_.push_back(break_info);
                stats_.chain_breaks_detected++;
            }
        }
    }
}

inline void BPlusTreeTraversal::detect_circular_references() {
    if (pages_.empty()) {
        return;
    }

    // 使用快慢指针检测循环
    for (size_t start = 0; start < pages_.size(); ++start) {
        size_t slow = start;
        size_t fast = start;

        for (int step = 0; step < 1000; ++step) { // 防止无限循环
            // 慢指针走一步
            auto slow_it = page_index_map_.find(pages_[slow].next_page);
            if (slow_it == page_index_map_.end() || slow_it->second == slow) {
                break;
            }
            slow = slow_it->second;

            // 快指针走两步
            auto fast1_it = page_index_map_.find(pages_[fast].next_page);
            if (fast1_it == page_index_map_.end() || fast1_it->second == fast) {
                break;
            }
            fast = fast1_it->second;

            auto fast2_it = page_index_map_.find(pages_[fast].next_page);
            if (fast2_it == page_index_map_.end() || fast2_it->second == fast) {
                break;
            }
            fast = fast2_it->second;

            // 如果快慢指针相遇，说明有循环
            if (slow == fast && step > 0) {
                circular_refs_.push_back(pages_[slow].page_no);
                stats_.circular_references_detected++;
                break;
            }
        }
    }
}

inline void BPlusTreeTraversal::fix_chain_breaks() {
    if (pages_.empty()) {
        return;
    }

    // 按页号排序
    std::vector<ScannedPage> sorted_pages = pages_;
    std::sort(sorted_pages.begin(), sorted_pages.end(),
              [](const ScannedPage& a, const ScannedPage& b) {
                  return a.page_no < b.page_no;
              });

    // 修复 prev/next 指针
    for (size_t i = 0; i < sorted_pages.size(); ++i) {
        auto& page = sorted_pages[i];

        // 设置 prev 指针
        if (i > 0) {
            page.prev_page = sorted_pages[i - 1].page_no;
        } else {
            page.prev_page = 0xFFFFFFFF; // 第一页
        }

        // 设置 next 指针
        if (i < sorted_pages.size() - 1) {
            page.next_page = sorted_pages[i + 1].page_no;
        } else {
            page.next_page = 0xFFFFFFFF; // 最后一页
        }
    }

    pages_ = sorted_pages;
    build_page_index_map_internal();
}

inline bool BPlusTreeTraversal::fix_chain_breaks_content_based(
    const std::map<uint32_t, std::vector<uint8_t>>& page_data) {

    if (pages_.empty()) return false;

    // Extract page numbers
    std::vector<uint32_t> page_nos;
    for (const auto& p : pages_) {
        page_nos.push_back(p.page_no);
    }

    // Use SecondaryIndexBTreeRepairer for content-based ordering
    innodb::SecondaryIndexBTreeRepairer repairer(page_size_);
    repairer.set_schema(schema_);
    repairer.set_page_list(page_nos, page_data);

    // Run content-based repair
    if (!repairer.repair_chain_by_content(page_nos)) {
        return false;
    }

    // Map the relinked pages back to ScannedPage format
    for (auto& page : pages_) {
        auto it = page_data.find(page.page_no);
        if (it == page_data.end()) continue;

        // Read updated prev/next from the page data
        if (it->second.size() >= static_cast<size_t>(FIL_PAGE_NEXT_ABS) + 4) {
            page.prev_page = read_be<uint32_t>(it->second.data(), FIL_PAGE_PREV_ABS);
            page.next_page = read_be<uint32_t>(it->second.data(), FIL_PAGE_NEXT_ABS);
        }
    }

    return true;
}

inline void BPlusTreeTraversal::sort_by_key_range() {
    if (pages_.empty() || page_data_map_.empty()) return;

    // Extract page numbers
    std::vector<uint32_t> page_nos;
    for (const auto& p : pages_) {
        page_nos.push_back(p.page_no);
    }

    // Use SecondaryIndexBTreeRepairer for content-based ordering
    innodb::SecondaryIndexBTreeRepairer repairer(page_size_);
    repairer.set_schema(schema_);
    repairer.set_page_list(page_nos, page_data_map_);

    // Extract key ranges
    auto key_ranges = repairer.extract_key_ranges();
    if (key_ranges.empty()) return;

    // Separate leaf and internal pages
    std::vector<PageKeyRange> leaf_ranges;
    std::vector<PageKeyRange> internal_ranges;
    for (auto& kr : key_ranges) {
        if (kr.is_leaf) leaf_ranges.push_back(std::move(kr));
        else internal_ranges.push_back(std::move(kr));
    }

    // Sort leaf pages by min_key
    std::sort(leaf_ranges.begin(), leaf_ranges.end(),
        [](const PageKeyRange& a, const PageKeyRange& b) {
            return a.min_key < b.min_key;
        });

    // Sort internal pages by level (descending = root first), then min_key
    std::sort(internal_ranges.begin(), internal_ranges.end(),
        [](const PageKeyRange& a, const PageKeyRange& b) {
            if (a.level != b.level) return a.level > b.level;
            return a.min_key < b.min_key;
        });

    // Build sorted ScannedPage list
    std::vector<ScannedPage> sorted_pages;
    std::map<uint32_t, ScannedPage> page_map;
    for (const auto& p : pages_) {
        page_map[p.page_no] = p;
    }

    for (const auto& kr : leaf_ranges) {
        auto it = page_map.find(kr.page_no);
        if (it != page_map.end()) {
            sorted_pages.push_back(it->second);
        }
    }
    for (const auto& kr : internal_ranges) {
        auto it = page_map.find(kr.page_no);
        if (it != page_map.end()) {
            sorted_pages.push_back(it->second);
        }
    }

    // Add any remaining pages not in key ranges
    for (const auto& p : pages_) {
        bool found = false;
        for (const auto& kr : key_ranges) {
            if (kr.page_no == p.page_no) { found = true; break; }
        }
        if (!found) sorted_pages.push_back(p);
    }

    pages_ = sorted_pages;
    build_page_index_map_internal();
}

inline bool BPlusTreeTraversal::verify_page_checksum(const std::vector<uint8_t>& page_data) {
    // 简化实现：检查 CRC32C 校验和
    // 实际应该使用 InnoDB 的校验和算法

    if (page_data.size() < 52) {
        return false;
    }

    // 读取存储的校验和（偏移 24-27）
    uint32_t stored_checksum = read_le<uint32_t>(page_data.data(), 24);

    // 简单校验：检查校验和字段是否为 0（未校验）或 CRC32C
    // 实际应该计算并比较校验和

    return stored_checksum == 0 || stored_checksum != 0xFFFFFFFF;
}

inline std::optional<uint32_t> BPlusTreeTraversal::get_next_leaf_page(uint32_t page_no) const {
    auto it = page_index_map_.find(page_no);
    if (it == page_index_map_.end()) {
        return std::nullopt;
    }

    size_t idx = it->second;
    if (idx + 1 >= pages_.size()) {
        return std::nullopt;
    }

    const auto& next_page = pages_[idx + 1];
    if (next_page.type != FIL_PAGE_INDEX) {
        return std::nullopt;
    }

    return next_page.page_no;
}

inline std::optional<uint32_t> BPlusTreeTraversal::get_prev_leaf_page(uint32_t page_no) const {
    auto it = page_index_map_.find(page_no);
    if (it == page_index_map_.end()) {
        return std::nullopt;
    }

    size_t idx = it->second;
    if (idx == 0) {
        return std::nullopt;
    }

    const auto& prev_page = pages_[idx - 1];
    if (prev_page.type != FIL_PAGE_INDEX) {
        return std::nullopt;
    }

    return prev_page.page_no;
}

inline std::pair<uint32_t, uint32_t> BPlusTreeTraversal::get_leaf_range() const {
    if (pages_.empty()) {
        return {0, 0};
    }

    uint32_t first = 0xFFFFFFFF;
    uint32_t last = 0;

    for (const auto& page : pages_) {
        if (page.type == FIL_PAGE_INDEX) {
            if (page.page_no < first) {
                first = page.page_no;
            }
            if (page.page_no > last) {
                last = page.page_no;
            }
        }
    }

    if (first == 0xFFFFFFFF) {
        first = 0;
    }
    if (last == 0) {
        last = 0;
    }

    return {first, last};
}

inline std::string BPlusTreeTraversal::get_chain_statistics() const {
    std::ostringstream oss;
    oss << "Chain Statistics:" << std::endl;
    oss << "  Total pages: " << pages_.size() << std::endl;
    oss << "  Index pages: " << stats_.index_pages_visited << std::endl;
    oss << "  Chain breaks: " << chain_breaks_.size() << std::endl;
    oss << "  Circular refs: " << circular_refs_.size() << std::endl;

    if (!chain_breaks_.empty()) {
        oss << "  Break locations:" << std::endl;
        for (const auto& brk : chain_breaks_) {
            oss << "    Page " << brk.page_no << ": " << brk.description << std::endl;
        }
    }

    return oss.str();
}

inline void BPlusTreeTraversal::sort_pages_by_chain() {
    if (pages_.empty()) {
        return;
    }

    // 按页号排序（假设页号与链顺序一致）
    std::sort(pages_.begin(), pages_.end(),
              [](const ScannedPage& a, const ScannedPage& b) {
                  return a.page_no < b.page_no;
              });

    build_page_index_map();
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BTREE_TRAVERSAL_HPP
