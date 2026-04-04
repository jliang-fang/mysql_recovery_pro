#ifndef DBRECOVERY_INNODB_PARALLEL_RECOVERY_HPP
#define DBRECOVERY_INNODB_PARALLEL_RECOVERY_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <future>
#include <functional>
#include <chrono>
#include <iostream>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== 并行恢复任务 ====================
enum class ParallelTaskType {
    PAGE_SCAN,        // 页扫描
    PAGE_PARSE,       // 页解析
    RECORD_PARSE,     // 记录解析
    BTREE_TRAVERSE,   // B+树遍历
    BLOB_RECOVERY,    // BLOB 恢复
    CHECKSUM_VERIFY,  // 校验和验证
    DATA_EXPORT       // 数据导出
};

struct ParallelTask {
    ParallelTaskType type;
    uint32_t page_no;
    std::vector<uint8_t> data;
    std::function<void()> callback;
    std::promise<void> promise;
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "ParallelTask{type=" << static_cast<int>(type)
            << ", page=" << page_no << "}";
        return oss.str();
    }
};

// ==================== 并行恢复统计 ====================
struct ParallelRecoveryStats {
    size_t total_pages = 0;
    size_t scanned_pages = 0;
    size_t parsed_pages = 0;
    size_t recovered_records = 0;
    size_t failed_pages = 0;
    double scan_time_ms = 0.0;
    double parse_time_ms = 0.0;
    double total_time_ms = 0.0;
    size_t threads_used = 0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "ParallelRecoveryStats{pages=" << total_pages
            << ", scanned=" << scanned_pages
            << ", parsed=" << parsed_pages
            << ", records=" << recovered_records
            << ", failed=" << failed_pages
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << ", threads=" << threads_used << "}";
        return oss.str();
    }
};

// ==================== 并行页扫描器 ====================
class ParallelPageScanner {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     * @param max_threads 最大线程数
     */
    ParallelPageScanner(size_t page_size = 16384, size_t max_threads = 0);

    /**
     * @brief 设置文件路径
     */
    void set_file_path(const std::filesystem::path& file_path);

    /**
     * @brief 设置最大线程数
     */
    void set_max_threads(size_t max_threads);

    /**
     * @brief 并行扫描文件
     * @return 扫描结果
     */
    std::vector<ScannedPage> scan();

    /**
     * @brief 获取统计信息
     */
    const ParallelRecoveryStats& stats() const { return stats_; }

    /**
     * @brief 获取扫描的页
     */
    const std::vector<ScannedPage>& pages() const { return pages_; }

private:
    size_t page_size_;
    size_t max_threads_;
    std::filesystem::path file_path_;
    std::vector<ScannedPage> pages_;
    ParallelRecoveryStats stats_;
    std::mutex pages_mutex_;

    void worker_thread(size_t start_page, size_t end_page);
    void process_page(size_t page_no);
};

// ==================== 并行记录解析器 ====================
class ParallelRecordParser {
public:
    /**
     * @brief 构造函数
     * @param max_threads 最大线程数
     */
    explicit ParallelRecordParser(size_t max_threads = 0);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 设置页数据
     */
    void set_pages(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages);

    /**
     * @brief 并行解析记录
     * @return 解析后的记录
     */
    std::vector<std::map<std::string, std::string>> parse();

    /**
     * @brief 获取统计信息
     */
    const ParallelRecoveryStats& stats() const { return stats_; }

private:
    size_t max_threads_;
    catalog::TableSchema schema_;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pages_;
    std::vector<std::map<std::string, std::string>> records_;
    ParallelRecoveryStats stats_;

    void worker_thread(size_t start_idx, size_t end_idx);
    void process_page(size_t idx);
};

// ==================== 并行 B+树遍历器 ====================
class ParallelBTreeTraverser {
public:
    /**
     * @brief 构造函数
     * @param max_threads 最大线程数
     */
    explicit ParallelBTreeTraverser(size_t max_threads = 0);

