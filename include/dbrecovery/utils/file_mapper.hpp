#ifndef DBRECOVERY_UTILS_FILE_MAPPER_HPP
#define DBRECOVERY_UTILS_FILE_MAPPER_HPP

#include <string>
#include <cstdint>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace dbrecovery {
namespace utils {

class FileMapper {
public:
    explicit FileMapper(const std::string& path);
    ~FileMapper();
    
    // 支持移动语义
    FileMapper(FileMapper&& other) noexcept;
    FileMapper& operator=(FileMapper&& other) noexcept;

    // 禁止拷贝
    FileMapper(const FileMapper&) = delete;
    FileMapper& operator=(const FileMapper&) = delete;

    bool is_valid() const { return is_valid_; }
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

private:
    int fd_ = -1;
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool is_valid_ = false;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

// ==================== 移动语义实现（必须在类外，但需要访问私有成员）====================
inline FileMapper::FileMapper(FileMapper&& other) noexcept 
    : fd_(other.fd_), data_(other.data_), size_(other.size_), is_valid_(other.is_valid_)
#ifdef _WIN32
      , handle_(other.handle_)
#endif
{
    // 转移所有权：清空源对象
    other.fd_ = -1;
    other.data_ = nullptr;
    other.size_ = 0;
    other.is_valid_ = false;
#ifdef _WIN32
    other.handle_ = INVALID_HANDLE_VALUE;
#endif
}

inline FileMapper& FileMapper::operator=(FileMapper&& other) noexcept {
    if (this != &other) {
        // 清理当前资源
#ifdef _WIN32
        if (data_) {
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
        
        // 移动资源
        fd_ = other.fd_;
        data_ = other.data_;
        size_ = other.size_;
        is_valid_ = other.is_valid_;
        
#ifdef _WIN32
        handle_ = other.handle_;
#else
        if (data_ == MAP_FAILED) {
            data_ = nullptr;
            is_valid_ = false;
        }
#endif
        
        // 清空源对象
        other.fd_ = -1;
        other.data_ = nullptr;
        other.size_ = 0;
        other.is_valid_ = false;
#ifdef _WIN32
        other.handle_ = INVALID_HANDLE_VALUE;
#else
        other.data_ = static_cast<uint8_t*>(MAP_FAILED); // 确保其他文件描述符的正确清理
#endif
    }
    
    return *this;
}

} // namespace utils
} // namespace dbrecovery

#endif // DBRECOVERY_UTILS_FILE_MAPPER_HPP
