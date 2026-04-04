#include "dbrecovery/disk/io_uring_scanner.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cerrno>
#include <chrono>

#ifdef HAVE_LIBURING_H
#include <liburing.h>
#elif defined(USE_KERNEL_IO_URING)
#include <sys/syscall.h>
#include <linux/types.h>
#include <stdint.h>
#endif

namespace dbrecovery {
namespace disk {

#ifdef HAVE_LIBURING_H

// ==================== liburing 实现 ====================

class IOUringScanner::Impl {
public:
    Impl(uint32_t queue_depth)
        : ring_(), fd_(-1), size_(0), is_open_(false),
          queue_depth_(queue_depth), stats_{} {
        memset(&ring_, 0, sizeof(ring_));
    }

    ~Impl() {
        close();
    }

    bool open(const std::string& path, bool read_only) {
        if (is_open_) {
            close();
        }

        int flags = read_only ? O_RDONLY : O_RDWR;
        fd_ = ::open(path.c_str(), flags);
        if (fd_ < 0) {
            return false;
        }

        struct stat st;
        if (fstat(fd_, &st) == 0) {
            size_ = st.st_size;
        }

        // Initialize io_uring
        int ret = io_uring_queue_init(queue_depth_, &ring_, 0);
        if (ret < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        is_open_ = true;
        return true;
    }

    void close() {
        if (is_open_) {
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
            }
            if (ring_.sqe_array) {
                io_uring_queue_exit(&ring_);
                memset(&ring_, 0, sizeof(ring_));
            }
            is_open_ = false;
        }
    }

    int read_async(uint64_t offset, uint8_t* buffer, size_t size, void* user_data = nullptr) {
        if (!is_open_ || fd_ < 0) {
            return -EBADF;
        }

        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            return -EAGAIN;
        }

        io_uring_prep_read(
            sqe, fd_, reinterpret_cast<char*>(buffer), size, offset);
        io_uring_sqe_set_data(sqe, user_data);

        stats_.submit_count++;
        return 0;
    }

    ssize_t read(uint64_t offset, uint8_t* buffer, size_t size) {
        if (!is_open_ || fd_ < 0) {
            return -EBADF;
        }

        struct io_uring_cqe* cqe;
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        
        if (!sqe) {
            // Fallback to sync read if no SQE available
            ssize_t result = ::pread(fd_, buffer, size, offset);
            if (result >= 0) {
                stats_.bytes_read += result;
                stats_.read_count++;
                stats_.completion_count++;
            }
            return result;
        }

        io_uring_prep_read(sqe, fd_, reinterpret_cast<char*>(buffer), size, offset);
        io_uring_sqe_set_data(sqe, nullptr);

        int ret = io_uring_submit(&ring_);
        if (ret < 0) {
            return ret;
        }

        stats_.submit_count++;

        ret = io_uring_wait_cqe(&ring_, &cqe);
        if (ret < 0) {
            return ret;
        }

        ssize_t result = cqe->res;
        io_uring_cqe_seen(&ring_, cqe);

        if (result >= 0) {
            stats_.bytes_read += result;
            stats_.read_count++;
            stats_.completion_count++;
        } else {
            stats_.errors++;
        }
        return result;
    }

    int submit() {
        if (!is_open_) {
            return -EBADF;
        }
        return io_uring_submit(&ring_);
    }

    int wait_and_complete(int min_complete, int timeout_ms) {
        if (!is_open_) {
            return -EBADF;
        }

        int completed = 0;
        struct io_uring_cqe* cqe;
        int ret;

        do {
            int to_wait = min_complete - completed;
            if (to_wait <= 0) {
                break;
            }

            if (timeout_ms > 0) {
                struct __kernel_timespec ts;
                ts.tv_sec = timeout_ms / 1000;
                ts.tv_nsec = (timeout_ms % 1000) * 1000000;
                ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
            } else {
                ret = io_uring_wait_cqe(&ring_, &cqe);
            }

            if (ret == 0) {
                completed++;
                stats_.completion_count++;
                
                if (cqe->res >= 0) {
                    stats_.bytes_read += cqe->res;
                    stats_.read_count++;
                } else {
                    stats_.errors++;
                }
                
                io_uring_cqe_seen(&ring_, cqe);
            }
        } while (completed < min_complete);

        return completed;
    }

    uint64_t get_size() const { return size_; }
    bool is_open() const { return is_open_; }
    uint32_t get_queue_depth() const { return queue_depth_; }

    Stats get_stats() const { return stats_; }
    void reset_stats() { stats_ = Stats{}; }

    size_t batch_read(const std::vector<std::tuple<uint64_t, uint8_t*, size_t>>& regions) {
        size_t queued = 0;

        for (const auto& region : regions) {
            uint64_t offset = std::get<0>(region);
            uint8_t* buffer = std::get<1>(region);
            size_t size = std::get<2>(region);

            if (read_async(offset, buffer, size, nullptr) == 0) {
                queued++;
            }
        }

        if (queued > 0) {
            submit();
        }

        return queued;
    }

private:
    struct io_uring ring_;
    int fd_;
    uint64_t size_;
    bool is_open_;
    uint32_t queue_depth_;
    Stats stats_;
};

#else

// ==================== Fallback 实现 (使用 pread) ====================

class IOUringScanner::Impl {
public:
    Impl(uint32_t queue_depth)
        : fd_(-1), size_(0), is_open_(false),
          queue_depth_(queue_depth), stats_{} {
    }

