/**
 * @file test_simd_operations.cpp
 * @brief SIMD操作测试
 *
 * 测试x86_64和ARM64架构的SIMD向量化操作
 */

#include "dbrecovery/common/simd_operations.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <chrono>
#include <cassert>
#include <iomanip>

using namespace dbrecovery;
using namespace dbrecovery::simd;

// ==================== 测试工具函数 ====================

/**
 * @brief 生成测试数据
 */
std::vector<uint8_t> generate_test_data(size_t size) {
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; i++) {
        data[i] = static_cast<uint8_t>(i % 256);
    }
    return data;
}

// ==================== SIMD操作测试 ====================

bool test_copy_16bytes() {
    std::cout << "Testing copy_16bytes..." << std::endl;

    auto src = generate_test_data(16);
    auto dest = std::make_unique<uint8_t[]>(16);

    fast_copy_16(dest.get(), src.data());

    if (std::memcmp(src.data(), dest.get(), 16) != 0) {
        std::cerr << "Failed: copy_16bytes" << std::endl;
        return false;
    }

    std::cout << "  copy_16bytes: PASS" << std::endl;
    return true;
}

bool test_copy_32bytes() {
    std::cout << "Testing copy_32bytes..." << std::endl;

    auto src = generate_test_data(32);
    auto dest = std::make_unique<uint8_t[]>(32);

    fast_copy_32(dest.get(), src.data());

    if (std::memcmp(src.data(), dest.get(), 32) != 0) {
        std::cerr << "Failed: copy_32bytes" << std::endl;
        return false;
    }

    std::cout << "  copy_32bytes: PASS" << std::endl;
    return true;
}

bool test_copy_64bytes() {
    std::cout << "Testing copy_64bytes..." << std::endl;

    auto src = generate_test_data(64);
    auto dest = std::make_unique<uint8_t[]>(64);

    fast_copy_64(dest.get(), src.data());

    if (std::memcmp(src.data(), dest.get(), 64) != 0) {
        std::cerr << "Failed: copy_64bytes" << std::endl;
        return false;
    }

    std::cout << "  copy_64bytes: PASS" << std::endl;
    return true;
}

bool test_zero_16bytes() {
    std::cout << "Testing zero_16bytes..." << std::endl;

    auto dest = std::make_unique<uint8_t[]>(16);
    std::memset(dest.get(), 0xFF, 16);  // 填充为0xFF

    fast_zero_16(dest.get());

    for (size_t i = 0; i < 16; i++) {
        if (dest[i] != 0) {
            std::cerr << "Failed: zero_16bytes" << std::endl;
            return false;
        }
    }

    std::cout << "  zero_16bytes: PASS" << std::endl;
    return true;
}

bool test_zero_32bytes() {
    std::cout << "Testing zero_32bytes..." << std::endl;

    auto dest = std::make_unique<uint8_t[]>(32);
    std::memset(dest.get(), 0xFF, 32);

    fast_zero_32(dest.get());

    for (size_t i = 0; i < 32; i++) {
        if (dest[i] != 0) {
            std::cerr << "Failed: zero_32bytes" << std::endl;
            return false;
        }
    }

    std::cout << "  zero_32bytes: PASS" << std::endl;
    return true;
}

bool test_zero_64bytes() {
    std::cout << "Testing zero_64bytes..." << std::endl;

    auto dest = std::make_unique<uint8_t[]>(64);
    std::memset(dest.get(), 0xFF, 64);

    fast_zero_64(dest.get());

    for (size_t i = 0; i < 64; i++) {
        if (dest[i] != 0) {
            std::cerr << "Failed: zero_64bytes" << std::endl;
            return false;
        }
    }

    std::cout << "  zero_64bytes: PASS" << std::endl;
    return true;
}

bool test_fast_memcpy() {
    std::cout << "Testing fast_memcpy..." << std::endl;

    auto src = generate_test_data(1024);  // 1KB
    auto dest = std::make_unique<uint8_t[]>(1024);

    fast_memcpy(dest.get(), src.data(), 1024);

    if (std::memcmp(src.data(), dest.get(), 1024) != 0) {
        std::cerr << "Failed: fast_memcpy" << std::endl;
        return false;
    }

    std::cout << "  fast_memcpy: PASS" << std::endl;
    return true;
}

bool test_fast_memset_zero() {
    std::cout << "Testing fast_memset_zero..." << std::endl;

    auto dest = std::make_unique<uint8_t[]>(1024);
    std::memset(dest.get(), 0xFF, 1024);

    fast_memset_zero(dest.get(), 1024);

    for (size_t i = 0; i < 1024; i++) {
        if (dest[i] != 0) {
            std::cerr << "Failed: fast_memset_zero" << std::endl;
            return false;
        }
    }

    std::cout << "  fast_memset_zero: PASS" << std::endl;
    return true;
}

// ==================== SIMD架构检测测试 ====================

bool test_simd_architecture() {
    std::cout << "Testing SIMD architecture..." << std::endl;

    std::cout << "  SIMD Architecture: " << get_arch_name() << std::endl;
    std::cout << "  Has SIMD: " << (has_simd() ? "YES" : "NO") << std::endl;

    #if defined(SIMD_ARCH_X86_64)
        std::cout << "  x86_64 SIMD: ENABLED" << std::endl;
    #elif defined(SIMD_ARCH_ARM64)
        std::cout << "  ARM64 SIMD: ENABLED" << std::endl;
    #endif

    return true;
}

// ==================== 性能基准测试 ====================

bool test_performance_benchmark() {
    std::cout << "Testing performance benchmark..." << std::endl;

    const size_t test_size = 10 * 1024 * 1024;  // 10MB
    auto src = generate_test_data(test_size);
    auto dest = std::make_unique<uint8_t[]>(test_size);

    // fast_memcpy基准测试
    {
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 10; i++) {
            fast_memcpy(dest.get(), src.data(), test_size);
        }
        auto elapsed = std::chrono::high_resolution_clock::now() - start;
        double throughput = (test_size * 10) / 
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
        std::cout << "  fast_memcpy throughput: " << std::fixed << std::setprecision(2) << throughput << " MB/s" << std::endl;
    }

    // fast_memset_zero基准测试
    {
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 10; i++) {
            fast_memset_zero(dest.get(), test_size);
        }
        auto elapsed = std::chrono::high_resolution_clock::now() - start;
        double throughput = (test_size * 10) / 
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
        std::cout << "  fast_memset_zero throughput: " << std::fixed << std::setprecision(2) << throughput << " MB/s" << std::endl;
    }

    return true;
}

// ==================== 主测试函数 ====================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "SIMD Operations Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    bool all_passed = true;

    // SIMD架构检测测试
    all_passed &= test_simd_architecture();
    std::cout << std::endl;

    // 复制操作测试
    all_passed &= test_copy_16bytes();
    all_passed &= test_copy_32bytes();
    all_passed &= test_copy_64bytes();
    std::cout << std::endl;

    // 清零操作测试
    all_passed &= test_zero_16bytes();
    all_passed &= test_zero_32bytes();
    all_passed &= test_zero_64bytes();
    std::cout << std::endl;

    // 快速操作测试
    all_passed &= test_fast_memcpy();
    all_passed &= test_fast_memset_zero();
    std::cout << std::endl;

    // 性能基准测试
    all_passed &= test_performance_benchmark();
    std::cout << std::endl;

    // 总结
    std::cout << "========================================" << std::endl;
    if (all_passed) {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cout << "Some tests FAILED!" << std::endl;
        return 1;
    }
}
