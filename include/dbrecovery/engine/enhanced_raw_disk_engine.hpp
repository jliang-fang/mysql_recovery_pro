/**
 * @file enhanced_raw_disk_engine.hpp
 * @brief Enhanced Raw Disk I/O Engine for MySQL 5.6 extreme recovery
 *
 * Provides advanced disk scanning capabilities:
 * - io_uring batch async reads for high throughput
 * - Streaming reads with memory-efficient callbacks
 * - Pinned buffer pool for forensic safety
 * - Memory-mapped disk image support
 *
 * @version 2.2.0
 */

#ifndef DBRECOVERY_ENGINE_ENHANCED_RAW_DISK_ENGINE_HPP
#define DBRECOVERY_ENGINE_ENHANCED_RAW_DISK_ENGINE_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <mutex>
#include <atomic>
#include <queue>

#include "dbrecovery/engine/raw_disk_engine.hpp"

#ifdef DBRECOVERY_HAVE_IO_URING
#include <liburing.h>
#endif

namespace dbrecovery {
namespace engine {

// ==================== Constants ====================

/// @brief Default io_uring queue depth
constexpr uint32_t DEFAULT_IO_URING_DEPTH = 64;

/// @brief Default streaming chunk size (4MB)
constexpr size_t DEFAULT_STREAM_CHUNK_SIZE = 4 * 1024 * 1024;

/// @brief Default pinned buffer pool size (256MB)
constexpr size_t DEFAULT_PINNED_POOL_SIZE = 256 * 1024 * 1024;

/// @brief Maximum batch read size
constexpr size_t MAX_BATCH_READ_SIZE = 16 * 1024 * 1024;

// ==================== Batch Read Result ====================

/**
 * @brief Result of a batch read operation
 */
struct BatchReadResult {
    uint64_t offset;              ///< Requested offset
    std::vector<uint8_t> data;    ///< Read data
    bool success;                 ///< Whether read succeeded
    int error_code;               ///< Error code (0 = success, errno otherwise)
    size_t bytes_read;            ///< Actual bytes read

    BatchReadResult() : offset(0), success(false), error_code(0), bytes_read(0) {}
};

// ==================== Enhanced Engine Configuration ====================

/**
 * @brief Configuration for EnhancedRawDiskEngine
 */
struct EnhancedDiskConfig {
    uint32_t io_uring_depth = DEFAULT_IO_URING_DEPTH;     ///< io_uring queue depth
    size_t stream_chunk_size = DEFAULT_STREAM_CHUNK_SIZE; ///< Streaming chunk size
    size_t pinned_pool_size = DEFAULT_PINNED_POOL_SIZE;   ///< Pinned buffer pool size
    bool use_huge_pages = false;                          ///< Use huge pages for buffers
    bool pin_memory = true;                               ///< mlock() buffers for forensic safety
    uint32_t readahead_size = 128 * 1024;                 ///< Readahead size (128KB)
    bool enable_stats = true;                             ///< Collect I/O statistics

    /**
     * @brief Validate configuration
     * @return true if valid
     */
    bool validate() const {
        return io_uring_depth > 0 &&
               stream_chunk_size >= 4096 &&
               pinned_pool_size >= 4096;
    }
};

// ==================== I/O Statistics ====================

/**
 * @brief I/O statistics for performance monitoring
 */
struct IOStats {
    std::atomic<uint64_t> total_reads{0};
    std::atomic<uint64_t> total_bytes_read{0};
    std::atomic<uint64_t> total_writes{0};
    std::atomic<uint64_t> total_bytes_written{0};
    std::atomic<uint64_t> read_errors{0};
    std::atomic<uint64_t> write_errors{0};
    std::atomic<uint64_t> cache_hits{0};
    std::atomic<uint64_t> cache_misses{0};

    double read_mb_per_second(double seconds) const {
        if (seconds <= 0) return 0.0;
        return (total_bytes_read.load() / (1024.0 * 1024.0)) / seconds;
    }

    void reset() {
        total_reads = 0;
        total_bytes_read = 0;
        total_writes = 0;
        total_bytes_written = 0;
        read_errors = 0;
        write_errors = 0;
        cache_hits = 0;
        cache_misses = 0;
    }
};

// ==================== Pinned Buffer Pool ====================

/**
 * @brief Pinned buffer pool for forensic-safe memory management
 *
 * Buffers are pinned (mlock) to prevent swapping during forensic operations.
 */
class PinnedBufferPool {
public:
    explicit PinnedBufferPool(size_t pool_size = DEFAULT_PINNED_POOL_SIZE);
    ~PinnedBufferPool();

    // Disable copy
    PinnedBufferPool(const PinnedBufferPool&) = delete;
    PinnedBufferPool& operator=(const PinnedBufferPool&) = delete;

