#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/utils/math_helper.hpp"
#include <cstring>
#include <cmath>

namespace dbrecovery::innodb {

// ✅ 显式实例化模板（避免链接错误）
template uint32_t read_le<std::uint32_t>(const uint8_t*, size_t);
template uint64_t read_le<std::uint64_t>(const uint8_t*, size_t);

} // namespace dbrecovery::innodb

// ==================== 单元测试入口（供 GTest 调用）====================
#ifdef DBRECOVERY_ENABLE_TESTS
#include <gtest/gtest.h>
#include "dbrecovery/utils/math_helper.hpp"

namespace dbrecovery::utils {

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

} // namespace dbrecovery::utils

#endif // DBRECOVERY_ENABLE_TESTS
