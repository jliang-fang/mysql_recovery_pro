#ifndef DBRECOVERY_UTILS_MATH_HELPER_HPP
#define DBRECOVERY_UTILS_MATH_HELPER_HPP

#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace dbrecovery {
namespace utils {

/**
 * @brief 阶乘计算辅助函数（使用预计算表，C++17/20 兼容）
 *
 * 使用预计算的阶乘表实现，避免依赖 C++23 的 std::factorial。
 * 支持 n <= 20 (uint64_t 最大安全范围)。
 */
template <typename T = uint64_t>
constexpr T factorial(T n) noexcept(false) {
    static_assert(std::is_integral_v<T>, "factorial requires integral type");
    
    // 预计算阶乘表（C++17 constexpr 支持）
    constexpr std::array<uint64_t, 21> factorial_table = []() noexcept {
        std::array<uint64_t, 21> table{};
        table[0] = 1;
        for (uint32_t i = 1; i <= 20; ++i) {
            table[i] = table[i-1] * static_cast<uint64_t>(i);
        }
        return table;
    }();
    
    if (n < 0 || n > 20) {
        throw std::overflow_error("factorial: n must be in range [0, 20]");
    }
    
    return factorial_table[static_cast<size_t>(n)];
}

}  // namespace utils
}  // namespace dbrecovery

#endif  // DBRECOVERY_UTILS_MATH_HELPER_HPP
