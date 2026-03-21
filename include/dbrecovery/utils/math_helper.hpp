#ifndef DBRECOVERY_UTILS_MATH_HELPER_HPP
#define DBRECOVERY_UTILS_MATH_HELPER_HPP

#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace dbrecovery {
namespace utils {

/**
 * @brief 阶乘计算辅助函数（C++23 兼容实现）
 *
 * 使用 std::factorial (C++23) 或预计算表作为后备，确保跨版本兼容性。
 */
template <typename T = uint64_t>
constexpr T factorial(T n) noexcept(false) {
    static_assert(std::is_integral_v<T>, "factorial requires integral type");
    
#if __cplusplus >= 202302L && defined(__cpp_lib_math_special_functions)
    // C++23: std::factorial is available
    return std::factorial<T>(n);
#else
    // Fallback to precomputed table for C++17/20 compatibility
    if (n > 20) throw std::overflow_error("factorial: input too large for 64-bit result");
    
    static constexpr std::array<uint64_t, 21> factorial_table = []() {
        std::array<uint64_t, 21> table{};
        table[0] = 1;
        for (uint32_t i = 1; i <= 20; ++i) {
            table[i] = table[i-1] * i;
        }
        return table;
    }();
    
    return factorial_table[n];
#endif
}

}  // namespace utils
}  // namespace dbrecovery

#endif  // DBRECOVERY_UTILS_MATH_HELPER_HPP
