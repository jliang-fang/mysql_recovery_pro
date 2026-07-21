#include <gtest/gtest.h>
#include "dbrecovery/innodb/undo_page_index.hpp"
#include <fstream>
#include <vector>
#include <cstdio>

namespace dbrecovery {
namespace innodb {

// Helper: create a temp file with specific page types at known positions
static std::string create_temp_ibdata(const std::vector<std::pair<uint32_t, uint16_t>>& pages,
                                       size_t page_size = 16384) {
    std::string path = "/tmp/test_undo_page_index.bin";

    // Find max page number to determine file size
    uint32_t max_page = 0;
    for (const auto& [pg, _] : pages) {
        if (pg > max_page) max_page = pg;
    }

    // Allocate the full file (page_size * (max_page + 1))
    std::vector<uint8_t> file(page_size * (static_cast<size_t>(max_page) + 1), 0);

    // Write page types at the FIL header offset (byte 24-25, big-endian)
    for (const auto& [pg, type] : pages) {
        size_t offset = static_cast<size_t>(pg) * page_size + 24;
        write_be<uint16_t>(file.data(), offset, type);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), file.size());
    out.close();
    return path;
}

// ==================== Build Tests ====================

TEST(UndoPageIndexTest, BuildWithUndoPages) {
    std::string path = create_temp_ibdata({
        {5, FIL_PAGE_UNDO_LOG},    // page 5 = UNDO_LOG
        {10, FIL_PAGE_INDEX},       // page 10 = INDEX (not undo)
        {15, FIL_PAGE_UNDO_LOG},   // page 15 = UNDO_LOG
        {20, FIL_PAGE_TYPE_FSP_HDR}, // page 20 = FSP_HDR (not undo)
        {25, FIL_PAGE_UNDO_LOG},   // page 25 = UNDO_LOG
    });

    UndoPageIndex index;
    bool result = index.build(path, 16384, 0);

    EXPECT_TRUE(result);
    EXPECT_TRUE(index.is_built());
    EXPECT_EQ(index.size(), 3u);
    EXPECT_EQ(index.total_pages(), 26u);  // max_page + 1 = 26

    // Verify individual page lookups
    EXPECT_TRUE(index.is_undo_page(5));
    EXPECT_TRUE(index.is_undo_page(15));
    EXPECT_TRUE(index.is_undo_page(25));
    EXPECT_FALSE(index.is_undo_page(10));
    EXPECT_FALSE(index.is_undo_page(20));

    // Verify ordered iteration
    const auto& pages = index.undo_pages();
    ASSERT_EQ(pages.size(), 3u);
    EXPECT_EQ(pages[0], 5u);
    EXPECT_EQ(pages[1], 15u);
    EXPECT_EQ(pages[2], 25u);

    std::remove(path.c_str());
}

TEST(UndoPageIndexTest, BuildNoUndoPages) {
    std::string path = create_temp_ibdata({
        {0, FIL_PAGE_TYPE_FSP_HDR},
        {1, FIL_PAGE_INDEX},
        {2, FIL_PAGE_INDEX},
    });

    UndoPageIndex index;
    bool result = index.build(path, 16384, 0);

    EXPECT_TRUE(result);
    EXPECT_TRUE(index.is_built());
    EXPECT_EQ(index.size(), 0u);
    EXPECT_FALSE(index.is_undo_page(0));
    EXPECT_FALSE(index.is_undo_page(1));

    std::remove(path.c_str());
}

TEST(UndoPageIndexTest, BuildNonExistentFile) {
    UndoPageIndex index;
    bool result = index.build("/nonexistent/path/ibdata1", 16384, 0);
    EXPECT_FALSE(result);
    EXPECT_FALSE(index.is_built());
    EXPECT_EQ(index.size(), 0u);
}

TEST(UndoPageIndexTest, BuildEmptyFile) {
    std::string path = "/tmp/test_undo_empty.bin";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.put(0);
        out.close();
    }

    UndoPageIndex index;
    // An empty file should naturally fail since total_pages_ == 0
    // but that depends on filesystem behavior (tellg returns 0)
    // The build() should return false
    bool result = index.build(path, 16384, 0);
    EXPECT_FALSE(result);

    std::remove(path.c_str());
}

