/**
 * @file platform/linux/memory_linux.cpp
 * @brief Linux内存管理实现
 */

#include "dbrecovery/common/environment_compat.hpp"
#include <cstdlib>
#include <cstring>

namespace dbrecovery {
namespace env {

// ==================== Linux内存管理实现 ====================

void* MemoryManager::allocate_impl(size_t size) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, 64, size) == 0) {
        return ptr;
    }
    return nullptr;
}

void* MemoryManager::aligned_allocate_impl(size_t size, size_t alignment) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) == 0) {
        return ptr;
    }
    return nullptr;
}

void MemoryManager::deallocate_impl(void* ptr) {
    std::free(ptr);
}

void MemoryManager::aligned_deallocate_impl(void* ptr) {
    std::free(ptr);
}

} // namespace env
} // namespace dbrecovery
