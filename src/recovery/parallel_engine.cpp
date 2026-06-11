#include "dbrecovery/recovery/parallel_engine.hpp"
#include <iostream>
#include <thread>
#include <chrono>

namespace dbrecovery {
namespace recovery {

ParallelRecoveryEngine::ParallelRecoveryEngine()
    : RecoveryEngine(), thread_count_(std::thread::hardware_concurrency()) {
    if (thread_count_ == 0) {
        thread_count_ = 4;
    }
    parallel_scanner_ = std::make_unique<engine::ParallelScanner>(thread_count_);
}

ParallelRecoveryEngine::~ParallelRecoveryEngine() = default;

void ParallelRecoveryEngine::set_thread_count(size_t threads) {
    if (threads > 0) {
        thread_count_ = threads;
        parallel_scanner_ = std::make_unique<engine::ParallelScanner>(thread_count_);
    }
}

void ParallelRecoveryEngine::set_progress_callback(std::function<void(double)> callback) {
    progress_callback_ = std::move(callback);
}

void record_progress(double progress, std::function<void(double)> callback) {
    if (callback) {
        callback(progress);
    }
}

std::vector<std::map<std::string, std::string>> 
ParallelRecoveryEngine::parallel_recover_truncate(const catalog::TableSchema& schema) {
    if (!file_reader_ || !file_reader_->is_valid()) {
        std::cerr << "[ERROR] No file opened" << std::endl;
        return {};
    }

    uint32_t total_pages = file_reader_->get_total_pages();
    std::vector<std::map<std::string, std::string>> all_records;
    std::set<uint32_t> seen_page_nums;

    auto start_time = std::chrono::steady_clock::now();

    // 设置并行扫描器的进度回调
    std::atomic<size_t> processed_count(0);
    parallel_scanner_->set_progress_callback([&](size_t processed, size_t total) {
        double progress = total > 0 ? static_cast<double>(processed) / total : 0.0;
        record_progress(progress, progress_callback_);
        processed_count.store(processed);
    });

    // 设置工作函数
    std::mutex records_mutex;
    parallel_scanner_->set_worker([&](uint32_t page_no) -> bool {
        auto page_data = file_reader_->read_page_raw(page_no);
        if (page_data.empty()) {
            return false;
        }

        // 检查页类型
        uint16_t page_type = innodb::read_le<uint16_t>(
            page_data.data(), innodb::FIL_PAGE_TYPE_ABS);

        // 跳过非索引页
        if (page_type != innodb::FIL_PAGE_INDEX) {
            return true;
        }

        // 跳过已处理的页
        {
            std::lock_guard<std::mutex> lock(records_mutex);
            if (seen_page_nums.find(page_no) != seen_page_nums.end()) {
                return true;
            }
            seen_page_nums.insert(page_no);
        }

        // 解析记录
        innodb::BPlusTreeParser parser(page_data);
        if (!parser.parse()) {
            return false;
        }

        auto records = parser.parse_records(schema);
        
        {
            std::lock_guard<std::mutex> lock(records_mutex);
            all_records.insert(all_records.end(), records.begin(), records.end());
        }

        return true;
    });

    // 开始并行扫描
    bool success = parallel_scanner_->scan_all(total_pages);

    auto end_time = std::chrono::steady_clock::now();
    auto stats = parallel_scanner_->get_stats();

    scan_stats_.total_pages = total_pages;
    scan_stats_.valid_pages = stats.processed_pages;
    scan_stats_.index_pages = stats.processed_pages;
    scan_stats_.elapsed_seconds = stats.elapsed_seconds;
    scan_stats_.pages_per_second = stats.pages_per_second;

    if (progress_callback_) {
        progress_callback_(1.0);
    }

    std::cout << "[INFO] Parallel scan completed: " 
              << all_records.size() << " records in " 
              << stats.elapsed_seconds << "s" << std::endl;

    return all_records;
}

ParallelRecoveryEngine::ScanStats ParallelRecoveryEngine::get_scan_stats() const {
    return scan_stats_;
}

} // namespace recovery
} // namespace dbrecovery
