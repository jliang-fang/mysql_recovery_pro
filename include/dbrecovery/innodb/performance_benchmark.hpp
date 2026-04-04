#ifndef DBRECOVERY_PERFORMANCE_BENCHMARK_HPP
#define DBRECOVERY_PERFORMANCE_BENCHMARK_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <map>
#include <functional>
#include <mutex>
#include <optional>

namespace dbrecovery {
namespace innodb {

/**
 * @brief Benchmark result structure
 */
struct BenchmarkResult {
    std::string name;
    uint64_t iterations;
    double total_time_ms;
    double avg_time_ms;
    double min_time_ms;
    double max_time_ms;
    double throughput_ops_per_sec;
    double throughput_mb_per_sec;

    std::string to_string() const {
        return name + ": " + std::to_string(avg_time_ms) + " ms/op, " +
               std::to_string(throughput_ops_per_sec) + " ops/sec";
    }
};

/**
 * @brief Performance Benchmark - Measures performance of recovery operations
 */
class PerformanceBenchmark {
public:
    PerformanceBenchmark();
    ~PerformanceBenchmark();

    /**
     * @brief Run a benchmark
     */
    BenchmarkResult run(const std::string& name, int iterations,
                        const std::function<void()>& func);

    /**
     * @brief Benchmark page parsing
     */
    BenchmarkResult benchmark_page_parse(uint32_t page_size, int iterations);

    /**
     * @brief Benchmark B+Tree parsing
     */
    BenchmarkResult benchmark_btree_parse(uint32_t page_size, int iterations);

    /**
     * @brief Benchmark record parsing
     */
    BenchmarkResult benchmark_record_parse(uint32_t page_size, int iterations);

    /**
     * @brief Benchmark BLOB reconstruction
     */
    BenchmarkResult benchmark_blob_reconstruct(uint32_t blob_size, int iterations);

    /**
     * @brief Benchmark undo log parsing
     */
    BenchmarkResult benchmark_undo_log_parse(uint32_t page_size, int iterations);

    /**
     * @brief Benchmark redo log replay
     */
    BenchmarkResult benchmark_redo_log_replay(uint32_t page_size, int iterations);

    /**
     * @brief Get all benchmark results
     */
    const std::vector<BenchmarkResult>& get_results() const;

    /**
     * @brief Print benchmark report
     */
    void print_report() const;

    /**
     * @brief Clear all results
     */
    void clear();

private:
    std::vector<BenchmarkResult> results_;

    /**
     * @brief Create test page
     */
    std::vector<uint8_t> create_test_page(uint32_t page_size, uint16_t page_type);

    /**
     * @brief Create test B+Tree page
     */
    std::vector<uint8_t> create_test_btree_page(uint32_t page_size);

    /**
     * @brief Create test undo log page
     */
    std::vector<uint8_t> create_test_undo_page(uint32_t page_size);

    /**
     * @brief Create test redo log records
     */
    std::vector<uint8_t> create_test_redo_records(uint32_t page_size, int count);
};

/**
 * @brief Memory Pool - Optimized memory allocation
 */
class MemoryPool {
public:
    explicit MemoryPool(size_t block_size = 16384, size_t initial_capacity = 1024);
    ~MemoryPool();

    /**
     * @brief Allocate memory from pool
     */
    uint8_t* allocate();

    /**
     * @brief Free memory back to pool
     */
    void free(uint8_t* ptr);

    /**
     * @brief Get pool statistics
     */
    struct Stats {
        size_t total_allocated;
        size_t total_freed;
        size_t active_blocks;
        size_t peak_usage;
    };

    Stats get_stats() const;

    /**
     * @brief Clear pool
     */
    void clear();

private:
    size_t block_size_;
    std::vector<uint8_t*> free_list_;
    std::vector<uint8_t*> all_blocks_;
    Stats stats_;

    /**
     * @brief Allocate new block
     */
    void allocate_block();
};

/**
 * @brief Page Cache - Caches parsed pages
 */
class PageCache {
public:
    explicit PageCache(size_t max_size = 10000);
    ~PageCache();

    /**
     * @brief Get page from cache
     */
    std::optional<std::vector<uint8_t>> get(uint32_t page_no);

    /**
     * @brief Put page in cache
     */
    void put(uint32_t page_no, std::vector<uint8_t> page_data);

    /**
     * @brief Remove page from cache
     */
    void remove(uint32_t page_no);

    /**
     * @brief Clear cache
     */
    void clear();

    /**
     * @brief Get cache statistics
     */
    struct Stats {
        size_t hits = 0;
        size_t misses = 0;
        size_t size = 0;
        size_t max_size = 0;
        double hit_rate = 0.0;
    };

    Stats get_stats() const;

private:
    size_t max_size_ = 10000;
    std::map<uint32_t, std::vector<uint8_t>> cache_;
    Stats stats_;

