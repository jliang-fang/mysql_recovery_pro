/**
 * @file platform/windows/file_system_windows.cpp
 * @brief Windows文件系统实现
 */

#include "dbrecovery/common/environment_compat.hpp"
#include <windows.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#include <cstring>
#include <cerrno>
#include <climits>
#include <fstream>
#include <filesystem>
#include <vector>
#include <psapi.h>

namespace dbrecovery {
namespace env {

// ==================== Windows文件系统实现 ====================

ErrorInfo FileSystem::file_exists_impl(const std::string& path) const {
    DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to get file attributes",
                        std::string("Error code: ") + std::to_string(GetLastError())};
    }
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::file_size_impl(const std::string& path, uint64_t& size) const {
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to open file",
                        std::string("Error code: ") + std::to_string(GetLastError())};
    }

    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li)) {
        CloseHandle(hFile);
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to get file size",
                        std::string("Error code: ") + std::to_string(GetLastError())};
    }

    size = static_cast<uint64_t>(li.QuadPart);
    CloseHandle(hFile);
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::file_info_impl(const std::string& path, FileInfo& info) const {
    info = FileInfo{};
    info.path = path;

    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to open file",
                        std::string("Error code: ") + std::to_string(GetLastError())};
    }

    FILETIME ftCreate, ftAccess, ftWrite;
    if (!GetFileTime(hFile, &ftCreate, &ftAccess, &ftWrite)) {
        CloseHandle(hFile);
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to get file time",
                        std::string("Error code: ") + std::to_string(GetLastError())};
    }

    // Convert FILETIME to uint64
    ULARGE_INTEGER li;
    li.LowPart = ftCreate.dwLowDateTime;
    li.HighPart = ftCreate.dwHighDateTime;
    info.creation_time = li.QuadPart;

    li.LowPart = ftAccess.dwLowDateTime;
    li.HighPart = ftAccess.dwHighDateTime;
    info.last_accessed = li.QuadPart;

    li.LowPart = ftWrite.dwLowDateTime;
    li.HighPart = ftWrite.dwHighDateTime;
    info.last_modified = li.QuadPart;

    // Get file size
    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile, &liSize)) {
        CloseHandle(hFile);
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to get file size",
                        std::string("Error code: ") + std::to_string(GetLastError())};
    }
    info.size = static_cast<uint64_t>(liSize.QuadPart);

    // Check if directory
    DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        info.is_directory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        info.is_regular_file = (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    CloseHandle(hFile);

    // 提取扩展名和文件名
    std::filesystem::path p(path);
    info.extension = p.extension().string();
    info.filename = p.filename().string();

    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::file_delete_impl(const std::string& path) const {
    if (!DeleteFileA(path.c_str())) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to delete file",
                        std::string("Error code: ") + std::to_string(GetLastError())};
    }
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::file_copy_impl(const std::string& src, const std::string& dst) const {
    if (!CopyFileA(src.c_str(), dst.c_str(), FALSE)) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to copy file",
                        std::string("Error code: ") + std::to_string(GetLastError())};
    }
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::file_move_impl(const std::string& src, const std::string& dst) const {
    if (!MoveFileExA(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to move file",
                        std::string("Error code: ") + std::to_string(GetLastError())};
    }
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::directory_create_impl(const std::string& path, bool recursive) const {
    if (recursive) {
        // Windows CreateDirectory doesn't support recursive creation
        // Use std::filesystem for recursive creation
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        if (ec) {
            return ErrorInfo{ErrorCode::IO_ERROR, "Failed to create directory", ec.message()};
        }
    } else {
        if (!CreateDirectoryA(path.c_str(), nullptr)) {
            return ErrorInfo{ErrorCode::IO_ERROR, "Failed to create directory",
                            std::string("Error code: ") + std::to_string(GetLastError())};
        }
    }
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::directory_remove_impl(const std::string& path, bool recursive) const {
    if (recursive) {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        if (ec) {
            return ErrorInfo{ErrorCode::IO_ERROR, "Failed to remove directory", ec.message()};
        }
    } else {
        if (!RemoveDirectoryA(path.c_str())) {
            return ErrorInfo{ErrorCode::IO_ERROR, "Failed to remove directory",
                            std::string("Error code: ") + std::to_string(GetLastError())};
        }
    }
    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::directory_list_impl(const std::string& path, std::vector<std::string>& files) const {
    std::string search_path = path;
    if (!search_path.empty() && search_path.back() != '\\' && search_path.back() != '/') {
        search_path += "\\*";
    } else {
        search_path += "*";
    }

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search_path.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to list directory",
                        std::string("Error code: ") + std::to_string(GetLastError())};
    }

    do {
        std::string name = fd.cFileName;
        if (name != "." && name != "..") {
            files.push_back(path + "\\" + name);
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return ErrorInfo{ErrorCode::SUCCESS};
}

// ==================== Windows内存管理实现 ====================

void* MemoryManager::allocate_impl(size_t size) {
    // 使用_aligned_malloc进行对齐分配
    void* ptr = _aligned_malloc(size, 64);
    return ptr;
}

void* MemoryManager::aligned_allocate_impl(size_t size, size_t alignment) {
    void* ptr = _aligned_malloc(size, alignment);
    return ptr;
}

void MemoryManager::deallocate_impl(void* ptr) {
    _aligned_free(ptr);
}

void MemoryManager::aligned_deallocate_impl(void* ptr) {
    _aligned_free(ptr);
}

// ==================== Windows线程管理实现 ====================

uint32_t ThreadManager::cpu_cores_impl() const {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<uint32_t>(si.dwNumberOfProcessors);
}

// ==================== Windows时间管理实现 ====================

uint64_t TimeManager::timestamp_ms_impl() const {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    
    ULARGE_INTEGER li;
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    
    // FILETIME is 100-nanosecond intervals since January 1, 1601 UTC
    // Convert to milliseconds
    return (li.QuadPart / 10000) - 11644473600000ULL;  // Subtract epoch difference
}

uint64_t TimeManager::timestamp_us_impl() const {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    
    ULARGE_INTEGER li;
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    
    // FILETIME is 100-nanosecond intervals since January 1, 1601 UTC
    // Convert to microseconds
    return (li.QuadPart / 10) - 11644473600000000ULL;
}

uint64_t TimeManager::timestamp_ns_impl() const {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    
    // Convert to nanoseconds (will be handled by frequency)
    return static_cast<uint64_t>(li.QuadPart);
}

uint64_t TimeManager::high_res_counter_impl() const {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart);
}

uint64_t TimeManager::high_res_frequency_impl() const {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return static_cast<uint64_t>(li.QuadPart);
}

// ==================== Windows系统信息实现 ====================

std::string SystemInfoGetter::os_name_impl() const {
    return "Windows";
}

std::string SystemInfoGetter::os_version_impl() const {
    OSVERSIONINFOEX osvi;
    ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    
    GetVersionEx(reinterpret_cast<OSVERSIONINFO*>(&osvi));
    
    std::string version = "Windows ";
    if (osvi.dwMajorVersion == 10 && osvi.dwMinorVersion == 0) {
        version += "10";
    } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 3) {
        version += "8.1";
    } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 2) {
        version += "10";  // Windows 10 preview
    } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 1) {
        version += "7";
    } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 0) {
        version += "Vista";
    } else if (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 1) {
        version += "XP";
    } else if (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 0) {
        version += "2000";
    } else {
        version += std::to_string(osvi.dwMajorVersion) + "." + std::to_string(osvi.dwMinorVersion);
    }
    
    return version;
}

uint64_t SystemInfoGetter::total_memory_impl() const {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&ms);
    return static_cast<uint64_t>(ms.ullTotalPhys);
}

uint64_t SystemInfoGetter::available_memory_impl() const {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&ms);
    return static_cast<uint64_t>(ms.ullAvailPhys);
}

} // namespace env
} // namespace dbrecovery
