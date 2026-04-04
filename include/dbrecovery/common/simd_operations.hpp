/**
 * @file simd_operations.hpp
 * @brief SIMD向量化操作统一接口
 *
 * 提供x86_64和ARM64架构的SIMD向量化操作
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>

// ==================== 自动选择SIMD架构 ====================
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include "platform/x86_64/simd_operations_x86_64.hpp"
    #define SIMD_ARCH "x86_64"
    #define SIMD_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__) || defined(_M_ARM)
    #include "platform/arm64/simd_operations_arm64.hpp"
    #define SIMD_ARCH "ARM64"
    #define SIMD_ARCH_ARM64 1
#else
    #warning "Unknown SIMD architecture, using reference implementation"
    namespace dbrecovery {
    namespace simd {
        // 参考实现
        class VectorizedOperations {
        public:
            static void copy_16bytes(void* dest, const void* src) {
                std::memcpy(dest, src, 16);
            }
            static void copy_32bytes(void* dest, const void* src) {
                std::memcpy(dest, src, 32);
            }
            static void copy_64bytes(void* dest, const void* src) {
                std::memcpy(dest, src, 64);
            }
            static void zero_memory_16bytes(void* dest) {
                std::memset(dest, 0, 16);
            }
            static void zero_memory_32bytes(void* dest) {
                std::memset(dest, 0, 32);
            }
            static void zero_memory_64bytes(void* dest) {
                std::memset(dest, 0, 64);
            }
        };
    } // namespace simd
    } // namespace dbrecovery
#endif

// ==================== 统一的SIMD操作接口 ====================
namespace dbrecovery {
namespace simd {

/**
 * @brief SIMD架构名称
 */
inline const char* get_arch_name() {
    return SIMD_ARCH;
}

/**
 * @brief 检查是否支持SIMD
 */
inline bool has_simd() {
    #if defined(SIMD_ARCH_X86_64)
        return CPUCapabilities::instance().has_sse2();
    #elif defined(SIMD_ARCH_ARM64)
        return CPUCapabilities::instance().has_neon();
    #else
        return false;
    #endif
}

/**
 * @brief 快速内存复制（16字节）
 */
inline void fast_copy_16(void* dest, const void* src) {
    VectorizedOperations::copy_16bytes(dest, src);
}

/**
 * @brief 快速内存复制（32字节）
 */
inline void fast_copy_32(void* dest, const void* src) {
    VectorizedOperations::copy_32bytes(dest, src);
}

/**
 * @brief 快速内存复制（64字节）
 */
inline void fast_copy_64(void* dest, const void* src) {
    VectorizedOperations::copy_64bytes(dest, src);
}

/**
 * @brief 快速内存清零（16字节）
 */
inline void fast_zero_16(void* dest) {
    VectorizedOperations::zero_memory_16bytes(dest);
}

/**
 * @brief 快速内存清零（32字节）
 */
inline void fast_zero_32(void* dest) {
    VectorizedOperations::zero_memory_32bytes(dest);
}

/**
 * @brief 快速内存清零（64字节）
 */
inline void fast_zero_64(void* dest) {
    VectorizedOperations::zero_memory_64bytes(dest);
}

/**
 * @brief 快速内存复制（任意大小）
 */
inline void fast_memcpy(void* dest, const void* src, size_t size) {
    if (size < 16) {
        std::memcpy(dest, src, size);
        return;
    }

    char* d = static_cast<char*>(dest);
    const char* s = static_cast<const char*>(src);

    // 处理64字节对齐的块
    size_t aligned_size = size & ~63;
    for (size_t i = 0; i < aligned_size; i += 64) {
        fast_copy_64(d + i, s + i);
    }

    // 处理剩余字节
    for (size_t i = aligned_size; i < size; i++) {
        d[i] = s[i];
    }
}

/**
 * @brief 快速内存清零（任意大小）
 */
inline void fast_memset_zero(void* dest, size_t size) {
    if (size == 0) return;

    char* d = static_cast<char*>(dest);

    // 处理64字节对齐的块
    size_t aligned_size = size & ~63;
    for (size_t i = 0; i < aligned_size; i += 64) {
        fast_zero_64(d + i);
    }

    // 处理剩余字节
    std::memset(d + aligned_size, 0, size - aligned_size);
}

/**
 * @brief SIMD对齐分配
 */
template<typename T>
inline T* simd_allocate(size_t count = 1) {
    size_t alignment = 64;  // AVX-512 requires 64-byte alignment
    void* ptr = nullptr;
    
    #if defined(_MSC_VER)
        ptr = _aligned_malloc(count * sizeof(T), alignment);
    #else
        if (posix_memalign(&ptr, alignment, count * sizeof(T)) != 0) {
            return nullptr;
        }
    #endif
    
    return static_cast<T*>(ptr);
}

/**
 * @brief SIMD对齐释放
 */
template<typename T>
inline void simd_deallocate(T* ptr) {
    #if defined(_MSC_VER)
        _aligned_free(ptr);
    #else
        std::free(ptr);
    #endif
}

} // namespace simd
} // namespace dbrecovery
