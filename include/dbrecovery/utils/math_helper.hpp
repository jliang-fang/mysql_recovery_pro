// ✅ 已废弃：此文件中的阶乘实现已被替换为标准库/Boost 实现
// 保留此文件以防止编译错误，但不应再使用其中的函数

#pragma once

#include <array>
#include <stdexcept>
#include <cstdint>
#include <cmath> // for std::factorial (C++20)

namespace dbrecovery {
namespace utils {

#if __cplusplus >= 202100L
// ✅ C++20 及以上：使用标准库 std::factorial
[[deprecated("Use std::factorial (C++20) directly instead")]]
inline uint64_t get_factorial(uint32_t n) {
    if (n > 20) {
        throw std::out_of_range("Factorial is not defined for n > 20");
    }
    return static_cast<uint64_t>(std::factorial(n));
}
#else
// ✅ C++17 fallback：使用预计算表
[[deprecated("Use std::factorial (C++20) or boost::math::factorial instead")]]
static constexpr std::array<uint64_t, 21> FACTORIAL_TABLE = []() noexcept {
    std::array<uint64_t, 21> table{};
    table[0] = 1;
    for (uint32_t i = 1; i <= 20; ++i) {
        table[i] = table[i-1] * i;
    }
    return table;
}();

[[deprecated("Use std::factorial (C++20) or boost::math::factorial instead")]]
inline uint64_t get_factorial(uint32_t n) {
    if (n >= FACTORIAL_TABLE.size()) {
        throw std::out_of_range("Factorial is not defined for n > 20");
    }
    return FACTORIAL_TABLE[n];
}
#endif

} // namespace utils
} // namespace dbrecovery