    ~Impl() {
        close();
    }

    bool open(const std::string& path, bool read_only) {
        if (is_open_) {
            close();
        }

        int flags = read_only ? O_RDONLY : O_RDWR;
        fd_ = ::open(path.c_str(), flags);
        if (fd_ < 0) {
            return false;
        }

        struct stat st;
        if (fstat(fd_, &st) == 0) {
            size_ = st.st_size;
        }

        is_open_ = true;
        return true;
    }

    void close() {
        if (is_open_) {
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
            }
            is_open_ = false;
        }
    }

    int read_async(uint64_t offset, uint8_t* buffer, size_t size, void* user_data = nullptr) {
        (void)user_data;
        if (!is_open_ || fd_ < 0) {
            return -EBADF;
        }

        ssize_t result = ::pread(fd_, buffer, size, offset);
        if (result >= 0) {
            stats_.bytes_read += result;
            stats_.read_count++;
            stats_.completion_count++;
        } else {
            stats_.errors++;
        }
        return result >= 0 ? 0 : -1;
    }

    ssize_t read(uint64_t offset, uint8_t* buffer, size_t size) {
        ssize_t result = ::pread(fd_, buffer, size, offset);
        if (result >= 0) {
            stats_.bytes_read += result;
            stats_.read_count++;
            stats_.completion_count++;
        } else {
            stats_.errors++;
        }
        return result;
    }

    int submit() {
        return 0;
    }

    int wait_and_complete(int min_complete, int timeout_ms) {
        (void)timeout_ms;
        return min_complete;
    }

    uint64_t get_size() const { return size_; }
    bool is_open() const { return is_open_; }
    uint32_t get_queue_depth() const { return queue_depth_; }

    Stats get_stats() const { return stats_; }
    void reset_stats() { stats_ = Stats{}; }

    size_t batch_read(const std::vector<std::tuple<uint64_t, uint8_t*, size_t>>& regions) {
        size_t queued = 0;

        for (const auto& region : regions) {
            uint64_t offset = std::get<0>(region);
            uint8_t* buffer = std::get<1>(region);
            size_t size = std::get<2>(region);

            if (read_async(offset, buffer, size, nullptr) == 0) {
                queued++;
            }
        }

        return queued;
    }

private:
    int fd_;
    uint64_t size_;
    bool is_open_;
    uint32_t queue_depth_;
    Stats stats_;
};

#endif

// ==================== IOUringScanner 公共接口 ====================

IOUringScanner::IOUringScanner(uint32_t queue_depth, uint32_t flags)
    : impl_(std::make_unique<Impl>(queue_depth)),
      queue_depth_(queue_depth),
      batch_size_(65536) {
}

IOUringScanner::~IOUringScanner() {
    close();
}

IOUringScanner::IOUringScanner(IOUringScanner&& other) noexcept
    : impl_(std::move(other.impl_)),
      queue_depth_(other.queue_depth_),
      batch_size_(other.batch_size_) {
    other.queue_depth_ = 32;
    other.batch_size_ = 65536;
}

IOUringScanner& IOUringScanner::operator=(IOUringScanner&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
        queue_depth_ = other.queue_depth_;
        batch_size_ = other.batch_size_;
        other.queue_depth_ = 32;
        other.batch_size_ = 65536;
    }
    return *this;
}

bool IOUringScanner::open(const std::string& path, bool read_only) {
    return impl_->open(path, read_only);
}

void IOUringScanner::close() {
    if (impl_) {
        impl_->close();
    }
}

int IOUringScanner::read_async(uint64_t offset, uint8_t* buffer, size_t size, void* user_data) {
    return impl_->read_async(offset, buffer, size, user_data);
}

ssize_t IOUringScanner::read(uint64_t offset, uint8_t* buffer, size_t size) {
    return impl_->read(offset, buffer, size);
}

int IOUringScanner::submit() {
    return impl_->submit();
}

int IOUringScanner::wait_and_complete(int min_complete, int timeout_ms) {
    return impl_->wait_and_complete(min_complete, timeout_ms);
}

uint64_t IOUringScanner::get_size() const {
    return impl_->get_size();
}

bool IOUringScanner::is_open() const {
    return impl_ && impl_->is_open();
}

size_t IOUringScanner::batch_read(const std::vector<std::tuple<uint64_t, uint8_t*, size_t>>& regions) {
    return impl_->batch_read(regions);
}

IOUringScanner::Stats IOUringScanner::get_stats() const {
    return impl_->get_stats();
}

void IOUringScanner::reset_stats() {
    if (impl_) {
        impl_->reset_stats();
    }
}

}  // namespace disk
}  // namespace dbrecovery
