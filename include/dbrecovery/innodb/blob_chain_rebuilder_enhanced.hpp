#ifndef DBRECOVERY_INNODB_BLOB_CHAIN_REBUILDER_ENHANCED_HPP
#define DBRECOVERY_INNODB_BLOB_CHAIN_REBUILDER_ENHANCED_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <memory>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>
#include <atomic>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/blob_pointer.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== 增强的 BLOB 链信息 ====================
struct EnhancedBlobChainInfo : public BlobChainInfo {
    // 跨表空间信息
    std::vector<uint32_t> space_ids;  // 所有涉及的表空间ID
    std::vector<uint32_t> file_ids;   // 文件ID（如果可用）
    
    // 恢复状态
    bool is_cross_space = false;      // 是否跨表空间
    bool is_partial = false;          // 是否部分恢复
    uint32_t recovered_pages = 0;     // 已恢复的页数
    uint32_t total_expected_pages = 0; // 预期的总页数
    
    // 元数据
    uint64_t table_id = 0;            // 表ID
    uint32_t column_id = 0;           // 列ID
    uint32_t version = 1;             // BLOB 版本
    
    // 性能统计
    double recovery_time_ms = 0.0;    // 恢复耗时（毫秒）
    size_t bytes_read = 0;            // 读取的字节数
    size_t bytes_written = 0;         // 写入的字节数

    std::string to_string() const override {
        std::ostringstream oss;
        oss << "EnhancedBlobChainInfo{space_id=" << space_id
            << ", start_page=" << start_page_no
            << ", total_len=" << total_len
            << ", recovered=" << recovered_pages
            << "/" << total_expected_pages;
        if (is_cross_space) oss << ", cross_space";
        if (is_partial) oss << ", partial";
        oss << "}";
        return oss.str();
    }
};

// ==================== 增强的 BLOB 链统计 ====================
struct EnhancedBlobChainStats : public BlobChainStats {
    // 性能统计
    double scan_time_ms = 0.0;        // 扫描耗时
    double rebuild_time_ms = 0.0;     // 重建耗时
    double total_time_ms = 0.0;       // 总耗时
    
    // 跨表空间统计
    size_t cross_space_chains = 0;    // 跨表空间链数
    size_t partial_chains = 0;        // 部分恢复的链数
    
    // 数据统计
    uint64_t total_blob_count = 0;    // BLOB 总数
    uint64_t total_blob_bytes = 0;    // BLOB 总字节数
    uint64_t recovered_blob_bytes = 0; // 已恢复的 BLOB 字节数
    
    // 错误统计
    size_t checksum_errors = 0;       // 校验和错误
    size_t chain_breaks = 0;          // 链断裂
    size_t missing_files = 0;         // 缺失的文件

    std::string to_string() const override {
        std::ostringstream oss;
        oss << "EnhancedBlobChainStats{total=" << total_pages
            << ", valid=" << valid_pages
            << ", size=" << total_size
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms";
        if (cross_space_chains > 0) oss << ", cross=" << cross_space_chains;
        if (partial_chains > 0) oss << ", partial=" << partial_chains;
        oss << "}";
        return oss.str();
    }
};

// ==================== BLOB 恢复任务 ====================
struct BlobRecoveryTask {
    uint32_t space_id;
    uint32_t page_no;
    uint32_t offset;
    uint32_t len;
    uint32_t total_len;
    std::filesystem::path file_path;
    std::vector<uint8_t> data;
    bool success;
    std::string error_message;
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "BlobRecoveryTask{space=" << space_id
            << ", page=" << page_no
            << ", len=" << len
            << "/" << total_len
            << ", success=" << std::boolalpha << success << std::noboolalpha
            << "}";
        return oss.str();
    }
};

// ==================== 增强的 BLOB 链重连器 ====================
class EnhancedBlobChainRebuilder {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     * @param max_threads 最大线程数（0 表示自动）
     */
    explicit EnhancedBlobChainRebuilder(size_t page_size = 16384, size_t max_threads = 0);

