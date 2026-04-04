/**
 * @file platform_detection.hpp
 * @brief 统一平台检测头文件
 *
 * 提供统一的平台和架构检测功能
 */

#pragma once

#include <cstdint>
#include <string>
#include <array>

// ==================== 平台检测 ====================
#if defined(_WIN32) || defined(_WIN64)
    #define DETECT_PLATFORM_WINDOWS 1
    #define DETECT_PLATFORM_NAME "Windows"
    #define DETECT_PLATFORM_WINDOWS_VERSION _WIN32_WINNT
#elif defined(__APPLE__)
    #include "TargetConditionals.h"
    #if defined(TARGET_OS_MAC)
        #define DETECT_PLATFORM_MACOS 1
        #define DETECT_PLATFORM_NAME "macOS"
    #else
        #define DETECT_PLATFORM_IOS 1
        #define DETECT_PLATFORM_NAME "iOS"
    #endif
#elif defined(__linux__)
    #define DETECT_PLATFORM_LINUX 1
    #define DETECT_PLATFORM_NAME "Linux"
#elif defined(__FreeBSD__)
    #define DETECT_PLATFORM_FREEBSD 1
    #define DETECT_PLATFORM_NAME "FreeBSD"
#elif defined(__OpenBSD__)
    #define DETECT_PLATFORM_OPENBSD 1
    #define DETECT_PLATFORM_NAME "OpenBSD"
#else
    #define DETECT_PLATFORM_UNKNOWN 1
    #define DETECT_PLATFORM_NAME "Unknown"
#endif

// ==================== 编译器检测 ====================
#if defined(__clang__)
    #define DETECT_COMPILER_CLANG 1
    #define DETECT_COMPILER_NAME "Clang"
    #define DETECT_COMPILER_VERSION __clang_version__
#elif defined(__GNUC__)
    #define DETECT_COMPILER_GCC 1
    #define DETECT_COMPILER_NAME "GCC"
    #define DETECT_COMPILER_VERSION __VERSION__
#elif defined(_MSC_VER)
    #define DETECT_COMPILER_MSVC 1
    #define DETECT_COMPILER_NAME "MSVC"
    #define DETECT_COMPILER_VERSION _MSC_VER
#else
    #define DETECT_COMPILER_UNKNOWN 1
    #define DETECT_COMPILER_NAME "Unknown"
#endif

// ==================== C++标准检测 ====================
#if __cplusplus >= 202302L
    #define DETECT_CPP_STANDARD 23
    #define DETECT_CPP_STANDARD_NAME "C++23"
#elif __cplusplus >= 202002L
    #define DETECT_CPP_STANDARD 20
    #define DETECT_CPP_STANDARD_NAME "C++20"
#elif __cplusplus >= 201703L
    #define DETECT_CPP_STANDARD 17
    #define DETECT_CPP_STANDARD_NAME "C++17"
#elif __cplusplus >= 201402L
    #define DETECT_CPP_STANDARD 14
    #define DETECT_CPP_STANDARD_NAME "C++14"
#elif __cplusplus >= 201103L
    #define DETECT_CPP_STANDARD 11
    #define DETECT_CPP_STANDARD_NAME "C++11"
#else
    #define DETECT_CPP_STANDARD 98
    #define DETECT_CPP_STANDARD_NAME "C++98"
#endif

// ==================== 位架构检测 ====================
#if defined(__x86_64__) || defined(_M_X64)
    #define DETECT_ARCH_X86_64 1
    #define DETECT_ARCH_NAME "x86_64"
    #define DETECT_IS_64BIT 1
#elif defined(__i386__) || defined(_M_IX86)
    #define DETECT_ARCH_X86 1
    #define DETECT_ARCH_NAME "x86"
    #define DETECT_IS_32BIT 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define DETECT_ARCH_ARM64 1
    #define DETECT_ARCH_NAME "ARM64"
    #define DETECT_IS_64BIT 1
#elif defined(__arm__) || defined(_M_ARM)
    #define DETECT_ARCH_ARM 1
    #define DETECT_ARCH_NAME "ARM"
    #define DETECT_IS_32BIT 1
