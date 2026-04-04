/**
 * @file platform/macos/thread_macos.cpp
 * @brief macOS线程管理实现
 */

#include "dbrecovery/common/environment_compat.hpp"
#include <pthread.h>
#include <unistd.h>
#include <sys/sysctl.h>
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

// ==================== macOS线程管理实现 ====================

uint32_t ThreadManager::cpu_cores_impl() const {
    int mib[2] = {CTL_HW, HW_NCPU};
    int cores = 0;
    size_t len = sizeof(cores);
    sysctl(mib, 2, &cores, &len, nullptr, 0);
    if (cores > 0) return static_cast<uint32_t>(cores);
    return 1;
}

uint64_t ThreadManager::current_thread_id() const {
    uint64_t tid;
    pthread_threadid_np(pthread_self(), &tid);
    return tid;
}

std::string ThreadManager::thread_name() const {
    char name[64];
    pthread_getname_np(pthread_self(), name, sizeof(name));
    return std::string(name);
}

void ThreadManager::sleep_ms(uint32_t milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, nullptr);
}

void ThreadManager::sleep_us(uint32_t microseconds) {
    struct timespec ts;
    ts.tv_sec = microseconds / 1000000;
    ts.tv_nsec = (microseconds % 1000000) * 1000;
    nanosleep(&ts, nullptr);
}

void ThreadManager::set_thread_priority(int priority) {
    // priority: -2 (lowest) to 2 (highest)
    int policy;
    struct sched_param param;
    pthread_getschedparam(pthread_self(), &policy, &param);
    
    switch (priority) {
        case -2: param.sched_priority = sched_get_priority_min(policy); break;
        case -1: param.sched_priority = sched_get_priority_min(policy) + 1; break;
        case 0:  param.sched_priority = (sched_get_priority_min(policy) + sched_get_priority_max(policy)) / 2; break;
        case 1:  param.sched_priority = sched_get_priority_max(policy) - 1; break;
        case 2:  param.sched_priority = sched_get_priority_max(policy); break;
    }
    pthread_setschedparam(pthread_self(), policy, &param);
}

int ThreadManager::get_thread_priority() const {
    int policy;
    struct sched_param param;
    pthread_getschedparam(pthread_self(), &policy, &param);
    
    int min = sched_get_priority_min(policy);
    int max = sched_get_priority_max(policy);
    int mid = (min + max) / 2;
    
    if (param.sched_priority <= min + 1) return -2;
    if (param.sched_priority <= min + 2) return -1;
    if (param.sched_priority >= max - 1) return 2;
    if (param.sched_priority >= max - 2) return 1;
    return 0;
}

// ==================== macOS线程池实现 ====================
/**
 * @brief macOS线程池实现
 */
class macOSThreadPool : public ThreadManager::ThreadPool {
public:
    macOSThreadPool(uint32_t num_threads) : running_(true) {
        for (uint32_t i = 0; i < num_threads; i++) {
            workers_.emplace_back([this] {
                this->worker_function();
            });
        }
    }

    ~macOSThreadPool() override {
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
    return std::make_unique<macOSThreadPool>(num_threads);
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
