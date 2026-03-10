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
    
    // ✅ Redo 日志记录类型定义（参考 MySQL 8.0 mtr0mtr.h）
    enum class RedoLogType : uint8_t {
        MLOG_1BYTE = 1,
        MLOG_2BYTES = 2,
        MLOG_4BYTES = 3,
        MLOG_8BYTES = 4,
        MLOG_REC_INSERT = 10,
        MLOG_REC_UPDATE = 11,
        MLOG_REC_DELETE = 12,
        MLOG_PAGE_CREATE = 15,
        MLOG_PAGE_REINIT = 16,
        MLOG_INDEX_ALLOC = 17,
        MLOG_COMP_REC_INSERT = 30,
        MLOG_COMP_REC_UPDATE = 31,
        MLOG_COMP_REC_DELETE = 32,
        MLOG_COMP_PAGE_CREATE = 33,
        MLOG_COMP_PAGE_REINIT = 34,
        MLOG_BLOB_WRITE = 40,
        MLOG_MULTI_REC_END = 50,
        MLOG_CHECKPOINT = 51,
        MLOG_INDEX_LOAD = 52
    };
    
    // ✅ 新增：应用 Redo 日志条目（增强实现）
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
        
        size_t pos = 0;
        while (pos < log_size) {
            if (pos + 1 > log_size) break;
            
            uint8_t type = log_data[pos];
            pos++;
            
            switch (static_cast<RedoLogType>(type)) {
                case RedoLogType::MLOG_1BYTE:
                case RedoLogType::MLOG_2BYTES:
                case RedoLogType::MLOG_4BYTES:
                case RedoLogType::MLOG_8BYTES: {
                    // 简单的字节写入操作
                    if (pos + 2 > log_size) break;
                    
                    uint16_t offset = dbrecovery::innodb::read_le<uint16_t>(log_data, pos);
                    pos += 2;
                    
                    size_t length = 0;
                    switch (static_cast<RedoLogType>(type)) {
                        case RedoLogType::MLOG_1BYTE: length = 1; break;
                        case RedoLogType::MLOG_2BYTES: length = 2; break;
                        case RedoLogType::MLOG_4BYTES: length = 4; break;
                        case RedoLogType::MLOG_8BYTES: length = 8; break;
                        default: break;
                    }
                    
                    if (pos + length > log_size) break;
                    
                    // 边界检查
                    if (offset < 38 || offset + length > it->second.data.size()) {
                        pos += length;
                        continue;
                    }
                    
                    std::memcpy(it->second.data.data() + offset, log_data + pos, length);
                    pos += length;
                    break;
                }
                
                case RedoLogType::MLOG_PAGE_REINIT: {
                    // 页重新初始化
                    if (it->second.data.size() >= 38) {
                        std::memset(it->second.data.data() + 38, 0, it->second.data.size() - 38);
                    }
                    break;
                }
                
                case RedoLogType::MLOG_COMP_REC_INSERT:
                case RedoLogType::MLOG_COMP_REC_UPDATE:
                case RedoLogType::MLOG_COMP_REC_DELETE: {
                    // 复杂的记录操作，需要解析完整 redo record 格式
                    // 这里提供框架，实际实现需要更详细的 redo record 解析器
                    std::cerr << "[WARN] Complex redo log type " << (int)type 
                              << " not fully implemented yet" << std::endl;
                    
                    // 跳过未知类型的 redo 记录（简化处理）
                    // 实际应根据具体类型解析剩余数据
                    pos = log_size; // 暂时退出循环
                    break;
                }
                
                default: {
                    std::cerr << "[WARN] Unknown redo log type: " << (int)type 
                              << ", skipping remaining data" << std::endl;
                    pos = log_size; // 未知类型，退出循环
                    break;
                }
            }
        }
        
        // 标记页为已修改
        mark_modified(page_no);
    }
};

} // namespace innodb
} // namespace dbrecovery
