/**
 * @file platform/linux/thread_linux.cpp
 * @brief Linux线程管理实现
 */

#include "dbrecovery/common/environment_compat.hpp"
#include <pthread.h>
#include <unistd.h>
#include <cstring>

namespace dbrecovery {
namespace env {

// ==================== Linux线程管理实现 ====================

uint32_t ThreadManager::cpu_cores_impl() const {
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) return static_cast<uint32_t>(cores);
    return 1;
}

} // namespace env
} // namespace dbrecovery
