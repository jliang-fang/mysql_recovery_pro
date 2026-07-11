/**
 * @file test_xfs_extent.cpp
 * @brief Unit tests for XFS extent validation
 *
 * Reference: xfs_undelete patterns
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <cstring>

// Test helper functions inline (since xfs_disk_carver.hpp requires Linux)
namespace {

// XFS extent structure (simplified for testing)
struct xfs_extent_test {
    uint64_t block_offset;   // Starting block
    uint64_t block_count;    // Number of blocks
    uint64_t file_offset;    // File offset
    bool is_unwritten;       // Unwritten extent flag
};

// Extent validation function (simplified)
bool validate_extent_basic(const xfs_extent_test& ext, uint32_t blocks_per_ag) {
    // Basic sanity checks
    if (ext.block_count == 0) {
        return false;
    }

    // Check extent is within AG bounds
    if (ext.block_offset >= blocks_per_ag) {
        return false;
    }

    if (ext.block_offset + ext.block_count > blocks_per_ag) {
        return false;
    }

    return true;
}

// Check if extent fits in free space (simplified heuristic)
bool extent_fits_in_free(const xfs_extent_test& ext, uint32_t longest_free) {
    return ext.block_count <= longest_free;
}

// Parse extent from packed BMBT record (reference: xfsprogs/xfs_bmbt_disk_get_all)
xfs_extent_test parse_extent_packed(uint64_t l0, uint64_t l1) {
    xfs_extent_test ext;

    // l0:63 - extent flag (1 = unwritten)
    ext.is_unwritten = (l0 >> 63) & 1;

    // l0:9-62 - startoff (file offset), 54 bits
    ext.file_offset = (l0 >> 9) & ((1ULL << 54) - 1);

    // l0:0-8 + l1:21-63 - startblock, 52 bits
    ext.block_offset = ((l0 & 0x1FF) << 43) | ((l1 >> 21) & ((1ULL << 43) - 1));

    // l1:0-20 - blockcount, 21 bits
    ext.block_count = l1 & ((1ULL << 21) - 1);

    return ext;
}

} // namespace

class XfsExtentTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ==================== Extent Validation Tests ====================

TEST_F(XfsExtentTest, ValidExtent) {
    xfs_extent_test ext;
    ext.block_offset = 1000;
    ext.block_count = 100;
    ext.file_offset = 0;
    ext.is_unwritten = false;

    EXPECT_TRUE(validate_extent_basic(ext, 1000000));
}

TEST_F(XfsExtentTest, ZeroBlockCount) {
    xfs_extent_test ext;
    ext.block_offset = 1000;
    ext.block_count = 0;  // Invalid
    ext.file_offset = 0;
    ext.is_unwritten = false;

    EXPECT_FALSE(validate_extent_basic(ext, 1000000));
}

TEST_F(XfsExtentTest, ExtentBeyondAgBounds) {
    xfs_extent_test ext;
    ext.block_offset = 900000;
    ext.block_count = 200000;  // Would exceed AG
    ext.file_offset = 0;
    ext.is_unwritten = false;

    EXPECT_FALSE(validate_extent_basic(ext, 1000000));  // AG has 1M blocks
}

TEST_F(XfsExtentTest, ExtentAtAgBoundary) {
    xfs_extent_test ext;
    ext.block_offset = 999900;
    ext.block_count = 100;  // Exactly fits
    ext.file_offset = 0;
    ext.is_unwritten = false;

    EXPECT_TRUE(validate_extent_basic(ext, 1000000));
}

TEST_F(XfsExtentTest, ExtentOverflowAgBoundary) {
    xfs_extent_test ext;
    ext.block_offset = 999900;
    ext.block_count = 101;  // One block over
    ext.file_offset = 0;
    ext.is_unwritten = false;

    EXPECT_FALSE(validate_extent_basic(ext, 1000000));
}

// ==================== Free Space Check Tests ====================

TEST_F(XfsExtentTest, ExtentFitsInFreeSpace) {
    xfs_extent_test ext;
    ext.block_count = 100;

    EXPECT_TRUE(extent_fits_in_free(ext, 150));  // 150 blocks free
    EXPECT_FALSE(extent_fits_in_free(ext, 50));  // Only 50 blocks free
}

TEST_F(XfsExtentTest, ExtentExactlyFitsFreeSpace) {
    xfs_extent_test ext;
    ext.block_count = 100;

    EXPECT_TRUE(extent_fits_in_free(ext, 100));  // Exactly fits
}

// ==================== Extent Parsing Tests ====================

TEST_F(XfsExtentTest, ParseSimpleExtent) {
    // Simple extent: block_offset=100, block_count=50, file_offset=0
    // l0: file_offset << 9 | block_offset_high
    // l1: block_offset_low << 21 | block_count

    uint64_t file_offset = 0;
    uint64_t block_offset = 100;
    uint64_t block_count = 50;

    // Construct packed extent
    uint64_t l0 = (file_offset << 9) | ((block_offset >> 43) & 0x1FF);
    uint64_t l1 = ((block_offset & ((1ULL << 43) - 1)) << 21) | block_count;

    xfs_extent_test ext = parse_extent_packed(l0, l1);

    EXPECT_EQ(ext.file_offset, file_offset);
    EXPECT_EQ(ext.block_offset, block_offset);
    EXPECT_EQ(ext.block_count, block_count);
    EXPECT_FALSE(ext.is_unwritten);
}

TEST_F(XfsExtentTest, ParseUnwrittenExtent) {
    // Unwritten extent has bit 63 of l0 set
    uint64_t l0 = (1ULL << 63) | (0 << 9);  // Unwritten flag + file_offset=0
    uint64_t l1 = (100ULL << 21) | 50;      // block_offset=100, block_count=50

    xfs_extent_test ext = parse_extent_packed(l0, l1);

    EXPECT_TRUE(ext.is_unwritten);
    EXPECT_EQ(ext.file_offset, 0);
}

TEST_F(XfsExtentTest, ParseLargeExtent) {
    // Large extent with high block offset
    uint64_t file_offset = 1000000;  // 1M blocks into file
    uint64_t block_offset = 0x123456789ULL;  // Large block offset
    uint64_t block_count = 10000;

    uint64_t l0 = (file_offset << 9) | ((block_offset >> 43) & 0x1FF);
    uint64_t l1 = ((block_offset & ((1ULL << 43) - 1)) << 21) | block_count;

    xfs_extent_test ext = parse_extent_packed(l0, l1);

    EXPECT_EQ(ext.file_offset, file_offset);
    EXPECT_EQ(ext.block_offset, block_offset);
    EXPECT_EQ(ext.block_count, block_count);
}

// ==================== Edge Case Tests ====================

TEST_F(XfsExtentTest, MaxBlockCount) {
    // Maximum block count is 2^21 - 1 = 2097151
    uint64_t max_count = (1ULL << 21) - 1;

    xfs_extent_test ext;
    ext.block_offset = 0;
    ext.block_count = max_count;

    // Would need a very large AG to fit
    EXPECT_TRUE(validate_extent_basic(ext, max_count));
    EXPECT_FALSE(validate_extent_basic(ext, max_count - 1));
}

TEST_F(XfsExtentTest, ZeroBlockOffset) {
    xfs_extent_test ext;
    ext.block_offset = 0;  // Start of AG
    ext.block_count = 100;
    ext.file_offset = 0;

    EXPECT_TRUE(validate_extent_basic(ext, 1000000));
}

TEST_F(XfsExtentTest, ContiguousExtents) {
    // Two contiguous extents
    xfs_extent_test ext1;
    ext1.block_offset = 1000;
    ext1.block_count = 100;
    ext1.file_offset = 0;

    xfs_extent_test ext2;
    ext2.block_offset = 1100;  // Starts right after ext1
    ext2.block_count = 50;
    ext2.file_offset = 100;

    EXPECT_TRUE(validate_extent_basic(ext1, 1000000));
    EXPECT_TRUE(validate_extent_basic(ext2, 1000000));

    // Verify they are contiguous
    EXPECT_EQ(ext1.block_offset + ext1.block_count, ext2.block_offset);
}

// ==================== Real-World Pattern Tests ====================

TEST_F(XfsExtentTest, TypicalMysqlTableExtent) {
    // Typical MySQL table extent: ~1000 blocks = 16MB for 16KB pages
    xfs_extent_test ext;
    ext.block_offset = 50000;
    ext.block_count = 1000;
    ext.file_offset = 0;
    ext.is_unwritten = false;

    EXPECT_TRUE(validate_extent_basic(ext, 1000000));

    // Check if it fits in typical free space
    EXPECT_TRUE(extent_fits_in_free(ext, 2000));
}

TEST_F(XfsExtentTest, FragmentedExtents) {
    // Simulate fragmented file with multiple small extents
    std::vector<xfs_extent_test> extents;

    for (int i = 0; i < 10; i++) {
        xfs_extent_test ext;
        ext.block_offset = 10000 + i * 1000;
        ext.block_count = 100;
        ext.file_offset = i * 100;
        ext.is_unwritten = false;
        extents.push_back(ext);
    }

    // All should be valid
    for (const auto& ext : extents) {
        EXPECT_TRUE(validate_extent_basic(ext, 1000000));
    }
}

TEST_F(XfsExtentTest, DeletedInodePattern) {
    // Deleted inode typically has:
    // - di_mode = 0 (deleted)
    // - di_nlink = 0 (no links)
    // - But extent data still intact

    xfs_extent_test ext;
    ext.block_offset = 75000;
    ext.block_count = 500;
    ext.file_offset = 0;
    ext.is_unwritten = false;

    // Extent should still be valid
    EXPECT_TRUE(validate_extent_basic(ext, 1000000));
}