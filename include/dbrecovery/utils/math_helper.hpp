#ifndef DBRECOVERY_UTILS_MATH_HELPER_HPP
#define DBRECOVERY_UTILS_MATH_HELPER_HPP

#include <cstdint>
#include <array>
#include <cmath>  // C++20 std::factorial

namespace dbrecovery {
namespace utils {

/**
 * @brief 阶乘计算辅助函数（C++17 兼容实现）
 *
 * 在 C++20 可用时自动切换到 std::factorial
 */
#if __cplusplus >= 202002L && defined(__cpp_lib_math_special_functions)

template <typename T>
constexpr T factorial(T n) noexcept {
    return std::factorial(n);
}

#else  // C++17 兼容实现

/**
 * @brief 预计算阶乘表（支持 0! 到 20!）
 *
 * 20! 是 uint64_t 能表示的最大阶乘值
 */
constexpr std::array<uint64_t, 21> factorial_table = []() noexcept {
    std::array<uint64_t, 21> table{};
    table[0] = 1;
    for (uint32_t i = 1; i <= 20; ++i) {
        table[i] = table[i-1] * i;
    }
    return table;
}();

inline uint64_t factorial(uint32_t n) noexcept {
    return factorial_table[n];
}

#endif  // C++20 检测

}  // namespace utils
}  // namespace dbrecovery

#endif  // DBRECOVERY_UTILS_MATH_HELPER_HPP
