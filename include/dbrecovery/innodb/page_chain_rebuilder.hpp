#ifndef DBRECOVERY_INNODB_PAGE_CHAIN_REBUILDER_HPP
#define DBRECOVERY_INNODB_PAGE_CHAIN_REBUILDER_HPP

#include <vector>
#include <cstdint>
#include <unordered_map>
#include <memory>
#include <sstream>
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/sec_index_btree_repair.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// 链重连统计结构体定义（在类之前声明）
struct ChainRebuildStats {
    size_t total_pages = 0;
    size_t fixed_chains = 0;
    size_t orphan_pages = 0;
    size_t circular_references = 0;
    uint32_t page_size = 16384;  // 页大小

    std::string to_string() const {
        std::ostringstream oss;
        oss << "ChainRebuildStats{total=" << total_pages
            << ", fixed=" << fixed_chains
            << ", orphan=" << orphan_pages
            << ", circular=" << circular_references
            << ", page_size=" << page_size << "}";
        return oss.str();
    }
};

/**
 * @brief 页链重连器
 * @details 修复断裂的 B+ 树，通过分析页链关系将分散的页重新连接
 */
class PageChainRebuilder {
public:
    PageChainRebuilder();
    explicit PageChainRebuilder(uint32_t page_size);

    /**
     * @brief 重连页链
     * @param pages 扫描到的页列表
     * @return 重连后的页链信息
     */
    bool rebuild_chain(const std::vector<ScannedPage>& pages);

    /**
     * @brief 获取重连后的页链
     */
    const std::vector<ScannedPage>& get_rebuilt_pages() const { return rebuilt_pages_; }

    /**
     * @brief 获取重连统计
     */
    const ChainRebuildStats& stats() const { return rebuild_stats_; }

    /**
     * @brief 修复页链中的断裂点
     * @details 通过分析 prev/next 指针，修复不一致的链表关系
     */
    void fix_chain_breaks();

    /**
     * @brief 检测页链中的循环引用
     */
    bool has_circular_reference() const;

    /**
     * @brief 获取页链中的孤立页（不在任何链中）
     */
    std::vector<size_t> get_orphan_pages() const;

    /**
     * @brief 按页号排序页链
     */
    void sort_by_page_number();

    /**
     * @brief 构建页号到索引的映射
     */
    std::unordered_map<uint32_t, size_t> build_page_index_map() const;

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& s) { schema_ = s; }

    /**
     * @brief 设置页大小
     */
    void set_page_size(uint32_t page_size) { page_size_ = page_size; }

    /**
     * @brief 获取页大小
     */
    uint32_t get_page_size() const { return page_size_; }

    /**
     * @brief 基于内容排序重连页链
     * @param page_data 页号到原始页数据的映射
     * @return true 如果至少重连了一个页
     */
    bool fix_chain_breaks_content_based(const std::map<uint32_t, std::vector<uint8_t>>& page_data);

private:
    std::vector<ScannedPage> rebuilt_pages_;
    ChainRebuildStats rebuild_stats_;
    std::unordered_map<uint32_t, size_t> page_index_map_;
    uint32_t page_size_ = 16384;  // 页大小
    catalog::TableSchema schema_;  // 表结构

    // 检测循环引用
    bool detect_circular_reference() const;

    // 标记孤立页
    void mark_orphan_pages();
};

// ==================== 实现 ====================

inline PageChainRebuilder::PageChainRebuilder() {
    rebuild_stats_.total_pages = 0;
    rebuild_stats_.fixed_chains = 0;
    rebuild_stats_.orphan_pages = 0;
    rebuild_stats_.circular_references = 0;
    rebuild_stats_.page_size = 16384;
    page_size_ = 16384;
}

inline PageChainRebuilder::PageChainRebuilder(uint32_t page_size) {
    rebuild_stats_.total_pages = 0;
    rebuild_stats_.fixed_chains = 0;
    rebuild_stats_.orphan_pages = 0;
    rebuild_stats_.circular_references = 0;
    rebuild_stats_.page_size = page_size;
    page_size_ = page_size;
}

inline bool PageChainRebuilder::rebuild_chain(const std::vector<ScannedPage>& pages) {
    if (pages.empty()) {
        rebuild_stats_.total_pages = 0;
        return false;
    }

    // 初始化
    rebuilt_pages_ = pages;
    rebuild_stats_.total_pages = pages.size();
    rebuild_stats_.fixed_chains = 0;
    rebuild_stats_.orphan_pages = 0;
    rebuild_stats_.circular_references = 0;

    // 构建页号索引
    page_index_map_.clear();
    for (size_t i = 0; i < rebuilt_pages_.size(); ++i) {
        page_index_map_[rebuilt_pages_[i].page_no] = i;
    }

    // 修复链断裂
    fix_chain_breaks();

    // 检测循环引用
    if (detect_circular_reference()) {
        rebuild_stats_.circular_references++;
    }

    // 标记孤立页
    mark_orphan_pages();

    return true;
}

