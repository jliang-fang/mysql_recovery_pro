// Test cases for page checksum validation
// Tests for P1-5: Page checksum validation

#include <gtest/gtest.h>
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== 页校验和验证测试 ====================

// Test Case 1: CRC32C 校验和验证（MySQL 8.0）
TEST(PageChecksumTest, CRC32CValidation) {
    std::vector<uint8_t> page_data(16384, 0);

    // 设置页类型为 INDEX
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);

    // 设置页级别为 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // 设置 flags
    write_le<uint16_t>(page_data.data(), 44, 0x0701);

    // 设置 heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // 设置 n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // 设置 INFIMUM 记录
    page_data[54] = REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 99);

    // 设置 SUPREMUM 记录
    page_data[97] = REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 98, 0xFFFF);

    // 设置 n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // 计算 CRC32C 校验和
    std::vector<uint8_t> page_copy = page_data;
    for (int i = 0; i < 4; ++i) {
        page_copy[24 + i] = 0;
    }

    uint32_t computed_crc = PageReader::compute_crc32c(page_copy);

    // 写入校验和
    for (int i = 0; i < 4; ++i) {
        page_data[24 + i] = (computed_crc >> (i * 8)) & 0xFF;
    }

    // 验证校验和
    uint32_t stored_crc = 0, computed = 0;
    bool valid = PageReader::validate_checksum(page_data, true, &stored_crc, &computed);

    EXPECT_TRUE(valid);
    EXPECT_EQ(stored_crc, computed_crc);
    EXPECT_EQ(computed, computed_crc);
}

// Test Case 2: INNODB 校验和验证（MySQL 5.6/5.7）
TEST(PageChecksumTest, InnodbChecksumValidation) {
    std::vector<uint8_t> page_data(16384, 0);

    // 设置页类型为 INDEX
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);

    // 设置页级别为 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // 设置 flags
    write_le<uint16_t>(page_data.data(), 44, 0x0701);

    // 设置 heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // 设置 n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // 设置 INFIMUM 记录
    page_data[54] = REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 99);

    // 设置 SUPREMUM 记录
    page_data[97] = REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 98, 0xFFFF);

    // 设置 n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // 计算 INNODB 校验和
    std::vector<uint8_t> page_copy = page_data;
    for (int i = 0; i < 4; ++i) {
        page_copy[24 + i] = 0;
    }

    uint32_t computed_innodb = PageReader::compute_innodb_checksum(page_copy);

    // 写入校验和
    for (int i = 0; i < 4; ++i) {
        page_data[24 + i] = (computed_innodb >> (i * 8)) & 0xFF;
    }

    // 验证校验和
    uint32_t stored_crc = 0, computed = 0;
    bool valid = PageReader::validate_checksum(page_data, true, &stored_crc, &computed);

    EXPECT_TRUE(valid);
    EXPECT_EQ(stored_crc, computed_innodb);
    EXPECT_EQ(computed, computed_innodb);
}

// Test Case 3: OLD 校验和验证（MySQL 5.5/5.6）
TEST(PageChecksumTest, OldChecksumValidation) {
    std::vector<uint8_t> page_data(16384, 0);

    // 设置页类型为 INDEX
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);

    // 设置页级别为 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // 设置 flags
    write_le<uint16_t>(page_data.data(), 44, 0x0701);

    // 设置 heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // 设置 n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // 设置 INFIMUM 记录
    page_data[54] = REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 99);

    // 设置 SUPREMUM 记录
    page_data[97] = REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 98, 0xFFFF);

    // 设置 n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // 计算 OLD 校验和
    std::vector<uint8_t> page_copy = page_data;
    for (int i = 0; i < 4; ++i) {
        page_copy[24 + i] = 0;
    }

    uint32_t computed_old = PageReader::compute_old_checksum(page_copy);

    // 写入校验和
    for (int i = 0; i < 4; ++i) {
        page_data[24 + i] = (computed_old >> (i * 8)) & 0xFF;
    }

    // 验证校验和
    uint32_t stored_crc = 0, computed = 0;
    bool valid = PageReader::validate_checksum(page_data, true, &stored_crc, &computed);

    EXPECT_TRUE(valid);
    EXPECT_EQ(stored_crc, computed_old);
    EXPECT_EQ(computed, computed_old);
}

// Test Case 4: NONE 校验和模式（校验和为 0）
TEST(PageChecksumTest, NoneChecksumMode) {
    std::vector<uint8_t> page_data(16384, 0);

    // 设置页类型为 INDEX
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);

    // 设置页级别为 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // 设置 flags
    write_le<uint16_t>(page_data.data(), 44, 0x0701);

    // 设置 heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // 设置 n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // 设置 INFIMUM 记录
    page_data[54] = REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 99);

    // 设置 SUPREMUM 记录
    page_data[97] = REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 98, 0xFFFF);

    // 设置 n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // 校验和保持为 0（NONE 模式）

    // 验证校验和
    uint32_t stored_crc = 0, computed = 0;
    bool valid = PageReader::validate_checksum(page_data, true, &stored_crc, &computed);

    EXPECT_TRUE(valid);
    EXPECT_EQ(stored_crc, 0);
}

