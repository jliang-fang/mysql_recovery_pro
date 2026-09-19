#include "dbrecovery/innodb/io_uring.hpp"
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <sys/stat.h>

namespace dbrecovery {
namespace innodb {

// ==================== IOUringScheduler ====================

int IOUringScheduler::submit_one(int fd, const void* buf, size_t count,
                                  off64_t offset, int opcode, void* user_data) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        return -1;
    }
    
    io_uring_prep_rw(opcode, sqe, fd, buf, count, offset);
    sqe->user_data = (uint64_t)user_data;
    
    int ret = io_uring_submit(&ring_);
    if (ret < 0) {
        return ret;
    }
    
    stats_.requests_submitted++;
    return 0;
}

bool IOUringScheduler::submit_request(const IORequest& request) {
    int opcode;
    switch (request.type) {
        case IORequestType::READ:
            opcode = IORING_OP_READ;
            break;
        case IORequestType::WRITE:
            opcode = IORING_OP_WRITE;
            break;
        case IORequestType::FSYNC:
            opcode = IORING_OP_FSYNC;
            break;
        case IORequestType::CLOSE:
            opcode = IORING_OP_CLOSE;
            break;
        default:
            return false;
    }
    
    int ret = submit_one(request.fd, request.buffer.data(),
                         request.buffer.size(), request.offset,
                         opcode, request.user_data);
    return ret == 0;
}

bool IOUringScheduler::submit_requests(const std::vector<IORequest>& requests) {
    for (const auto& req : requests) {
        if (!submit_request(req)) {
            return false;
        }
    }
    return true;
}

std::vector<IOCompletion> IOUringScheduler::wait_for_completion(
    size_t min_completions, int timeout_ms) {
    std::vector<IOCompletion> completions;
    
    struct io_uring_cqe* cqe;
    unsigned head;
    int ret;
    
    // 设置超时
    if (timeout_ms >= 0) {
        struct __kernel_timespec ts;
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        ret = io_uring_submit_and_wait_timeout(&ring_, &cqe, min_completions,
                                                &ts, nullptr);
    } else {
        ret = io_uring_submit_and_wait(&ring_, &cqe, min_completions);
    }
    
    if (ret < 0) {
        stats_.errors++;
        return completions;
    }
    
    io_uring_for_each_cqe(&ring_, head, cqe) {
        IOCompletion comp;
        comp.res = cqe->res;
        comp.user_data = cqe->user_data;
        comp.success = (cqe->res >= 0);
        
        if (!comp.success) {
            comp.error_message = strerror(-cqe->res);
            stats_.errors++;
        }
        
        completions.push_back(comp);
        stats_.requests_completed++;
        
        if (cqe->res > 0 && cqe->opcode == IORING_OP_READ) {
            stats_.bytes_read += cqe->res;
        } else if (cqe->res > 0 && cqe->opcode == IORING_OP_WRITE) {
            stats_.bytes_written += cqe->res;
        }
    }
    
    io_uring_cq_advance(&ring_, completions.size());
    return completions;
}

std::optional<std::vector<uint8_t>> IOUringScheduler::read_file(
    const std::string& path, size_t offset, size_t size) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::nullopt;
    }

    auto result = read_fd(fd, offset, size);
    ::close(fd);
    return result;
}

std::optional<std::vector<uint8_t>> IOUringScheduler::read_fd(
    int fd, size_t offset, size_t size, size_t known_size) {
    if (fd < 0) {
        return std::nullopt;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        return std::nullopt;
    }

    const size_t file_size = known_size > 0 ? known_size : static_cast<size_t>(st.st_size);
    if (offset > file_size) {
        return std::nullopt;
    }

    if (size == 0) {
        size = file_size - offset;
    }
    if (size > file_size - offset) {
        size = file_size - offset;
    }

    std::vector<uint8_t> buffer(size);
    const ssize_t bytes_read = ::pread(fd, buffer.data(), size, offset);
    if (bytes_read < 0) {
        return std::nullopt;
    }

    buffer.resize(static_cast<size_t>(bytes_read));
    stats_.bytes_read += static_cast<size_t>(bytes_read);
    stats_.requests_completed++;
    return buffer;
}