inline void PageChainRebuilder::fix_chain_breaks() {
    if (rebuilt_pages_.empty()) {
        return;
    }

    // 按页号排序以确保顺序正确
    std::vector<ScannedPage> sorted_pages = rebuilt_pages_;
    std::sort(sorted_pages.begin(), sorted_pages.end(),
              [](const ScannedPage& a, const ScannedPage& b) {
                  return a.page_no < b.page_no;
              });

    // 第一遍：标记孤立页（prev 和 next 都是 0xFFFFFFFF）
    std::vector<bool> is_orphan(sorted_pages.size(), false);
    for (size_t i = 0; i < sorted_pages.size(); ++i) {
        if (sorted_pages[i].prev_page == 0xFFFFFFFF && sorted_pages[i].next_page == 0xFFFFFFFF) {
            is_orphan[i] = true;
        }
    }

    // 第二遍：只修复非孤立页的链指针
    for (size_t i = 0; i < sorted_pages.size(); ++i) {
        if (is_orphan[i]) {
            continue;  // 跳过孤立页
        }
        
        auto& page = sorted_pages[i];

        // 设置 prev 指针
        if (i > 0 && !is_orphan[i - 1]) {
            page.prev_page = sorted_pages[i - 1].page_no;
        } else {
            page.prev_page = 0xFFFFFFFF;  // 第一页或链的起始页
        }

        // 设置 next 指针
        if (i < sorted_pages.size() - 1 && !is_orphan[i + 1]) {
            page.next_page = sorted_pages[i + 1].page_no;
        } else {
            page.next_page = 0xFFFFFFFF;  // 最后一页或链的结束页
        }
    }

    rebuilt_pages_ = sorted_pages;
}

inline bool PageChainRebuilder::detect_circular_reference() const {
    if (rebuilt_pages_.empty()) {
        return false;
    }

    // 使用快慢指针检测循环
    for (size_t start = 0; start < rebuilt_pages_.size(); ++start) {
        size_t slow = start;
        size_t fast = start;

        while (true) {
            // 慢指针走一步
            auto it = page_index_map_.find(rebuilt_pages_[slow].next_page);
            if (it == page_index_map_.end() || it->second == slow) {
                break;
            }
            slow = it->second;

            // 快指针走两步
            auto it1 = page_index_map_.find(rebuilt_pages_[fast].next_page);
            if (it1 == page_index_map_.end() || it1->second == fast) {
                break;
            }
            fast = it1->second;

            auto it2 = page_index_map_.find(rebuilt_pages_[fast].next_page);
            if (it2 == page_index_map_.end() || it2->second == fast) {
                break;
            }
            fast = it2->second;

            // 如果快慢指针相遇，说明有循环
            if (slow == fast) {
                return true;
            }
        }
    }

    return false;
}

inline bool PageChainRebuilder::has_circular_reference() const {
    return detect_circular_reference();
}

inline std::vector<size_t> PageChainRebuilder::get_orphan_pages() const {
    std::vector<size_t> orphan_indices;

    for (size_t i = 0; i < rebuilt_pages_.size(); ++i) {
        const auto& page = rebuilt_pages_[i];

        // 孤立页的判断：
        // prev 和 next 都是 0xFFFFFFFF（没有任何链接）
        if (page.prev_page == 0xFFFFFFFF && page.next_page == 0xFFFFFFFF) {
            orphan_indices.push_back(i);
        }
    }

    return orphan_indices;
}

inline void PageChainRebuilder::mark_orphan_pages() {
    auto orphan_indices = get_orphan_pages();
    rebuild_stats_.orphan_pages = orphan_indices.size();
}

inline void PageChainRebuilder::sort_by_page_number() {
    std::sort(rebuilt_pages_.begin(), rebuilt_pages_.end(),
              [](const ScannedPage& a, const ScannedPage& b) {
                  return a.page_no < b.page_no;
              });

    // 更新索引映射
    page_index_map_.clear();
    for (size_t i = 0; i < rebuilt_pages_.size(); ++i) {
        page_index_map_[rebuilt_pages_[i].page_no] = i;
    }
}

inline bool PageChainRebuilder::fix_chain_breaks_content_based(
    const std::map<uint32_t, std::vector<uint8_t>>& page_data) {

    if (rebuilt_pages_.empty()) return false;

    // Extract page numbers from rebuilt_pages_
    std::vector<uint32_t> page_nos;
    for (const auto& p : rebuilt_pages_) {
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
    // The repairer's page_data_map_ was modified in-place with new FIL_PAGE_PREV/NEXT
    for (auto& page : rebuilt_pages_) {
        auto it = page_data.find(page.page_no);
        if (it == page_data.end()) continue;

        // Read updated prev/next from the page data
        if (it->second.size() >= page_size_) {
            page.prev_page = read_be<uint32_t>(it->second.data(), FIL_PAGE_PREV_ABS);
            page.next_page = read_be<uint32_t>(it->second.data(), FIL_PAGE_NEXT_ABS);
        }
    }

    rebuild_stats_.fixed_chains += repairer.get_stats().chain_breaks_fixed;
    return repairer.get_stats().pages_relinked > 0;
}

inline std::unordered_map<uint32_t, size_t> PageChainRebuilder::build_page_index_map() const {
    std::unordered_map<uint32_t, size_t> map;
    for (size_t i = 0; i < rebuilt_pages_.size(); ++i) {
        map[rebuilt_pages_[i].page_no] = i;
    }
    return map;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_CHAIN_REBUILDER_HPP