    /**
     * @brief Evict oldest entry if needed
     */
    void evict_if_needed();
};

// Inline implementations for PageCache
inline PageCache::PageCache(size_t max_size) : max_size_(max_size) {
    stats_ = {0, 0, 0, max_size, 0.0};
}

inline PageCache::~PageCache() = default;

inline std::optional<std::vector<uint8_t>> PageCache::get(uint32_t page_no) {
    auto it = cache_.find(page_no);
    if (it == cache_.end()) {
        stats_.misses++;
        return std::nullopt;
    }
    stats_.hits++;
    return it->second;
}

inline void PageCache::put(uint32_t page_no, std::vector<uint8_t> page_data) {
    evict_if_needed();
    cache_[page_no] = std::move(page_data);
    stats_.size = cache_.size();
}

inline void PageCache::remove(uint32_t page_no) {
    cache_.erase(page_no);
    stats_.size = cache_.size();
}

inline void PageCache::clear() {
    cache_.clear();
    stats_ = {0, 0, 0, max_size_, 0.0};
}

inline PageCache::Stats PageCache::get_stats() const {
    Stats result = stats_;
    if (result.hits + result.misses > 0) {
        result.hit_rate = static_cast<double>(result.hits) / (result.hits + result.misses);
    }
    return result;
}

inline void PageCache::evict_if_needed() {
    if (cache_.size() >= max_size_) {
        cache_.erase(cache_.begin());
    }
}

/**
 * @brief IO Buffer Pool - Optimized I/O operations
 */
class IOBufferPool {
public:
    explicit IOBufferPool(size_t buffer_size = 1024 * 1024, size_t num_buffers = 4);
    ~IOBufferPool();

    /**
     * @brief Get a buffer
     */
    uint8_t* get_buffer();

    /**
     * @brief Return a buffer
     */
    void return_buffer(uint8_t* ptr);

    /**
     * @brief Get pool statistics
     */
    struct Stats {
        size_t total_allocated = 0;
        size_t active_buffers = 0;
        size_t peak_usage = 0;
    };

    Stats get_stats() const;

private:
    size_t buffer_size_ = 1024 * 1024;
    std::vector<uint8_t*> buffers_;
    std::vector<uint8_t*> free_buffers_;
    Stats stats_;
    mutable std::mutex mutex_;
};

// Inline implementations for IOBufferPool
inline IOBufferPool::IOBufferPool(size_t buffer_size, size_t num_buffers)
    : buffer_size_(buffer_size) {
    for (size_t i = 0; i < num_buffers; ++i) {
        uint8_t* buffer = new uint8_t[buffer_size];
        buffers_.push_back(buffer);
        free_buffers_.push_back(buffer);
    }
    stats_.total_allocated = num_buffers * buffer_size;
}

inline IOBufferPool::~IOBufferPool() {
    for (auto buffer : buffers_) {
        delete[] buffer;
    }
}

inline uint8_t* IOBufferPool::get_buffer() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_buffers_.empty()) {
        uint8_t* buffer = new uint8_t[buffer_size_];
        buffers_.push_back(buffer);
        stats_.total_allocated += buffer_size_;
        stats_.peak_usage = std::max(stats_.peak_usage, buffers_.size());
        return buffer;
    }
    uint8_t* buffer = free_buffers_.back();
    free_buffers_.pop_back();
    stats_.active_buffers = buffers_.size() - free_buffers_.size();
    return buffer;
}

inline void IOBufferPool::return_buffer(uint8_t* ptr) {
    if (ptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        free_buffers_.push_back(ptr);
        stats_.active_buffers = buffers_.size() - free_buffers_.size();
    }
}

inline IOBufferPool::Stats IOBufferPool::get_stats() const {
    return stats_;
}

/**
 * @brief Timer - High-resolution timer
 */
class Timer {
public:
    Timer();
    ~Timer();

    /**
     * @brief Start timer
     */
    void start();

    /**
     * @brief Stop timer
     */
    void stop();

    /**
     * @brief Reset timer
     */
    void reset();

    /**
     * @brief Get elapsed time in milliseconds
     */
    double elapsed_ms() const;

    /**
     * @brief Get elapsed time in microseconds
     */
    double elapsed_us() const;

    /**
     * @brief Check if timer is running
     */
    bool is_running() const;

private:
    std::chrono::high_resolution_clock::time_point start_time_;
    std::chrono::high_resolution_clock::time_point end_time_;
    bool running_;
};

/**
 * @brief Benchmark Runner - Runs all benchmarks
 */
class BenchmarkRunner {
public:
    BenchmarkRunner();
    ~BenchmarkRunner();

    /**
     * @brief Run all benchmarks
     */
    void run_all();

    /**
     * @brief Get results
     */
    const std::vector<BenchmarkResult>& get_results() const;

    /**
     * @brief Print report
     */
    void print_report() const;

private:
    PerformanceBenchmark benchmark_;
    std::vector<BenchmarkResult> results_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_PERFORMANCE_BENCHMARK_HPP
