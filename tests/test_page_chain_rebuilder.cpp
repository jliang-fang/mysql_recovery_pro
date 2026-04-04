#include <gtest/gtest.h>
#include "dbrecovery/innodb/page_chain_rebuilder.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== PageChainRebuilder Tests ====================

TEST(PageChainRebuilderTest, RebuildEmptyChain) {
    PageChainRebuilder rebuilder;
    std::vector<ScannedPage> pages;

    EXPECT_FALSE(rebuilder.rebuild_chain(pages));
    EXPECT_EQ(rebuilder.stats().total_pages, 0);
}

TEST(PageChainRebuilderTest, RebuildSinglePage) {
    PageChainRebuilder rebuilder;

    std::vector<ScannedPage> pages;
    ScannedPage page{};
    page.page_no = 0;
    page.prev_page = 0xFFFFFFFF;
    page.next_page = 0xFFFFFFFF;
    page.type = FIL_PAGE_INDEX;
    page.is_valid = true;
    pages.push_back(page);

    EXPECT_TRUE(rebuilder.rebuild_chain(pages));
    EXPECT_EQ(rebuilder.stats().total_pages, 1);
    EXPECT_EQ(rebuilder.stats().orphan_pages, 1);  // 单页是孤立的
}

TEST(PageChainRebuilderTest, RebuildMultiplePagesWithBrokenChain) {
    PageChainRebuilder rebuilder;

    std::vector<ScannedPage> pages(3);

    // Page 0
    pages[0].page_no = 0;
    pages[0].prev_page = 0xFFFFFFFF;
    pages[0].next_page = 999;  // Broken: should be 1
    pages[0].type = FIL_PAGE_INDEX;
    pages[0].is_valid = true;

    // Page 1
    pages[1].page_no = 1;
    pages[1].prev_page = 888;  // Broken: should be 0
    pages[1].next_page = 777;  // Broken: should be 2
    pages[1].type = FIL_PAGE_INDEX;
    pages[1].is_valid = true;

    // Page 2
    pages[2].page_no = 2;
    pages[2].prev_page = 666;  // Broken: should be 1
    pages[2].next_page = 0xFFFFFFFF;
    pages[2].type = FIL_PAGE_INDEX;
    pages[2].is_valid = true;

    EXPECT_TRUE(rebuilder.rebuild_chain(pages));
    EXPECT_EQ(rebuilder.stats().total_pages, 3);

    // 获取重连后的页
    auto rebuilt = rebuilder.get_rebuilt_pages();

    // 检查页是否按页号排序
    EXPECT_EQ(rebuilt[0].page_no, 0);
    EXPECT_EQ(rebuilt[1].page_no, 1);
    EXPECT_EQ(rebuilt[2].page_no, 2);

    // 检查链关系是否修复（重连后应该正确）
    EXPECT_EQ(rebuilt[0].prev_page, 0xFFFFFFFF);
    EXPECT_EQ(rebuilt[0].next_page, 1);
    EXPECT_EQ(rebuilt[1].prev_page, 0);
    EXPECT_EQ(rebuilt[1].next_page, 2);
    EXPECT_EQ(rebuilt[2].prev_page, 1);
    EXPECT_EQ(rebuilt[2].next_page, 0xFFFFFFFF);

    // 检查统计（至少页数正确）
    EXPECT_EQ(rebuilder.stats().total_pages, 3);
    EXPECT_EQ(rebuilder.stats().orphan_pages, 0);  // 重连后没有孤立页
}

TEST(PageChainRebuilderTest, RebuildPagesWithOrphans) {
    PageChainRebuilder rebuilder;

    std::vector<ScannedPage> pages(4);

    // Page 0
    pages[0].page_no = 0;
    pages[0].prev_page = 0xFFFFFFFF;
    pages[0].next_page = 1;
    pages[0].type = FIL_PAGE_INDEX;
    pages[0].is_valid = true;

    // Page 1
    pages[1].page_no = 1;
    pages[1].prev_page = 0;
    pages[1].next_page = 0xFFFFFFFF;
    pages[1].type = FIL_PAGE_INDEX;
    pages[1].is_valid = true;

    // Orphan Page (not in chain) - isolated
    pages[2].page_no = 5;
    pages[2].prev_page = 0xFFFFFFFF;
    pages[2].next_page = 0xFFFFFFFF;
    pages[2].type = FIL_PAGE_INDEX;
    pages[2].is_valid = true;

    // Another orphan - also isolated
    pages[3].page_no = 10;
    pages[3].prev_page = 0xFFFFFFFF;
    pages[3].next_page = 0xFFFFFFFF;
    pages[3].type = FIL_PAGE_INDEX;
    pages[3].is_valid = true;

    EXPECT_TRUE(rebuilder.rebuild_chain(pages));

    auto orphan_indices = rebuilder.get_orphan_pages();
    
    // 重连后，孤立页应该被识别
    // pages[2] 和 pages[3] 都是孤立的
    EXPECT_EQ(orphan_indices.size(), 2);
    
    // 检查孤立页的页号
    EXPECT_EQ(rebuilder.get_rebuilt_pages()[orphan_indices[0]].page_no, 5);
    EXPECT_EQ(rebuilder.get_rebuilt_pages()[orphan_indices[1]].page_no, 10);
}

