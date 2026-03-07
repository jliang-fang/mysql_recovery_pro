#ifndef DBRECOVERY_DISK_RAW_SCANNER_H
#define DBRECOVERY_DISK_RAW_SCANNER_H

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace dbrecovery {
namespace disk {

class RawScanner {
public:
    RawScanner();
    ~RawScanner();

    // 打开原始磁盘设备或镜像文件
    bool open(const std::string& path);

    // 关闭当前打开的设备/文件
    void close();

    // 从指定偏移读取数据（返回实际读取字节数）
    size_t read(uint64_t offset, uint8_t* buffer, size_t size);

    // 获取磁盘大小（字节）
    uint64_t get_size() const;

    // 判断是否已打开有效设备/文件
    bool is_open() const;

private:
    class Impl {
    public:
        Impl() : fd_(-1), size_(0), is_open_(false) {}
        
        int fd_;
        uint64_t size_;
        bool is_open_;
        std::unique_ptr<std::vector<uint8_t>> buffer_; // 用于缓存读取数据
    };

    std::unique_ptr<Impl> impl_;
};

}  // namespace disk
}  // namespace dbrecovery

#endif  // DBRECOVERY_DISK_RAW_SCANNER_H
