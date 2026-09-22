/**
 * @file enhanced_raw_disk_engine.cpp
 * @brief Implementation of Enhanced Raw Disk I/O Engine
 *
 * @version 2.2.0
 */

#include "dbrecovery/engine/enhanced_raw_disk_engine.hpp"

#include <cstring>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <system_error>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

namespace dbrecovery {
namespace engine {

// ==================== PinnedBufferPool Implementation ====================

class PinnedBufferPool::Impl {
public:
    explicit Impl(size_t pool_size)
        : pool_size_(pool_size), used_size_(0), num_allocations_(0) {
        // Allocate the pool
        pool_memory_ = static_cast<uint8_t*>(
            aligned_alloc(4096, pool_size));

        if (!pool_memory_) {
            throw std::runtime_error("Failed to allocate buffer pool");
        }

        // Pin memory to prevent swapping (forensic safety)
        if (mlock(pool_memory_, pool_size_) != 0) {
            // Non-fatal: continue without pinning
            perror("mlock failed (non-fatal)");
        }

        // Initialize free list with one large block
        free_blocks_.push_back({0, pool_size_});
    }

    ~Impl() {
        if (pool_memory_) {
            munlock(pool_memory_, pool_size_);
            free(pool_memory_);
        }
    }

    uint8_t* allocate(size_t size, size_t alignment) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Align size
        size = (size + alignment - 1) & ~(alignment - 1);

        // Find best-fit block
        auto best = free_blocks_.end();
        for (auto it = free_blocks_.begin(); it != free_blocks_.end(); ++it) {
            if (it->size >= size) {
                if (best == free_blocks_.end() || it->size < best->size) {
                    best = it;
                }
            }
        }

        if (best == free_blocks_.end()) {
            return nullptr; // No suitable block found
        }

        // Allocate from this block
        size_t offset = best->offset;
        allocated_blocks_[pool_memory_ + offset] = size;
        used_size_ += size;
        num_allocations_++;

        if (best->size == size) {
            // Exact fit
            free_blocks_.erase(best);
        } else {
            // Split block
            best->offset += size;
            best->size -= size;
        }

        return pool_memory_ + offset;
    }

    void deallocate(uint8_t* ptr) {
        if (!ptr || ptr < pool_memory_ || ptr >= pool_memory_ + pool_size_) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        auto it = allocated_blocks_.find(ptr);
        if (it == allocated_blocks_.end()) {
            return; // Not allocated from this pool
        }

        size_t offset = ptr - pool_memory_;
        size_t size = it->second;

        allocated_blocks_.erase(it);
        used_size_ -= size;
        num_allocations_--;

        // Add back to free list and coalesce
        free_blocks_.push_back({offset, size});
        coalesce_free_blocks();
    }

    PoolStats get_stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {pool_size_, used_size_, pool_size_ - used_size_, num_allocations_};
    }

    bool owns(const uint8_t* ptr) const {
        return ptr >= pool_memory_ && ptr < pool_memory_ + pool_size_;
    }

private:
    struct FreeBlock {
        size_t offset;
        size_t size;
    };

    void coalesce_free_blocks() {
        if (free_blocks_.size() <= 1) return;

        // Sort by offset
        std::sort(free_blocks_.begin(), free_blocks_.end(),
                  [](const FreeBlock& a, const FreeBlock& b) {
                      return a.offset < b.offset;
                  });

        // Merge adjacent blocks
        std::vector<FreeBlock> merged;
        merged.push_back(free_blocks_[0]);

        for (size_t i = 1; i < free_blocks_.size(); ++i) {
            auto& last = merged.back();
            if (last.offset + last.size == free_blocks_[i].offset) {
                last.size += free_blocks_[i].size;
            } else {
                merged.push_back(free_blocks_[i]);
            }
        }

        free_blocks_ = std::move(merged);
    }

    uint8_t* pool_memory_;
    size_t pool_size_;
    size_t used_size_;
    size_t num_allocations_;
    std::vector<FreeBlock> free_blocks_;
    std::unordered_map<uint8_t*, size_t> allocated_blocks_;
    mutable std::mutex mutex_;
};

PinnedBufferPool::PinnedBufferPool(size_t pool_size)
    : impl_(std::make_unique<Impl>(pool_size)) {}

PinnedBufferPool::~PinnedBufferPool() = default;

