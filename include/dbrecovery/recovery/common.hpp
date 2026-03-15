#ifndef DBRECOVERY_RECOVERY_COMMON_HPP
#define DBRECOVERY_RECOVERY_COMMON_HPP
#include <cstdint>
#include <limits>

namespace dbrecovery {
namespace recovery {

struct Common {
    // BLOB 相关常量
    static constexpr size_t MAX_BLOB_SIZE = 1024 * 1024 * 1024; // 1GB - 单个BLOB最大大小
    
    // 页相关常量
    static constexpr uint32_t MAX_PAGE_SIZE = 65536; // 64KB - 最大InnoDB页大小
    static constexpr uint32_t DEFAULT_PAGE_SIZE = 16384; // 16KB - 默认InnoDB页大小
    
    // 链表深度限制
    static constexpr size_t MAX_BLOB_CHAIN_DEPTH = 1024; // BLOB链最大深度
    static constexpr size_t MAX_INDEX_DEPTH = 100; // B+树最大深度
    
    // 安全边界检查
    static constexpr uint64_t MAX_FILE_SIZE = 1024ULL * 1024 * 1024 * 1024 * 1024; // 1PB - 最大文件大小
};

// 辅助函数：验证BLOB大小是否合理
inline bool is_valid_blob_size(size_t size) {
    return size <= Common::MAX_BLOB_SIZE;
}

// 辅助函数：验证页大小是否有效
inline bool is_valid_page_size(uint32_t page_size) {
    // InnoDB 页大小必须是2的幂，且在4KB到64KB之间
    if (page_size < Common::DEFAULT_PAGE_SIZE / 4 || page_size > Common::MAX_PAGE_SIZE) {
        return false;
    }
    return (page_size & (page_size - 1)) == 0;
}

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_COMMON_HPP
