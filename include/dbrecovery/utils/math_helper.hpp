#pragma once

#include <cmath>
#include <stdexcept>
#include <cstdint>

namespace dbrecovery {
namespace utils {

/**
 * @brief 计算阶乘（安全实现，使用 std::tgamma）
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
    
    // 使用 tgamma(n+1) 计算 n!
    return static_cast<uint64_t>(std::tgamma(static_cast<double>(n) + 1.0));
}

} // namespace utils
} // namespace dbrecovery
