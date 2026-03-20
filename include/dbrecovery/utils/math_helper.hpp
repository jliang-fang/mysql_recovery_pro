#pragma once

#include <cmath>
#include <stdexcept>
#include <cstdint>

namespace dbrecovery {
namespace utils {

/**
 * @brief 计算阶乘（安全实现，使用循环）
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
    
    uint64_t result = 1;
    for (uint32_t i = 2; i <= n; ++i) {
        result *= i;
    }
    return result;
}

} // namespace utils
} // namespace dbrecovery
