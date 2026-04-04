/**
 * @file environment_compat_platform.cpp
 * @brief 多环境兼容性层平台特定实现
 */

#include "dbrecovery/common/environment_compat.hpp"
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <atomic>
#include <iostream>
#include <stdexcept>

#ifdef ENV_PLATFORM_WINDOWS
    #include <windows.h>
    #include <sysinfoapi.h>
    #include <process.h>
    #include <tlhelp32.h>
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/time.h>
    #include <dirent.h>
    #include <pthread.h>
    #include <pwd.h>
    #include <sys/resource.h>
    #include <sys/sysinfo.h>
    #ifdef ENV_PLATFORM_MACOS
        #include <sys/sysctl.h>
        #include <mach/mach.h>
        #include <mach/task_info.h>
    #endif
#endif

namespace dbrecovery {
namespace env {

// ==================== MemoryManager 实现 ====================

MemoryManager::MemoryManager() 
    : total_allocated_(0), peak_allocated_(0), allocation_count_(0) {
}

MemoryManager::~MemoryManager() = default;

MemoryManager& MemoryManager::instance() {
    static MemoryManager instance_;
    return instance_;
}

void* MemoryManager::allocate(size_t size) {
    if (size == 0) return nullptr;
    
    void* ptr = allocate_impl(size);
    if (ptr) {
        total_allocated_ += size;
        allocation_count_++;
        
        uint64_t current_peak = peak_allocated_.load();
        while (total_allocated_.load() > current_peak) {
            if (peak_allocated_.compare_exchange_weak(current_peak, total_allocated_.load())) {
                break;
            }
        }
    }
    return ptr;
}

void* MemoryManager::aligned_allocate(size_t size, size_t alignment) {
    if (size == 0) return nullptr;
    
    void* ptr = aligned_allocate_impl(size, alignment);
    if (ptr) {
        total_allocated_ += size;
        allocation_count_++;
        
        uint64_t current_peak = peak_allocated_.load();
        while (total_allocated_.load() > current_peak) {
            if (peak_allocated_.compare_exchange_weak(current_peak, total_allocated_.load())) {
                break;
            }
        }
    }
    return ptr;
}

void MemoryManager::deallocate(void* ptr) {
    if (ptr) {
        deallocate_impl(ptr);
    }
}

void MemoryManager::aligned_deallocate(void* ptr) {
    if (ptr) {
        aligned_deallocate_impl(ptr);
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

// 平台特定实现
void* MemoryManager::allocate_impl(size_t size) {
#ifdef ENV_PLATFORM_WINDOWS
    return _aligned_malloc(size, 16);
#else
    return std::malloc(size);
#endif
}

void* MemoryManager::aligned_allocate_impl(size_t size, size_t alignment) {
#ifdef ENV_PLATFORM_WINDOWS
    return _aligned_malloc(size, alignment);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    return aligned_alloc(alignment, (size + alignment - 1) & ~(alignment - 1));
#else
    // Fallback for C++11
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) == 0) {
        return ptr;
    }
    return nullptr;
#endif
}

void MemoryManager::deallocate_impl(void* ptr) {
#ifdef ENV_PLATFORM_WINDOWS
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void MemoryManager::aligned_deallocate_impl(void* ptr) {
#ifdef ENV_PLATFORM_WINDOWS
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

// ==================== ThreadManager 实现 ====================

ThreadManager::ThreadManager() {
    cpu_cores_ = cpu_cores_impl();
}

uint32_t ThreadManager::cpu_cores() const {
    return cpu_cores_;
}

uint64_t ThreadManager::current_thread_id() const {
#ifdef ENV_PLATFORM_WINDOWS
    return static_cast<uint64_t>(GetCurrentThreadId());
#else
    return static_cast<uint64_t>(pthread_self());
#endif
}

std::string ThreadManager::thread_name() const {
#ifdef ENV_PLATFORM_WINDOWS
    // Windows doesn't support getting thread name easily
    return "Thread_" + std::to_string(current_thread_id());
#else
    char name[16];
    int result = pthread_getname_np(pthread_self(), name, sizeof(name));
    if (result == 0 && name[0] != '\0') {
        return std::string(name);
    }
    return "Thread_" + std::to_string(current_thread_id());
#endif
}

void ThreadManager::sleep_ms(uint32_t milliseconds) {
#ifdef ENV_PLATFORM_WINDOWS
    Sleep(milliseconds);
#else
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, nullptr);
#endif
}

void ThreadManager::sleep_us(uint32_t microseconds) {
#ifdef ENV_PLATFORM_WINDOWS
    Sleep(microseconds / 1000);
#else
    struct timespec ts;
    ts.tv_sec = microseconds / 1000000;
    ts.tv_nsec = (microseconds % 1000000) * 1000;
    nanosleep(&ts, nullptr);
#endif
}

void ThreadManager::set_thread_priority(int priority) {
#ifdef ENV_PLATFORM_WINDOWS
    HANDLE hThread = GetCurrentThread();
    int win_priority = THREAD_PRIORITY_NORMAL;
    if (priority < -10) win_priority = THREAD_PRIORITY_IDLE;
    else if (priority < 0) win_priority = THREAD_PRIORITY_BELOW_NORMAL;
    else if (priority > 10) win_priority = THREAD_PRIORITY_TIME_CRITICAL;
    else if (priority > 0) win_priority = THREAD_PRIORITY_ABOVE_NORMAL;
    SetThreadPriority(hThread, win_priority);
#else
    int policy;
    struct sched_param param;
    pthread_getschedparam(pthread_self(), &policy, &param);
    
    if (priority < -10) {
        param.sched_priority = sched_get_priority_min(policy);
    } else if (priority > 10) {
        param.sched_priority = sched_get_priority_max(policy);
    } else {
        int min = sched_get_priority_min(policy);
        int max = sched_get_priority_max(policy);
        param.sched_priority = min + (max - min) * (priority + 10) / 20;
    }
    pthread_setschedparam(pthread_self(), policy, &param);
#endif
}

int ThreadManager::get_thread_priority() const {
#ifdef ENV_PLATFORM_WINDOWS
    HANDLE hThread = GetCurrentThread();
    int win_priority = GetThreadPriority(hThread);
    if (win_priority == THREAD_PRIORITY_IDLE) return -20;
    if (win_priority == THREAD_PRIORITY_BELOW_NORMAL) return -5;
    if (win_priority == THREAD_PRIORITY_NORMAL) return 0;
    if (win_priority == THREAD_PRIORITY_ABOVE_NORMAL) return 5;
    if (win_priority == THREAD_PRIORITY_TIME_CRITICAL) return 20;
    return 0;
#else
    int policy;
    struct sched_param param;
    pthread_getschedparam(pthread_self(), &policy, &param);
    return param.sched_priority;
#endif
}

uint32_t ThreadManager::cpu_cores_impl() const {
#ifdef ENV_PLATFORM_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<uint32_t>(si.dwNumberOfProcessors);
#elif defined(_SC_NPROCESSORS_ONLN)
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) return static_cast<uint32_t>(cores);
#endif
    return 1;
}

// ==================== TimeManager 实现 ====================

TimeManager::TimeManager() = default;

TimeManager& TimeManager::instance() {
    static TimeManager instance_;
    return instance_;
}

std::chrono::system_clock::time_point TimeManager::now() const {
    return std::chrono::system_clock::now();
}

uint64_t TimeManager::timestamp_ms() const {
    return timestamp_ms_impl();
}

uint64_t TimeManager::timestamp_us() const {
    return timestamp_us_impl();
}

uint64_t TimeManager::timestamp_ns() const {
    return timestamp_ns_impl();
}

uint64_t TimeManager::high_res_counter() const {
    return high_res_counter_impl();
}

uint64_t TimeManager::high_res_frequency() const {
    return high_res_frequency_impl();
}

std::string TimeManager::format_time(const std::chrono::system_clock::time_point& time,
                                     const std::string& format) const {
    auto tt = std::chrono::system_clock::to_time_t(time);
    std::tm* tm = std::localtime(&tt);
    
    std::ostringstream oss;
    oss << std::put_time(tm, format.c_str());
    return oss.str();
}

std::string TimeManager::format_duration(std::chrono::milliseconds duration) const {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    auto s = std::chrono::duration_cast<std::chrono::seconds>(ms);
    auto m = std::chrono::duration_cast<std::chrono::minutes>(s);
    auto h = std::chrono::duration_cast<std::chrono::hours>(m);
    
    std::ostringstream oss;
    if (h.count() > 0) {
        oss << h.count() << "h ";
    }
    if (m.count() % 60 > 0 || h.count() > 0) {
        oss << (m.count() % 60) << "m ";
    }
    if (s.count() % 60 > 0 || m.count() > 0 || h.count() > 0) {
        oss << (s.count() % 60) << "s";
    }
    if (ms.count() % 1000 > 0 && h.count() == 0 && m.count() == 0) {
        oss << (ms.count() % 1000) << "ms";
    }
    
    return oss.str();
}

// 平台特定实现
uint64_t TimeManager::timestamp_ms_impl() const {
#ifdef ENV_PLATFORM_WINDOWS
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    double frequency = static_cast<double>(li.QuadPart);
    
    QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart / frequency * 1000.0);
#else
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
#endif
}

uint64_t TimeManager::timestamp_us_impl() const {
#ifdef ENV_PLATFORM_WINDOWS
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    double frequency = static_cast<double>(li.QuadPart);
    
    QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart / frequency * 1000000.0);
#else
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
#endif
}

