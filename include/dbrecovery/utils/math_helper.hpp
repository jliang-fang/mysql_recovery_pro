#ifndef DBRECOVERY_UTILS_MATH_HELPER_HPP
#define DBRECOVERY_UTILS_MATH_HELPER_HPP

#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace dbrecovery {
namespace utils {

/**
 * @brief 阶乘计算辅助函数（使用 C++20 标准库 std::factorial）
 *
 * 依赖于 <cmath> 中的 std::factorial (自 C++23 起为 constexpr，C++20 可用)。
 * 此实现简洁、高效，并由标准库保证正确性。
 */
template <typename T = uint64_t>
constexpr T factorial(T n) noexcept(false) {
    static_assert(std::is_integral_v<T>, "factorial requires integral type");
    
    // C++20/23: std::factorial is available in <cmath>
    // It throws std::overflow_error for n > 20 (for uint64_t)
    return std::factorial<T>(n);
}

}  // namespace utils
}  // namespace dbrecovery

#endif  // DBRECOVERY_UTILS_MATH_HELPER_HPP
