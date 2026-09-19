#ifndef DBRECOVERY_DISK_RAW_SCANNER_H
#define DBRECOVERY_DISK_RAW_SCANNER_H

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

#ifdef HAVE_IO_URING
#include "dbrecovery/disk/io_uring_scanner.hpp"
#endif

namespace dbrecovery {
namespace disk {

/**
 * @brief Raw disk scanner with io_uring support
 * @details Automatically uses io_uring when available, falls back to POSIX I/O
 */
class RawScanner {
public:
    RawScanner();
    ~RawScanner();

    // 打开原始磁盘设备或镜像文件
    bool open(const std::string& path);

    // 移交一个已验证的只读文件描述符；无论成功或失败均由本对象负责关闭。
    bool adopt_fd(int owned_fd, uint64_t known_size);

    // 关闭当前打开的设备/文件
    void close();

    // 从指定偏移读取数据（返回实际读取字节数）
    size_t read(uint64_t offset, uint8_t* buffer, size_t size);

    // 获取磁盘大小（字节）
    uint64_t get_size() const;

    // 判断是否已打开有效设备/文件
    bool is_open() const;

#ifdef HAVE_IO_URING
    // Get underlying io_uring scanner (if available)
    IOUringScanner* get_io_uring_scanner() { return io_uring_scanner_.get(); }
#endif

private:
    class Impl {
    public:
        Impl() : fd_(-1), size_(0), is_open_(false) {}

        int fd_;
        uint64_t size_;
        bool is_open_;
    };

    std::unique_ptr<Impl> impl_;

#ifdef HAVE_IO_URING
    std::unique_ptr<IOUringScanner> io_uring_scanner_;
#endif
};

}  // namespace disk
}  // namespace dbrecovery

#endif  // DBRECOVERY_DISK_RAW_SCANNER_H