uint64_t TimeManager::timestamp_ns_impl() const {
#ifdef ENV_PLATFORM_WINDOWS
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    double frequency = static_cast<double>(li.QuadPart);
    
    QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart / frequency * 1000000000.0);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#endif
}

uint64_t TimeManager::high_res_counter_impl() const {
#ifdef ENV_PLATFORM_WINDOWS
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#endif
}

uint64_t TimeManager::high_res_frequency_impl() const {
#ifdef ENV_PLATFORM_WINDOWS
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return static_cast<uint64_t>(li.QuadPart);
#else
    return 1000000000ULL;  // 1 GHz for nanosecond resolution
#endif
}

// ==================== SystemInfoGetter 实现 ====================

SystemInfoGetter::SystemInfoGetter() = default;

SystemInfoGetter& SystemInfoGetter::instance() {
    static SystemInfoGetter instance_;
    return instance_;
}

SystemInfo SystemInfoGetter::get_system_info() const {
    return get_system_info_impl();
}

std::string SystemInfoGetter::os_name() const {
    return os_name_impl();
}

std::string SystemInfoGetter::os_version() const {
    return os_version_impl();
}

std::string SystemInfoGetter::hostname() const {
    return hostname_impl();
}

uint32_t SystemInfoGetter::cpu_cores() const {
    return cpu_cores_impl();
}