    /**
     * @brief Allocate a buffer from the pool
     * @param size Required size
     * @param alignment Alignment requirement (default 4K)
     * @return Pointer to buffer, or nullptr if pool exhausted
     */
    uint8_t* allocate(size_t size, size_t alignment = 4096);

    /**
     * @brief Return buffer to pool
     * @param ptr Buffer pointer
     */
    void deallocate(uint8_t* ptr);

    /**
     * @brief Get pool statistics
     */
    struct PoolStats {
        size_t total_size;
        size_t used_size;
        size_t free_size;
        size_t num_allocations;
    };
    PoolStats get_stats() const;

    /**
     * @brief Check if buffer is from this pool
     */
    bool owns(const uint8_t* ptr) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ==================== Enhanced Raw Disk Engine ====================

/**
 * @brief Enhanced Raw Disk I/O Engine with advanced capabilities
 *
 * Extends RawDiskIOEngine with:
 * - Batch async reads using io_uring
 * - Memory-efficient streaming reads
 * - Pinned buffer pool for forensic safety
 * - Performance statistics
 */
class EnhancedRawDiskEngine : public RawDiskIOEngine {
public:
    /**
     * @brief Construct with configuration
     */
    explicit EnhancedRawDiskEngine(const EnhancedDiskConfig& config = {});

    /**
     * @brief Destructor
     */
    ~EnhancedRawDiskEngine();

    // Disable copy
    EnhancedRawDiskEngine(const EnhancedRawDiskEngine&) = delete;
    EnhancedRawDiskEngine& operator=(const EnhancedRawDiskEngine&) = delete;

    // ==================== Batch Async Operations ====================

    /**
     * @brief Read multiple regions asynchronously using io_uring
     *
     * @param regions Vector of (offset, size) pairs
     * @return Vector of read results
     *
     * @note Falls back to synchronous reads if io_uring unavailable
     */
    std::vector<BatchReadResult> batch_read_async(
        const std::vector<std::pair<uint64_t, size_t>>& regions);

    /**
     * @brief Read multiple regions with callback
     *
     * @param regions Vector of (offset, size) pairs
     * @param callback Called for each completed read
     * @return Number of successful reads
     */
    size_t batch_read_callback(
        const std::vector<std::pair<uint64_t, size_t>>& regions,
        std::function<void(uint64_t, const uint8_t*, size_t)> callback);

    // ==================== Streaming Operations ====================

    /**
     * @brief Stream read with memory-efficient callback
     *
     * Reads data in chunks and calls callback for each chunk.
     * Memory-efficient for scanning large disks.
     *
     * @param start Start offset
     * @param end End offset (exclusive)
     * @param callback Called for each chunk with (offset, data, size)
     * @param chunk_size Size of each chunk (default from config)
     * @return Total bytes read
     */
    uint64_t stream_read(
        uint64_t start,
        uint64_t end,
        std::function<void(uint64_t, const uint8_t*, size_t)> callback,
        size_t chunk_size = 0);

    /**
     * @brief Stream read with progress callback
     *
     * @param start Start offset
     * @param end End offset
     * @param data_callback Called for each chunk
     * @param progress_callback Called with progress percentage
     * @return Total bytes read
     */
    uint64_t stream_read_with_progress(
        uint64_t start,
        uint64_t end,
        std::function<void(uint64_t, const uint8_t*, size_t)> data_callback,
        std::function<void(double)> progress_callback);

    // ==================== Buffer Pool Access ====================

    /**
     * @brief Get the pinned buffer pool
     */
    PinnedBufferPool& get_buffer_pool() { return *buffer_pool_; }
    const PinnedBufferPool& get_buffer_pool() const { return *buffer_pool_; }

    /**
     * @brief Allocate a buffer from the pool
     */
    uint8_t* allocate_buffer(size_t size) {
        return buffer_pool_->allocate(size);
    }

    /**
     * @brief Return buffer to pool
     */
    void deallocate_buffer(uint8_t* ptr) {
        buffer_pool_->deallocate(ptr);
    }

    // ==================== Statistics ====================

    /**
     * @brief Get I/O statistics
     */
    const IOStats& get_stats() const { return stats_; }
    IOStats& get_stats() { return stats_; }

    /**
     * @brief Reset statistics
     */
    void reset_stats() { stats_.reset(); }

    // ==================== Configuration ====================

    /**
     * @brief Get current configuration
     */
    const EnhancedDiskConfig& get_config() const { return config_; }

    /**
     * @brief Check if io_uring is available
     */
    bool has_io_uring() const;

    /**
     * @brief Get optimal I/O alignment for current device
     */
    uint32_t get_optimal_alignment() const;

