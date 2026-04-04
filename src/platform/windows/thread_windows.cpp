/**
 * @file platform/windows/thread_windows.cpp
 * @brief Windows线程管理实现
 */

#include "dbrecovery/common/environment_compat.hpp"
#include <windows.h>
#include <process.h>
#include <thread>
#include <atomic>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>

namespace dbrecovery {
namespace env {

// ==================== Windows线程管理实现 ====================

uint32_t ThreadManager::cpu_cores_impl() const {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<uint32_t>(si.dwNumberOfProcessors);
}

uint64_t ThreadManager::current_thread_id() const {
    return static_cast<uint64_t>(GetCurrentThreadId());
}

std::string ThreadManager::thread_name() const {
    // Windows没有直接获取线程名称的API
    return "Thread-" + std::to_string(current_thread_id());
}

void ThreadManager::sleep_ms(uint32_t milliseconds) {
    ::Sleep(milliseconds);
}

void ThreadManager::sleep_us(uint32_t microseconds) {
    ::Sleep(microseconds / 1000);
}

void ThreadManager::set_thread_priority(int priority) {
    // priority: -2 (lowest) to 2 (highest)
    int win_priority = THREAD_PRIORITY_NORMAL;
    switch (priority) {
        case -2: win_priority = THREAD_PRIORITY_LOWEST; break;
        case -1: win_priority = THREAD_PRIORITY_BELOW_NORMAL; break;
        case 0:  win_priority = THREAD_PRIORITY_NORMAL; break;
        case 1:  win_priority = THREAD_PRIORITY_ABOVE_NORMAL; break;
        case 2:  win_priority = THREAD_PRIORITY_HIGHEST; break;
    }
    SetThreadPriority(GetCurrentThread(), win_priority);
}

int ThreadManager::get_thread_priority() const {
    int win_priority = GetThreadPriority(GetCurrentThread());
    switch (win_priority) {
        case THREAD_PRIORITY_LOWEST: return -2;
        case THREAD_PRIORITY_BELOW_NORMAL: return -1;
        case THREAD_PRIORITY_NORMAL: return 0;
        case THREAD_PRIORITY_ABOVE_NORMAL: return 1;
        case THREAD_PRIORITY_HIGHEST: return 2;
        default: return 0;
    }
}

// ==================== Windows线程池实现 ====================
/**
 * @brief Windows线程池实现
 */
class WindowsThreadPool : public ThreadManager::ThreadPool {
public:
    WindowsThreadPool(uint32_t num_threads) : running_(true) {
        for (uint32_t i = 0; i < num_threads; i++) {
            workers_.emplace_back([this] {
                this->worker_function();
            });
        }
    }

    ~WindowsThreadPool() override {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            running_ = false;
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void enqueue(std::function<void()> task) override {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            tasks_.push(std::move(task));
        }
        condition_.notify_one();
    }

    void wait_all() override {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        condition_.wait(lock, [this] {
            return tasks_.empty() && active_tasks_ == 0;
        });
    }

    uint32_t thread_count() const override {
        return static_cast<uint32_t>(workers_.size());
    }

private:
    void worker_function() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                condition_.wait(lock, [this] {
                    return !running_ || !tasks_.empty();
                });

                if (!running_ && tasks_.empty()) {
                    return;
                }

                task = std::move(tasks_.front());
                tasks_.pop();
                active_tasks_++;
            }

            task();

            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                active_tasks_--;
            }
            condition_.notify_all();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> running_;
    std::atomic<uint32_t> active_tasks_{0};
};

// ==================== 线程管理类实现 ====================

ThreadManager::ThreadManager() : cpu_cores_(cpu_cores_impl()) {
    // 初始化
}

ThreadManager& ThreadManager::instance() {
    static ThreadManager instance_;
    return instance_;
}

uint32_t ThreadManager::cpu_cores() const {
    return cpu_cores_;
}

std::unique_ptr<ThreadManager::ThreadPool> ThreadManager::create_thread_pool(uint32_t num_threads) {
    return std::make_unique<WindowsThreadPool>(num_threads);
}

// 创建线程（使用std::thread）
ThreadManager::ThreadHandle ThreadManager::create_thread(std::function<void()> func) {
    auto thread = std::make_unique<std::thread>(std::move(func));
    return thread;
}

void ThreadManager::join_thread(ThreadHandle handle) {
    if (handle && handle->joinable()) {
        handle->join();
    }
}

} // namespace env
} // namespace dbrecovery