// Test Case 5: 校验和不匹配（严格模式）
TEST(PageChecksumTest, ChecksumMismatchStrictMode) {
    std::vector<uint8_t> page_data(16384, 0);

    // 设置页类型为 INDEX
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);

    // 设置页级别为 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // 设置 flags
    write_le<uint16_t>(page_data.data(), 44, 0x0701);

    // 设置 heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // 设置 n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // 设置 INFIMUM 记录
    page_data[54] = REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 99);

    // 设置 SUPREMUM 记录
    page_data[97] = REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 98, 0xFFFF);

    // 设置 n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // 写入错误的校验和
    write_le<uint32_t>(page_data.data(), 24, 0xDEADBEEF);

    // 验证校验和（严格模式）
    uint32_t stored_crc = 0, computed = 0;
    bool valid = PageReader::validate_checksum(page_data, true, &stored_crc, &computed);

    // 严格模式下，校验和不匹配应返回 false
    EXPECT_FALSE(valid);
}

// Test Case 6: 校验和不匹配（非严格模式）
TEST(PageChecksumTest, ChecksumMismatchNonStrictMode) {
    std::vector<uint8_t> page_data(16384, 0);

    // 设置页类型为 INDEX
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);

    // 设置页级别为 0
    write_le<uint16_t>(page_data.data(), 42, 0);

    // 设置 flags
    write_le<uint16_t>(page_data.data(), 44, 0x0701);

    // 设置 heap_top
    write_le<uint16_t>(page_data.data(), 40, 100);

    // 设置 n_dir_slots
    write_le<uint16_t>(page_data.data(), 38, 2);

    // 设置 INFIMUM 记录
    page_data[54] = REC_STATUS_INFIMUM;
    write_le<uint16_t>(page_data.data(), 55, 99);

    // 设置 SUPREMUM 记录
    page_data[97] = REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page_data.data(), 98, 0xFFFF);

    // 设置 n_heap
    write_le<uint16_t>(page_data.data(), 42, 4);

    // 写入错误的校验和
    write_le<uint32_t>(page_data.data(), 24, 0xDEADBEEF);

    // 验证校验和（非严格模式）
    uint32_t stored_crc = 0, computed = 0;
    bool valid = PageReader::validate_checksum(page_data, false, &stored_crc, &computed);

    // 非严格模式下，校验和不匹配应返回 true（继续处理）
    EXPECT_TRUE(valid);
}

// Test Case 7: SDI 页校验和验证
TEST(PageChecksumTest, SDIChecksumValidation) {
    std::vector<uint8_t> page_data(16384, 0);

    // 设置页类型为 SDI
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_SDI);

    // 设置 SDI meta header
    write_le<uint32_t>(page_data.data(), 38, 1);  // version
    write_le<uint32_t>(page_data.data(), 42, 0);  // type
    write_le<uint64_t>(page_data.data(), 46, 12345);  // table_id

    // SDI 页通常使用 NONE 校验和（0）
    write_le<uint32_t>(page_data.data(), 24, 0);

    // 验证校验和
    uint32_t stored_crc = 0, computed = 0;
    bool valid = PageReader::validate_checksum(page_data, true, &stored_crc, &computed);

    EXPECT_TRUE(valid);
}

// Test Case 8: 不同页大小的校验和验证
TEST(PageChecksumTest, DifferentPageSizeChecksum) {
    std::vector<uint32_t> page_sizes = {4096, 8192, 16384, 32768, 65536};

    for (auto page_size : page_sizes) {
        std::vector<uint8_t> page_data(page_size, 0);

        // 设置页类型为 INDEX
        write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);

        // 设置页级别为 0
        write_le<uint16_t>(page_data.data(), 42, 0);

        // 设置 flags
        write_le<uint16_t>(page_data.data(), 44, 0x0701);

        // 设置 heap_top
        write_le<uint16_t>(page_data.data(), 40, 100);

        // 设置 n_dir_slots
        write_le<uint16_t>(page_data.data(), 38, 2);

        // 设置 INFIMUM 记录
        page_data[54] = REC_STATUS_INFIMUM;
        write_le<uint16_t>(page_data.data(), 55, 99);

        // 设置 SUPREMUM 记录
        page_data[97] = REC_STATUS_SUPREMUM;
        write_le<uint16_t>(page_data.data(), 98, 0xFFFF);

        // 设置 n_heap
        write_le<uint16_t>(page_data.data(), 42, 4);

        // 计算 CRC32C 校验和
        std::vector<uint8_t> page_copy = page_data;
        for (int i = 0; i < 4; ++i) {
            page_copy[24 + i] = 0;
        }

        uint32_t computed_crc = PageReader::compute_crc32c(page_copy);

        // 写入校验和
        for (int i = 0; i < 4; ++i) {
            page_data[24 + i] = (computed_crc >> (i * 8)) & 0xFF;
        }

        // 验证校验和
        uint32_t stored_crc = 0, computed = 0;
        bool valid = PageReader::validate_checksum(page_data, true, &stored_crc, &computed);

        EXPECT_TRUE(valid) << "Failed for page size: " << page_size;
        EXPECT_EQ(stored_crc, computed_crc);
    }
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