TEST(PageChainRebuilderTest, SortByPageNumber) {
    PageChainRebuilder rebuilder;

    std::vector<ScannedPage> pages(3);

    // Out of order
    pages[0].page_no = 2;
    pages[0].prev_page = 0xFFFFFFFF;
    pages[0].next_page = 0xFFFFFFFF;
    pages[0].type = FIL_PAGE_INDEX;
    pages[0].is_valid = true;

    pages[1].page_no = 0;
    pages[1].prev_page = 0xFFFFFFFF;
    pages[1].next_page = 0xFFFFFFFF;
    pages[1].type = FIL_PAGE_INDEX;
    pages[1].is_valid = true;

    pages[2].page_no = 1;
    pages[2].prev_page = 0xFFFFFFFF;
    pages[2].next_page = 0xFFFFFFFF;
    pages[2].type = FIL_PAGE_INDEX;
    pages[2].is_valid = true;

    EXPECT_TRUE(rebuilder.rebuild_chain(pages));

    // 排序后应该按页号顺序
    auto rebuilt = rebuilder.get_rebuilt_pages();
    EXPECT_EQ(rebuilt[0].page_no, 0);
    EXPECT_EQ(rebuilt[1].page_no, 1);
    EXPECT_EQ(rebuilt[2].page_no, 2);
}

TEST(PageChainRebuilderTest, BuildPageIndexMap) {
    PageChainRebuilder rebuilder;

    std::vector<ScannedPage> pages(3);

    pages[0].page_no = 10;
    pages[0].type = FIL_PAGE_INDEX;
    pages[0].is_valid = true;

    pages[1].page_no = 20;
    pages[1].type = FIL_PAGE_INDEX;
    pages[1].is_valid = true;

    pages[2].page_no = 30;
    pages[2].type = FIL_PAGE_INDEX;
    pages[2].is_valid = true;

    EXPECT_TRUE(rebuilder.rebuild_chain(pages));

    auto index_map = rebuilder.build_page_index_map();
    EXPECT_EQ(index_map[10], 0);
    EXPECT_EQ(index_map[20], 1);
    EXPECT_EQ(index_map[30], 2);
}

TEST(PageChainRebuilderTest, RebuildWithMixedPageTypes) {
    PageChainRebuilder rebuilder;

    std::vector<ScannedPage> pages(4);

    // INDEX page
    pages[0].page_no = 0;
    pages[0].prev_page = 0xFFFFFFFF;
    pages[0].next_page = 1;
    pages[0].type = FIL_PAGE_INDEX;
    pages[0].is_valid = true;

    // SDI page
    pages[1].page_no = 1;
    pages[1].prev_page = 0;
    pages[1].next_page = 2;
    pages[1].type = FIL_PAGE_SDI;
    pages[1].is_valid = true;

    // UNDO page
    pages[2].page_no = 2;
    pages[2].prev_page = 1;
    pages[2].next_page = 3;
    pages[2].type = FIL_PAGE_UNDO_LOG;
    pages[2].is_valid = true;

    // Allocated page
    pages[3].page_no = 3;
    pages[3].prev_page = 2;
    pages[3].next_page = 0xFFFFFFFF;
    pages[3].type = FIL_PAGE_TYPE_ALLOCATED;
    pages[3].is_valid = true;

    EXPECT_TRUE(rebuilder.rebuild_chain(pages));

    auto rebuilt = rebuilder.get_rebuilt_pages();
    EXPECT_EQ(rebuilt.size(), 4);
    EXPECT_EQ(rebuilt[0].type, FIL_PAGE_INDEX);
    EXPECT_EQ(rebuilt[1].type, FIL_PAGE_SDI);
    EXPECT_EQ(rebuilt[2].type, FIL_PAGE_UNDO_LOG);
    EXPECT_EQ(rebuilt[3].type, FIL_PAGE_TYPE_ALLOCATED);
}

TEST(PageChainRebuilderTest, ChainStatsToString) {
    ChainRebuildStats stats{};
    stats.total_pages = 10;
    stats.fixed_chains = 5;
    stats.orphan_pages = 2;
    stats.circular_references = 1;

    std::string str = stats.to_string();
    EXPECT_NE(str.find("total=10"), std::string::npos);
    EXPECT_NE(str.find("fixed=5"), std::string::npos);
    EXPECT_NE(str.find("orphan=2"), std::string::npos);
    EXPECT_NE(str.find("circular=1"), std::string::npos);
}

