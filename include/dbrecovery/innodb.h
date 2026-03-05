#pragma once

#include <cstdint>
#include <string>
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"

namespace dbrecovery {
namespace innodb {

// ✅ 已移除：RecoveryCache 现在统一定义于 recovery_cache.hpp
// 此文件仅保留全局工具函数（如页大小常量、类型转换）

} // namespace innodb
} // namespace dbrecovery