bool IOUringScheduler::read_file_async(
    const std::string& path, size_t offset, size_t size,
    std::function<void(std::optional<std::vector<uint8_t>>)> callback) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        callback(std::nullopt);
        return false;
    }

    const bool result = read_fd_async(fd, offset, size, 0, std::move(callback));
    ::close(fd);
    return result;
}

bool IOUringScheduler::read_fd_async(
    int fd, size_t offset, size_t size, size_t known_size,
    std::function<void(std::optional<std::vector<uint8_t>>)> callback) {
    if (fd < 0) {
        callback(std::nullopt);
        return false;
    }

    auto result = read_fd(fd, offset, size, known_size);
    callback(std::move(result));
    return true;
}

bool IOUringScheduler::write_file(const std::string& path,
                                   const std::vector<uint8_t>& data,
                                   size_t offset) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    
    ssize_t bytes_written = pwrite(fd, data.data(), data.size(), offset);
    ::close(fd);
    
    if (bytes_written < 0) {
        return false;
    }
    
    stats_.bytes_written += bytes_written;
    stats_.requests_completed++;
    
    return true;
}

std::vector<std::optional<std::vector<uint8_t>>> IOUringScheduler::read_pages(
    int fd, const std::vector<std::pair<size_t, size_t>>& page_offsets) {
    std::vector<std::optional<std::vector<uint8_t>>> results;
    
    for (const auto& [page_no, page_size] : page_offsets) {
        size_t offset = page_no * page_size;
        
        auto buffer = std::make_shared<std::vector<uint8_t>>(page_size);
        
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            results.push_back(std::nullopt);
            continue;
        }
        
        io_uring_prep_read(sqe, fd, buffer->data(), page_size, offset);
        sqe->user_data = (uint64_t)buffer.get();
        
        stats_.requests_submitted++;
        
        // 等待完成
        struct io_uring_cqe* cqe;
        int ret = io_uring_submit_and_wait(&ring_, &cqe, 1);
        
        if (ret < 0) {
            results.push_back(std::nullopt);
            continue;
        }
        
        io_uring_for_each_cqe(&ring_, head, cqe) {
            if (cqe->res >= 0) {
                buffer->resize(cqe->res);
                stats_.bytes_read += cqe->res;
                results.push_back(buffer);
            } else {
                results.push_back(std::nullopt);
                stats_.errors++;
            }
            stats_.requests_completed++;
        }
        
        io_uring_cq_advance(&ring_, 1);
    }
    
    return results;
}

// ==================== IOUringFileReader ====================

std::optional<std::vector<uint8_t>> IOUringFileReader::read(size_t offset,
                                                             size_t size) {
    return scheduler_.read_fd(fd_, offset, size, size_);
}

bool IOUringFileReader::read_async(size_t offset, size_t size,
                                    std::function<void(std::optional<std::vector<uint8_t>>)> callback) {
    return scheduler_.read_fd_async(fd_, offset, size, size_, std::move(callback));
}

std::optional<std::vector<uint8_t>> IOUringFileReader::read_all() {
    return scheduler_.read_fd(fd_, 0, size_, size_);
}

std::optional<std::vector<uint8_t>> IOUringFileReader::read_page(size_t page_no,
                                                                   size_t page_size) {
    return read(page_no * page_size, page_size);
}

std::vector<std::optional<std::vector<uint8_t>>> IOUringFileReader::read_pages(
    const std::vector<size_t>& page_numbers, size_t page_size) {
    std::vector<std::optional<std::vector<uint8_t>>> results;
    
    for (size_t page_no : page_numbers) {
        results.push_back(read_page(page_no, page_size));
    }
    
    return results;
}

} // namespace innodb
} // namespace dbrecovery