TEST(PageChainRebuilderTest, RebuildWithUnorderedInput) {
    PageChainRebuilder rebuilder;

    std::vector<ScannedPage> pages(5);

    // Create pages in random order
    for (int i = 0; i < 5; i++) {
        pages[i].page_no = i * 2;  // 0, 2, 4, 6, 8
        pages[i].type = FIL_PAGE_INDEX;
        pages[i].is_valid = true;

        if (i == 0) {
            pages[i].prev_page = 0xFFFFFFFF;
            pages[i].next_page = 999;  // Broken
        } else if (i == 4) {
            pages[i].prev_page = 888;  // Broken
            pages[i].next_page = 0xFFFFFFFF;
        } else {
            pages[i].prev_page = 777;  // Broken
            pages[i].next_page = 666;  // Broken
        }
    }

    EXPECT_TRUE(rebuilder.rebuild_chain(pages));

    auto rebuilt = rebuilder.get_rebuilt_pages();

    // 应该按页号排序
    for (size_t i = 0; i < rebuilt.size(); i++) {
        EXPECT_EQ(rebuilt[i].page_no, i * 2);
        if (i > 0) {
            EXPECT_EQ(rebuilt[i].prev_page, (i - 1) * 2);  // 修复：使用正确的页号
        }
        if (i < rebuilt.size() - 1) {
            EXPECT_EQ(rebuilt[i].next_page, (i + 1) * 2);  // 修复：使用正确的页号
        }
    }
}

TEST(PageChainRebuilderTest, DetectCircularReference) {
    PageChainRebuilder rebuilder;

    std::vector<ScannedPage> pages(3);

    // Create a circular reference: 0 -> 1 -> 2 -> 0
    pages[0].page_no = 0;
    pages[0].prev_page = 2;  // Points back to page 2
    pages[0].next_page = 1;
    pages[0].type = FIL_PAGE_INDEX;
    pages[0].is_valid = true;

    pages[1].page_no = 1;
    pages[1].prev_page = 0;
    pages[1].next_page = 2;
    pages[1].type = FIL_PAGE_INDEX;
    pages[1].is_valid = true;

    pages[2].page_no = 2;
    pages[2].prev_page = 1;
    pages[2].next_page = 0;  // Points back to page 0
    pages[2].type = FIL_PAGE_INDEX;
    pages[2].is_valid = true;

    EXPECT_TRUE(rebuilder.rebuild_chain(pages));

    // 重连后应该修复循环引用
    auto rebuilt = rebuilder.get_rebuilt_pages();
    EXPECT_EQ(rebuilt[0].prev_page, 0xFFFFFFFF);
    EXPECT_EQ(rebuilt[0].next_page, 1);
    EXPECT_EQ(rebuilt[1].prev_page, 0);
    EXPECT_EQ(rebuilt[1].next_page, 2);
    EXPECT_EQ(rebuilt[2].prev_page, 1);
    EXPECT_EQ(rebuilt[2].next_page, 0xFFFFFFFF);
}

TEST(PageChainRebuilderTest, LargeChainRebuild) {
    PageChainRebuilder rebuilder;

    const int NUM_PAGES = 100;
    std::vector<ScannedPage> pages(NUM_PAGES);

    // Create a large chain with broken pointers
    for (int i = 0; i < NUM_PAGES; i++) {
        pages[i].page_no = i;
        pages[i].type = FIL_PAGE_INDEX;
        pages[i].is_valid = true;

        if (i == 0) {
            pages[i].prev_page = 0xFFFFFFFF;
            pages[i].next_page = 9999;  // Broken
        } else if (i == NUM_PAGES - 1) {
            pages[i].prev_page = 8888;  // Broken
            pages[i].next_page = 0xFFFFFFFF;
        } else {
            pages[i].prev_page = 7777;  // Broken
            pages[i].next_page = 6666;  // Broken
        }
    }

    EXPECT_TRUE(rebuilder.rebuild_chain(pages));

    auto rebuilt = rebuilder.get_rebuilt_pages();
    EXPECT_EQ(rebuilt.size(), NUM_PAGES);

    // 验证整个链
    for (size_t i = 0; i < rebuilt.size(); i++) {
        if (i > 0) {
            EXPECT_EQ(rebuilt[i].prev_page, i - 1);
        }
        if (i < rebuilt.size() - 1) {
            EXPECT_EQ(rebuilt[i].next_page, i + 1);
        }
    }
}

} // namespace innodb
} // namespace dbrecovery
