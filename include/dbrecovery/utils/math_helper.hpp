#pragma once

#include <cmath>
#include <stdexcept>
#include <cstdint>

namespace dbrecovery {
namespace utils {

/**
 * @brief 计算阶乘（使用 std::tgamma 函数实现）
 *
 * 适用于 n <= 20 (uint64_t 范围内)
 * 
 * @param n 非负整数
 * @return uint64_t n! 的值
 */
inline uint64_t factorial(uint32_t n) {
    if (n > 20) {
        throw std::overflow_error("Factorial result exceeds uint64_t range");
    }
    
    // 使用 std::tgamma(n+1) 计算阶乘，等价于 n!
    // std::tgamma(x) 计算的是 (x-1)!
    return static_cast<uint64_t>(std::tgamma(static_cast<double>(n + 1)));
}

} // namespace utils
} // namespace dbrecovery
