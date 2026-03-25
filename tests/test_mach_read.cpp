#include <gtest/gtest.h>
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// 测试 sint24 负值解析（P0-1 修复）
TEST(MachReadTest, SignedInt24NegativeValue) {
    uint8_t data[3] = {0xFF, 0xFF, 0xFF}; // -1 in 24-bit two's complement
    
    auto val = read_le<int32_t>(data);
    EXPECT_EQ(val, -1);
}

// 测试 sint24 正值解析
TEST(MachReadTest, SignedInt24PositiveValue) {
    uint8_t data[3] = {0x7F, 0xFF, 0x00}; // 32767 in 24-bit
    
    auto val = read_le<int32_t>(data);
    EXPECT_EQ(val, 32767);
}

// 测试 uint24 正值解析（确保无符号类型不受影响）
TEST(MachReadTest, UnsignedInt24PositiveValue) {
    uint8_t data[3] = {0xFF, 0xFF, 0x00}; // 65535 in 24-bit
    
    auto val = read_le<uint32_t>(data);
    EXPECT_EQ(val, 65535);
}

// 测试 sint64 负值解析
TEST(MachReadTest, SignedInt64NegativeValue) {
    uint8_t data[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // -1
    
    auto val = read_le<int64_t>(data);
    EXPECT_EQ(val, -1);
}

// 测试 sint16 负值解析
TEST(MachReadTest, SignedInt16NegativeValue) {
    uint8_t data[2] = {0xFF, 0xFF}; // -1
    
    auto val = read_le<int16_t>(data);
    EXPECT_EQ(val, -1);
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
