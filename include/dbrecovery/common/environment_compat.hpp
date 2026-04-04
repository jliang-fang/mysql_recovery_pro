/**
 * @file environment_compat.hpp
 * @brief 多环境兼容性层 - 支持Linux/Windows/macOS
 * 
 * 本文件提供跨平台的环境兼容性抽象层，
 * 统一不同操作系统之间的API差异。
 */

#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <chrono>
#include <atomic>

// ==================== 平台检测 ====================
#if defined(_WIN32) || defined(_WIN64)
    #define ENV_PLATFORM_WINDOWS 1
    #define ENV_PLATFORM_NAME "Windows"
#elif defined(__APPLE__)
    #define ENV_PLATFORM_MACOS 1
    #define ENV_PLATFORM_NAME "macOS"
#elif defined(__linux__)
    #define ENV_PLATFORM_LINUX 1
    #define ENV_PLATFORM_NAME "Linux"
#else
    #define ENV_PLATFORM_UNKNOWN 1
    #define ENV_PLATFORM_NAME "Unknown"
#endif

// ==================== 编译器检测 ====================
#if defined(__clang__)
    #define ENV_COMPILER_CLANG 1
    #define ENV_COMPILER_NAME "Clang"
    #define ENV_COMPILER_VERSION __clang_version__
#elif defined(__GNUC__)
    #define ENV_COMPILER_GCC 1
    #define ENV_COMPILER_NAME "GCC"
    #define ENV_COMPILER_VERSION __VERSION__
#elif defined(_MSC_VER)
    #define ENV_COMPILER_MSVC 1
    #define ENV_COMPILER_NAME "MSVC"
    #define ENV_COMPILER_VERSION _MSC_VER
#else
    #define ENV_COMPILER_UNKNOWN 1
    #define ENV_COMPILER_NAME "Unknown"
#endif

// ==================== C++标准检测 ====================
#if __cplusplus >= 202002L
    #define ENV_CPP_STANDARD 20
    #define ENV_CPP_STANDARD_NAME "C++20"
#elif __cplusplus >= 201703L
    #define ENV_CPP_STANDARD 17
    #define ENV_CPP_STANDARD_NAME "C++17"
#elif __cplusplus >= 201402L
    #define ENV_CPP_STANDARD 14
    #define ENV_CPP_STANDARD_NAME "C++14"
#else
    #define ENV_CPP_STANDARD 11
    #define ENV_CPP_STANDARD_NAME "C++11"
#endif

// ==================== 位架构检测 ====================
#if defined(__x86_64__) || defined(_M_X64)
    #define ENV_ARCH_X64 1
    #define ENV_ARCH_NAME "x86_64"
    #define ENV_IS_64BIT 1
#elif defined(__i386__) || defined(_M_IX86)
    #define ENV_ARCH_X86 1
    #define ENV_ARCH_NAME "x86"
    #define ENV_IS_32BIT 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define ENV_ARCH_ARM64 1
    #define ENV_ARCH_NAME "ARM64"
    #define ENV_IS_64BIT 1
#elif defined(__arm__) || defined(_M_ARM)
    #define ENV_ARCH_ARM 1
    #define ENV_ARCH_NAME "ARM"
    #define ENV_IS_32BIT 1
#else
    #define ENV_ARCH_UNKNOWN 1
    #define ENV_ARCH_NAME "Unknown"
#endif

// ==================== 导出宏 ====================
#if defined(_WIN32) || defined(_WIN64)
    #ifdef ENV_BUILDING_DLL
        #define ENV_API __declspec(dllexport)
    #elif defined(ENV_USING_DLL)
        #define ENV_API __declspec(dllimport)
    #else
        #define ENV_API
    #endif
#else
    #define ENV_API __attribute__((visibility("default")))
#endif

// ==================== 内联提示 ====================
#if defined(__GNUC__) || defined(__clang__)
    #define ENV_INLINE inline __attribute__((always_inline))
    #define ENV_FORCE_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
    #define ENV_INLINE __forceinline
    #define ENV_FORCE_INLINE __forceinline
#else
    #define ENV_INLINE inline
    #define ENV_FORCE_INLINE inline
#endif

// ==================== 预测宏 ====================
#if defined(__GNUC__) || defined(__clang__)
    #define ENV_LIKELY(x) __builtin_expect(!!(x), 1)
    #define ENV_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define ENV_LIKELY(x) (x)
    #define ENV_UNLIKELY(x) (x)
#endif

// ==================== 未使用参数警告抑制 ====================
#if defined(__GNUC__) || defined(__clang__)
    #define ENV_UNUSED __attribute__((unused))
#else
    #define ENV_UNUSED
#endif

