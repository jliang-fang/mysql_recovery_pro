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
 * @brief 计算阶乘（使用查表法实现）
 *
 * ✅ 重构：避免浮点精度损失，O(1) 时间复杂度
 * 适用于 n <= 20 (uint64_t 范围内)
 * 
 * @param n 非负整数
 * @return uint64_t n! 的值
 */
inline uint64_t factorial(uint32_t n) {
    if (n >= FACTORIAL_TABLE.size()) {
        throw std::overflow_error("Factorial result exceeds uint64_t range");
    }
    
    // ✅ 直接查表，无精度损失
    return FACTORIAL_TABLE[n];
}

} // namespace utils
} // namespace dbrecovery
