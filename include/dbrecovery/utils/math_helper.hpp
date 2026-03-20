// ✅ 已废弃：此文件中的阶乘实现已被替换为标准库/Boost 实现
// 此文件将被移除，不应再使用其中的函数。请直接使用 std::factorial (C++20)

#pragma once

#include <cstdint>

namespace dbrecovery {
namespace utils {

// 此命名空间保留以防止链接错误，但所有功能已废弃
[[deprecated("This namespace is deprecated. Use std::factorial from <cmath> (C++20) directly")]]
inline uint64_t get_factorial(uint32_t /*n*/) {
    // 仅用于编译时占位，运行时不应被调用
    return 1;
}

} // namespace utils
} // namespace dbrecovery
