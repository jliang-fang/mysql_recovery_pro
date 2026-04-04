/**
 * @file test_cpu_optimizations.cpp
 * @brief CPU优化测试
 *
 * 测试x86_64和ARM64架构的CPU优化功能
 */

#include "dbrecovery/common/cpu_optimizations.hpp"
#include "dbrecovery/common/endian_compat.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <chrono>
#include <cassert>

using namespace dbrecovery;
using namespace dbrecovery::cpu;

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

// ==================== CRC32优化测试 ====================

bool test_crc32_optimization() {
    std::cout << "Testing CRC32 optimization..." << std::endl;

    auto data = generate_test_data(1024 * 1024);  // 1MB

    // 测试参考实现
    auto start = std::chrono::high_resolution_clock::now();
    uint32_t ref_crc = CRC32Optimizer::calculate_crc32_ref(
        data.data(), data.size(), 0);
    auto ref_time = std::chrono::high_resolution_clock::now() - start;

    // 测试优化实现
    start = std::chrono::high_resolution_clock::now();
    uint32_t opt_crc = CRC32Optimizer::calculate(
        data.data(), data.size(), 0);
    auto opt_time = std::chrono::high_resolution_clock::now() - start;

    // 验证结果一致
    if (ref_crc != opt_crc) {
        std::cerr << "Failed: CRC32 results don't match" << std::endl;
        std::cerr << "  Reference: 0x" << std::hex << ref_crc << std::dec << std::endl;
        std::cerr << "  Optimized: 0x" << std::hex << opt_crc << std::dec << std::endl;
        return false;
    }

    std::cout << "  CRC32 calculation: PASS" << std::endl;
    std::cout << "    Reference time: " 
              << std::chrono::duration_cast<std::chrono::microseconds>(ref_time).count() << " us" << std::endl;
    std::cout << "    Optimized time: " 
              << std::chrono::duration_cast<std::chrono::microseconds>(opt_time).count() << " us" << std::endl;

    return true;
}

bool test_crc32_consistency() {
    std::cout << "Testing CRC32 consistency..." << std::endl;

    auto data = generate_test_data(64 * 1024);

    // 多次计算应该得到相同结果
    uint32_t crc1 = CRC32Optimizer::calculate(data.data(), data.size());
    uint32_t crc2 = CRC32Optimizer::calculate(data.data(), data.size());
    uint32_t crc3 = CRC32Optimizer::calculate(data.data(), data.size());

    if (crc1 != crc2 || crc2 != crc3) {
        std::cerr << "Failed: CRC32 not consistent" << std::endl;
        return false;
    }

    std::cout << "  CRC32 consistency: PASS (0x" << std::hex << crc1 << ")" << std::dec << std::endl;
    return true;
}

// ==================== 内存操作优化测试 ====================

bool test_memory_copy() {
    std::cout << "Testing memory copy optimization..." << std::endl;

    auto data = generate_test_data(1024 * 1024);  // 1MB

    // 测试参考实现
    auto dest1 = std::make_unique<uint8_t[]>(data.size());
    auto start = std::chrono::high_resolution_clock::now();
    std::memcpy(dest1.get(), data.data(), data.size());
    auto ref_time = std::chrono::high_resolution_clock::now() - start;

    // 测试优化实现
    auto dest2 = std::make_unique<uint8_t[]>(data.size());
    start = std::chrono::high_resolution_clock::now();
    MemoryOptimizer::copy_memory(dest2.get(), data.data(), data.size());
    auto opt_time = std::chrono::high_resolution_clock::now() - start;

    // 验证结果一致
    if (std::memcmp(dest1.get(), dest2.get(), data.size()) != 0) {
        std::cerr << "Failed: Memory copy results don't match" << std::endl;
        return false;
    }

    std::cout << "  Memory copy: PASS" << std::endl;
    std::cout << "    Reference time: " 
              << std::chrono::duration_cast<std::chrono::microseconds>(ref_time).count() << " us" << std::endl;
    std::cout << "    Optimized time: " 
              << std::chrono::duration_cast<std::chrono::microseconds>(opt_time).count() << " us" << std::endl;

    return true;
}

