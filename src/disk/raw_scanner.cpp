#include "dbrecovery/disk/raw_scanner.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <stdexcept>

#ifdef HAVE_IO_URING
#include "dbrecovery/disk/io_uring_scanner.hpp"
#endif

namespace dbrecovery {
namespace disk {

// ==================== RawScanner 实现 ====================

RawScanner::RawScanner() : impl_(std::make_unique<Impl>()) {
#ifdef HAVE_IO_URING
    io_uring_scanner_ = std::make_unique<IOUringScanner>();
#endif
}

RawScanner::~RawScanner() {
    close();
}

bool RawScanner::open(const std::string& path) {
    if (impl_->is_open_) {
        close();
    }

#ifdef HAVE_IO_URING
    // Try io_uring first
    if (io_uring_scanner_->open(path, true)) {
        impl_->fd_ = -1;  // io_uring manages its own fd
        impl_->size_ = io_uring_scanner_->get_size();
        impl_->is_open_ = true;
        return true;
    }
#endif

    // Fallback to POSIX I/O
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
    if (fd < 0) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }
    }

    // Get file size
    struct stat st;
    if (fstat(fd, &st) == 0) {
        impl_->size_ = st.st_size;
    }

    impl_->fd_ = fd;
    impl_->is_open_ = true;
    return true;
}

void RawScanner::close() {
    if (impl_->is_open_) {
#ifdef HAVE_IO_URING
        if (io_uring_scanner_) {
            io_uring_scanner_->close();
        }
#endif
        if (impl_->fd_ >= 0) {
            ::close(impl_->fd_);
            impl_->fd_ = -1;
        }
        impl_->size_ = 0;
        impl_->is_open_ = false;
    }
}

size_t RawScanner::read(uint64_t offset, uint8_t* buffer, size_t size) {
    if (!impl_->is_open_) {
        return 0;
    }

#ifdef HAVE_IO_URING
    if (io_uring_scanner_ && io_uring_scanner_->is_open()) {
        ssize_t result = io_uring_scanner_->read(offset, buffer, size);
        if (result >= 0) {
            return static_cast<size_t>(result);
        }
        return 0;
    }
#endif

    // Fallback to POSIX read
    if (impl_->fd_ < 0) {
        return 0;
    }

    ssize_t result = ::pread(impl_->fd_, buffer, size, offset);
    if (result >= 0) {
        return static_cast<size_t>(result);
    }
    return 0;
}

uint64_t RawScanner::get_size() const {
    return impl_->size_;
}

bool RawScanner::is_open() const {
    return impl_->is_open_;
}

}  // namespace disk
}  // namespace dbrecovery