uint64_t SystemInfoGetter::total_memory() const {
    return total_memory_impl();
}

uint64_t SystemInfoGetter::available_memory() const {
    return available_memory_impl();
}

std::string SystemInfoGetter::architecture() const {
    return architecture_impl();
}

// 平台特定实现
SystemInfo SystemInfoGetter::get_system_info_impl() const {
    SystemInfo info;
    info.os_name = os_name_impl();
    info.os_version = os_version_impl();
    info.hostname = hostname_impl();
    info.cpu_cores = cpu_cores_impl();
    info.total_memory = total_memory_impl();
    info.available_memory = available_memory_impl();
    info.architecture = architecture_impl();
    
#ifdef ENV_COMPILER_NAME
    info.compiler = ENV_COMPILER_NAME;
#else
    info.compiler = "Unknown";
#endif

#ifdef ENV_COMPILER_VERSION
    info.compiler_version = std::string(ENV_COMPILER_VERSION);
#else
    info.compiler_version = "Unknown";
#endif

#ifdef ENV_CPP_STANDARD_NAME
    info.cpp_standard = ENV_CPP_STANDARD_NAME;
#else
    info.cpp_standard = "Unknown";
#endif

    return info;
}

std::string SystemInfoGetter::os_name_impl() const {
#ifdef ENV_PLATFORM_WINDOWS
    return "Windows";
#elif defined(ENV_PLATFORM_MACOS)
    return "macOS";
#elif defined(ENV_PLATFORM_LINUX)
    return "Linux";
#else
    return "Unknown";
#endif
}

