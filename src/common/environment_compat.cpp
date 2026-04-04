/**
 * @file environment_compat.cpp
 * @brief 多环境兼容性层实现
 */

#include "dbrecovery/common/environment_compat.hpp"
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <mutex>

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

// ==================== FileSystem 实现 ====================

FileSystem::FileSystem() = default;

FileSystem& FileSystem::instance() {
    static FileSystem instance_;
    return instance_;
}

ErrorInfo FileSystem::file_exists(const std::string& path) const {
    return file_exists_impl(path);
}

ErrorInfo FileSystem::file_size(const std::string& path, uint64_t& size) const {
    return file_size_impl(path, size);
}

ErrorInfo FileSystem::file_info(const std::string& path, FileInfo& info) const {
    return file_info_impl(path, info);
}

ErrorInfo FileSystem::file_delete(const std::string& path) const {
    return file_delete_impl(path);
}

ErrorInfo FileSystem::file_copy(const std::string& src, const std::string& dst) const {
    return file_copy_impl(src, dst);
}

ErrorInfo FileSystem::file_move(const std::string& src, const std::string& dst) const {
    return file_move_impl(src, dst);
}

ErrorInfo FileSystem::directory_exists(const std::string& path) const {
    FileInfo info;
    auto result = file_info_impl(path, info);
    if (result.is_success()) {
        return info.is_directory ? ErrorInfo{ErrorCode::SUCCESS} : 
                                   ErrorInfo{ErrorCode::INVALID_ARGUMENT, "Path is not a directory"};
    }
    return result;
}

ErrorInfo FileSystem::directory_create(const std::string& path, bool recursive) const {
    return directory_create_impl(path, recursive);
}

ErrorInfo FileSystem::directory_remove(const std::string& path, bool recursive) const {
    return directory_remove_impl(path, recursive);
}

ErrorInfo FileSystem::directory_list(const std::string& path, std::vector<std::string>& files) const {
    return directory_list_impl(path, files);
}

ErrorInfo FileSystem::temp_directory(std::string& path) const {
#ifdef ENV_PLATFORM_WINDOWS
    char temp_path[MAX_PATH];
    DWORD result = GetTempPathA(MAX_PATH, temp_path);
    if (result > 0 && result < MAX_PATH) {
        path = temp_path;
        // 移除尾部反斜杠
        if (!path.empty() && path.back() == '\\') {
            path.pop_back();
        }
        return ErrorInfo{ErrorCode::SUCCESS};
    }
    return ErrorInfo{ErrorCode::IO_ERROR, "Failed to get temp directory"};
#else
    const char* tmpdir = std::getenv("TMPDIR");
    if (tmpdir) {
        path = tmpdir;
        return ErrorInfo{ErrorCode::SUCCESS};
    }
    tmpdir = std::getenv("TMP");
    if (tmpdir) {
        path = tmpdir;
        return ErrorInfo{ErrorCode::SUCCESS};
    }
    path = "/tmp";
    return ErrorInfo{ErrorCode::SUCCESS};
#endif
}

ErrorInfo FileSystem::create_temp_file(std::string& path) const {
    std::string temp_dir;
    auto dir_result = temp_directory(temp_dir);
    if (dir_result.is_error()) {
        return dir_result;
    }

#ifdef ENV_PLATFORM_WINDOWS
    char temp_filename[MAX_PATH];
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    GetTempFileNameA(temp_path, "DBR", 0, temp_filename);
    path = temp_filename;
    
    // 创建文件
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, 
                               CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to create temp file", 
                        std::to_string(GetLastError())};
    }
    CloseHandle(hFile);
#else
    std::string template_str = temp_dir + "/dbr_XXXXXX";
    char* buffer = new char[template_str.size() + 1];
    std::strcpy(buffer, template_str.c_str());
    
    int fd = mkstemp(buffer);
    if (fd == -1) {
        delete[] buffer;
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to create temp file",
                        std::string(strerror(errno))};
    }
    
    path = buffer;
    delete[] buffer;
    close(fd);
#endif

    return ErrorInfo{ErrorCode::SUCCESS};
}

std::string FileSystem::path_join(const std::string& base, const std::string& relative) const {
    if (base.empty()) return relative;
    if (relative.empty()) return base;
    
    char last_char = base.back();
    if (last_char == '/' || last_char == '\\') {
        return base + relative;
    }
    return base + ENV_PATH_SEPARATOR + relative;
}

