/**
 * @file platform/x86_64/simd_operations_x86_64.hpp
 * @brief x86_64 SIMD操作实现
 *
 * 包含SSE2、AVX2、AVX-512等指令集的向量化操作
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)

#include <emmintrin.h>  // SSE2
#include <xmmintrin.h>  // SSE

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
    bool has_sse2() const { return has_sse2_; }
    bool has_sse42() const { return has_sse42_; }
    bool has_avx() const { return has_avx_; }
    bool has_avx2() const { return has_avx2_; }
    bool has_avx512() const { return has_avx512_; }

private:
    CPUCapabilities() {
        detect_capabilities();
    }

    void detect_capabilities() {
        int cpu_info[4];

        // 检查SSE2
        #if defined(__GNUC__) || defined(__clang__)
            __cpuid(1, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
            has_sse2_ = (cpu_info[3] & (1 << 26)) != 0;

            // 检查SSE4.2
            has_sse42_ = (cpu_info[2] & (1 << 20)) != 0;

            // 检查AVX
            if (cpu_info[2] & (1 << 28)) {  // OSXSAVE
                uint64_t xcr0 = _xgetbv(0);
                if (xcr0 & 0x6) {  // XMM and YMM state
                    has_avx_ = true;
                    
                    // 检查AVX2
                    __cpuid(7, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
                    has_avx2_ = (cpu_info[1] & (1 << 5)) != 0;
                    has_avx512_ = (cpu_info[1] & (1 << 16)) != 0;
                }
            }
        #elif defined(_MSC_VER)
            __cpuid(cpu_info, 1);
            has_sse2_ = (cpu_info[3] & (1 << 26)) != 0;
            has_sse42_ = (cpu_info[2] & (1 << 20)) != 0;

            if (cpu_info[2] & (1 << 28)) {
                __cpuidex(cpu_info, 7, 0);
                has_avx_ = true;
                has_avx2_ = (cpu_info[1] & (1 << 5)) != 0;
                has_avx512_ = (cpu_info[1] & (1 << 16)) != 0;
            }
        #endif
    }

    bool has_sse2_ = false;
    bool has_sse42_ = false;
    bool has_avx_ = false;
    bool has_avx2_ = false;
    bool has_avx512_ = false;
};

// ==================== 向量化操作类 ====================
class VectorizedOperations {
public:
    // 使用SSE2进行快速内存复制（16字节）
    static void copy_16bytes(void* dest, const void* src) {
        #if defined(__GNUC__) || defined(__clang__)
            __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), data);
        #elif defined(_MSC_VER)
            __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), data);
        #else
            std::memcpy(dest, src, 16);
        #endif
    }

    // 使用SSE2进行快速内存复制（32字节）
    static void copy_32bytes(void* dest, const void* src) {
        #if defined(__GNUC__) || defined(__clang__)
            __m128i data1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            __m128i data2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), data1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, data2);
        #elif defined(_MSC_VER)
            __m128i data1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            __m128i data2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), data1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, data2);
        #else
            std::memcpy(dest, src, 32);
        #endif
    }

    // 使用AVX2进行快速内存复制（32字节）
    static void copy_32bytes_avx2(void* dest, const void* src) {
        if (!CPUCapabilities::instance().has_avx2()) {
            copy_32bytes(dest, src);
            return;
        }

        #if defined(__GNUC__) || defined(__clang__)
            __m256i data = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest), data);
        #elif defined(_MSC_VER)
            __m256i data = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest), data);
        #else
            std::memcpy(dest, src, 32);
        #endif
    }

    // 使用AVX2进行快速内存复制（64字节）
    static void copy_64bytes_avx2(void* dest, const void* src) {
        if (!CPUCapabilities::instance().has_avx2()) {
            copy_64bytes_sse2(dest, src);
            return;
        }

        #if defined(__GNUC__) || defined(__clang__)
            __m256i data1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
            __m256i data2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src) + 1);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest), data1);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest) + 1, data2);
        #elif defined(_MSC_VER)
            __m256i data1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
            __m256i data2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src) + 1);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest), data1);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest) + 1, data2);
        #else
            std::memcpy(dest, src, 64);
        #endif
    }

    // 使用SSE2进行快速内存复制（64字节）
    static void copy_64bytes_sse2(void* dest, const void* src) {
        #if defined(__GNUC__) || defined(__clang__)
            __m128i data1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            __m128i data2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 1);
            __m128i data3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 2);
            __m128i data4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 3);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), data1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, data2);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 2, data3);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 3, data4);
        #elif defined(_MSC_VER)
            __m128i data1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            __m128i data2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 1);
            __m128i data3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 2);
            __m128i data4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 3);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), data1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, data2);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 2, data3);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 3, data4);
        #else
            std::memcpy(dest, src, 64);
        #endif
    }

    // 使用AVX-512进行快速内存复制（64字节）
    static void copy_64bytes_avx512(void* dest, const void* src) {
        if (!CPUCapabilities::instance().has_avx512()) {
            copy_64bytes_avx2(dest, src);
            return;
        }

        #if defined(__GNUC__) || defined(__clang__)
            __m512i data = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(src));
            _mm512_storeu_si512(reinterpret_cast<__m512i*>(dest), data);
        #elif defined(_MSC_VER)
            __m512i data = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(src));
            _mm512_storeu_si512(reinterpret_cast<__m512i*>(dest), data);
        #else
            std::memcpy(dest, src, 64);
        #endif
    }

    // 自动选择最佳内存复制方法（16字节）
    static void copy_16bytes(void* dest, const void* src) {
        copy_16bytes_sse2(dest, src);
    }

    // 自动选择最佳内存复制方法（32字节）
    static void copy_32bytes(void* dest, const void* src) {
        if (CPUCapabilities::instance().has_avx2()) {
            copy_32bytes_avx2(dest, src);
        } else {
            copy_32bytes_sse2(dest, src);
        }
    }

    // 自动选择最佳内存复制方法（64字节）
    static void copy_64bytes(void* dest, const void* src) {
        if (CPUCapabilities::instance().has_avx512()) {
            copy_64bytes_avx512(dest, src);
        } else if (CPUCapabilities::instance().has_avx2()) {
            copy_64bytes_avx2(dest, src);
        } else {
            copy_64bytes_sse2(dest, src);
        }
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

private:
    // SSE2实现（16字节）
    static void copy_16bytes_sse2(void* dest, const void* src) {
        #if defined(__GNUC__) || defined(__clang__)
            __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), data);
        #elif defined(_MSC_VER)
            __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), data);
        #else
            std::memcpy(dest, src, 16);
        #endif
    }

    // SSE2实现（32字节）
    static void copy_32bytes_sse2(void* dest, const void* src) {
        #if defined(__GNUC__) || defined(__clang__)
            __m128i data1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            __m128i data2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), data1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, data2);
        #elif defined(_MSC_VER)
            __m128i data1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            __m128i data2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), data1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, data2);
        #else
            std::memcpy(dest, src, 32);
        #endif
    }

    // SSE2实现（64字节）
    static void copy_64bytes_sse2(void* dest, const void* src) {
        #if defined(__GNUC__) || defined(__clang__)
            __m128i data1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            __m128i data2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 1);
            __m128i data3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 2);
            __m128i data4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 3);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), data1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, data2);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 2, data3);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 3, data4);
        #elif defined(_MSC_VER)
            __m128i data1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            __m128i data2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 1);
            __m128i data3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 2);
            __m128i data4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 3);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), data1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, data2);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 2, data3);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 3, data4);
        #else
            std::memcpy(dest, src, 64);
        #endif
    }
};

// ==================== 内存清零操作 ====================
class MemoryZeroOperations {
public:
    // 使用SSE2进行快速内存清零（16字节）
    static void zero_16bytes(void* dest) {
        #if defined(__GNUC__) || defined(__clang__)
            __m128i zero = _mm_setzero_si128();
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), zero);
        #elif defined(_MSC_VER)
            __m128i zero = _mm_setzero_si128();
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), zero);
        #else
            std::memset(dest, 0, 16);
        #endif
    }

    // 使用SSE2进行快速内存清零（32字节）
    static void zero_32bytes(void* dest) {
        #if defined(__GNUC__) || defined(__clang__)
            __m128i zero = _mm_setzero_si128();
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), zero);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, zero);
        #elif defined(_MSC_VER)
            __m128i zero = _mm_setzero_si128();
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), zero);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, zero);
        #else
            std::memset(dest, 0, 32);
        #endif
    }

    // 使用AVX2进行快速内存清零（32字节）
    static void zero_32bytes_avx2(void* dest) {
        if (!CPUCapabilities::instance().has_avx2()) {
            zero_32bytes(dest);
            return;
        }

        #if defined(__GNUC__) || defined(__clang__)
            __m256i zero = _mm256_setzero_si256();
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest), zero);
        #elif defined(_MSC_VER)
            __m256i zero = _mm256_setzero_si256();
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest), zero);
        #else
            std::memset(dest, 0, 32);
        #endif
    }

    // 使用AVX2进行快速内存清零（64字节）
    static void zero_64bytes_avx2(void* dest) {
        if (!CPUCapabilities::instance().has_avx2()) {
            zero_64bytes_sse2(dest);
            return;
        }

        #if defined(__GNUC__) || defined(__clang__)
            __m256i zero = _mm256_setzero_si256();
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest), zero);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest) + 1, zero);
        #elif defined(_MSC_VER)
            __m256i zero = _mm256_setzero_si256();
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest), zero);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest) + 1, zero);
        #else
            std::memset(dest, 0, 64);
        #endif
    }

    // 使用AVX-512进行快速内存清零（64字节）
    static void zero_64bytes_avx512(void* dest) {
        if (!CPUCapabilities::instance().has_avx512()) {
            zero_64bytes_avx2(dest);
            return;
        }

        #if defined(__GNUC__) || defined(__clang__)
            __m512i zero = _mm512_setzero_si512();
            _mm512_storeu_si512(reinterpret_cast<__m512i*>(dest), zero);
        #elif defined(_MSC_VER)
            __m512i zero = _mm512_setzero_si512();
            _mm512_storeu_si512(reinterpret_cast<__m512i*>(dest), zero);
        #else
            std::memset(dest, 0, 64);
        #endif
    }

    // 自动选择最佳内存清零方法（16字节）
    static void zero_16bytes(void* dest) {
        zero_16bytes_sse2(dest);
    }

    // 自动选择最佳内存清零方法（32字节）
    static void zero_32bytes(void* dest) {
        if (CPUCapabilities::instance().has_avx2()) {
            zero_32bytes_avx2(dest);
        } else {
            zero_32bytes_sse2(dest);
        }
    }

    // 自动选择最佳内存清零方法（64字节）
    static void zero_64bytes(void* dest) {
        if (CPUCapabilities::instance().has_avx512()) {
            zero_64bytes_avx512(dest);
        } else if (CPUCapabilities::instance().has_avx2()) {
            zero_64bytes_avx2(dest);
        } else {
            zero_64bytes_sse2(dest);
        }
    }

private:
    // SSE2实现（16字节）
    static void zero_16bytes_sse2(void* dest) {
        #if defined(__GNUC__) || defined(__clang__)
            __m128i zero = _mm_setzero_si128();
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), zero);
        #elif defined(_MSC_VER)
            __m128i zero = _mm_setzero_si128();
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), zero);
        #else
            std::memset(dest, 0, 16);
        #endif
    }

    // SSE2实现（32字节）
    static void zero_32bytes_sse2(void* dest) {
        #if defined(__GNUC__) || defined(__clang__)
            __m128i zero = _mm_setzero_si128();
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), zero);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, zero);
        #elif defined(_MSC_VER)
            __m128i zero = _mm_setzero_si128();
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), zero);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, zero);
        #else
            std::memset(dest, 0, 32);
        #endif
    }

    // SSE2实现（64字节）
    static void zero_64bytes_sse2(void* dest) {
        #if defined(__GNUC__) || defined(__clang__)
            __m128i zero = _mm_setzero_si128();
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), zero);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, zero);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 2, zero);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 3, zero);
        #elif defined(_MSC_VER)
            __m128i zero = _mm_setzero_si128();
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest), zero);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 1, zero);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 2, zero);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest) + 3, zero);
        #else
            std::memset(dest, 0, 64);
        #endif
    }
};

} // namespace simd
} // namespace dbrecovery

#endif // x86_64 / i386