    /**
     * @brief Check if device supports direct I/O
     */
    bool supports_direct_io() const;

private:
    EnhancedDiskConfig config_;
    std::unique_ptr<PinnedBufferPool> buffer_pool_;
    IOStats stats_;

#ifdef DBRECOVERY_HAVE_IO_URING
    struct io_uring ring_;
    bool io_uring_initialized_ = false;

    bool init_io_uring_internal();
    void cleanup_io_uring_internal();
    std::vector<BatchReadResult> batch_read_io_uring(
        const std::vector<std::pair<uint64_t, size_t>>& regions);
#endif

    std::vector<BatchReadResult> batch_read_fallback(
        const std::vector<std::pair<uint64_t, size_t>>& regions);
};

// ==================== Disk Image Mapper ====================

/**
 * @brief Memory-mapped disk image for fast scanning
 *
 * Provides efficient random access to disk images using mmap.
 * Supports sequential access optimization via madvise.
 */
class DiskImageMapper {
public:
    /**
     * @brief Map mode
     */
    enum class MapMode {
        READ_ONLY,      ///< Read-only mapping
        READ_WRITE,     ///< Read-write mapping
        COPY_ON_WRITE   ///< Copy-on-write mapping
    };

    /**
     * @brief Construct and open disk image
     * @param path Path to disk image file
     * @param mode Mapping mode
     */
    explicit DiskImageMapper(const std::string& path, MapMode mode = MapMode::READ_ONLY);
    ~DiskImageMapper();

    // Disable copy
    DiskImageMapper(const DiskImageMapper&) = delete;
    DiskImageMapper& operator=(const DiskImageMapper&) = delete;

    /**
     * @brief Check if image is open
     */
    bool is_open() const { return fd_ >= 0; }

    /**
     * @brief Get total size of disk image
     */
    uint64_t size() const { return size_; }

    /**
     * @brief Map a region into memory
     *
     * @param offset Start offset
     * @param size Size of region
     * @return Pointer to mapped region, or nullptr on error
     */
    const uint8_t* map(uint64_t offset, size_t size);

    /**
     * @brief Map a region for read-write access
     */
    uint8_t* map_rw(uint64_t offset, size_t size);

    /**
     * @brief Unmap a region
     * @param ptr Pointer to mapped region
     * @param size Size of region
     */
    void unmap(const uint8_t* ptr, size_t size);

    /**
     * @brief Flush changes to disk
     */
    bool flush();

    /**
     * @brief Advise kernel about access pattern
     */
    void advise_sequential();
    void advise_random();
    void advise_will_need(uint64_t offset, size_t size);

    /**
     * @brief Get path to disk image
     */
    const std::string& path() const { return path_; }

    /**
     * @brief Get mapping statistics
     */
    struct MappingStats {
        size_t total_mappings;
        size_t active_mappings;
        size_t total_bytes_mapped;
    };
    MappingStats get_stats() const;

private:
    std::string path_;
    int fd_;
    uint64_t size_;
    MapMode mode_;

    struct Mapping {
        void* ptr;
        size_t size;
        uint64_t offset;
    };
    std::vector<Mapping> mappings_;
    mutable std::mutex mutex_;
};

// ==================== Streaming Scanner ====================

/**
 * @brief Streaming scanner for large disk images
 *
 * Scans disk images in a memory-efficient manner using streaming reads.
 * Suitable for scanning multi-TB disks with limited memory.
 */
class StreamingScanner {
public:
    /**
     * @brief Configuration
     */
    struct Config {
        size_t chunk_size = DEFAULT_STREAM_CHUNK_SIZE;  ///< Chunk size
        uint32_t num_buffers = 4;                        ///< Number of buffers to use
        bool use_io_uring = true;                        ///< Use io_uring if available
        bool prefetch = true;                            ///< Enable prefetching
    };

    explicit StreamingScanner(EnhancedRawDiskEngine& engine);
    ~StreamingScanner() = default;

    /**
     * @brief Scan a range with callback
     *
     * @param start Start offset
     * @param end End offset
     * @param callback Called for each chunk
     * @param progress Optional progress callback
     */
    void scan(
        uint64_t start,
        uint64_t end,
        std::function<void(uint64_t, const uint8_t*, size_t)> callback,
        std::function<void(double)> progress = nullptr);

    /**
     * @brief Scan entire device
     */
    void scan_all(
        std::function<void(uint64_t, const uint8_t*, size_t)> callback,
        std::function<void(double)> progress = nullptr);

    /**
     * @brief Get scan statistics
     */
    struct ScanStats {
        uint64_t bytes_scanned;
        uint64_t chunks_processed;
        double elapsed_seconds;
        double mb_per_second;
    };
    ScanStats get_stats() const { return stats_; }

private:
    EnhancedRawDiskEngine& engine_;
    Config config_;
    ScanStats stats_;
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_ENHANCED_RAW_DISK_ENGINE_HPP