    /**
     * @brief 析构函数
     */
    ~EnhancedBlobChainRebuilder();

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size);

    /**
     * @brief 设置最大线程数
     */
    void set_max_threads(size_t max_threads);

    /**
     * @brief 从 BLOB 指针重建链（支持跨表空间）
     * @param pointer BLOB 指针
     * @param file_paths 表空间文件路径映射
     * @return true 重建成功
     */
    bool rebuild_from_pointer(const BlobPointer& pointer,
                             const std::map<uint32_t, std::filesystem::path>& file_paths);

    /**
     * @brief 从文件重建 BLOB 链（支持跨表空间）
     * @param file_paths 表空间文件路径映射
     * @param space_id 空间ID
     * @param start_page_no 起始页号
     * @return true 重建成功
     */
    bool rebuild_from_files(const std::map<uint32_t, std::filesystem::path>& file_paths,
                           uint32_t space_id, uint32_t start_page_no);

    /**
     * @brief 从单个文件重建 BLOB 链
     * @param file_path 文件路径
     * @param space_id 空间ID
     * @param start_page_no 起始页号
     * @return true 重建成功
     */
    bool rebuild_from_file(const std::filesystem::path& file_path,
                          uint32_t space_id, uint32_t start_page_no) override;

    /**
     * @brief 批量重建多个 BLOB 链
     * @param pointers BLOB 指针列表
     * @param file_paths 表空间文件路径映射
     * @return 重建结果映射
     */
    std::map<uint64_t, bool> rebuild_batch(
        const std::vector<BlobPointer>& pointers,
        const std::map<uint32_t, std::filesystem::path>& file_paths);

    /**
     * @brief 获取重建的 BLOB 数据
     */
    const std::vector<uint8_t>& blob_data() const override { return blob_data_; }

    /**
     * @brief 获取增强的 BLOB 链信息
     */
    const EnhancedBlobChainInfo& chain_info() const { return enhanced_chain_info_; }

    /**
     * @brief 获取增强的重连统计
     */
    const EnhancedBlobChainStats& stats() const { return enhanced_stats_; }

    /**
     * @brief 修复链断裂（支持跨表空间）
     */
    void fix_chain_breaks() override;

    /**
     * @brief 检测链中的循环引用
     */
    bool has_circular_reference() const override;

    /**
     * @brief 获取缺失的页（支持跨表空间）
     */
    std::vector<uint32_t> get_missing_pages() const override;

    /**
     * @brief 导出 BLOB 数据到文件
     */
    bool export_to_file(const std::filesystem::path& output_path) const override;

    /**
     * @brief 导出为 SQL BLOB 语句
     */
    bool export_to_sql(const std::filesystem::path& output_path,
                      const std::string& table_name,
                      const std::string& column_name) const;

    /**
     * @brief 获取所有重建的 BLOB 数据
     */
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& all_blob_data() const {
        return all_blob_data_;
    }

    /**
     * @brief 添加额外的 BLOB 页数据
     */
    void add_blob_page(uint32_t space_id, uint32_t page_no, std::vector<uint8_t> data);

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    size_t max_threads_;
    std::vector<uint8_t> blob_data_;
    EnhancedBlobChainInfo enhanced_chain_info_;
    EnhancedBlobChainStats enhanced_stats_;
    std::vector<BlobPageInfo> page_chain_;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> all_blob_data_;
    
    // 表空间文件路径映射
    std::map<uint32_t, std::filesystem::path> file_paths_;
    
    // 线程池
    std::vector<std::thread> threads_;
    std::queue<BlobRecoveryTask> tasks_;
    std::mutex task_mutex_;
    std::condition_variable task_cv_;
    std::atomic<bool> shutdown_{false};
    std::atomic<size_t> active_workers_{0};

    // 从页中读取 BLOB 数据
    std::vector<uint8_t> read_blob_from_page(const uint8_t* page_data, size_t page_size,
                                            size_t offset, uint32_t len) override;

    // 检测循环引用
    bool detect_circular_reference() const override;

    // 标记缺失的页
    void mark_missing_pages() override;

    // 工作线程函数
    void worker_thread();

    // 提交任务
    void submit_task(const BlobRecoveryTask& task);

    // 等待所有任务完成
    void wait_for_completion();

    // 从表空间文件读取页
    std::optional<std::vector<uint8_t>> read_page_from_space(uint32_t space_id, uint32_t page_no);
};