std::string SystemInfoGetter::os_version_impl() const {
#ifdef ENV_PLATFORM_WINDOWS
    OSVERSIONINFOA osvi;
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);
    GetVersionExA(&osvi);
    return std::to_string(osvi.dwMajorVersion) + "." + 
           std::to_string(osvi.dwMinorVersion) + "." +
           std::to_string(osvi.dwBuildNumber);
#elif defined(ENV_PLATFORM_MACOS)
    return "macOS";  // macOS version requires sysctl
#elif defined(ENV_PLATFORM_LINUX)
    std::ifstream release("/etc/os-release");
    std::string line;
    while (std::getline(release, line)) {
        if (line.find("PRETTY_NAME=") == 0) {
            return line.substr(12, line.size() - 13);
        }
    }
    return "Linux";
#else
    return "Unknown";
#endif
}

std::string SystemInfoGetter::hostname_impl() const {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        return std::string(hostname);
    }
    return "Unknown";
}

uint32_t SystemInfoGetter::cpu_cores_impl() const {
#ifdef ENV_PLATFORM_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<uint32_t>(si.dwNumberOfProcessors);
#elif defined(_SC_NPROCESSORS_ONLN)
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) return static_cast<uint32_t>(cores);
#endif
    return 1;
}

uint64_t SystemInfoGetter::total_memory_impl() const {
#ifdef ENV_PLATFORM_WINDOWS
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&ms);
    return static_cast<uint64_t>(ms.ullTotalPhys);
#elif defined(ENV_PLATFORM_MACOS)
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    uint64_t memsize;
    size_t len = sizeof(memsize);
    sysctl(mib, 2, &memsize, &len, nullptr, 0);
    return memsize;
#elif defined(_SC_PHYS_PAGES)
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<uint64_t>(pages) * page_size;
    }
#endif
    return 0;
}

uint64_t SystemInfoGetter::available_memory_impl() const {
#ifdef ENV_PLATFORM_WINDOWS
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&ms);
    return static_cast<uint64_t>(ms.ullAvailPhys);
#elif defined(ENV_PLATFORM_MACOS)
    mach_port_t host_port = mach_host_self();
    mach_msg_type_number_t count = HOST_VM_INFO_COUNT;
    vm_statistics_data_t vm_stats;
    if (host_statistics(host_port, HOST_VM_INFO, reinterpret_cast<host_info_t>(&vm_stats), &count) == 0) {
        return static_cast<uint64_t>(vm_stats.free_count) * vm_page_size;
    }
#elif defined(_SC_AVPHYS_PAGES)
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<uint64_t>(pages) * page_size;
    }
#endif
    return 0;
}

std::string SystemInfoGetter::architecture_impl() const {
#ifdef ENV_ARCH_NAME
    return ENV_ARCH_NAME;
#else
    return "Unknown";
#endif
}

// ==================== Logger 实现 ====================

Logger::Logger() 
    : level_(Level::INFO), output_stdout_(true), output_file_("") {
}

Logger::~Logger() {
    if (!output_file_.empty()) {
        // 关闭文件
    }
}

Logger& Logger::instance() {
    static Logger instance_;
    return instance_;
}

void Logger::set_level(Level level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

Logger::Level Logger::level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

void Logger::set_output_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_file_ = path;
}

void Logger::set_output_stdout(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_stdout_ = enable;
}

void Logger::log(Level level, const std::string& message) {
    log_impl(level, message, "", 0);
}

void Logger::log(Level level, const std::string& message, const std::string& file, int line) {
    log_impl(level, message, file, line);
}