// ==================== 警告抑制 ====================
#if defined(__GNUC__) || defined(__clang__)
    #define ENV_PUSH_WARNING _Pragma("GCC diagnostic push")
    #define ENV_POP_WARNING _Pragma("GCC diagnostic pop")
    #define ENV_DISABLE_WARNING(warning) _Pragma(#warning)
#else
    #define ENV_PUSH_WARNING
    #define ENV_POP_WARNING
    #define ENV_DISABLE_WARNING(warning)
#endif

// ==================== 内存对齐 ====================
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #include <stdalign.h>
    #define ENV_ALIGNAS(n) alignas(n)
#elif defined(_MSC_VER)
    #define ENV_ALIGNAS(n) __declspec(align(n))
#elif defined(__GNUC__) || defined(__clang__)
    #define ENV_ALIGNAS(n) __attribute__((aligned(n)))
#else
    #define ENV_ALIGNAS(n)
#endif

// ==================== 文件路径分隔符 ====================
#ifdef ENV_PLATFORM_WINDOWS
    #define ENV_PATH_SEPARATOR "\\"
    #define ENV_PATH_SEPARATOR_CHAR '\\'
#else
    #define ENV_PATH_SEPARATOR "/"
    #define ENV_PATH_SEPARATOR_CHAR '/'
#endif

// ==================== 线程本地存储 ====================
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #define ENV_THREAD_LOCAL _Thread_local
#elif defined(_MSC_VER)
    #define ENV_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
    #define ENV_THREAD_LOCAL __thread
#else
    #define ENV_THREAD_LOCAL
#endif

// ==================== 大端/小端检测 ====================
#if defined(__BYTE_ORDER__)
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        #define ENV_IS_LITTLE_ENDIAN 1
    #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        #define ENV_IS_BIG_ENDIAN 1
    #else
        #define ENV_IS_UNKNOWN_ENDIAN 1
    #endif
#elif defined(_WIN32)
    #define ENV_IS_LITTLE_ENDIAN 1
#elif defined(__BYTE_ORDER)
    #if __BYTE_ORDER == __LITTLE_ENDIAN
        #define ENV_IS_LITTLE_ENDIAN 1
    #elif __BYTE_ORDER == __BIG_ENDIAN
        #define ENV_IS_BIG_ENDIAN 1
    #else
        #define ENV_IS_UNKNOWN_ENDIAN 1
    #endif
#else
    // 运行时检测
    inline bool is_little_endian() {
        uint16_t test = 1;
        return *reinterpret_cast<uint8_t*>(&test) == 1;
    }
    #define ENV_IS_LITTLE_ENDIAN is_little_endian()
#endif

// ==================== CPU特性检测 ====================
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    // x86/x86_64特定功能
    #if defined(__SSE4_2__) || defined(_M_X64)
        #define ENV_CPU_HAS_SSE42 1
    #endif
    #if defined(__AVX2__)
        #define ENV_CPU_HAS_AVX2 1
    #endif
    #if defined(__PCLMUL__)
        #define ENV_CPU_HAS_PCLMUL 1
    #endif
#endif

namespace dbrecovery {
namespace env {

// ==================== 错误码枚举 ====================
enum class ErrorCode {
    SUCCESS = 0,
    FILE_NOT_FOUND = 1,
    PERMISSION_DENIED = 2,
    IO_ERROR = 3,
    INVALID_ARGUMENT = 4,
    OUT_OF_MEMORY = 5,
    NOT_SUPPORTED = 6,
    TIMEOUT = 7,
    ALREADY_EXISTS = 8,
    UNKNOWN = 99
};

// ==================== 错误信息 ====================
struct ErrorInfo {
    ErrorCode code;
    std::string message;
    std::string detail;
    
    bool is_success() const { return code == ErrorCode::SUCCESS; }
    bool is_error() const { return code != ErrorCode::SUCCESS; }
};

// ==================== 文件信息 ====================
struct FileInfo {
    std::string path;
    uint64_t size;
    uint64_t creation_time;
    uint64_t last_modified;
    uint64_t last_accessed;
    bool is_directory;
    bool is_regular_file;
    std::string extension;
    std::string filename;
};

// ==================== 系统信息 ====================
struct SystemInfo {
    std::string os_name;
    std::string os_version;
    std::string hostname;
    uint32_t cpu_cores;
    uint64_t total_memory;
    uint64_t available_memory;
    std::string architecture;
    std::string compiler;
    std::string compiler_version;
    std::string cpp_standard;
};

// ==================== 文件系统类 ====================
/**
 * @brief 跨平台文件系统操作类
 */
class ENV_API FileSystem {
public:
    static FileSystem& instance();

    // 文件操作
    ErrorInfo file_exists(const std::string& path) const;
    ErrorInfo file_size(const std::string& path, uint64_t& size) const;
    ErrorInfo file_info(const std::string& path, FileInfo& info) const;
    ErrorInfo file_delete(const std::string& path) const;
    ErrorInfo file_copy(const std::string& src, const std::string& dst) const;
    ErrorInfo file_move(const std::string& src, const std::string& dst) const;

