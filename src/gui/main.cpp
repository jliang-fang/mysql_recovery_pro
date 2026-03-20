#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/utils/math_helper.hpp"
#include <cstring>
#include <cmath>

namespace dbrecovery::innodb {

// ✅ 显式实例化模板（避免链接错误）
template uint32_t MachRead::read_le<std::uint32_t>(std::span<const std::uint8_t>, size_t);
template uint64_t MachRead::read_le<std::uint64_t>(std::span<const std::uint8_t>, size_t);

// ✅ 模板实现（确保跨平台字节序安全）
template<typename T>
T MachRead::read_le(std::span<const uint8_t> data, size_t offset) {
  static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "Only integral types supported");
  if (offset + sizeof(T) > data.size()) {
    throw std::out_of_range("MachRead: read_le out of range");
  }
  const uint8_t* ptr = data.data() + offset;
  T val = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    val |= static_cast<T>(ptr[i]) << (8 * i);
  }
  return val;
}

} // namespace dbrecovery::innodb

// ==================== 单元测试入口（供 GTest 调用）====================
#ifdef DBRECOVERY_ENABLE_TESTS
#include <gtest/gtest.h>
#include "dbrecovery/utils/math_helper.hpp"

namespace dbrecovery {
namespace utils {

TEST(MathHelperTest, FactorialZero) {
    EXPECT_EQ(factorial(0), 1);
}

TEST(MathHelperTest, FactorialSmall) {
    EXPECT_EQ(factorial(5), 120);
    EXPECT_EQ(factorial(10), 3628800);
}

TEST(MathHelperTest, FactorialLarge) {
    EXPECT_EQ(factorial(20), 2432902008176640000ULL);
}

TEST(MathHelperTest, FactorialOverflow) {
    EXPECT_THROW(factorial(21), std::overflow_error);
}

} // namespace utils
} // namespace dbrecovery

#endif // DBRECOVERY_ENABLE_TESTS
