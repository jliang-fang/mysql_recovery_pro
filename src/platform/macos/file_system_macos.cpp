/**
 * @file platform/macos/file_system_macos.cpp
 * @brief macOS文件系统实现
 */

#include "dbrecovery/common/environment_compat.hpp"
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <cstring>
#include <cerrno>
#include <climits>
#include <fstream>
#include <filesystem>
#include <vector>
#include <mach/mach.h>
#include <sys/sysctl.h>

namespace dbrecovery {
namespace env {

// ==================== macOS文件系统实现 ====================

ErrorInfo FileSystem::file_exists_impl(const std::string& path) const {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to stat file",
                        std::string(strerror(errno))};
    }
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::file_size_impl(const std::string& path, uint64_t& size) const {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to stat file",
                        std::string(strerror(errno))};
    }
    size = static_cast<uint64_t>(st.st_size);
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::file_info_impl(const std::string& path, FileInfo& info) const {
    info = FileInfo{};
    info.path = path;

    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to stat file",
                        std::string(strerror(errno))};
    }

    info.size = static_cast<uint64_t>(st.st_size);
    info.last_modified = static_cast<uint64_t>(st.st_mtime);
    info.last_accessed = static_cast<uint64_t>(st.st_atime);
    info.creation_time = static_cast<uint64_t>(st.st_birthtime);  // macOS有创建时间
    info.is_directory = S_ISDIR(st.st_mode);
    info.is_regular_file = S_ISREG(st.st_mode);

    // 提取扩展名和文件名
    std::filesystem::path p(path);
    info.extension = p.extension().string();
    info.filename = p.filename().string();

    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::file_delete_impl(const std::string& path) const {
    if (unlink(path.c_str()) != 0) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to delete file",
                        std::string(strerror(errno))};
    }
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::file_copy_impl(const std::string& src, const std::string& dst) const {
    // 使用标准库进行文件复制
    std::error_code ec;
    std::filesystem::copy_file(src, dst,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to copy file", ec.message()};
    }
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::file_move_impl(const std::string& src, const std::string& dst) const {
    if (rename(src.c_str(), dst.c_str()) != 0) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to move file",
                        std::string(strerror(errno))};
    }
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::directory_create_impl(const std::string& path, bool recursive) const {
    std::error_code ec;
    if (recursive) {
        std::filesystem::create_directories(path, ec);
    } else {
        std::filesystem::create_directory(path, ec);
    }
    if (ec) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to create directory", ec.message()};
    }
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::directory_remove_impl(const std::string& path, bool recursive) const {
    std::error_code ec;
    if (recursive) {
        std::filesystem::remove_all(path, ec);
    } else {
        std::filesystem::remove(path, ec);
    }
    if (ec) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to remove directory", ec.message()};
    }
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::directory_list_impl(const std::string& path, std::vector<std::string>& files) const {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
        if (!ec) {
            files.push_back(entry.path().string());
        }
    }
    if (ec) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to list directory", ec.message()};
    }
    return ErrorInfo{ErrorCode::SUCCESS};
}

// ==================== macOS内存管理实现 ====================

void* MemoryManager::allocate_impl(size_t size) {
    // 使用posix_memalign进行对齐分配
    void* ptr = nullptr;
    if (posix_memalign(&ptr, 64, size) == 0) {
        return ptr;
    }
    return nullptr;
}

void* MemoryManager::aligned_allocate_impl(size_t size, size_t alignment) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) == 0) {
        return ptr;
    }
    return nullptr;
}

void MemoryManager::deallocate_impl(void* ptr) {
    std::free(ptr);
}

void MemoryManager::aligned_deallocate_impl(void* ptr) {
    std::free(ptr);
}

// ==================== macOS线程管理实现 ====================

uint32_t ThreadManager::cpu_cores_impl() const {
    int mib[2] = {CTL_HW, HW_NCPU};
    int cores = 0;
    size_t len = sizeof(cores);
    sysctl(mib, 2, &cores, &len, nullptr, 0);
    if (cores > 0) return static_cast<uint32_t>(cores);
    return 1;
}

// ==================== macOS时间管理实现 ====================

uint64_t TimeManager::timestamp_ms_impl() const {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

uint64_t TimeManager::timestamp_us_impl() const {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}

uint64_t TimeManager::timestamp_ns_impl() const {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

uint64_t TimeManager::high_res_counter_impl() const {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

uint64_t TimeManager::high_res_frequency_impl() const {
    return 1000000000ULL;  // 1 GHz for nanosecond resolution
}

// ==================== macOS系统信息实现 ====================

std::string SystemInfoGetter::os_name_impl() const {
    return "macOS";
}

std::string SystemInfoGetter::os_version_impl() const {
    char version[256];
    size_t len = sizeof(version);
    int mib[2] = {CTL_KERN, KERN_OSRELEASE};
    sysctl(mib, 2, version, &len, nullptr, 0);
    return std::string(version, len);
}

uint64_t SystemInfoGetter::total_memory_impl() const {
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    uint64_t memsize = 0;
    size_t len = sizeof(memsize);
    sysctl(mib, 2, &memsize, &len, nullptr, 0);
    return memsize;
}

uint64_t SystemInfoGetter::available_memory_impl() const {
    mach_port_t host_port = mach_host_self();
    mach_msg_type_number_t count = HOST_VM_INFO_COUNT;
    vm_statistics64_data_t vm_stats;
    
    kern_return_t kr = host_statistics64(host_port, HOST_VM_INFO,
                                        reinterpret_cast<host_info_t>(&vm_stats),
                                        &count);
    if (kr != KERN_SUCCESS) {
        return 0;
    }

    uint64_t page_size = vm_page_size;
    uint64_t available = static_cast<uint64_t>(vm_stats.free_count) * page_size;
    return available;
}

} // namespace env
} // namespace dbrecovery
