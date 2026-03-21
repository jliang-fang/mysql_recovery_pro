#ifndef DBRECOVERY_UTILS_MATH_HELPER_HPP
#define DBRECOVERY_UTILS_MATH_HELPER_HPP

#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace dbrecovery {
namespace utils {

/**
 * @brief 阶乘计算辅助函数（C++20 兼容实现）
 *
 * 使用 std::factorial (C++20) 或预计算表作为后备
 */
template <typename T = uint64_t>
constexpr T factorial(T n) noexcept(false) {
    static_assert(std::is_integral_v<T>, "factorial requires integral type");
    
#if __cplusplus >= 202002L && defined(__cpp_lib_math_special_functions)
    // C++23: std::factorial is available
    return std::factorial<T>(n);
#else
    // Fallback to precomputed table for C++17/20 compatibility
    if (n > 20) throw std::overflow_error("factorial: input too large for 64-bit result");
    
    constexpr std::array<uint64_t, 21> table{1,1,2,6,24,120,720,5040,40320,362880,3628800,39916800,
                                             479001600,6227020800,87178291200,1307674368000,
                                             20922789888000,355687428096000,6402373705728000,
                                             121645100408832000,2432902008176640000};
    return static_cast<T>(table[n]);
#endif
}

}  // namespace utils
}  // namespace dbrecovery

#endif  // DBRECOVERY_UTILS_MATH_HELPER_HPP
