#include "dbrecovery/utils/file_mapper.hpp"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

namespace dbrecovery {
namespace utils {

FileMapper::FileMapper(const std::string& path) 
#ifdef _WIN32
    : fd_(-1), data_(nullptr), size_(0), is_valid_(false), handle_(INVALID_HANDLE_VALUE)
#else
    : fd_(-1), data_(nullptr), size_(0), is_valid_(false)
#endif
{
    
#ifdef _WIN32
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "[ERROR] Failed to open file: " << path << ", error: " << GetLastError() << std::endl;
        return;
    }

    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile, &liSize)) {
        std::cerr << "[ERROR] Failed to get file size: " << path << ", error: " << GetLastError() << std::endl;
        CloseHandle(hFile);
        return;
    }
    
    size_ = static_cast<size_t>(liSize.QuadPart);
    if (size_ == 0) {
        std::cerr << "[ERROR] File is empty: " << path << std::endl;
        CloseHandle(hFile);
        return;
    }

    HANDLE hMapping = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMapping) {
        std::cerr << "[ERROR] Failed to create file mapping: " << path << ", error: " << GetLastError() << std::endl;
        CloseHandle(hFile);
        return;
    }

    data_ = static_cast<uint8_t*>(MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0));
    
    // 映射成功后关闭文件映射对象句柄（文件句柄需要保留）
    CloseHandle(hMapping);
    
    if (!data_) {
        std::cerr << "[ERROR] Failed to map view of file: " << path << ", error: " << GetLastError() << std::endl;
        CloseHandle(hFile);
        return;
    }
    
    // 保存文件句柄用于后续清理
    handle_ = hFile;
    is_valid_ = true;
#else
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        std::cerr << "[ERROR] Failed to open file: " << path << ", error: " << strerror(errno) << std::endl;
        return;
    }

    struct stat st;
    if (fstat(fd_, &st) != 0) {
        std::cerr << "[ERROR] Failed to fstat file: " << path << ", error: " << strerror(errno) << std::endl;
        close(fd_);
        fd_ = -1;
        return;
    }
    
    size_ = static_cast<size_t>(st.st_size);
    if (size_ == 0) {
        std::cerr << "[ERROR] File is empty: " << path << std::endl;
        close(fd_);
        fd_ = -1;
        return;
    }

    data_ = static_cast<uint8_t*>(mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0));
    if (data_ == MAP_FAILED) {
        std::cerr << "[ERROR] Failed to mmap file: " << path << ", error: " << strerror(errno) << std::endl;
        close(fd_);
        fd_ = -1;
        return;
    }
    
    is_valid_ = true;
#endif
}

FileMapper::~FileMapper() {
#ifdef _WIN32
    if (data_) {
        if (!UnmapViewOfFile(data_)) {
            std::cerr << "[ERROR] Failed to unmap view of file: " << GetLastError() << std::endl;
        }
        data_ = nullptr;
    }
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (data_ && data_ != MAP_FAILED) {
        if (munmap(data_, size_) != 0) {
            std::cerr << "[ERROR] Failed to unmap memory: " << strerror(errno) << std::endl;
        }
        data_ = nullptr;
    }
    
    if (fd_ >= 0) {
        if (close(fd_) != 0) {
            std::cerr << "[ERROR] Failed to close file descriptor: " << strerror(errno) << std::endl;
        }
        fd_ = -1;
    }
#endif
}

FileMapper::FileMapper(FileMapper&& other) noexcept 
    : fd_(other.fd_), data_(other.data_), size_(other.size_), is_valid_(other.is_valid_)
#ifdef _WIN32
    , handle_(INVALID_HANDLE_VALUE)
#endif
{
    // 转移所有权：先保存源对象的值，再清空源对象
    other.fd_ = -1;
    other.data_ = nullptr;
    other.size_ = 0;
    other.is_valid_ = false;
    
#ifdef _WIN32
    handle_ = other.handle_;
    other.handle_ = INVALID_HANDLE_VALUE;
#endif
}

FileMapper& FileMapper::operator=(FileMapper&& other) noexcept {
    if (this != &other) {
        // 清理当前资源（仅在当前对象有效时）
#ifdef _WIN32
        if (data_ && data_ != nullptr) {
            UnmapViewOfFile(data_);
            data_ = nullptr;
        }
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (data_ && data_ != MAP_FAILED) {
            munmap(data_, size_);
            data_ = nullptr;
        }
        
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
#endif
        
        // 移动资源（直接赋值，不关闭源对象）
        fd_ = other.fd_;
        data_ = other.data_;
        size_ = other.size_;
        is_valid_ = other.is_valid_;
        
#ifdef _WIN32
        handle_ = other.handle_;
        if (data_ == nullptr) {
            is_valid_ = false;
        }
#else
        if (data_ == MAP_FAILED) {
            data_ = nullptr;
            is_valid_ = false;
        }
#endif
        
        // 清空源对象（仅重置为初始状态，不关闭资源）
        other.fd_ = -1;
        other.data_ = nullptr;
        other.size_ = 0;
        other.is_valid_ = false;
#ifdef _WIN32
        other.handle_ = INVALID_HANDLE_VALUE;
#else
        other.data_ = nullptr;
#endif
    }
    
    return *this;
}

} // namespace utils
} // namespace dbrecovery