uint8_t* PinnedBufferPool::allocate(size_t size, size_t alignment) {
    return impl_->allocate(size, alignment);
}

void PinnedBufferPool::deallocate(uint8_t* ptr) {
    impl_->deallocate(ptr);
}

PinnedBufferPool::PoolStats PinnedBufferPool::get_stats() const {
    return impl_->get_stats();
}

bool PinnedBufferPool::owns(const uint8_t* ptr) const {
    return impl_->owns(ptr);
}

// ==================== EnhancedRawDiskEngine Implementation ====================

EnhancedRawDiskEngine::EnhancedRawDiskEngine(const EnhancedDiskConfig& config)
    : config_(config)
    , buffer_pool_(std::make_unique<PinnedBufferPool>(config.pinned_pool_size)) {

    if (!config_.validate()) {
        throw std::invalid_argument("Invalid configuration");
    }

#ifdef DBRECOVERY_HAVE_IO_URING
    if (config_.io_uring_depth > 0) {
        io_uring_initialized_ = init_io_uring_internal();
    }
#endif
}

EnhancedRawDiskEngine::~EnhancedRawDiskEngine() {
#ifdef DBRECOVERY_HAVE_IO_URING
    if (io_uring_initialized_) {
        cleanup_io_uring_internal();
    }
#endif
}

#ifdef DBRECOVERY_HAVE_IO_URING
bool EnhancedRawDiskEngine::init_io_uring_internal() {
    struct io_uring_params params = {};
    params.flags = IORING_SETUP_IOPOLL; // Use polling for lower latency

    int ret = io_uring_queue_init_params(config_.io_uring_depth, &ring_, &params);
    if (ret < 0) {
        return false;
    }

    return true;
}

void EnhancedRawDiskEngine::cleanup_io_uring_internal() {
    io_uring_queue_exit(&ring_);
    io_uring_initialized_ = false;
}

std::vector<BatchReadResult> EnhancedRawDiskEngine::batch_read_io_uring(
    const std::vector<std::pair<uint64_t, size_t>>& regions) {

    std::vector<BatchReadResult> results(regions.size());
    std::vector<uint8_t*> buffers(regions.size());

    // Allocate aligned buffers
    for (size_t i = 0; i < regions.size(); ++i) {
        buffers[i] = static_cast<uint8_t*>(
            aligned_alloc(4096, regions[i].second));
        if (!buffers[i]) {
            // Allocation failed, use fallback
            for (size_t j = 0; j < i; ++j) {
                free(buffers[j]);
            }
            return batch_read_fallback(regions);
        }
    }

    // Submit all reads
    size_t submitted = 0;
    for (size_t i = 0; i < regions.size(); ++i) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) break;

        io_uring_prep_read(sqe, get_fd(), buffers[i], regions[i].second, regions[i].first);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(i));
        submitted++;
    }

    io_uring_submit(&ring_);

    // Wait for completions
    for (size_t i = 0; i < submitted; ++i) {
        struct io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe(&ring_, &cqe);

        if (ret < 0 || !cqe) {
            stats_.read_errors++;
            continue;
        }

        size_t idx = reinterpret_cast<size_t>(cqe->user_data);
        results[idx].offset = regions[idx].first;

        if (cqe->res >= 0) {
            results[idx].success = true;
            results[idx].bytes_read = static_cast<size_t>(cqe->res);
            results[idx].data.assign(buffers[idx], buffers[idx] + cqe->res);
            stats_.total_reads++;
            stats_.total_bytes_read += cqe->res;
        } else {
            results[idx].success = false;
            results[idx].error_code = -cqe->res;
            stats_.read_errors++;
        }

        io_uring_cqe_seen(&ring_, cqe);
    }

    // Free buffers
    for (auto* buf : buffers) {
        if (buf) free(buf);
    }

    return results;
}
#endif

std::vector<BatchReadResult> EnhancedRawDiskEngine::batch_read_fallback(
    const std::vector<std::pair<uint64_t, size_t>>& regions) {

    std::vector<BatchReadResult> results(regions.size());

    for (size_t i = 0; i < regions.size(); ++i) {
        auto& result = results[i];
        result.offset = regions[i].first;
        result.data.resize(regions[i].second);

        size_t bytes_read = read(regions[i].first, result.data.data(), regions[i].second);

        if (bytes_read > 0) {
            result.success = true;
            result.bytes_read = bytes_read;
            result.data.resize(bytes_read);
            stats_.total_reads++;
            stats_.total_bytes_read += bytes_read;
        } else {
            result.success = false;
            result.error_code = errno;
            stats_.read_errors++;
        }
    }

    return results;
}

