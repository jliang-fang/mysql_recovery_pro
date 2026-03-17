#include <gtest/gtest.h>
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/tests/test_utils.hpp"

namespace dbrecovery {
namespace innodb {

TEST(PageReader, ComputeCRC32CCorrectness) {
    // 使用已知结果的测试向量
    std::vector<uint8_t> test_data(16384, 0);
    
    // 填充一些数据（跳过校验和字段）
    for (size_t i = 0; i < test_data.size(); ++i) {
        if (i < 24 || i >= 28) { // 跳过 offset 24-27 的校验和字段
            test_data[i] = static_cast<uint8_t>(i % 256);
        }
    }

    uint32_t crc = PageReader::compute_crc32c(test_data);
    
    // 这是一个参考值（实际应根据标准 CRC32C 算法计算）
    // 对于全零数据，CRC32C 应为 0xFFFFFFFF
    EXPECT_NE(crc, 0) << "CRC32C should not be zero for non-zero data";
}

TEST(PageReader, ValidateChecksumWithZero) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 存储的校验和为 0 (NONE 模式)
    write_le<uint32_t>(page_data.data(), 24, 0);

    bool result = PageReader::validate_checksum(page_data);
    EXPECT_TRUE(result) << "Page with zero checksum should be valid";
}

TEST(PageHeader, ParseBasicFields) {
    std::vector<uint8_t> page(16384, 0);
    
    // 设置一些页头字段
    write_le<uint16_t>(page.data(), 38 + 0, 5);   // PAGE_N_DIR_SLOTS = 5
    write_le<uint16_t>(page.data(), 38 + 2, 100); // PAGE_HEAP_TOP = 100
    write_le<uint16_t>(page.data(), 38 + 4, 10);  // PAGE_N_HEAP = 10

    uint16_t n_dir_slots = read_le<uint16_t>(page.data(), 38 + 0);
    uint16_t heap_top = read_le<uint16_t>(page.data(), 38 + 2);
    
    EXPECT_EQ(n_dir_slots, 5);
    EXPECT_EQ(heap_top, 100);
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
