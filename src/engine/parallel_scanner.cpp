#include "dbrecovery/engine/parallel_scanner.hpp"
#include <iostream>
#include <algorithm>

namespace dbrecovery {
namespace engine {

ParallelScanner::ParallelScanner(size_t num_threads)
    : num_threads_(num_threads), stop_requested_(false), scanning_(false),
      processed_count_(0), failed_count_(0) {
    if (num_threads_ == 0) {
        // 自动检测线程数
        num_threads_ = std::thread::hardware_concurrency();
        if (num_threads_ == 0) {
            num_threads_ = 4; // 默认 4 线程
        }
    }
}

ParallelScanner::~ParallelScanner() {
    stop();
}

ParallelScanner::ParallelScanner(ParallelScanner&& other) noexcept
    : num_threads_(other.num_threads_), threads_(std::move(other.threads_)),
      stop_requested_(other.stop_requested_.load()),
      scanning_(other.scanning_.load()),
      work_queue_(std::move(other.work_queue_)),
      worker_(std::move(other.worker_)),
      progress_callback_(std::move(other.progress_callback_)),
      stats_(other.stats_),
      processed_count_(other.processed_count_.load()),
      failed_count_(other.failed_count_.load()) {
    other.stop_requested_ = true;
    other.scanning_ = false;
    other.processed_count_ = 0;
    other.failed_count_ = 0;
}

ParallelScanner& ParallelScanner::operator=(ParallelScanner&& other) noexcept {
    if (this != &other) {
        stop();
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        num_threads_ = other.num_threads_;
        threads_ = std::move(other.threads_);
        stop_requested_ = other.stop_requested_.load();
        scanning_ = other.scanning_.load();
        work_queue_ = std::move(other.work_queue_);
        worker_ = std::move(other.worker_);
        progress_callback_ = std::move(other.progress_callback_);
        stats_ = other.stats_;
        processed_count_ = other.processed_count_.load();
        failed_count_ = other.failed_count_.load();
        
        other.stop_requested_ = true;
        other.scanning_ = false;
        other.processed_count_ = 0;
        other.failed_count_ = 0;
    }
    return *this;
}

void ParallelScanner::set_worker(std::function<bool(uint32_t)> worker) {
    std::lock_guard<std::mutex> lock(mutex_);
    worker_ = std::move(worker);
}

void ParallelScanner::set_progress_callback(std::function<void(size_t, size_t)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_callback_ = std::move(callback);
}

bool ParallelScanner::scan(uint32_t start_page, uint32_t end_page) {
    if (start_page >= end_page) {
        return true;
    }
    
    if (!worker_) {
        std::cerr << "[ERROR] Worker function not set" << std::endl;
        return false;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scanning_) {
            std::cerr << "[WARN] Scanner is already running" << std::endl;
            return false;
        }
        
        // 清空工作队列
        while (!work_queue_.empty()) {
            work_queue_.pop();
        }
        
        // 添加工作项
        for (uint32_t page_no = start_page; page_no < end_page; ++page_no) {
            work_queue_.push(page_no);
        }
        
        scanning_ = true;
        stop_requested_ = false;
        processed_count_ = 0;
        failed_count_ = 0;
        stats_ = {};
        stats_.total_pages = end_page - start_page;
        start_time_ = std::chrono::steady_clock::now();
    }
    
    // 启动工作线程
    threads_.clear();
    for (size_t i = 0; i < num_threads_; ++i) {
        threads_.emplace_back(&ParallelScanner::worker_thread, this);
    }
    
    // 等待所有线程完成
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    // 更新统计信息
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scanning_ = false;
        
        auto end_time = std::chrono::steady_clock::now();
        stats_.elapsed_seconds = std::chrono::duration<double>(end_time - start_time_).count();
        stats_.processed_pages = processed_count_;
        stats_.failed_pages = failed_count_;
        
        if (stats_.elapsed_seconds > 0) {
            stats_.pages_per_second = stats_.processed_pages / stats_.elapsed_seconds;
        }
    }
    
    return failed_count_ == 0;
}

bool ParallelScanner::scan_all(uint32_t total_pages) {
    return scan(0, total_pages);
}

ParallelScanner::ScanStats ParallelScanner::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

size_t ParallelScanner::get_thread_count() const {
    return num_threads_;
}

void ParallelScanner::stop() {
    stop_requested_ = true;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scanning_ = false;
    }
    
    cv_.notify_all();
    
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    threads_.clear();
}

bool ParallelScanner::is_scanning() const {
    return scanning_;
}

void ParallelScanner::worker_thread() {
    while (!stop_requested_) {
        uint32_t page_no;
        
        if (!get_next_page(page_no)) {
            break; // 没有更多工作
        }
        
        // 处理页
        bool success = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (worker_) {
                success = worker_(page_no);
            }
        }
        
        update_stats(page_no, success);
        
        // 更新进度回调
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (progress_callback_) {
                size_t processed = processed_count_ + failed_count_;
                progress_callback_(processed, stats_.total_pages);
            }
        }
    }
}

bool ParallelScanner::get_next_page(uint32_t& page_no) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (work_queue_.empty() || stop_requested_) {
        return false;
    }
    
    page_no = work_queue_.front();
    work_queue_.pop();
    
    return true;
}

void ParallelScanner::update_stats(uint32_t page_no, bool success) {
    if (success) {
        processed_count_++;
    } else {
        failed_count_++;
    }
}

// ==================== ParallelScannerManager 实现 ====================

ParallelScannerManager& ParallelScannerManager::instance() {
    static ParallelScannerManager instance;
    return instance;
}

ParallelScannerManager::ParallelScannerManager()
    : default_threads_(std::thread::hardware_concurrency()) {
    if (default_threads_ == 0) {
        default_threads_ = 4;
    }
}

ParallelScannerManager::~ParallelScannerManager() = default;

std::unique_ptr<ParallelScanner> ParallelScannerManager::create_scanner(size_t num_threads) {
    if (num_threads == 0) {
        num_threads = default_threads_;
    }
    return std::make_unique<ParallelScanner>(num_threads);
}

void ParallelScannerManager::set_default_threads(size_t num_threads) {
    if (num_threads > 0) {
        default_threads_ = num_threads;
    }
}

size_t ParallelScannerManager::get_default_threads() const {
    return default_threads_;
}

} // namespace engine
} // namespace dbrecovery