    /**
     * @brief 设置根页号
     */
    void set_root_page(uint32_t root_page_no);

    /**
     * @brief 添加页数据
     */
    void add_page(uint32_t page_no, std::vector<uint8_t> page_data);

    /**
     * @brief 并行遍历 B+树
     * @param schema 表结构
     * @return 解析后的记录
     */
    std::vector<std::map<std::string, std::string>> traverse(const catalog::TableSchema& schema);

    /**
     * @brief 获取统计信息
     */
    const ParallelRecoveryStats& stats() const { return stats_; }

private:
    size_t max_threads_;
    uint32_t root_page_no_;
    std::map<uint32_t, std::vector<uint8_t>> pages_;
    std::vector<std::map<std::string, std::string>> records_;
    ParallelRecoveryStats stats_;
    std::mutex pages_mutex_;

    void traverse_subtree(uint32_t page_no, const catalog::TableSchema& schema);
    void worker_thread(uint32_t page_no, const catalog::TableSchema& schema);
};

// ==================== 并行恢复引擎 ====================
class ParallelRecoveryEngine {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     * @param max_threads 最大线程数
     */
    ParallelRecoveryEngine(size_t page_size = 16384, size_t max_threads = 0);

    /**
     * @brief 设置文件路径
     */
    void set_file_path(const std::filesystem::path& file_path);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 设置最大线程数
     */
    void set_max_threads(size_t max_threads);

    /**
     * @brief 执行并行恢复
     * @return 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover();

    /**
     * @brief 获取统计信息
     */
    const ParallelRecoveryStats& stats() const { return stats_; }

    /**
     * @brief 获取扫描的页
     */
    const std::vector<ScannedPage>& pages() const { return scanner_.pages(); }

    /**
     * @brief 获取错误信息
     */
    const std::vector<std::string>& errors() const { return errors_; }

private:
    size_t page_size_;
    size_t max_threads_;
    std::filesystem::path file_path_;
    catalog::TableSchema schema_;
    ParallelPageScanner scanner_;
    ParallelRecordParser parser_;
    ParallelRecoveryStats stats_;
    std::vector<std::string> errors_;
    std::mutex errors_mutex_;

    void add_error(const std::string& error);
};

// ==================== 并行页扫描器实现 ====================

inline ParallelPageScanner::ParallelPageScanner(size_t page_size, size_t max_threads)
    : page_size_(page_size), max_threads_(max_threads) {
    if (max_threads_ == 0) {
        max_threads_ = std::thread::hardware_concurrency();
        if (max_threads_ == 0) max_threads_ = 4;
    }
}

inline void ParallelPageScanner::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
}

inline void ParallelPageScanner::set_max_threads(size_t max_threads) {
    max_threads_ = max_threads;
    if (max_threads_ == 0) {
        max_threads_ = std::thread::hardware_concurrency();
        if (max_threads_ == 0) max_threads_ = 4;
    }
}

