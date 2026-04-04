/**
 * @file platform/arm64/simd_operations_arm64.hpp
 * @brief ARM64 SIMD操作实现
 *
 * 包含NEON指令集的向量化操作
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <cstring>

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__) || defined(_M_ARM)

#include <arm_neon.h>

namespace dbrecovery {
namespace simd {

// ==================== CPU特性检测 ====================
class CPUCapabilities {
public:
    static CPUCapabilities& instance() {
        static CPUCapabilities instance_;
        return instance_;
    }

    // 检查CPU特性
    bool has_neon() const { return has_neon_; }
    bool has_crc32() const { return has_crc32_; }
    bool has_aes() const { return has_aes_; }
    bool has_pmull() const { return has_pmull_; }
    bool has_sha() const { return has_sha_; }

private:
    CPUCapabilities() {
        detect_capabilities();
    }

    void detect_capabilities() {
        // ARM64通常都支持NEON
        has_neon_ = true;

        #if defined(__aarch64__) || defined(_M_ARM64)
            // 在ARM64上，这些特性通常都可用
            has_crc32_ = true;
            has_aes_ = true;
            has_pmull_ = true;
            has_sha_ = true;
        #else
            // 对于ARM32，需要检查
            has_crc32_ = false;
            has_aes_ = false;
            has_pmull_ = false;
            has_sha_ = false;
        #endif
    }

    bool has_neon_ = false;
    bool has_crc32_ = false;
    bool has_aes_ = false;
    bool has_pmull_ = false;
    bool has_sha_ = false;
};

// ==================== 向量化操作类 ====================
class VectorizedOperations {
public:
    // 使用NEON进行快速内存复制（16字节）
    static void copy_16bytes(void* dest, const void* src) {
        uint8x16_t data = vld1q_u8(reinterpret_cast<const uint8_t*>(src));
        vst1q_u8(reinterpret_cast<uint8_t*>(dest), data);
    }

    // 使用NEON进行快速内存复制（32字节）
    static void copy_32bytes(void* dest, const void* src) {
        uint8x16_t data1 = vld1q_u8(reinterpret_cast<const uint8_t*>(src));
        uint8x16_t data2 = vld1q_u8(reinterpret_cast<const uint8_t*>(src) + 16);
        vst1q_u8(reinterpret_cast<uint8_t*>(dest), data1);
        vst1q_u8(reinterpret_cast<uint8_t*>(dest) + 16, data2);
    }

    // 使用NEON进行快速内存复制（64字节）
    static void copy_64bytes(void* dest, const void* src) {
        uint8x16_t data1 = vld1q_u8(reinterpret_cast<const uint8_t*>(src));
        uint8x16_t data2 = vld1q_u8(reinterpret_cast<const uint8_t*>(src) + 16);
        uint8x16_t data3 = vld1q_u8(reinterpret_cast<const uint8_t*>(src) + 32);
        uint8x16_t data4 = vld1q_u8(reinterpret_cast<const uint8_t*>(src) + 48);
        vst1q_u8(reinterpret_cast<uint8_t*>(dest), data1);
        vst1q_u8(reinterpret_cast<uint8_t*>(dest) + 16, data2);
        vst1q_u8(reinterpret_cast<uint8_t*>(dest) + 32, data3);
        vst1q_u8(reinterpret_cast<uint8_t*>(dest) + 48, data4);
    }

    // 自动选择最佳内存复制方法（任意大小）
    static void copy_memory(void* dest, const void* src, size_t size) {
        if (size < 16) {
            std::memcpy(dest, src, size);
            return;
        }

        char* d = static_cast<char*>(dest);
        const char* s = static_cast<const char*>(src);

        // 处理64字节对齐的块
        size_t aligned_size = size & ~63;
        for (size_t i = 0; i < aligned_size; i += 64) {
            copy_64bytes(d + i, s + i);
        }

        // 处理剩余字节
        for (size_t i = aligned_size; i < size; i++) {
            d[i] = s[i];
        }
    }
};

// ==================== 内存清零操作 ====================
class MemoryZeroOperations {
public:
    // 使用NEON进行快速内存清零（16字节）
    static void zero_16bytes(void* dest) {
        uint8x16_t zero = vdupq_n_u8(0);
        vst1q_u8(reinterpret_cast<uint8_t*>(dest), zero);
    }

    // 使用NEON进行快速内存清零（32字节）
    static void zero_32bytes(void* dest) {
        uint8x16_t zero = vdupq_n_u8(0);
        vst1q_u8(reinterpret_cast<uint8_t*>(dest), zero);
        vst1q_u8(reinterpret_cast<uint8_t*>(dest) + 16, zero);
    }

    // 使用NEON进行快速内存清零（64字节）
    static void zero_64bytes(void* dest) {
        uint8x16_t zero = vdupq_n_u8(0);
        vst1q_u8(reinterpret_cast<uint8_t*>(dest), zero);
        vst1q_u8(reinterpret_cast<uint8_t*>(dest) + 16, zero);
        vst1q_u8(reinterpret_cast<uint8_t*>(dest) + 32, zero);
        vst1q_u8(reinterpret_cast<uint8_t*>(dest) + 48, zero);
    }
};

} // namespace simd
} // namespace dbrecovery

#endif // ARM64 / ARM