std::string FileSystem::path_normalize(const std::string& path) const {
    std::filesystem::path p(path);
    return p.make_preferred().string();
}

std::string FileSystem::path_extension(const std::string& path) const {
    std::filesystem::path p(path);
    return p.extension().string();
}

std::string FileSystem::path_filename(const std::string& path) const {
    std::filesystem::path p(path);
    return p.filename().string();
}

std::string FileSystem::path_basename(const std::string& path) const {
    std::filesystem::path p(path);
    return p.stem().string();
}

// 平台特定实现
ErrorInfo FileSystem::file_exists_impl(const std::string& path) const {
#ifdef ENV_PLATFORM_WINDOWS
    DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to get file attributes",
                        std::to_string(GetLastError())};
    }
    return ErrorInfo{ErrorCode::SUCCESS};
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to stat file",
                        std::string(strerror(errno))};
    }
    return ErrorInfo{ErrorCode::SUCCESS};
#endif
}

ErrorInfo FileSystem::file_size_impl(const std::string& path, uint64_t& size) const {
#ifdef ENV_PLATFORM_WINDOWS
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return ErrorInfo{ErrorCode::FILE_NOT_FOUND, "Failed to open file",
                        std::to_string(GetLastError())};
    }

    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile, &liSize)) {
        CloseHandle(hFile);
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to get file size",
                        std::to_string(GetLastError())};
    }

    size = static_cast<uint64_t>(liSize.QuadPart);
    CloseHandle(hFile);
    return ErrorInfo{ErrorCode::SUCCESS};
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to stat file",
                        std::string(strerror(errno))};
    }
    size = static_cast<uint64_t>(st.st_size);
    return ErrorInfo{ErrorCode::SUCCESS};
#endif
}

ErrorInfo FileSystem::file_info_impl(const std::string& path, FileInfo& info) const {
    info = FileInfo{};
    info.path = path;

#ifdef ENV_PLATFORM_WINDOWS
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return ErrorInfo{ErrorCode::FILE_NOT_FOUND, "Failed to open file",
                        std::to_string(GetLastError())};
    }

    // 获取文件大小
    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile, &liSize)) {
        CloseHandle(hFile);
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to get file size",
                        std::to_string(GetLastError())};
    }
    info.size = static_cast<uint64_t>(liSize.QuadPart);

    // 获取文件时间
    FILETIME ct, at, mt;
    if (GetFileTime(hFile, &ct, &at, &mt)) {
        // 转换为 Unix timestamp
        ULARGE_INTEGER uli;
        uli.LowPart = mt.dwLowDateTime;
        uli.HighPart = mt.dwHighDateTime;
        info.last_modified = (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;
        
        uli.LowPart = at.dwLowDateTime;
        uli.HighPart = at.dwHighDateTime;
        info.last_accessed = (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;
        
        uli.LowPart = ct.dwLowDateTime;
        uli.HighPart = ct.dwHighDateTime;
        info.creation_time = (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;
    }

    // 获取文件属性
    DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        info.is_directory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        info.is_regular_file = (attrs & FILE_ATTRIBUTE_NORMAL) != 0 ||
                              (attrs & FILE_ATTRIBUTE_ARCHIVE) != 0;
    }

    CloseHandle(hFile);
#else
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
#endif

    return ErrorInfo{ErrorCode::SUCCESS};
}

ErrorInfo FileSystem::file_delete_impl(const std::string& path) const {
#ifdef ENV_PLATFORM_WINDOWS
    if (!DeleteFileA(path.c_str())) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to delete file",
                        std::to_string(GetLastError())};
    }
#else
    if (unlink(path.c_str()) != 0) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to delete file",
                        std::string(strerror(errno))};
    }
#endif
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
#ifdef ENV_PLATFORM_WINDOWS
    if (!MoveFileExA(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to move file",
                        std::to_string(GetLastError())};
    }
#else
    if (rename(src.c_str(), dst.c_str()) != 0) {
        return ErrorInfo{ErrorCode::IO_ERROR, "Failed to move file",
                        std::string(strerror(errno))};
    }
#endif
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

} // namespace env
} // namespace dbrecovery