    // 目录操作
    ErrorInfo directory_exists(const std::string& path) const;
    ErrorInfo directory_create(const std::string& path, bool recursive = false) const;
    ErrorInfo directory_remove(const std::string& path, bool recursive = false) const;
    ErrorInfo directory_list(const std::string& path, std::vector<std::string>& files) const;

    // 临时文件
    ErrorInfo temp_directory(std::string& path) const;
    ErrorInfo create_temp_file(std::string& path) const;

    // 路径操作
    std::string path_join(const std::string& base, const std::string& relative) const;
    std::string path_normalize(const std::string& path) const;
    std::string path_extension(const std::string& path) const;
    std::string path_filename(const std::string& path) const;
    std::string path_basename(const std::string& path) const;

private:
    FileSystem();
    ~FileSystem() = default;
    FileSystem(const FileSystem&) = delete;
    FileSystem& operator=(const FileSystem&) = delete;

    // 平台特定实现
    ErrorInfo file_exists_impl(const std::string& path) const;
    ErrorInfo file_size_impl(const std::string& path, uint64_t& size) const;
    ErrorInfo file_info_impl(const std::string& path, FileInfo& info) const;
    ErrorInfo file_delete_impl(const std::string& path) const;
    ErrorInfo file_copy_impl(const std::string& src, const std::string& dst) const;
    ErrorInfo file_move_impl(const std::string& src, const std::string& dst) const;
    ErrorInfo directory_create_impl(const std::string& path, bool recursive) const;
    ErrorInfo directory_remove_impl(const std::string& path, bool recursive) const;
    ErrorInfo directory_list_impl(const std::string& path, std::vector<std::string>& files) const;
};

// ==================== 内存管理类 ====================
/**
 * @brief 跨平台内存管理类
 */
class ENV_API MemoryManager {
public:
    static MemoryManager& instance();

    // 分配内存
    void* allocate(size_t size);
    void* aligned_allocate(size_t size, size_t alignment);
    
    // 释放内存
    void deallocate(void* ptr);
    void aligned_deallocate(void* ptr);

    // 内存操作
    void zero_memory(void* ptr, size_t size);
    void copy_memory(void* dest, const void* src, size_t size);
    void set_memory(void* ptr, uint8_t value, size_t size);

    // 统计信息
    uint64_t total_allocated() const;
    uint64_t peak_allocated() const;
    uint64_t allocation_count() const;

private:
    MemoryManager();
    ~MemoryManager();
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    // 平台特定实现
    void* allocate_impl(size_t size);
    void* aligned_allocate_impl(size_t size, size_t alignment);
    void deallocate_impl(void* ptr);
    void aligned_deallocate_impl(void* ptr);

    mutable std::atomic<uint64_t> total_allocated_;
    mutable std::atomic<uint64_t> peak_allocated_;
    mutable std::atomic<uint64_t> allocation_count_;
};

// ==================== 线程管理类 ====================
/**
 * @brief 跨平台线程管理类
 */
class ENV_API ThreadManager {
public:
    static ThreadManager& instance();

    // 获取CPU核心数
    uint32_t cpu_cores() const;

    // 线程ID
    uint64_t current_thread_id() const;
    std::string thread_name() const;

    // 线程睡眠
    void sleep_ms(uint32_t milliseconds);
    void sleep_us(uint32_t microseconds);

    // 进程优先级
    void set_thread_priority(int priority);
    int get_thread_priority() const;

    // 平台特定实现
    uint32_t cpu_cores_impl() const;

private:
    ThreadManager();
    ~ThreadManager() = default;
    ThreadManager(const ThreadManager&) = delete;
    ThreadManager& operator=(const ThreadManager&) = delete;

    uint32_t cpu_cores_;
};

// ==================== 时间管理类 ====================
/**
 * @brief 跨平台时间管理类
 */
class ENV_API TimeManager {
public:
    static TimeManager& instance();

    // 当前时间
    std::chrono::system_clock::time_point now() const;
    uint64_t timestamp_ms() const;
    uint64_t timestamp_us() const;
    uint64_t timestamp_ns() const;

    // 高精度计时
    uint64_t high_res_counter() const;
    uint64_t high_res_frequency() const;

    // 格式化时间
    std::string format_time(const std::chrono::system_clock::time_point& time,
                           const std::string& format = "%Y-%m-%d %H:%M:%S") const;
    std::string format_duration(std::chrono::milliseconds duration) const;

private:
    TimeManager();
    ~TimeManager() = default;
    TimeManager(const TimeManager&) = delete;
    TimeManager& operator=(const TimeManager&) = delete;