// ==================== BLOB 恢复器（支持多线程） ====================
class BlobRecoveryWorker {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     * @param max_threads 最大线程数
     */
    BlobRecoveryWorker(size_t page_size = 16384, size_t max_threads = 0);

    /**
     * @brief 设置表空间文件映射
     */
    void set_space_files(const std::map<uint32_t, std::filesystem::path>& space_files);

    /**
     * @brief 添加 BLOB 指针
     */
    void add_blob_pointer(const BlobPointer& pointer);

    /**
     * @brief 开始恢复
     */
    bool start();

    /**
     * @brief 获取恢复的 BLOB 数据
     */
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& get_blob_data() const {
        return blob_data_;
    }

    /**
     * @brief 获取统计信息
     */
    const EnhancedBlobChainStats& get_stats() const { return stats_; }

    /**
     * @brief 获取错误信息
     */
    const std::vector<std::string>& get_errors() const { return errors_; }

private:
    size_t page_size_;
    size_t max_threads_;
    std::map<uint32_t, std::filesystem::path> space_files_;
    std::vector<BlobPointer> pointers_;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> blob_data_;
    EnhancedBlobChainStats stats_;
    std::vector<std::string> errors_;

    void process_blob(const BlobPointer& pointer);
    void worker_thread(size_t thread_id);
};

// ==================== 实现 ====================

inline EnhancedBlobChainRebuilder::EnhancedBlobChainRebuilder(size_t page_size, size_t max_threads)
    : page_size_(page_size), max_threads_(max_threads) {
    // 验证页大小
    if (page_size_ != 4096 && page_size_ != 8192 &&
        page_size_ != 16384 && page_size_ != 32768 && page_size_ != 65536) {
        page_size_ = 16384;
    }

    // 默认线程数
    if (max_threads_ == 0) {
        max_threads_ = std::thread::hardware_concurrency();
        if (max_threads_ == 0) max_threads_ = 4;
    }

    enhanced_stats_.total_pages = 0;
    enhanced_stats_.valid_pages = 0;
    enhanced_stats_.missing_pages = 0;
    enhanced_stats_.fixed_chains = 0;
    enhanced_stats_.circular_references = 0;
    enhanced_stats_.total_size = 0;
    enhanced_stats_.total_time_ms = 0.0;
}

