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
    : fd_(-1), data_(nullptr), size_(0), is_valid_(false) {
    
#ifdef _WIN32
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return;
    }

    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile, &liSize)) {
        CloseHandle(hFile);
        return;
    }
    
    size_ = static_cast<size_t>(liSize.QuadPart);
    if (size_ == 0) {
        CloseHandle(hFile);
        return;
    }

    HANDLE hMapping = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMapping) {
        CloseHandle(hFile);
        return;
    }

    data_ = static_cast<uint8_t*>(MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(hMapping); // 映射成功后可关闭句柄
    
    if (!data_) {
        CloseHandle(hFile);
        return;
    }
    
    // 保存文件句柄用于后续清理
    fd_ = reinterpret_cast<int>(hFile);
    is_valid_ = true;
#else
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        return;
    }

    struct stat st;
    if (fstat(fd_, &st) != 0) {
        close(fd_);
        fd_ = -1;
        return;
    }
    
    size_ = static_cast<size_t>(st.st_size);
    if (size_ == 0) {
        close(fd_);
        fd_ = -1;
        return;
    }

    data_ = static_cast<uint8_t*>(mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0));
    if (data_ == MAP_FAILED) {
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
        UnmapViewOfFile(data_);
        data_ = nullptr;
    }
    if (fd_ != -1) {
        CloseHandle(reinterpret_cast<HANDLE>(fd_));
        fd_ = -1;
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
}

bool FileMapper::is_valid() const { 
    return is_valid_; 
}

const uint8_t* FileMapper::data() const { 
    return data_; 
}

size_t FileMapper::size() const { 
    return size_; 
}

} // namespace utils
} // namespace dbrecovery