inline std::vector<ScannedPage> ParallelPageScanner::scan() {
    auto start = std::chrono::high_resolution_clock::now();

    pages_.clear();
    stats_ = ParallelRecoveryStats{};

    // 打开文件
    std::ifstream file(file_path_, std::ios::binary | std::ios::ate);
    if (!file) {
        return pages_;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // 计算页数
    size_t total_pages = file_size / page_size_;
    stats_.total_pages = total_pages;

    if (total_pages == 0) {
        return pages_;
    }

    // 创建线程池
    std::vector<std::thread> threads;
    size_t pages_per_thread = (total_pages + max_threads_ - 1) / max_threads_;

    // 启动工作线程
    for (size_t i = 0; i < max_threads_; ++i) {
        size_t start_page = i * pages_per_thread;
        size_t end_page = std::min(start_page + pages_per_thread, total_pages);
        
        if (start_page >= total_pages) break;

        threads.emplace_back(&ParallelPageScanner::worker_thread, this, start_page, end_page);
    }

    // 等待所有线程完成
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    // 排序结果
    std::sort(pages_.begin(), pages_.end(),
        [](const ScannedPage& a, const ScannedPage& b) {
            return a.page_no < b.page_no;
        });

    auto end = std::chrono::high_resolution_clock::now();
    stats_.scan_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.scanned_pages = pages_.size();
    stats_.threads_used = threads.size();

    return pages_;
}

inline void ParallelPageScanner::worker_thread(size_t start_page, size_t end_page) {
    for (size_t page_no = start_page; page_no < end_page; ++page_no) {
        process_page(page_no);
    }
}

inline void ParallelPageScanner::process_page(size_t page_no) {
    std::ifstream file(file_path_, std::ios::binary);
    if (!file) {
        return;
    }

    size_t offset = page_no * page_size_;
    file.seekg(offset, std::ios::beg);

    std::vector<uint8_t> page_data(page_size_);
    file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

    if (file.gcount() < static_cast<std::streamsize>(page_size_)) {
        return;
    }

    // 扫描单个页
    ScannedPage page{};
    page.offset = static_cast<uint32_t>(offset);
    page.page_no = page_no;
    page.page_type = read_le<uint16_t>(page_data.data(), 16);
    page.prev_page = read_le<uint32_t>(page_data.data(), 0);
    page.next_page = read_le<uint32_t>(page_data.data(), 4);
    page.lsn = read_le<uint64_t>(page_data.data(), 8);
    page.space_id = read_le<uint32_t>(page_data.data(), 24);
    page.is_valid = PageScanner::is_innodb_page(page_data.data(), page_size_);

    {
        std::lock_guard<std::mutex> lock(pages_mutex_);
        pages_.push_back(page);
    }
}

// ==================== 并行记录解析器实现 ====================

inline ParallelRecordParser::ParallelRecordParser(size_t max_threads)
    : max_threads_(max_threads) {
    if (max_threads_ == 0) {
        max_threads_ = std::thread::hardware_concurrency();
        if (max_threads_ == 0) max_threads_ = 4;
    }
}

inline void ParallelRecordParser::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

inline void ParallelRecordParser::set_pages(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages) {
    pages_ = pages;
}

inline std::vector<std::map<std::string, std::string>> ParallelRecordParser::parse() {
    auto start = std::chrono::high_resolution_clock::now();

    records_.clear();
    stats_ = ParallelRecoveryStats{};

    if (pages_.empty()) {
        return records_;
    }

    // 创建线程池
    std::vector<std::thread> threads;
    size_t pages_per_thread = (pages_.size() + max_threads_ - 1) / max_threads_;

    // 启动工作线程
    for (size_t i = 0; i < max_threads_; ++i) {
        size_t start_idx = i * pages_per_thread;
        size_t end_idx = std::min(start_idx + pages_per_thread, pages_.size());
        
        if (start_idx >= pages_.size()) break;

        threads.emplace_back(&ParallelRecordParser::worker_thread, this, start_idx, end_idx);
    }

    // 等待所有线程完成
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    stats_.parse_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.parsed_pages = pages_.size();
    stats_.recovered_records = records_.size();
    stats_.threads_used = threads.size();

    return records_;
}

inline void ParallelRecordParser::worker_thread(size_t start_idx, size_t end_idx) {
    for (size_t i = start_idx; i < end_idx; ++i) {
        process_page(i);
    }
}

inline void ParallelRecordParser::process_page(size_t idx) {
    const auto& [page_no, page_data] = pages_[idx];

    BPlusTreeParser parser(page_data);
    if (!parser.parse()) {
        return;
    }

    auto page_records = parser.parse_records(schema_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.insert(records_.end(), page_records.begin(), page_records.end());
    }
}

// ==================== 并行 B+树遍历器实现 ====================

inline ParallelBTreeTraverser::ParallelBTreeTraverser(size_t max_threads)
    : max_threads_(max_threads) {
    if (max_threads_ == 0) {
        max_threads_ = std::thread::hardware_concurrency();
        if (max_threads_ == 0) max_threads_ = 4;
    }
}

inline void ParallelBTreeTraverser::set_root_page(uint32_t root_page_no) {
    root_page_no_ = root_page_no;
}

inline void ParallelBTreeTraverser::add_page(uint32_t page_no, std::vector<uint8_t> page_data) {
    std::lock_guard<std::mutex> lock(pages_mutex_);
    pages_[page_no] = std::move(page_data);
}

inline std::vector<std::map<std::string, std::string>> ParallelBTreeTraverser::traverse(
    const catalog::TableSchema& schema) {
    
    auto start = std::chrono::high_resolution_clock::now();

    records_.clear();
    stats_ = ParallelRecoveryStats{};

    if (pages_.empty() || pages_.find(root_page_no_) == pages_.end()) {
        return records_;
    }

    // 创建线程池
    std::vector<std::thread> threads;

    // 启动工作线程
    for (const auto& [page_no, _] : pages_) {
        threads.emplace_back(&ParallelBTreeTraverser::worker_thread, this, page_no, std::cref(schema));
    }

    // 等待所有线程完成
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    stats_.parse_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.parsed_pages = pages_.size();
    stats_.recovered_records = records_.size();
    stats_.threads_used = threads.size();

    return records_;
}

inline void ParallelBTreeTraverser::worker_thread(uint32_t page_no, const catalog::TableSchema& schema) {
    auto it = pages_.find(page_no);
    if (it == pages_.end()) {
        return;
    }

    BPlusTreeParser parser(it->second);
    if (!parser.parse()) {
        return;
    }

    auto page_records = parser.parse_records(schema);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.insert(records_.end(), page_records.begin(), page_records.end());
    }
}

