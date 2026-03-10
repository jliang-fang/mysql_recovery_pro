#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <optional>
#include "dbrecovery/innodb/v2/types.hpp"

namespace dbrecovery {
namespace innodb {

// Redo 回放缓存（用于崩溃恢复）
struct RecoveryCache {
    // 表 ID -> 最新记录 LSN 映射
    std::unordered_map<uint64_t, uint64_t> table_latest_lsn;
    
    // 页 ID -> 页数据缓存
    struct PageData {
        uint32_t page_offset;
        std::vector<uint8_t> data;
        bool is_modified = false;
        
        PageData() : page_offset(0), data(), is_modified(false) {}
        PageData(uint32_t offset, const std::vector<uint8_t>& buf)
            : page_offset(offset), data(buf), is_modified(false) {}
    };
    
    std::unordered_map<uint32_t, PageData> page_cache;
    
    // ✅ 新增：获取页数据（若不存在则返回空）
    [[nodiscard]] std::optional<PageData> get_page(uint32_t page_offset) const {
        auto it = page_cache.find(page_offset);
        if (it != page_cache.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    
    // ✅ 新增：添加或更新页数据
    void add_page(uint32_t page_offset, const uint8_t* data, size_t size) {
        if (!data || size == 0) return;
        
        PageData pd{page_offset, std::vector<uint8_t>(data, data + size)};
        page_cache[page_offset] = std::move(pd);
    }
    
    // ✅ 新增：标记页为已修改
    void mark_modified(uint32_t page_offset) {
        auto it = page_cache.find(page_offset);
        if (it != page_cache.end()) {
            it->second.is_modified = true;
        }
    }
    
    // ✅ 新增：刷新脏页（返回脏页列表）
    [[nodiscard]] std::vector<std::pair<uint32_t, PageData>> flush_dirty_pages() {
        std::vector<std::pair<uint32_t, PageData>> dirty;
        for (auto it = page_cache.begin(); it != page_cache.end();) {
            if (it->second.is_modified) {
                dirty.emplace_back(it->first, std::move(it->second));
                it = page_cache.erase(it);
            } else {
                ++it;
            }
        }
        return dirty;
    }
    
    // ✅ 新增：应用 Redo 日志条目（占位实现）
    void apply_redo_log_entry(uint64_t lsn, uint32_t space_id, uint32_t page_no,
                              const uint8_t* log_data, size_t log_size) {
        if (!log_data || log_size == 0) return;
        
        // 获取或创建页数据
        auto it = page_cache.find(page_no);
        if (it == page_cache.end()) {
            // 如果页不存在，从磁盘加载（此处简化为分配新页）
            std::vector<uint8_t> page_data(16384, 0); // 默认16KB页
            it = page_cache.emplace(page_no, PageData{page_no, page_data}).first;
        }
        
        // 应用 Redo 日志（简化实现，实际需要解析 redo 记录类型）
        if (log_size + 38 <= it->second.data.size()) {
            std::memcpy(it->second.data.data() + 38, log_data, log_size);
        }
        
        // 标记页为已修改
        mark_modified(page_no);
    }
};

} // namespace innodb
} // namespace dbrecovery