    // 平台特定实现
    uint64_t timestamp_ms_impl() const;
    uint64_t timestamp_us_impl() const;
    uint64_t timestamp_ns_impl() const;
    uint64_t high_res_counter_impl() const;
    uint64_t high_res_frequency_impl() const;
};

// ==================== 系统信息类 ====================
/**
 * @brief 系统信息获取类
 */
class ENV_API SystemInfoGetter {
public:
    static SystemInfoGetter& instance();

    // 获取系统信息
    SystemInfo get_system_info() const;

    // 获取OS信息
    std::string os_name() const;
    std::string os_version() const;
    std::string hostname() const;

    // 获取硬件信息
    uint32_t cpu_cores() const;
    uint64_t total_memory() const;
    uint64_t available_memory() const;
    std::string architecture() const;

private:
    SystemInfoGetter();
    ~SystemInfoGetter() = default;
    SystemInfoGetter(const SystemInfoGetter&) = delete;
    SystemInfoGetter& operator=(const SystemInfoGetter&) = delete;

    // 平台特定实现
    SystemInfo get_system_info_impl() const;
    std::string os_name_impl() const;
    std::string os_version_impl() const;
    std::string hostname_impl() const;
    uint32_t cpu_cores_impl() const;
    uint64_t total_memory_impl() const;
    uint64_t available_memory_impl() const;
    std::string architecture_impl() const;
};

// ==================== 日志类 ====================
/**
 * @brief 跨平台日志类
 */
class ENV_API Logger {
public:
    enum class Level {
        TRACE = 0,
        DEBUG = 1,
        INFO = 2,
        WARN = 3,
        ERROR = 4,
        FATAL = 5
    };

    static Logger& instance();

    // 设置日志级别
    void set_level(Level level);
    Level level() const;

    // 设置日志输出
    void set_output_file(const std::string& path);
    void set_output_stdout(bool enable);

    // 日志记录
    void log(Level level, const std::string& message);
    void log(Level level, const std::string& message, const std::string& file, int line);
    
    // 便捷方法
    void trace(const std::string& message);
    void debug(const std::string& message);
    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);
    void fatal(const std::string& message);

    // 带位置的日志
    void trace(const std::string& message, const std::string& file, int line);
    void debug(const std::string& message, const std::string& file, int line);
    void info(const std::string& message, const std::string& file, int line);
    void warn(const std::string& message, const std::string& file, int line);
    void error(const std::string& message, const std::string& file, int line);
    void fatal(const std::string& message, const std::string& file, int line);

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log_impl(Level level, const std::string& message, const std::string& file, int line);

    Level level_;
    bool output_stdout_;
    std::string output_file_;
    mutable std::mutex mutex_;
};

// ==================== 错误处理类 ====================
/**
 * @brief 跨平台错误处理类
 */
class ENV_API ErrorHandler {
public:
    static ErrorHandler& instance();

    // 获取最后的系统错误
    ErrorInfo last_error() const;
    ErrorInfo last_error(const std::string& context) const;

    // 设置错误
    void set_error(ErrorCode code, const std::string& message);
    void set_error(ErrorCode code, const std::string& message, const std::string& detail);

    // 错误转换
    ErrorInfo system_error_to_error_info(int system_error_code) const;

private:
    ErrorHandler();
    ~ErrorHandler() = default;
    ErrorHandler(const ErrorHandler&) = delete;
    ErrorHandler& operator=(const ErrorHandler&) = delete;

    ErrorInfo last_error_;
};

// ==================== 平台检测辅助函数 ====================
inline bool is_windows() {
#ifdef ENV_PLATFORM_WINDOWS
    return true;
#else
    return false;
#endif
}
inline bool is_macos() {
#ifdef ENV_PLATFORM_MACOS
    return true;
#else
    return false;
#endif
}
inline bool is_linux() {
#ifdef ENV_PLATFORM_LINUX
    return true;
#else
    return false;
#endif
}
inline bool is_64bit() {
#ifdef ENV_IS_64BIT
    return true;
#else
    return false;
#endif
}
inline bool is_little_endian_runtime() {
#ifdef ENV_IS_LITTLE_ENDIAN
    return ENV_IS_LITTLE_ENDIAN;
#else
    return is_little_endian();
#endif
}

// ==================== 便捷函数 ====================
inline FileSystem& fs() { return FileSystem::instance(); }
inline MemoryManager& mem() { return MemoryManager::instance(); }
inline ThreadManager& thread() { return ThreadManager::instance(); }
inline TimeManager& time() { return TimeManager::instance(); }
inline SystemInfoGetter& sysinfo() { return SystemInfoGetter::instance(); }
inline Logger& log() { return Logger::instance(); }
inline ErrorHandler& err() { return ErrorHandler::instance(); }

} // namespace env
} // namespace dbrecovery
