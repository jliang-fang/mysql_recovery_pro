/**
 * @file test_ext4_block_bitmap.cpp
 * @brief Unit tests for ext4 block bitmap validation
 *
 * Reference: testdisk ext2p.c patterns
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>

// Test helper functions inline (since ext4_recovery.hpp requires Linux)
namespace {

// Inline implementation of block bitmap functions for testing
bool is_block_free(const std::vector<uint8_t>& bitmap, uint32_t block_offset) {
    if (bitmap.empty() || block_offset >= bitmap.size() * 8) {
        return false;
    }
    uint32_t byte_index = block_offset / 8;
    uint32_t bit_index = block_offset % 8;
    return (bitmap[byte_index] & (1 << bit_index)) == 0;
}

bool is_block_used(const std::vector<uint8_t>& bitmap, uint32_t block_offset) {
    return !is_block_free(bitmap, block_offset);
}

uint32_t count_free_blocks(const std::vector<uint8_t>& bitmap, uint32_t blocks_per_group) {
    uint32_t free_count = 0;
    uint32_t blocks_to_check = std::min(blocks_per_group, static_cast<uint32_t>(bitmap.size() * 8));
    for (uint32_t i = 0; i < blocks_to_check; i++) {
        if (is_block_free(bitmap, i)) {
            free_count++;
        }
    }
    return free_count;
}

} // namespace

class Ext4BlockBitmapTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ==================== Block Free/Used Tests ====================

TEST_F(Ext4BlockBitmapTest, EmptyBitmap) {
    std::vector<uint8_t> empty_bitmap;
    EXPECT_FALSE(is_block_free(empty_bitmap, 0));
    EXPECT_FALSE(is_block_free(empty_bitmap, 100));
}

TEST_F(Ext4BlockBitmapTest, AllFreeBitmap) {
    // All bits 0 = all blocks free
    std::vector<uint8_t> bitmap(1024, 0);  // 8192 blocks

    EXPECT_TRUE(is_block_free(bitmap, 0));
    EXPECT_TRUE(is_block_free(bitmap, 100));
    EXPECT_TRUE(is_block_free(bitmap, 8191));

    EXPECT_FALSE(is_block_used(bitmap, 0));
    EXPECT_FALSE(is_block_used(bitmap, 100));
}

TEST_F(Ext4BlockBitmapTest, AllUsedBitmap) {
    // All bits 1 = all blocks used
    std::vector<uint8_t> bitmap(1024, 0xFF);  // 8192 blocks

    EXPECT_FALSE(is_block_free(bitmap, 0));
    EXPECT_FALSE(is_block_free(bitmap, 100));
    EXPECT_FALSE(is_block_free(bitmap, 8191));

    EXPECT_TRUE(is_block_used(bitmap, 0));
    EXPECT_TRUE(is_block_used(bitmap, 100));
}

TEST_F(Ext4BlockBitmapTest, SingleBlockUsed) {
    std::vector<uint8_t> bitmap(1024, 0);  // All free initially

    // Set block 100 as used
    bitmap[100 / 8] |= (1 << (100 % 8));

    EXPECT_TRUE(is_block_free(bitmap, 99));
    EXPECT_FALSE(is_block_free(bitmap, 100));  // Block 100 is now used
    EXPECT_TRUE(is_block_free(bitmap, 101));
}

TEST_F(Ext4BlockBitmapTest, BoundaryBlockUsed) {
    std::vector<uint8_t> bitmap(1024, 0);

    // Test boundary cases
    // Block 0
    bitmap[0] |= 0x01;
    EXPECT_FALSE(is_block_free(bitmap, 0));
    EXPECT_TRUE(is_block_free(bitmap, 1));

    // Block 7 (last bit of first byte)
    bitmap[0] |= 0x80;
    EXPECT_FALSE(is_block_free(bitmap, 7));

    // Block 8 (first bit of second byte)
    bitmap[1] |= 0x01;
    EXPECT_FALSE(is_block_free(bitmap, 8));
}

TEST_F(Ext4BlockBitmapTest, OutOfBounds) {
    std::vector<uint8_t> bitmap(10, 0);  // Only 80 blocks

    // In bounds
    EXPECT_TRUE(is_block_free(bitmap, 79));

    // Out of bounds - should return false (not free, treat as used)
    EXPECT_FALSE(is_block_free(bitmap, 80));
    EXPECT_FALSE(is_block_free(bitmap, 1000));
}

// ==================== Count Free Blocks Tests ====================

TEST_F(Ext4BlockBitmapTest, CountAllFree) {
    std::vector<uint8_t> bitmap(1024, 0);  // 8192 blocks all free

    uint32_t free_count = count_free_blocks(bitmap, 8192);
    EXPECT_EQ(free_count, 8192);
}

TEST_F(Ext4BlockBitmapTest, CountAllUsed) {
    std::vector<uint8_t> bitmap(1024, 0xFF);  // 8192 blocks all used

    uint32_t free_count = count_free_blocks(bitmap, 8192);
    EXPECT_EQ(free_count, 0);
}

TEST_F(Ext4BlockBitmapTest, CountMixed) {
    std::vector<uint8_t> bitmap(1024, 0);

    // Set first 100 blocks as used
    for (uint32_t i = 0; i < 100; i++) {
        bitmap[i / 8] |= (1 << (i % 8));
    }

    uint32_t free_count = count_free_blocks(bitmap, 8192);
    EXPECT_EQ(free_count, 8192 - 100);
}

TEST_F(Ext4BlockBitmapTest, CountPartialBitmap) {
    // Small bitmap with fewer blocks
    std::vector<uint8_t> bitmap(10, 0);  // 80 blocks

    // Set 20 blocks as used
    for (uint32_t i = 0; i < 20; i++) {
        bitmap[i / 8] |= (1 << (i % 8));
    }

    uint32_t free_count = count_free_blocks(bitmap, 80);
    EXPECT_EQ(free_count, 60);
}

TEST_F(Ext4BlockBitmapTest, CountWithLimit) {
    std::vector<uint8_t> bitmap(1024, 0);  // 8192 possible blocks

    // Count only first 1000 blocks
    uint32_t free_count = count_free_blocks(bitmap, 1000);
    EXPECT_EQ(free_count, 1000);  // All should be free
}

// ==================== Real-World Pattern Tests ====================

TEST_F(Ext4BlockBitmapTest, TypicalGroupPattern) {
    // Simulate a typical block group with some metadata blocks used
    // and some data blocks free
    std::vector<uint8_t> bitmap(512, 0);  // 4096 blocks

    // First few blocks are typically used (superblock, bitmaps, inode table)
    for (uint32_t i = 0; i < 50; i++) {
        bitmap[i / 8] |= (1 << (i % 8));
    }

    // Verify first 50 are used
    EXPECT_FALSE(is_block_free(bitmap, 0));
    EXPECT_FALSE(is_block_free(bitmap, 49));

    // Verify rest are free
    EXPECT_TRUE(is_block_free(bitmap, 50));
    EXPECT_TRUE(is_block_free(bitmap, 100));

    uint32_t free_count = count_free_blocks(bitmap, 4096);
    EXPECT_EQ(free_count, 4096 - 50);
}

TEST_F(Ext4BlockBitmapTest, DeletedFilePattern) {
    // Simulate blocks from a deleted file becoming free
    std::vector<uint8_t> bitmap(512, 0xFF);  // All used initially

    // Free blocks 100-199 (simulating file deletion)
    for (uint32_t i = 100; i < 200; i++) {
        bitmap[i / 8] &= ~(1 << (i % 8));
    }

    // Verify blocks before 100 are still used
    EXPECT_FALSE(is_block_free(bitmap, 99));

    // Verify blocks 100-199 are free
    EXPECT_TRUE(is_block_free(bitmap, 100));
    EXPECT_TRUE(is_block_free(bitmap, 150));
    EXPECT_TRUE(is_block_free(bitmap, 199));

    // Verify blocks after 199 are still used
    EXPECT_FALSE(is_block_free(bitmap, 200));

    uint32_t free_count = count_free_blocks(bitmap, 4096);
    EXPECT_EQ(free_count, 100);
}

TEST_F(Ext4BlockBitmapTest, FragmentedPattern) {
    // Simulate fragmented free space
    std::vector<uint8_t> bitmap(512, 0xFF);  // All used initially

    // Free alternate blocks
    for (uint32_t i = 0; i < 1000; i += 2) {
        if (i / 8 < bitmap.size()) {
            bitmap[i / 8] &= ~(1 << (i % 8));
        }
    }

    // Verify fragmentation
    EXPECT_TRUE(is_block_free(bitmap, 0));
    EXPECT_FALSE(is_block_free(bitmap, 1));
    EXPECT_TRUE(is_block_free(bitmap, 2));
    EXPECT_FALSE(is_block_free(bitmap, 3));
}