std::vector<BatchReadResult> EnhancedRawDiskEngine::batch_read_async(
    const std::vector<std::pair<uint64_t, size_t>>& regions) {

    if (!is_open()) {
        return {};
    }

#ifdef DBRECOVERY_HAVE_IO_URING
    if (io_uring_initialized_) {
        return batch_read_io_uring(regions);
    }
#endif

    return batch_read_fallback(regions);
}

size_t EnhancedRawDiskEngine::batch_read_callback(
    const std::vector<std::pair<uint64_t, size_t>>& regions,
    std::function<void(uint64_t, const uint8_t*, size_t)> callback) {

    if (!callback) return 0;

    auto results = batch_read_async(regions);
    size_t success_count = 0;

    for (const auto& result : results) {
        if (result.success && !result.data.empty()) {
            callback(result.offset, result.data.data(), result.bytes_read);
            success_count++;
        }
    }

    return success_count;
}

uint64_t EnhancedRawDiskEngine::stream_read(
    uint64_t start,
    uint64_t end,
    std::function<void(uint64_t, const uint8_t*, size_t)> callback,
    size_t chunk_size) {

    if (!is_open() || !callback) return 0;

    if (chunk_size == 0) {
        chunk_size = config_.stream_chunk_size;
    }

    // Ensure chunk size is page-aligned
    chunk_size = (chunk_size + 4095) & ~4095ULL;

    uint64_t total_read = 0;
    uint64_t current = start;

    while (current < end) {
        size_t to_read = std::min(chunk_size, static_cast<size_t>(end - current));

        // Allocate buffer from pool
        uint8_t* buffer = buffer_pool_->allocate(to_read);
        std::vector<uint8_t> fallback_buffer;

        if (!buffer) {
            // Pool exhausted, use heap
            fallback_buffer.resize(to_read);
            buffer = fallback_buffer.data();
        }

        size_t bytes_read = read(current, buffer, to_read);

        if (bytes_read > 0) {
            callback(current, buffer, bytes_read);
            total_read += bytes_read;
            stats_.total_reads++;
            stats_.total_bytes_read += bytes_read;
        } else {
            stats_.read_errors++;
        }

        if (buffer && fallback_buffer.empty()) {
            buffer_pool_->deallocate(buffer);
        }

        current += to_read;
    }

    return total_read;
}

uint64_t EnhancedRawDiskEngine::stream_read_with_progress(
    uint64_t start,
    uint64_t end,
    std::function<void(uint64_t, const uint8_t*, size_t)> data_callback,
    std::function<void(double)> progress_callback) {

    if (!is_open() || !data_callback) return 0;

    uint64_t total_size = end - start;
    uint64_t total_read = 0;
    uint64_t current = start;

    auto last_progress_time = std::chrono::steady_clock::now();

    stream_read(start, end, [&](uint64_t offset, const uint8_t* data, size_t size) {
        data_callback(offset, data, size);
        total_read += size;

        if (progress_callback) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_progress_time).count();

            // Update progress at most every 100ms
            if (elapsed >= 100) {
                double progress = static_cast<double>(total_read) / total_size;
                progress_callback(progress);
                last_progress_time = now;
            }
        }
    });

    // Final progress update
    if (progress_callback) {
        progress_callback(1.0);
    }

    return total_read;
}

bool EnhancedRawDiskEngine::has_io_uring() const {
#ifdef DBRECOVERY_HAVE_IO_URING
    return io_uring_initialized_;
#else
    return false;
#endif
}

uint32_t EnhancedRawDiskEngine::get_optimal_alignment() const {
    return get_optimal_block_size();
}

bool EnhancedRawDiskEngine::supports_direct_io() const {
    return has_direct_io();
}

// ==================== DiskImageMapper Implementation ====================