// ==================== Lookup Tests ====================

TEST(UndoPageIndexTest, IsUndoPageBeforeBuild) {
    UndoPageIndex index;
    // Before build, is_undo_page should return false
    EXPECT_FALSE(index.is_undo_page(5));
}

TEST(UndoPageIndexTest, UndoPagesIterationEmpty) {
    UndoPageIndex index;
    const auto& pages = index.undo_pages();
    EXPECT_TRUE(pages.empty());
}

// ==================== Management Tests ====================

TEST(UndoPageIndexTest, Reset) {
    std::string path = create_temp_ibdata({
        {5, FIL_PAGE_UNDO_LOG},
        {10, FIL_PAGE_UNDO_LOG},
    });

    UndoPageIndex index;
    index.build(path, 16384, 0);
    EXPECT_EQ(index.size(), 2u);
    EXPECT_TRUE(index.is_built());

    index.reset();
    EXPECT_EQ(index.size(), 0u);
    EXPECT_FALSE(index.is_built());
    EXPECT_EQ(index.total_pages(), 0u);
    EXPECT_TRUE(index.undo_pages().empty());

    std::remove(path.c_str());
}

TEST(UndoPageIndexTest, RebuildAfterReset) {
    std::string path = create_temp_ibdata({
        {5, FIL_PAGE_UNDO_LOG},
        {10, FIL_PAGE_UNDO_LOG},
    });

    UndoPageIndex index;
    index.build(path, 16384, 0);
    ASSERT_EQ(index.size(), 2u);

    index.reset();
    bool result = index.build(path, 16384, 0);
    EXPECT_TRUE(result);
    EXPECT_EQ(index.size(), 2u);

    std::remove(path.c_str());
}

// ==================== Statistics Tests ====================

TEST(UndoPageIndexTest, Stats) {
    std::string path = create_temp_ibdata({
        {5, FIL_PAGE_UNDO_LOG},
        {10, FIL_PAGE_UNDO_LOG},
        {15, FIL_PAGE_INDEX},
    });

    UndoPageIndex index;
    index.build(path, 16384, 0);

    const auto& stats = index.stats();
    EXPECT_EQ(stats.total_pages_scanned, 16u);
    EXPECT_EQ(stats.undo_pages_found, 2u);
    EXPECT_GT(stats.build_time_ms, 0.0);

    std::remove(path.c_str());
}

// ==================== Different Page Sizes ====================

TEST(UndoPageIndexTest, DifferentPageSizes) {
    // Test with 4K, 8K, 32K page sizes
    const std::vector<size_t> page_sizes = {4096, 8192, 32768};

    for (size_t ps : page_sizes) {
        std::string path = create_temp_ibdata({
            {2, FIL_PAGE_UNDO_LOG},
            {7, FIL_PAGE_UNDO_LOG},
        }, ps);

        UndoPageIndex index;
        bool result = index.build(path, ps, 0);
        EXPECT_TRUE(result) << "Failed for page_size=" << ps;
        EXPECT_EQ(index.size(), 2u) << "Wrong size for page_size=" << ps;
        EXPECT_TRUE(index.is_undo_page(2)) << "Failed lookup for page_size=" << ps;
        EXPECT_TRUE(index.is_undo_page(7)) << "Failed lookup for page_size=" << ps;

        std::remove(path.c_str());
    }
}

// ==================== Large Page Range ====================

TEST(UndoPageIndexTest, UndoPagesAtHighPageNumbers) {
    std::string path = create_temp_ibdata({
        {1000, FIL_PAGE_UNDO_LOG},
        {5000, FIL_PAGE_UNDO_LOG},
        {9999, FIL_PAGE_UNDO_LOG},
    });

    UndoPageIndex index;
    bool result = index.build(path, 16384, 0);

    EXPECT_TRUE(result);
    EXPECT_EQ(index.size(), 3u);
    EXPECT_TRUE(index.is_undo_page(1000));
    EXPECT_TRUE(index.is_undo_page(5000));
    EXPECT_TRUE(index.is_undo_page(9999));
    EXPECT_FALSE(index.is_undo_page(0));
    EXPECT_FALSE(index.is_undo_page(9998));

    std::remove(path.c_str());
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}