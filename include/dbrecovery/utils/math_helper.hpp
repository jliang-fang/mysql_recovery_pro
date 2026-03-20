#pragma once

#include <array>
#include <stdexcept>
#include <cstdint>
#include <cmath> // For std::factorial in C++23

#if __cplusplus >= 202302L
#  include <numeric> // For std::factorial (C++23)
#endif

namespace dbrecovery {
namespace utils {

// ✅ 阶乘查表（uint64_t 最大支持 20!）
static constexpr std::array<uint64_t, 21> FACTORIAL_TABLE = []() noexcept {
    std::array<uint64_t, 21> table{};
    table[0] = 1;
    for (uint32_t i = 1; i <= 20; ++i) {
        table[i] = table[i-1] * i;
    }
    return table;
}();

/**
 * @brief 计算阶乘（使用查表法或 std::factorial 实现）
 *
 * ✅ 重构：优先使用 C++23 的 std::factorial，回退到查表法
 * 适用于 n <= 20 (uint64_t 范围内)
 * 
 * @param n 非负整数
 * @return uint64_t n! 的值
 */
inline uint64_t factorial(uint32_t n) {
    static_assert(FACTORIAL_TABLE.size() == 21, "FACTORIAL_TABLE must have 21 entries (0! to 20!)");
    
#if __cplusplus >= 202302L
    if constexpr (std::is_constant_evaluated()) {
        // 编译期计算使用查表法（std::factorial 在编译期可能不可用）
        return FACTORIAL_TABLE[n];
    } else {
        // 运行时优先使用 std::factorial
        return std::factorial(n);
    }
#else
    // C++23 之前使用查表法
    if (n >= FACTORIAL_TABLE.size()) {
        throw std::overflow_error("Factorial result exceeds uint64_t range (n > 20)");
    }
    return FACTORIAL_TABLE[n];
#endif
}

// ✅ 新增：get_factorial() 包装函数，以满足原始请求
inline uint64_t get_factorial(uint32_t n) {
    return factorial(n);
}

} // namespace utils
} // namespace dbrecovery