DiskImageMapper::DiskImageMapper(const std::string& path, MapMode mode)
    : path_(path), fd_(-1), size_(0), mode_(mode) {

    int flags = O_RDONLY;
    if (mode == MapMode::READ_WRITE) {
        flags = O_RDWR;
    } else if (mode == MapMode::COPY_ON_WRITE) {
        flags = O_RDONLY; // MAP_PRIVATE will handle COW
    }

    fd_ = open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd_ < 0) {
        throw std::system_error(errno, std::system_category(), "Failed to open: " + path);
    }

    struct stat st;
    if (fstat(fd_, &st) < 0) {
        close(fd_);
        fd_ = -1;
        throw std::system_error(errno, std::system_category(), "Failed to stat: " + path);
    }

    if (!S_ISREG(st.st_mode)) {
        close(fd_);
        fd_ = -1;
        throw std::invalid_argument("Disk image mapper accepts regular files only: " + path);
    }

    size_ = st.st_size;
}

DiskImageMapper::~DiskImageMapper() {
    // Unmap all mappings
    for (auto& m : mappings_) {
        if (m.ptr) {
            munmap(m.ptr, m.size);
        }
    }

    if (fd_ >= 0) {
        close(fd_);
    }
}

const uint8_t* DiskImageMapper::map(uint64_t offset, size_t size) {
    if (!is_open() || offset > size_ || size > size_ - offset) {
        return nullptr;
    }

    int prot = PROT_READ;
    int flags = MAP_SHARED;

    if (mode_ == MapMode::READ_WRITE) {
        prot |= PROT_WRITE;
    } else if (mode_ == MapMode::COPY_ON_WRITE) {
        flags = MAP_PRIVATE;
    }

    void* ptr = mmap(nullptr, size, prot, flags, fd_, offset);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    mappings_.push_back({ptr, size, offset});

    return static_cast<uint8_t*>(ptr);
}

uint8_t* DiskImageMapper::map_rw(uint64_t offset, size_t size) {
    if (!is_open() || offset > size_ || size > size_ - offset) {
        return nullptr;
    }

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, offset);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    mappings_.push_back({ptr, size, offset});

    return static_cast<uint8_t*>(ptr);
}

void DiskImageMapper::unmap(const uint8_t* ptr, size_t size) {
    if (!ptr) return;

    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = mappings_.begin(); it != mappings_.end(); ++it) {
        if (it->ptr == ptr) {
            munmap(it->ptr, it->size);
            mappings_.erase(it);
            return;
        }
    }
}

bool DiskImageMapper::flush() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& m : mappings_) {
        if (msync(m.ptr, m.size, MS_SYNC) != 0) {
            return false;
        }
    }

    return true;
}

void DiskImageMapper::advise_sequential() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& m : mappings_) {
        madvise(m.ptr, m.size, MADV_SEQUENTIAL);
    }
}

void DiskImageMapper::advise_random() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& m : mappings_) {
        madvise(m.ptr, m.size, MADV_RANDOM);
    }
}

void DiskImageMapper::advise_will_need(uint64_t offset, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& m : mappings_) {
        if (offset >= m.offset && offset - m.offset <= m.size &&
            size <= m.size - (offset - m.offset)) {
            size_t map_offset = offset - m.offset;
            madvise(static_cast<char*>(m.ptr) + map_offset, size, MADV_WILLNEED);
            break;
        }
    }
}

DiskImageMapper::MappingStats DiskImageMapper::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    MappingStats stats = {};
    stats.total_mappings = mappings_.size();
    stats.active_mappings = mappings_.size();

    for (const auto& m : mappings_) {
        stats.total_bytes_mapped += m.size;
    }

    return stats;
}

// ==================== StreamingScanner Implementation ====================

StreamingScanner::StreamingScanner(EnhancedRawDiskEngine& engine)
    : engine_(engine) {
    memset(&stats_, 0, sizeof(stats_));
}

void StreamingScanner::scan(
    uint64_t start,
    uint64_t end,
    std::function<void(uint64_t, const uint8_t*, size_t)> callback,
    std::function<void(double)> progress) {

    auto start_time = std::chrono::steady_clock::now();

    engine_.stream_read_with_progress(start, end, callback, progress);

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration<double>(end_time - start_time).count();

    stats_.bytes_scanned = end - start;
    stats_.chunks_processed = (end - start + config_.chunk_size - 1) / config_.chunk_size;
    stats_.elapsed_seconds = duration;
    stats_.mb_per_second = (stats_.bytes_scanned / (1024.0 * 1024.0)) / duration;
}

void StreamingScanner::scan_all(
    std::function<void(uint64_t, const uint8_t*, size_t)> callback,
    std::function<void(double)> progress) {

    scan(0, engine_.get_size(), callback, progress);
}

} // namespace engine
} // namespace dbrecovery
