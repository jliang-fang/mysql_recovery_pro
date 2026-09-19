#ifndef DBRECOVERY_DISK_IO_URING_SCANNER_H
#define DBRECOVERY_DISK_IO_URING_SCANNER_H

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>

// Try to include liburing, fall back to kernel headers if not available
#ifdef __has_include
#if __has_include(<liburing.h>)
#include <liburing.h>
#define HAVE_LIBURING_H 1
#elif __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#include <linux/types.h>
#define USE_KERNEL_IO_URING 1
#endif
#endif

#ifndef HAVE_LIBURING_H
#ifndef USE_KERNEL_IO_URING
// Fallback: define minimal structures for compilation
struct io_uring_sqe {
    uint8_t opcode;
    uint8_t flags;
    uint16_t ioprio;
    int32_t fd;
    uint64_t off;
    uint64_t addr;
    uint32_t len;
    uint32_t flags1;
    uint32_t op_flags;
    uint32_t user_data;
    uint8_t buf[0];
};
struct io_uring_cqe {
    uint64_t user_data;
    int32_t res;
    uint32_t flags;
};
struct io_uring {
    uint32_t head;
    uint32_t tail;
    uint32_t ring_mask;
    uint32_t ring_entries;
    uint32_t flags;
    uint32_t sq_thread_idle;
    uint32_t features;
    uint32_t sq_off;
    uint32_t cq_off;
    void* sqe_array;
    void* ring_ptr;
    int sq_fd;
    int cq_fd;
    uint32_t sq_flags;
    uint32_t cq_flags;
};
#define io_uring_queue_init(depth, r, flags) (-1)
#define io_uring_queue_exit(r) do {} while(0)
#define io_uring_get_sqe(r) (nullptr)
#define io_uring_prep_read(sqe, fd, buf, len, offset) do {} while(0)
#define io_uring_sqe_set_data(sqe, data) do {} while(0)
#define io_uring_submit(r) (-1)
#define io_uring_wait_cqe(r, cqe) (-1)
#define io_uring_cqe_seen(r, cqe) do {} while(0)
#define io_uring_peek_cqe(r, cqe) (-1)
#define io_uring_queue_set_flags(r, flags) do {} while(0)
#endif
#endif

namespace dbrecovery {
namespace disk {

/**
 * @brief High-performance I/O scanner using io_uring
 * @details Uses Linux kernel's io_uring interface for asynchronous I/O
 *          with zero-copy capabilities and batch operations.
 */
class IOUringScanner {
public:
    /**
     * @brief Default constructor
     * @param queue_depth IO queue depth (default: 32)
     * @param flags io_uring setup flags
     */
    explicit IOUringScanner(uint32_t queue_depth = 32, uint32_t flags = 0);

    /**
     * @brief Destructor
     */
    ~IOUringScanner();

    // Disable copy
    IOUringScanner(const IOUringScanner&) = delete;
    IOUringScanner& operator=(const IOUringScanner&) = delete;

    // Enable move
    IOUringScanner(IOUringScanner&& other) noexcept;
    IOUringScanner& operator=(IOUringScanner&& other) noexcept;

    /**
     * @brief Open file/device for I/O
     * @param path Path to file or device
     * @param read_only Open in read-only mode
     * @return true if successful
     */
    bool open(const std::string& path, bool read_only = true);

    /**
     * @brief Transfer an already-open descriptor with caller-provided size.
     *
     * Ownership transfers regardless of success. The scanner closes owned_fd
     * if it cannot adopt it.
     */
    bool adopt_fd(int owned_fd, uint64_t known_size);

    /**
     * @brief Close the file/device
     */
    void close();

    /**
     * @brief Asynchronously read data from offset
     * @param offset File offset
     * @param buffer Buffer to read into
     * @param size Number of bytes to read
     * @param user_data User data to associate with this operation
     * @return 0 on success, negative on error
     */
    int read_async(uint64_t offset, uint8_t* buffer, size_t size, void* user_data = nullptr);

    /**
     * @brief Synchronous read (wrapper around async)
     * @param offset File offset
     * @param buffer Buffer to read into
     * @param size Number of bytes to read
     * @return Number of bytes read, or -1 on error
     */
    ssize_t read(uint64_t offset, uint8_t* buffer, size_t size);

    /**
     * @brief Submit pending I/O operations
     * @return Number of operations submitted
     */
    int submit();

    /**
     * @brief Wait for and complete I/O operations
     * @param min_complete Minimum number of completions to wait for
     * @param timeout_ms Timeout in milliseconds (0 = no timeout)
     * @return Number of completions received
     */
    int wait_and_complete(int min_complete = 1, int timeout_ms = 0);

    /**
     * @brief Get file size
     * @return File size in bytes
     */
    uint64_t get_size() const;

    /**
     * @brief Check if scanner is open
     * @return true if open
     */
    bool is_open() const;

    /**
     * @brief Get queue depth
     * @return Maximum number of in-flight operations
     */
    uint32_t get_queue_depth() const { return queue_depth_; }

    /**
     * @brief Set batch read size for optimized sequential reads
     * @param batch_size Batch size in bytes
     */
    void set_batch_size(size_t batch_size) { batch_size_ = batch_size; }

    /**
     * @brief Get batch read size
     * @return Batch size in bytes
     */
    size_t get_batch_size() const { return batch_size_; }

    /**
     * @brief Batch read multiple regions
     * @param regions Vector of (offset, buffer, size) tuples
     * @return Number of regions successfully queued
     */
    size_t batch_read(const std::vector<std::tuple<uint64_t, uint8_t*, size_t>>& regions);

    /**
     * @brief Get statistics
     */
    struct Stats {
        uint64_t bytes_read = 0;
        uint64_t read_count = 0;
        uint64_t submit_count = 0;
        uint64_t completion_count = 0;
        uint64_t errors = 0;
    };

    Stats get_stats() const;

    /**
     * @brief Reset statistics
     */
    void reset_stats();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    uint32_t queue_depth_;
    size_t batch_size_;
};

}  // namespace disk
}  // namespace dbrecovery

#endif  // DBRECOVERY_DISK_IO_URING_SCANNER_H