bool test_memory_alignment() {
    std::cout << "Testing memory alignment..." << std::endl;

    // 测试64字节对齐
    auto ptr = AlignedAllocator<uint8_t>::allocate(1024);
    if (!ptr) {
        std::cerr << "Failed: Allocation returned null" << std::endl;
        return false;
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr % 64 != 0) {
        std::cerr << "Failed: Not 64-byte aligned: " << addr << std::endl;
        AlignedAllocator<uint8_t>::deallocate(ptr);
        return false;
    }

    std::cout << "  64-byte alignment: PASS (addr = " << addr << ")" << std::endl;

    // 测试32字节对齐
    auto ptr32 = AlignedAllocator<uint8_t>::allocate(1024);
    if (!ptr32) {
        std::cerr << "Failed: 32-byte allocation returned null" << std::endl;
        AlignedAllocator<uint8_t>::deallocate(ptr);
        return false;
    }

    addr = reinterpret_cast<uintptr_t>(ptr32);
    if (addr % 32 != 0) {
        std::cerr << "Failed: Not 32-byte aligned: " << addr << std::endl;
        AlignedAllocator<uint8_t>::deallocate(ptr);
        AlignedAllocator<uint8_t>::deallocate(ptr32);
        return false;
    }

    std::cout << "  32-byte alignment: PASS (addr = " << addr << ")" << std::endl;

    AlignedAllocator<uint8_t>::deallocate(ptr);
    AlignedAllocator<uint8_t>::deallocate(ptr32);

    return true;
}

// ==================== CPU特性检测测试 ====================

bool test_cpu_capabilities() {
    std::cout << "Testing CPU capabilities..." << std::endl;

    auto& caps = CPUCapabilities::instance();

    std::cout << "  CPU Architecture: " << CPU_OPTIMIZATION_ARCH << std::endl;

    #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        std::cout << "  SSE4.2: " << (caps.has_sse42() ? "YES" : "NO") << std::endl;
        std::cout << "  AVX2: " << (caps.has_avx2() ? "YES" : "NO") << std::endl;
        std::cout << "  AVX-512: " << (caps.has_avx512() ? "YES" : "NO") << std::endl;
        std::cout << "  PCLMULQDQ: " << (caps.has_pclmulqdq() ? "YES" : "NO") << std::endl;
        std::cout << "  AES-NI: " << (caps.has_aesni() ? "YES" : "NO") << std::endl;
        std::cout << "  CRC32: " << (caps.has_crc32() ? "YES" : "NO") << std::endl;
    #elif defined(__aarch64__) || defined(_M_ARM64)
        std::cout << "  NEON: " << (caps.has_neon() ? "YES" : "NO") << std::endl;
        std::cout << "  CRC32: " << (caps.has_crc32_arm() ? "YES" : "NO") << std::endl;
        std::cout << "  AES: " << (caps.has_aes() ? "YES" : "NO") << std::endl;
        std::cout << "  PMULL: " << (caps.has_pmull() ? "YES" : "NO") << std::endl;
        std::cout << "  SHA: " << (caps.has_sha() ? "YES" : "NO") << std::endl;
    #endif

    return true;
}

// ==================== 统一接口测试 ====================

bool test_unified_interface() {
    std::cout << "Testing unified interface..." << std::endl;

    auto data = generate_test_data(4096);

    // 测试统一CRC32接口
    uint32_t crc = calculate_crc32(data.data(), data.size());
    std::cout << "  Unified CRC32: PASS (0x" << std::hex << crc << ")" << std::dec << std::endl;

    // 测试统一内存复制接口
    auto dest = std::make_unique<uint8_t[]>(data.size());
    fast_memcpy(dest.get(), data.data(), data.size());
    if (std::memcmp(dest.get(), data.data(), data.size()) == 0) {
        std::cout << "  Unified memory copy: PASS" << std::endl;
    } else {
        std::cerr << "Failed: Unified memory copy" << std::endl;
        return false;
    }

    return true;
}

// ==================== 性能基准测试 ====================

bool test_performance_benchmark() {
    std::cout << "Testing performance benchmark..." << std::endl;

    const size_t test_size = 10 * 1024 * 1024;  // 10MB
    auto data = generate_test_data(test_size);

    // CRC32基准测试
    {
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 10; i++) {
            calculate_crc32(data.data(), data.size());
        }
        auto elapsed = std::chrono::high_resolution_clock::now() - start;
        double throughput = (test_size * 10) / 
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
        std::cout << "  CRC32 throughput: " << throughput << " MB/s" << std::endl;
    }

    // 内存复制基准测试
    {
        auto dest = std::make_unique<uint8_t[]>(test_size);
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 10; i++) {
            fast_memcpy(dest.get(), data.data(), data.size());
        }
        auto elapsed = std::chrono::high_resolution_clock::now() - start;
        double throughput = (test_size * 10) / 
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
        std::cout << "  Memory copy throughput: " << throughput << " MB/s" << std::endl;
    }

    return true;
}

// ==================== 主测试函数 ====================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "CPU Optimizations Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    bool all_passed = true;

    // CPU特性检测
    all_passed &= test_cpu_capabilities();
    std::cout << std::endl;

    // CRC32优化测试
    all_passed &= test_crc32_optimization();
    all_passed &= test_crc32_consistency();
    std::cout << std::endl;

    // 内存操作测试
    all_passed &= test_memory_copy();
    all_passed &= test_memory_alignment();
    std::cout << std::endl;

    // 统一接口测试
    all_passed &= test_unified_interface();
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
