/**
 * @file platform/windows/memory_windows.cpp
 * @brief Windows内存管理实现
 */

#include "dbrecovery/common/environment_compat.hpp"
#include <windows.h>
#include <malloc.h>
#include <cstdlib>
#include <cstring>

namespace dbrecovery {
namespace env {

// ==================== Windows内存管理实现 ====================

void* MemoryManager::allocate_impl(size_t size) {
    // 使用_aligned_malloc进行对齐分配
    void* ptr = _aligned_malloc(size, 64);
    if (ptr) {
        total_allocated_ += size;
        allocation_count_++;
        uint64_t current = peak_allocated_.load();
        while (size + total_allocated_.load() > current) {
            if (peak_allocated_.compare_exchange_weak(current, size + total_allocated_.load())) {
                break;
            }
        }
    }
    return ptr;
}

void* MemoryManager::aligned_allocate_impl(size_t size, size_t alignment) {
    void* ptr = _aligned_malloc(size, alignment);
    if (ptr) {
        total_allocated_ += size;
        allocation_count_++;
        uint64_t current = peak_allocated_.load();
        while (size + total_allocated_.load() > current) {
            if (peak_allocated_.compare_exchange_weak(current, size + total_allocated_.load())) {
                break;
            }
        }
    }
    return ptr;
}

void MemoryManager::deallocate_impl(void* ptr) {
    if (ptr) {
        _aligned_free(ptr);
    }
}

void MemoryManager::aligned_deallocate_impl(void* ptr) {
    if (ptr) {
        _aligned_free(ptr);
    }
}

void MemoryManager::zero_memory(void* ptr, size_t size) {
    if (ptr && size > 0) {
        std::memset(ptr, 0, size);
    }
}

void MemoryManager::copy_memory(void* dest, const void* src, size_t size) {
    if (dest && src && size > 0) {
        std::memcpy(dest, src, size);
    }
}

void MemoryManager::set_memory(void* ptr, uint8_t value, size_t size) {
    if (ptr && size > 0) {
        std::memset(ptr, value, size);
    }
}

uint64_t MemoryManager::total_allocated() const {
    return total_allocated_.load();
}

uint64_t MemoryManager::peak_allocated() const {
    return peak_allocated_.load();
}

uint64_t MemoryManager::allocation_count() const {
    return allocation_count_.load();
}

} // namespace env
} // namespace dbrecovery