void Logger::trace(const std::string& message) { log(Level::TRACE, message); }
void Logger::debug(const std::string& message) { log(Level::DEBUG, message); }
void Logger::info(const std::string& message) { log(Level::INFO, message); }
void Logger::warn(const std::string& message) { log(Level::WARN, message); }
void Logger::error(const std::string& message) { log(Level::ERROR, message); }
void Logger::fatal(const std::string& message) { log(Level::FATAL, message); }

void Logger::trace(const std::string& message, const std::string& file, int line) {
    log(Level::TRACE, message, file, line);
}
void Logger::debug(const std::string& message, const std::string& file, int line) {
    log(Level::DEBUG, message, file, line);
}
void Logger::info(const std::string& message, const std::string& file, int line) {
    log(Level::INFO, message, file, line);
}
void Logger::warn(const std::string& message, const std::string& file, int line) {
    log(Level::WARN, message, file, line);
}
void Logger::error(const std::string& message, const std::string& file, int line) {
    log(Level::ERROR, message, file, line);
}
void Logger::fatal(const std::string& message, const std::string& file, int line) {
    log(Level::FATAL, message, file, line);
}

void Logger::log_impl(Level level, const std::string& message, const std::string& file, int line) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (level < level_) return;
    
    // 获取时间戳
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&tt), "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    
    // 添加级别
    const char* level_str = "";
    switch (level) {
        case Level::TRACE: level_str = "TRACE"; break;
        case Level::DEBUG: level_str = "DEBUG"; break;
        case Level::INFO:  level_str = "INFO";  break;
        case Level::WARN:  level_str = "WARN";  break;
        case Level::ERROR: level_str = "ERROR"; break;
        case Level::FATAL: level_str = "FATAL"; break;
    }
    oss << " [" << level_str << "]";
    
    // 添加位置信息
    if (!file.empty() && line > 0) {
        oss << " (" << file << ":" << line << ")";
    }
    
    oss << " " << message;
    
    std::string output = oss.str();
    
    // 输出到stdout
    if (output_stdout_) {
        if (level == Level::ERROR || level == Level::FATAL) {
            std::cerr << output << std::endl;
        } else {
            std::cout << output << std::endl;
        }
    }
    
    // 输出到文件
    if (!output_file_.empty()) {
        std::ofstream file(output_file_, std::ios::app);
        if (file) {
            file << output << std::endl;
        }
    }
}

// ==================== ErrorHandler 实现 ====================

ErrorHandler::ErrorHandler() = default;

ErrorHandler& ErrorHandler::instance() {
    static ErrorHandler instance_;
    return instance_;
}

ErrorInfo ErrorHandler::last_error() const {
    return last_error_;
}

ErrorInfo ErrorHandler::last_error(const std::string& context) const {
    ErrorInfo err = last_error_;
    if (!context.empty() && err.message.empty()) {
        err.message = context;
    }
    return err;
}

void ErrorHandler::set_error(ErrorCode code, const std::string& message) {
    last_error_ = {code, message, ""};
}

void ErrorHandler::set_error(ErrorCode code, const std::string& message, const std::string& detail) {
    last_error_ = {code, message, detail};
}

ErrorInfo ErrorHandler::system_error_to_error_info(int system_error_code) const {
    if (system_error_code == 0) {
        return ErrorInfo{ErrorCode::SUCCESS};
    }

#ifdef ENV_PLATFORM_WINDOWS
    char buffer[256];
    DWORD len = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, system_error_code, 0, buffer, sizeof(buffer), nullptr);
    if (len > 0) {
        return ErrorInfo{ErrorCode::IO_ERROR, buffer, std::to_string(system_error_code)};
    }
#else
    return ErrorInfo{ErrorCode::IO_ERROR, strerror(system_error_code), 
                    std::to_string(system_error_code)};
#endif
    
    return ErrorInfo{ErrorCode::UNKNOWN, "Unknown error", std::to_string(system_error_code)};
}

} // namespace env
} // namespace dbrecovery
