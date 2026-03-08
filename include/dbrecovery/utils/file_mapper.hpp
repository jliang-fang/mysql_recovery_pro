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

    // 禁止拷贝
    FileMapper(const FileMapper&) = delete;
    FileMapper& operator=(const FileMapper&) = delete;

    bool is_valid() const { return is_valid_; }
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

#ifdef _WIN32
    HANDLE handle_;
#endif
    int fd_;
    uint8_t* data_;
    size_t size_;
    bool is_valid_;
};

} // namespace utils
} // namespace dbrecovery

#endif // DBRECOVERY_UTILS_FILE_MAPPER_HPP
