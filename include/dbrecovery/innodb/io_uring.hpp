#ifndef DBRECOVERY_INNODB_IO_URING_HPP
#define DBRECOVERY_INNODB_IO_URING_HPP

#include <liburing.h>
#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <string>

namespace dbrecovery {
namespace innodb {

/**
 * @brief I/O 请求类型
 */
enum class IORequestType {
    READ = 0,
    WRITE = 1,
    FSYNC = 2,
    CLOSE = 3,
};

/**
 * @brief I/O 请求
 */
struct IORequest {
    IORequestType type;
    int fd;
    uint64_t offset;
    std::vector<uint8_t> buffer;
    std::function<void(int)> callback;
    void* user_data;
    
    std::string to_string() const;
};

/**
 * @brief I/O 完成事件
 */
struct IOCompletion {
    int res;              // 结果
    uint64_t user_data;   // 用户数据指针
    bool success;
    std::string error_message;
    
    std::string to_string() const;
};

/**
 * @brief 高性能 I/O 调度器 (基于 io_uring)
 */
class IOUringScheduler {
public:
    /**
     * @brief 构造函数
     * @param depth 队列深度
     * @param flags io_uring flags
     */
    IOUringScheduler(size_t depth = 128, unsigned int flags = 0);
    
    /**
     * @brief 析构函数
     */
    ~IOUringScheduler();
    
    /**
     * @brief 提交 I/O 请求
     */
    bool submit_request(const IORequest& request);
    
    /**
     * @brief 提交多个 I/O 请求
     */
    bool submit_requests(const std::vector<IORequest>& requests);
    
    /**
     * @brief 等待完成
     * @param min_completions 最小完成数
     * @param timeout_ms 超时时间 (毫秒)
     */
    std::vector<IOCompletion> wait_for_completion(
        size_t min_completions = 1,
        int timeout_ms = -1);
    
    /**
     * @brief 同步读取文件
     */
    std::optional<std::vector<uint8_t>> read_file(
        const std::string& path,
        size_t offset = 0,
        size_t size = 0);

    /**
     * @brief 从借用的文件描述符同步读取
     */
    std::optional<std::vector<uint8_t>> read_fd(
        int fd,
        size_t offset = 0,
        size_t size = 0,
        size_t known_size = 0);

    /**
     * @brief 异步读取文件
     */
    bool read_file_async(
        const std::string& path,
        size_t offset,
        size_t size,
        std::function<void(std::optional<std::vector<uint8_t>>)> callback);

    /**
     * @brief 从借用的文件描述符异步读取
     */
    bool read_fd_async(
        int fd,
        size_t offset,
        size_t size,
        size_t known_size,
        std::function<void(std::optional<std::vector<uint8_t>>)> callback);
    
    /**
     * @brief 同步写入文件
     */
    bool write_file(
        const std::string& path,
        const std::vector<uint8_t>& data,
        size_t offset = 0);
    
    /**
     * @brief 同步读取多个页
     */
    std::vector<std::optional<std::vector<uint8_t>>> read_pages(
        int fd,
        const std::vector<std::pair<size_t, size_t>>& page_offsets);
    
    /**
     * @brief 获取统计信息
     */
    struct Stats {
        size_t requests_submitted;
        size_t requests_completed;
        size_t bytes_read;
        size_t bytes_written;
        size_t errors;
    };
    
    Stats get_stats() const;
    
    /**
     * @brief 检查系统是否支持 io_uring
     */
    static bool is_supported();
    
private:
    struct io_uring ring_;
    size_t depth_;
    Stats stats_;
    
    int submit_one(int fd, const void* buf, size_t count, off64_t offset,
                   int opcode, void* user_data);
};

/**
 * @brief 文件读取器 (使用 io_uring)
 */
class IOUringFileReader {
public:
    /**
     * @brief 构造函数
     * @param path 文件路径
     * @param scheduler I/O 调度器
     */
    IOUringFileReader(const std::string& path, IOUringScheduler& scheduler);
    
    /**
     * @brief 析构函数
     */
    ~IOUringFileReader();
    
    /**
     * @brief 读取指定范围的数据
     */
    std::optional<std::vector<uint8_t>> read(size_t offset, size_t size);
    
    /**
     * @brief 异步读取
     */
    bool read_async(size_t offset, size_t size,
                   std::function<void(std::optional<std::vector<uint8_t>>)> callback);
    
    /**
     * @brief 读取整个文件
     */
    std::optional<std::vector<uint8_t>> read_all();
    
    /**
     * @brief 获取文件大小
     */
    size_t get_size() const;
    
    /**
     * @brief 获取页数量
     */
    size_t get_page_count(size_t page_size = 16384) const;
    
    /**
     * @brief 读取指定页
     */
    std::optional<std::vector<uint8_t>> read_page(size_t page_no,
                                                   size_t page_size = 16384);
    
    /**
     * @brief 批量读取页
     */
    std::vector<std::optional<std::vector<uint8_t>>> read_pages(
        const std::vector<size_t>& page_numbers,
        size_t page_size = 16384);
    
private:
    std::string path_;
    IOUringScheduler& scheduler_;
    int fd_;
    size_t size_;
};

// ==================== 实现 ====================

inline IOUringScheduler::Stats IOUringScheduler::get_stats() const {
    return stats_;
}

inline std::string IORequest::to_string() const {
    std::ostringstream oss;
    oss << "IORequest{"
        << "type=" << static_cast<int>(type)
        << ", fd=" << fd
        << ", offset=" << offset
        << ", size=" << buffer.size()
        << "}";
    return oss.str();
}

inline std::string IOCompletion::to_string() const {
    std::ostringstream oss;
    oss << "IOCompletion{"
        << "res=" << res
        << ", success=" << (success ? "true" : "false");
    if (!error_message.empty()) {
        oss << ", error=" << error_message;
    }
    oss << "}";
    return oss.str();
}

// ==================== IOUringScheduler ====================

inline IOUringScheduler::IOUringScheduler(size_t depth, unsigned int flags)
    : depth_(depth) {
    if (io_uring_queue_init(depth, &ring_, flags) < 0) {
        throw std::runtime_error("Failed to initialize io_uring");
    }
    stats_ = {0, 0, 0, 0, 0};
}

inline IOUringScheduler::~IOUringScheduler() {
    io_uring_queue_exit(&ring_);
}

inline bool IOUringScheduler::is_supported() {
    struct io_uring ring;
    int ret = io_uring_queue_init(8, &ring, 0);
    if (ret < 0) {
        return false;
    }
    io_uring_queue_exit(&ring);
    return true;
}

// ==================== IOUringFileReader ====================

inline IOUringFileReader::IOUringFileReader(const std::string& path,
                                            IOUringScheduler& scheduler)
    : path_(path), scheduler_(scheduler), fd_(-1), size_(0) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_DIRECT);
    if (fd_ < 0) {
        throw std::runtime_error("Failed to open file: " + path);
    }
    
    // 获取文件大小
    struct stat st;
    if (fstat(fd_, &st) == 0) {
        size_ = st.st_size;
    }
}

inline IOUringFileReader::~IOUringFileReader() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

inline size_t IOUringFileReader::get_size() const {
    return size_;
}

inline size_t IOUringFileReader::get_page_count(size_t page_size) const {
    return (size_ + page_size - 1) / page_size;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_IO_URING_HPP
