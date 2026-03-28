#ifndef DBRECOVERY_ENGINE_PARALLEL_SCANNER_HPP
#define DBRECOVERY_ENGINE_PARALLEL_SCANNER_HPP

#include <cstdint>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <functional>
#include <future>
#include <atomic>
#include <memory>

namespace dbrecovery {
namespace engine {

/**
 * @brief ParallelScanner: 并行扫描器
 *
 * 提供以下能力：
 * - 多线程并行处理页数据
 * - 工作队列管理
 * - 进度跟踪
 * - 错误处理
 */
class ParallelScanner {
public:
    /**
     * @brief 构造函数
     *
     * @param num_threads 线程数（0 表示自动检测）
     */
    explicit ParallelScanner(size_t num_threads = 0);
    ~ParallelScanner();

    // 禁止拷贝
    ParallelScanner(const ParallelScanner&) = delete;
    ParallelScanner& operator=(const ParallelScanner&) = delete;

    // 支持移动语义
    ParallelScanner(ParallelScanner&& other) noexcept;
    ParallelScanner& operator=(ParallelScanner&& other) noexcept;

    /**
     * @brief 设置工作函数
     *
     * @param worker 工作函数，返回处理结果
     */
    void set_worker(std::function<bool(uint32_t)> worker);

    /**
     * @brief 设置进度回调
     *
     * @param callback 进度回调函数
     */
    void set_progress_callback(std::function<void(size_t, size_t)> callback);

    /**
     * @brief 扫描指定范围的页
     *
     * @param start_page 起始页号
     * @param end_page 结束页号（不包含）
     * @return true 所有页处理成功
     * @return false 部分或全部页处理失败
     */
    bool scan(uint32_t start_page, uint32_t end_page);

    /**
     * @brief 扫描所有页
     *
     * @param total_pages 总页数
     * @return true 所有页处理成功
     * @return false 部分或全部页处理失败
     */
    bool scan_all(uint32_t total_pages);

    /**
     * @brief 获取处理统计信息
     */
    struct ScanStats {
        size_t total_pages = 0;       // 总页数
        size_t processed_pages = 0;   // 已处理页数
        size_t failed_pages = 0;      // 处理失败页数
        double elapsed_seconds = 0.0; // 耗时（秒）
        double pages_per_second = 0.0;// 每秒处理页数
    };

    ScanStats get_stats() const;

    /**
     * @brief 获取线程数
     */
    size_t get_thread_count() const;

    /**
     * @brief 停止扫描
     */
    void stop();

    /**
     * @brief 检查是否正在扫描
     */
    bool is_scanning() const;

private:
    // 工作线程函数
    void worker_thread();

    // 获取下一个待处理页
    bool get_next_page(uint32_t& page_no);

    // 更新统计信息
    void update_stats(uint32_t page_no, bool success);

private:
    size_t num_threads_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stop_requested_;
    std::atomic<bool> scanning_;
    
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<uint32_t> work_queue_;
    
    std::function<bool(uint32_t)> worker_;
    std::function<void(size_t, size_t)> progress_callback_;
    
    // 统计信息
    mutable std::mutex stats_mutex_;
    ScanStats stats_;
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<size_t> processed_count_;
    std::atomic<size_t> failed_count_;
};

/**
 * @brief ParallelScannerManager: 并行扫描器管理器（单例）
 */
class ParallelScannerManager {
public:
    /**
     * @brief 获取全局实例
     */
    static ParallelScannerManager& instance();

    /**
     * @brief 创建新的扫描器
     *
     * @param num_threads 线程数
     * @return std::unique_ptr<ParallelScanner> 扫描器
     */
    std::unique_ptr<ParallelScanner> create_scanner(size_t num_threads = 0);

    /**
     * @brief 设置默认线程数
     *
     * @param num_threads 线程数
     */
    void set_default_threads(size_t num_threads);

    /**
     * @brief 获取默认线程数
     */
    size_t get_default_threads() const;

private:
    ParallelScannerManager();
    ~ParallelScannerManager();

    size_t default_threads_;
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_PARALLEL_SCANNER_HPP
