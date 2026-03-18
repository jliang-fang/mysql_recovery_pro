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
// ✅ 修复：将移动构造函数和赋值运算符的实现移至 file_mapper.cpp 中，避免头文件中定义导致的多重定义错误
// 此处仅保留声明，具体实现在 .cpp 文件中

} // namespace utils
} // namespace dbrecovery

#endif // DBRECOVERY_UTILS_FILE_MAPPER_HPP
