/**
 * @file test_platform_detection.cpp
 * @brief 平台检测测试
 *
 * 测试跨平台的平台检测功能
 */

#include "dbrecovery/common/platform_detection.hpp"
#include <iostream>
#include <cassert>

using namespace dbrecovery;
using namespace dbrecovery::platform;

// ==================== 平台检测测试 ====================

bool test_platform_detection() {
    std::cout << "Testing platform detection..." << std::endl;

    PlatformDetector& detector = PlatformDetector::instance();

    // 获取平台信息
    PlatformInfo info = detector.get_platform_info();

    std::cout << "  Platform: " << info.platform_name << std::endl;
    std::cout << "  Compiler: " << info.compiler_name << " " << info.compiler_version << std::endl;
    std::cout << "  Architecture: " << info.arch_name << std::endl;
    std::cout << "  C++ Standard: " << info.cpp_standard << std::endl;
    std::cout << "  Build Type: " << info.build_type << std::endl;
    std::cout << "  64-bit: " << (info.is_64bit ? "YES" : "NO") << std::endl;
    std::cout << "  Little Endian: " << (info.is_little_endian ? "YES" : "NO") << std::endl;
    std::cout << "  Page Size: " << info.page_size << " bytes" << std::endl;

    // 验证基本字段
    assert(!info.platform_name.empty());
    assert(!info.compiler_name.empty());
    assert(!info.arch_name.empty());
    assert(!info.cpp_standard.empty());

    std::cout << "  Platform detection: PASS" << std::endl;

    return true;
}

bool test_platform_specific_functions() {
    std::cout << "Testing platform-specific functions..." << std::endl;

    PlatformDetector& detector = PlatformDetector::instance();

    // 测试平台检测函数
    #ifdef DETECT_PLATFORM_WINDOWS
        assert(detector.is_windows());
        std::cout << "  Windows detection: PASS" << std::endl;
    #elif defined(DETECT_PLATFORM_MACOS)
        assert(detector.is_macos());
        std::cout << "  macOS detection: PASS" << std::endl;
    #elif defined(DETECT_PLATFORM_LINUX)
        assert(detector.is_linux());
        std::cout << "  Linux detection: PASS" << std::endl;
    #endif

    // 测试架构检测函数
    #ifdef DETECT_ARCH_X86_64
        assert(detector.is_x86_64());
        std::cout << "  x86_64 detection: PASS" << std::endl;
    #elif defined(DETECT_ARCH_ARM64)
        assert(detector.is_arm64());
        std::cout << "  ARM64 detection: PASS" << std::endl;
    #endif

    // 测试便捷函数
    std::cout << "  get_platform_name(): " << get_platform_name() << std::endl;
    std::cout << "  get_arch_name(): " << get_arch_name() << std::endl;
    std::cout << "  is_64bit(): " << (is_64bit() ? "true" : "false") << std::endl;
    std::cout << "  is_little_endian(): " << (is_little_endian() ? "true" : "false") << std::endl;

    std::cout << "  Platform-specific functions: PASS" << std::endl;

    return true;
}

bool test_to_string() {
    std::cout << "Testing to_string()..." << std::endl;

    PlatformDetector& detector = PlatformDetector::instance();
    std::string str = detector.to_string();

    std::cout << "  Platform string: " << str << std::endl;

    assert(!str.empty());
    assert(str.find("Platform:") != std::string::npos);

    std::cout << "  to_string(): PASS" << std::endl;

    return true;
}

// ==================== 主测试函数 ====================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Platform Detection Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    bool all_passed = true;

    // 平台检测测试
    all_passed &= test_platform_detection();
    std::cout << std::endl;

    // 平台特定函数测试
    all_passed &= test_platform_specific_functions();
    std::cout << std::endl;

    // to_string测试
    all_passed &= test_to_string();
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
