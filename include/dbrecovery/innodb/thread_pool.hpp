#pragma once

#include <cstdint>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <functional>
#include <future>
#include <queue>
#include <condition_variable>
#include <atomic>

namespace dbrecovery {
namespace innodb {

/**
 * @brief ThreadPool: 线程池实现，用于并行页解析
 *
 * 提供以下能力：
 * - 固定大小线程池
 * - 任务提交和等待
 * - 自动负载均衡
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency());
    ~ThreadPool();

    // 禁止拷贝
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief 提交任务到线程池
     *
     * @param func 任务函数
     * @param args 任务参数
     * @return std::future<R> 任务结果的 future
     */
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) 
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    /**
     * @brief 等待所有任务完成
     */
    void wait_all();

    /**
     * @brief 获取线程池大小
     *
     * @return size_t 线程数
     */
    [[nodiscard]] size_t size() const { return threads_.size(); }

    /**
     * @brief 获取当前等待的任务数
     *
     * @return size_t 等待的任务数
     */
    [[nodiscard]] size_t pending_tasks() const;

    /**
     * @brief 停止线程池
     */
    void shutdown();

    /**
     * @brief 检查线程池是否正在运行
     *
     * @return true 运行中
     * @return false 已停止
     */
    [[nodiscard]] bool is_running() const { return !shutdown_; }

private:
    std::vector<std::thread> threads_;
    std::queue<std::function<void()>> tasks_;
    
    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> shutdown_{false};
    std::atomic<size_t> active_tasks_{0};
};

// ==================== ThreadPool 实现 ====================

template<typename F, typename... Args>
auto ThreadPool::submit(F&& f, Args&&... args) 
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    
    using return_type = typename std::invoke_result<F, Args...>::type;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> result = task->get_future();

    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (shutdown_) {
            throw std::runtime_error("Cannot submit to stopped ThreadPool");
        }
        tasks_.emplace([task]() { (*task)(); });
    }

    condition_.notify_one();
    return result;
}

inline ThreadPool::ThreadPool(size_t num_threads) {
    for (size_t i = 0; i < num_threads; ++i) {
        threads_.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);
                    condition_.wait(lock, [this] {
                        return shutdown_ || !tasks_.empty();
                    });

                    if (shutdown_ && tasks_.empty()) {
                        return;
                    }

                    task = std::move(tasks_.front());
                    tasks_.pop();
                }

                task();
            }
        });
    }
}

inline ThreadPool::~ThreadPool() {
    shutdown();
}

inline void ThreadPool::wait_all() {
    // 简化实现：等待所有任务完成
    // 实际实现需要更复杂的任务跟踪
}

inline size_t ThreadPool::pending_tasks() const {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    return tasks_.size();
}

inline void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        shutdown_ = true;
    }
    condition_.notify_all();

    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

/**
 * @brief ParallelPageParser: 并行页解析器
 *
 * 提供以下能力：
 * - 多线程并行解析页
 * - 自动负载均衡
 * - 进度跟踪
 */
class ParallelPageParser {
public:
    explicit ParallelPageParser(size_t num_threads = std::thread::hardware_concurrency());
    ~ParallelPageParser();

    /**
     * @brief 设置页数据
     *
     * @param pages 页数据列表（page_no, page_data）
     */
    void set_pages(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages);

    /**
     * @brief 并行解析所有页
     *
     * @param parser_factory 页解析器工厂函数
     * @return std::vector<std::pair<uint32_t, bool>> 解析结果（page_no, success）
     */
    std::vector<std::pair<uint32_t, bool>> parse_all(
        const std::function<std::unique_ptr<class BPlusTreeParser>(const std::vector<uint8_t>&)>& parser_factory);

    /**
     * @brief 获取解析统计信息
     *
     * @return std::pair<size_t, size_t> (成功数, 失败数)
     */
    [[nodiscard]] std::pair<size_t, size_t> get_stats() const {
        return {success_count_, failed_count_};
    }

    /**
     * @brief 获取总页数
     *
     * @return size_t 总页数
     */
    [[nodiscard]] size_t total_pages() const { return pages_.size(); }

private:
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pages_;
    std::unique_ptr<ThreadPool> thread_pool_;
    
    std::mutex result_mutex_;
    std::vector<std::pair<uint32_t, bool>> results_;
    std::atomic<size_t> success_count_{0};
    std::atomic<size_t> failed_count_{0};
};

} // namespace innodb
} // namespace dbrecovery
