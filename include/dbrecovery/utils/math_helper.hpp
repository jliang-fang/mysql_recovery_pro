#ifndef DBRECOVERY_UTILS_MATH_HELPER_HPP
#define DBRECOVERY_UTILS_MATH_HELPER_HPP

#include <cstdint>
#include <array>
#include <stdexcept>
#include <type_traits>

namespace dbrecovery {
namespace utils {

/**
 * @brief 阶乘计算辅助函数（C++17 兼容实现）
 *
 * 在 C++20 可用时自动切换到 std::factorial
 */
#if __cplusplus >= 202002L && defined(__cpp_lib_math_special_functions)

template <typename T = uint64_t>
constexpr T factorial(T n) noexcept(false) {
    static_assert(std::is_integral_v<T>, "factorial requires integral type");
    if (n > 20) throw std::overflow_error("factorial: input too large for 64-bit result");
    return std::factorial(n);
}

#else  // C++17 兼容实现

/**
 * @brief 预计算阶乘表（支持 0! 到 20!）
 *
 * 20! 是 uint64_t 能表示的最大阶乘值
 */
template <typename T = uint64_t>
constexpr T factorial(T n) noexcept(false) {
    static_assert(std::is_integral_v<T>, "factorial requires integral type");
    if (n > 20) throw std::overflow_error("factorial: input too large for 64-bit result");
    
    constexpr std::array<uint64_t, 21> table{1,1,2,6,24,120,720,5040,40320,362880,3628800,39916800,479001600,6227020800,87178291200,1307674368000,20922789888000,355687428096000,6402373705728000,121645100408832000,2432902008176640000};
    return static_cast<T>(table[n]);
}

#endif  // C++20 检测

}  // namespace utils
}  // namespace dbrecovery

#endif  // DBRECOVERY_UTILS_MATH_HELPER_HPP