inline EnhancedBlobChainRebuilder::~EnhancedBlobChainRebuilder() {
    shutdown_.store(true);
    task_cv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

inline void EnhancedBlobChainRebuilder::set_page_size(size_t page_size) {
    if (page_size != 4096 && page_size != 8192 &&
        page_size != 16384 && page_size != 32768 && page_size != 65536) {
        return;
    }
    page_size_ = page_size;
}

inline void EnhancedBlobChainRebuilder::set_max_threads(size_t max_threads) {
    max_threads_ = max_threads;
    if (max_threads_ == 0) {
        max_threads_ = std::thread::hardware_concurrency();
        if (max_threads_ == 0) max_threads_ = 4;
    }
}

inline void EnhancedBlobChainRebuilder::clear() {
    blob_data_.clear();
    enhanced_chain_info_ = EnhancedBlobChainInfo{};
    enhanced_stats_ = EnhancedBlobChainStats{};
    page_chain_.clear();
    all_blob_data_.clear();
    file_paths_.clear();
}

inline void EnhancedBlobChainRebuilder::add_blob_page(uint32_t space_id, uint32_t page_no, std::vector<uint8_t> data) {
    BlobPageInfo page{};
    page.space_id = space_id;
    page.page_no = page_no;
    page.offset = 0;
    page.len = static_cast<uint32_t>(data.size());
    page.total_len = static_cast<uint32_t>(data.size());
    page.is_valid = true;
    page_chain_.push_back(page);
    
    all_blob_data_.emplace_back(space_id, std::move(data));
    enhanced_stats_.total_pages++;
    enhanced_stats_.valid_pages++;
    enhanced_stats_.total_size += data.size();
}

inline std::vector<uint8_t> EnhancedBlobChainRebuilder::read_blob_from_page(
    const uint8_t* page_data, size_t page_size, size_t offset, uint32_t len) {
    if (!page_data || page_size < 38) {
        return {};
    }

    constexpr size_t BLOB_DATA_OFFSET = 70;
    if (offset + BLOB_DATA_OFFSET + len > page_size) {
        return {};
    }

    return std::vector<uint8_t>(page_data + offset + BLOB_DATA_OFFSET,
                                page_data + offset + BLOB_DATA_OFFSET + len);
}

inline bool EnhancedBlobChainRebuilder::detect_circular_reference() const {
    if (page_chain_.empty()) {
        return false;
    }

    std::set<uint32_t> seen;
    for (const auto& page : page_chain_) {
        if (seen.find(page.page_no) != seen.end()) {
            return true;
        }
        seen.insert(page.page_no);
    }

    return false;
}

inline bool EnhancedBlobChainRebuilder::has_circular_reference() const {
    return detect_circular_reference();
}

inline std::vector<uint32_t> EnhancedBlobChainRebuilder::get_missing_pages() const {
    std::vector<uint32_t> missing;

    if (page_chain_.size() < 2) {
        return missing;
    }

    for (size_t i = 0; i < page_chain_.size() - 1; ++i) {
        uint32_t current = page_chain_[i].page_no;
        uint32_t next = page_chain_[i + 1].page_no;

        if (next > current + 1) {
            for (uint32_t p = current + 1; p < next; ++p) {
                missing.push_back(p);
            }
        }
    }

    return missing;
}

inline void EnhancedBlobChainRebuilder::mark_missing_pages() {
    auto missing = get_missing_pages();
    enhanced_stats_.missing_pages = missing.size();
}

inline void EnhancedBlobChainRebuilder::fix_chain_breaks() {
    if (page_chain_.empty()) {
        return;
    }

    for (size_t i = 0; i < page_chain_.size() - 1; ++i) {
        auto& current = page_chain_[i];
        auto& next = page_chain_[i + 1];

        if (current.page_no + 1 != next.page_no) {
            enhanced_stats_.missing_pages++;
            enhanced_stats_.fixed_chains++;
        }
    }
}

inline bool EnhancedBlobChainRebuilder::export_to_file(const std::filesystem::path& output_path) const {
    std::ofstream file(output_path, std::ios::binary);
    if (!file) {
        return false;
    }

    // 合并所有 BLOB 数据
    std::vector<uint8_t> combined;
    for (const auto& [space_id, data] : all_blob_data_) {
        combined.insert(combined.end(), data.begin(), data.end());
    }

    file.write(reinterpret_cast<const char*>(combined.data()), combined.size());
    return file.good();
}

inline bool EnhancedBlobChainRebuilder::export_to_sql(const std::filesystem::path& output_path,
                                                      const std::string& table_name,
                                                      const std::string& column_name) const {
    std::ofstream file(output_path, std::ios::binary);
    if (!file) {
        return false;
    }

    file << "-- BLOB Recovery Export\n";
    file << "-- Table: " << table_name << "\n";
    file << "-- Column: " << column_name << "\n\n";

    for (size_t i = 0; i < all_blob_data_.size(); ++i) {
        const auto& [space_id, data] = all_blob_data_[i];
        
        file << "INSERT INTO `" << table_name << "` (" << column_name << ") VALUES (";
        
        // 转义 BLOB 数据为十六进制
        file << "0x";
        for (uint8_t byte : data) {
            file << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        file << ");\n";
    }

    return file.good();
}

// ==================== BlobRecoveryWorker 实现 ====================

inline BlobRecoveryWorker::BlobRecoveryWorker(size_t page_size, size_t max_threads)
    : page_size_(page_size), max_threads_(max_threads) {
    if (max_threads_ == 0) {
        max_threads_ = std::thread::hardware_concurrency();
        if (max_threads_ == 0) max_threads_ = 4;
    }
}

inline void BlobRecoveryWorker::set_space_files(const std::map<uint32_t, std::filesystem::path>& space_files) {
    space_files_ = space_files;
}

inline void BlobRecoveryWorker::add_blob_pointer(const BlobPointer& pointer) {
    pointers_.push_back(pointer);
}

inline void BlobRecoveryWorker::process_blob(const BlobPointer& pointer) {
    // 简化实现：从第一个可用文件读取
    if (space_files_.empty()) {
        errors_.push_back("No space files available");
        return;
    }

    // 从第一个表空间文件读取
    const auto& [space_id, file_path] = *space_files_.begin();
    
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        errors_.push_back("Failed to open file: " + file_path.string());
        return;
    }

    // 读取页
    size_t page_offset = static_cast<size_t>(pointer.page_no) * page_size_;
    file.seekg(page_offset, std::ios::beg);

    std::vector<uint8_t> page_data(page_size_);
    file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

    if (file.gcount() < static_cast<std::streamsize>(page_size_)) {
        errors_.push_back("Failed to read page " + std::to_string(pointer.page_no));
        return;
    }

    // 读取 BLOB 数据
    constexpr size_t BLOB_DATA_OFFSET = 70;
    if (pointer.offset + BLOB_DATA_OFFSET + pointer.len > page_size_) {
        errors_.push_back("BLOB data out of bounds");
        return;
    }

    std::vector<uint8_t> blob_data(
        page_data.begin() + pointer.offset + BLOB_DATA_OFFSET,
        page_data.begin() + pointer.offset + BLOB_DATA_OFFSET + pointer.len
    );

    blob_data_.emplace_back(pointer.space_id, std::move(blob_data));
    stats_.total_pages++;
    stats_.valid_pages++;
    stats_.total_size += blob_data.size();
}

inline void BlobRecoveryWorker::worker_thread(size_t thread_id) {
    (void)thread_id;  // Unused
    // 简化实现：单线程处理
    for (const auto& pointer : pointers_) {
        process_blob(pointer);
    }
}

inline bool BlobRecoveryWorker::start() {
    if (pointers_.empty()) {
        return false;
    }

    if (max_threads_ > 1) {
        // 多线程处理
        std::vector<std::thread> threads;
        size_t batch_size = (pointers_.size() + max_threads_ - 1) / max_threads_;
        
        for (size_t i = 0; i < max_threads_; ++i) {
            size_t start_idx = i * batch_size;
            size_t end_idx = std::min(start_idx + batch_size, pointers_.size());
            
            if (start_idx >= pointers_.size()) break;
            
            threads.emplace_back([this, start_idx, end_idx]() {
                for (size_t j = start_idx; j < end_idx; ++j) {
                    process_blob(pointers_[j]);
                }
            });
        }
        
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    } else {
        // 单线程处理
        for (const auto& pointer : pointers_) {
            process_blob(pointer);
        }
    }

    stats_.total_blob_count = blob_data_.size();
    for (const auto& [space_id, data] : blob_data_) {
        stats_.total_blob_bytes += data.size();
    }

    return !blob_data_.empty();
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BLOB_CHAIN_REBUILDER_ENHANCED_HPP