// ==================== 并行恢复引擎实现 ====================

inline ParallelRecoveryEngine::ParallelRecoveryEngine(size_t page_size, size_t max_threads)
    : page_size_(page_size), max_threads_(max_threads), scanner_(page_size, max_threads) {
}

inline void ParallelRecoveryEngine::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
    scanner_.set_file_path(file_path);
}

inline void ParallelRecoveryEngine::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
    parser_.set_schema(schema);
}

inline void ParallelRecoveryEngine::set_max_threads(size_t max_threads) {
    max_threads_ = max_threads;
    scanner_.set_max_threads(max_threads);
    parser_.set_max_threads(max_threads);
}

inline std::vector<std::map<std::string, std::string>> ParallelRecoveryEngine::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    records_.clear();
    errors_.clear();
    stats_ = ParallelRecoveryStats{};

    // 扫描页
    auto pages = scanner_.scan();
    stats_.scanned_pages = pages.size();

    if (pages.empty()) {
        add_error("No pages scanned");
        return records_;
    }

    // 准备页数据
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> page_data;
    for (const auto& page : pages) {
        if (page.is_valid) {
            std::ifstream file(file_path_, std::ios::binary);
            if (!file) continue;

            size_t offset = page.page_no * page_size_;
            file.seekg(offset, std::ios::beg);

            std::vector<uint8_t> data(page_size_);
            file.read(reinterpret_cast<char*>(data.data()), page_size_);

            if (file.gcount() == static_cast<std::streamsize>(page_size_)) {
                page_data.emplace_back(page.page_no, std::move(data));
            }
        }
    }

    // 解析记录
    parser_.set_pages(page_data);
    records_ = parser_.parse();
    stats_.recovered_records = records_.size();

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();
    stats_.threads_used = std::max(scanner_.stats().threads_used, parser_.stats().threads_used);

    return records_;
}

inline void ParallelRecoveryEngine::add_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    errors_.push_back(error);
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PARALLEL_RECOVERY_HPP