#elif defined(__riscv)
    #define DETECT_ARCH_RISCV 1
    #define DETECT_ARCH_NAME "RISC-V"
    #if __riscv_xlen == 64
        #define DETECT_IS_64BIT 1
    #else
        #define DETECT_IS_32BIT 1
    #endif
#elif defined(__powerpc64__) || defined(_M_PPC)
    #define DETECT_ARCH_PPC64 1
    #define DETECT_ARCH_NAME "PowerPC64"
    #define DETECT_IS_64BIT 1
#elif defined(__powerpc__)
    #define DETECT_ARCH_PPC 1
    #define DETECT_ARCH_NAME "PowerPC"
    #define DETECT_IS_32BIT 1
#else
    #define DETECT_ARCH_UNKNOWN 1
    #define DETECT_ARCH_NAME "Unknown"
#endif

// ==================== 字节序检测 ====================
#if defined(__BYTE_ORDER__)
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        #define DETECT_IS_LITTLE_ENDIAN 1
    #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        #define DETECT_IS_BIG_ENDIAN 1
    #elif __BYTE_ORDER__ == __ORDER_PDP_ENDIAN__
        #define DETECT_IS_PDP_ENDIAN 1
    #endif
#elif defined(_WIN32)
    #define DETECT_IS_LITTLE_ENDIAN 1
#elif defined(__BYTE_ORDER)
    #if __BYTE_ORDER == __LITTLE_ENDIAN
        #define DETECT_IS_LITTLE_ENDIAN 1
    #elif __BYTE_ORDER == __BIG_ENDIAN
        #define DETECT_IS_BIG_ENDIAN 1
    #elif __BYTE_ORDER == __PDP_ENDIAN
        #define DETECT_IS_PDP_ENDIAN 1
    #endif
#endif

// ==================== 导出宏 ====================
#if defined(_WIN32) || defined(_WIN64)
    #ifdef DETECT_BUILDING_DLL
        #define DETECT_API __declspec(dllexport)
    #elif defined(DETECT_USING_DLL)
        #define DETECT_API __declspec(dllimport)
    #else
        #define DETECT_API
    #endif
#else
    #if defined(__GNUC__) && __GNUC__ >= 4
        #define DETECT_API __attribute__((visibility("default")))
    #else
        #define DETECT_API
    #endif
#endif

// ==================== 内联提示 ====================
#if defined(__GNUC__) || defined(__clang__)
    #define DETECT_INLINE inline __attribute__((always_inline))
    #define DETECT_FORCE_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
    #define DETECT_INLINE __forceinline
    #define DETECT_FORCE_INLINE __forceinline
#else
    #define DETECT_INLINE inline
    #define DETECT_FORCE_INLINE inline
#endif

// ==================== 预测宏 ====================
#if defined(__GNUC__) || defined(__clang__)
    #define DETECT_LIKELY(x) __builtin_expect(!!(x), 1)
    #define DETECT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define DETECT_LIKELY(x) (x)
    #define DETECT_UNLIKELY(x) (x)
#endif

// ==================== 未使用参数警告抑制 ====================
#if defined(__GNUC__) || defined(__clang__)
    #define DETECT_UNUSED __attribute__((unused))
#else
    #define DETECT_UNUSED
#endif

// ==================== 警告抑制 ====================
#if defined(__GNUC__) || defined(__clang__)
    #define DETECT_PUSH_WARNING _Pragma("GCC diagnostic push")
    #define DETECT_POP_WARNING _Pragma("GCC diagnostic pop")
    #define DETECT_DISABLE_WARNING(warning) _Pragma(#warning)
#else
    #define DETECT_PUSH_WARNING
    #define DETECT_POP_WARNING
    #define DETECT_DISABLE_WARNING(warning)
#endif

// ==================== 内存对齐 ====================
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #include <stdalign.h>
    #define DETECT_ALIGNAS(n) alignas(n)
#elif defined(_MSC_VER)
    #define DETECT_ALIGNAS(n) __declspec(align(n))
