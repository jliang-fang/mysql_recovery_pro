/**
 * @file platform/linux/file_system_linux.cpp
 * @brief Linux文件系统实现
 */

#include "dbrecovery/common/environment_compat.hpp"
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysinfo.h>
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

namespace dbrecovery {
namespace env {

// ==================== Linux文件系统实现 ====================

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
    info.creation_time = static_cast<uint64_t>(st.st_mtime);  // Linux没有创建时间
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

// ==================== Linux内存管理实现 ====================

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

// ==================== Linux线程管理实现 ====================

uint32_t ThreadManager::cpu_cores_impl() const {
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) return static_cast<uint32_t>(cores);
    return 1;
}

// ==================== Linux时间管理实现 ====================

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

// ==================== Linux系统信息实现 ====================

std::string SystemInfoGetter::os_name_impl() const {
    return "Linux";
}

std::string SystemInfoGetter::os_version_impl() const {
    std::ifstream release("/etc/os-release");
    std::string line;
    while (std::getline(release, line)) {
        if (line.find("PRETTY_NAME=") == 0) {
            return line.substr(12, line.size() - 13);
        }
    }
    return "Linux";
}

uint64_t SystemInfoGetter::total_memory_impl() const {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        return static_cast<uint64_t>(si.totalram) * si.mem_unit;
    }
    
    // Fallback: 使用sysconf
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<uint64_t>(pages) * page_size;
    }
    return 0;
}

uint64_t SystemInfoGetter::available_memory_impl() const {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        return static_cast<uint64_t>(si.freeram) * si.mem_unit;
    }
    
    // Fallback: 使用sysconf
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<uint64_t>(pages) * page_size;
    }
    return 0;
}

} // namespace env
} // namespace dbrecovery
