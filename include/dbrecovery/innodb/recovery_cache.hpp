#pragma once

#include <cstdint>
#include <unordered_map>
#include "dbrecovery/innodb/v2/types.hpp"

namespace dbrecovery::innodb {

// Redo 回放缓存（用于崩溃恢复）
struct RecoveryCache {
    // 表 ID -> 最新记录 LSN 映射
    std::unordered_map<uint64_t, uint64_t> table_latest_lsn;
    
    // 页 ID -> 页数据缓存
    struct PageData {
        uint32_t page_offset;
        std::vector<uint8_t> data;
        bool is_modified;
    };
    std::unordered_map<uint32_t, PageData> page_cache;
};

} // namespace dbrecovery::innodb
