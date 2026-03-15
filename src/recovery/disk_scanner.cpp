#include "dbrecovery/disk/raw_scanner.hpp"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace dbrecovery {
namespace disk {

RawScanner::RawScanner() : impl_(std::make_unique<Impl>()) {}

RawScanner::~RawScanner() { close(); }

bool RawScanner::open(const std::string& path) {
    if (impl_->is_open_) {
        close();
    }
    
#ifdef _WIN32
    HANDLE handle = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, 
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle, &size)) {
        CloseHandle(handle);
        return false;
    }
    
    impl_->fd_ = -1; // Windows doesn't use fd
    impl_->size_ = static_cast<uint64_t>(size.QuadPart);
    impl_->is_open_ = true;
    impl_->handle_ = handle;
#else
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return false;
    }
    
    impl_->fd_ = fd;
    impl_->size_ = static_cast<uint64_t>(st.st_size);
    impl_->is_open_ = true;
#endif
    
    return true;
}

void RawScanner::close() {
    if (!impl_->is_open_) {
        return;
    }
    
#ifdef _WIN32
    if (impl_->handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->handle_);
        impl_->handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (impl_->fd_ >= 0) {
        close(impl_->fd_);
        impl_->fd_ = -1;
    }
#endif
    
    impl_->size_ = 0;
    impl_->is_open_ = false;
}

size_t RawScanner::read(uint64_t offset, uint8_t* buffer, size_t size) {
    if (!impl_->is_open_ || !buffer || size == 0) {
        return 0;
    }
    
#ifdef _WIN32
    if (offset >= impl_->size_) {
        return 0;
    }
    
    DWORD bytes_read;
    LARGE_INTEGER offset_li;
    offset_li.QuadPart = static_cast<LONGLONG>(offset);
    
    OVERLAPPED overlapped = {0};
    overlapped.Offset = offset_li.LowPart;
    overlapped.OffsetHigh = offset_li.HighPart;
    
    if (!ReadFile(impl_->handle_, buffer, static_cast<DWORD>(size), &bytes_read, &overlapped)) {
        return 0;
    }
    
    return bytes_read;
#else
    if (offset >= impl_->size_) {
        return 0;
    }
    
    ssize_t bytes_read = pread(impl_->fd_, buffer, size, static_cast<off_t>(offset));
    if (bytes_read < 0) {
        return 0;
    }
    
    return static_cast<size_t>(bytes_read);
#endif
}

uint64_t RawScanner::get_size() const {
    return impl_->size_;
}

bool RawScanner::is_open() const {
    return impl_->is_open_;
}

} // namespace disk
} // namespace dbrecovery