#elif defined(__GNUC__) || defined(__clang__)
    #define DETECT_ALIGNAS(n) __attribute__((aligned(n)))
#else
    #define DETECT_ALIGNAS(n)
#endif

// ==================== 文件路径分隔符 ====================
#ifdef DETECT_PLATFORM_WINDOWS
    #define DETECT_PATH_SEPARATOR "\\"
    #define DETECT_PATH_SEPARATOR_CHAR '\\'
#else
    #define DETECT_PATH_SEPARATOR "/"
    #define DETECT_PATH_SEPARATOR_CHAR '/'
#endif

// ==================== 线程本地存储 ====================
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #define DETECT_THREAD_LOCAL _Thread_local
#elif defined(_MSC_VER)
    #define DETECT_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
    #define DETECT_THREAD_LOCAL __thread
#else
    #define DETECT_THREAD_LOCAL
#endif

// ==================== 构建配置 ====================
#ifndef DETECT_BUILD_TYPE
    #ifdef NDEBUG
        #define DETECT_BUILD_TYPE "Release"
    #else
        #define DETECT_BUILD_TYPE "Debug"
    #endif
#endif

// ==================== 平台检测类 ====================
namespace dbrecovery {
namespace platform {

/**
 * @brief 平台信息结构
 */
struct PlatformInfo {
    std::string platform_name;
    std::string compiler_name;
    std::string compiler_version;
    std::string arch_name;
    std::string cpp_standard;
    std::string build_type;
    bool is_64bit;
    bool is_little_endian;
    uint32_t page_size;
};

/**
 * @brief 平台检测器
 */
class DETECT_API PlatformDetector {
public:
    static PlatformDetector& instance();

    /**
     * @brief 获取平台信息
     */
    PlatformInfo get_platform_info() const;

    /**
     * @brief 获取平台名称
     */
    std::string get_platform_name() const;

    /**
     * @brief 获取编译器名称
     */
    std::string get_compiler_name() const;

    /**
     * @brief 获取编译器版本
     */
    std::string get_compiler_version() const;

    /**
     * @brief 获取架构名称
     */
    std::string get_arch_name() const;

    /**
     * @brief 获取C++标准版本
     */
    std::string get_cpp_standard() const;

    /**
     * @brief 获取构建类型
     */
    std::string get_build_type() const;

    /**
     * @brief 是否为64位
     */
    bool is_64bit() const;

    /**
     * @brief 是否为小端
     */
    bool is_little_endian() const;

    /**
     * @brief 获取系统页大小
     */
    uint32_t get_page_size() const;

    /**
     * @brief 获取系统页面大小
     */
    uint32_t get_memory_page_size() const;

    /**
     * @brief 检查是否为指定平台
     */
    bool is_windows() const;
    bool is_macos() const;
    bool is_linux() const;
    bool is_freebsd() const;
    bool is_openbsd() const;

    /**
     * @brief 检查是否为指定架构
     */
    bool is_x86_64() const;
    bool is_x86() const;
    bool is_arm64() const;
    bool is_arm() const;
    bool is_riscv() const;
    bool is_ppc64() const;
    bool is_ppc() const;

    /**
     * @brief 获取平台字符串表示
     */
    std::string to_string() const;

private:
    PlatformDetector();
    ~PlatformDetector() = default;
    PlatformDetector(const PlatformDetector&) = delete;
    PlatformDetector& operator=(const PlatformDetector&) = delete;

    PlatformInfo info_;
};

// ==================== 便捷函数 ====================
inline PlatformInfo get_platform_info() {
    return PlatformDetector::instance().get_platform_info();
}

inline std::string get_platform_name() {
    return PlatformDetector::instance().get_platform_name();
}

inline std::string get_compiler_name() {
    return PlatformDetector::instance().get_compiler_name();
}

inline std::string get_arch_name() {
    return PlatformDetector::instance().get_arch_name();
}

inline bool is_64bit() {
    return PlatformDetector::instance().is_64bit();
}

inline bool is_little_endian() {
    return PlatformDetector::instance().is_little_endian();
}

} // namespace platform
} // namespace dbrecovery
