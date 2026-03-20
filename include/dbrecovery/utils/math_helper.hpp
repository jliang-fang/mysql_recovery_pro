#pragma once

#include <array>
#include <stdexcept>
#include <cstdint>

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
 * @brief 计算阶乘（使用查表法）
 *
 * ✅ 优化：直接返回查表结果，避免函数调用开销
 * 适用于 n <= 20 (uint64_t 范围内)
 * 
 * @param n 非负整数
 * @return uint64_t n! 的值
 * @throws std::out_of_range 当 n > 20 时抛出异常
 */
inline uint64_t get_factorial(uint32_t n) {
    if (n >= FACTORIAL_TABLE.size()) {
        throw std::out_of_range("Factorial is not defined for n > 20");
    }
    return FACTORIAL_TABLE[n];
}

} // namespace utils
} // namespace dbrecovery
