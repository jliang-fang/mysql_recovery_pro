/**
 * @file test_sec_btree_repair.cpp
 * @brief Tests for SecondaryIndexBTreeRepairer — content-based B+Tree page chain repair
 *
 * P3: B+Tree Fragmentation Repair — when a secondary index IBD has broken page
 * chains, the current fix_chain_breaks() sorts pages by page_no — wrong for
 * fragmented B+Trees. This test verifies content-based ordering: extract min/max
 * key from each leaf page and sort by key range.
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "dbrecovery/innodb/sec_index_btree_repair.hpp"
#include "dbrecovery/innodb/page_chain_rebuilder.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/btree_traversal.hpp"

using namespace dbrecovery::innodb;
using namespace dbrecovery::catalog;

// ==================== Test Helpers ====================

/**
 * @brief Create a test INDEX page with the given records
 *
 * Layout (all multi-byte fields are BIG-ENDIAN per InnoDB spec):
 *   FIL header: offsets 0-37
 *     [4]  page_no      (uint32)
 *     [8]  prev_page    (uint32)
 *     [12] next_page    (uint32)
 *     [16] lsn          (uint64)
 *     [24] page_type    (uint16, FIL_PAGE_INDEX=0x45BF)
 *     [34] space_id     (uint32)
 *   BTR header: offsets 38+
 *     [38] n_dir_slots  (uint16)
 *     [40] heap_top     (uint16)
 *     [42] level        (uint16, BTR_N_LEVELS_ABS)
 *     [44] index_id     (uint64, BTR_IND_PAGE_OFFSET_ABS)
 *   INFIMUM at offset 54 (PAGE_INFIMUM_REC_OFFSET_ABS)
 *   User records (each: 5-byte header + key data)
 *   SUPREMUM record
 */
static std::vector<uint8_t> create_page_with_records(
    uint32_t page_no,
    uint16_t level,
    uint64_t index_id,
    const std::vector<std::string>& record_keys,
    uint32_t prev_page = FIL_NULL,
    uint32_t next_page = FIL_NULL,
    size_t page_size = 16384) {

    std::vector<uint8_t> page(page_size, 0);

    // --- FIL header (big-endian) ---
    write_be<uint32_t>(page.data(), 4, page_no);
    write_be<uint32_t>(page.data(), 8, prev_page);
    write_be<uint32_t>(page.data(), 12, next_page);
    write_be<uint64_t>(page.data(), 16, 1000 + page_no);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);
    write_be<uint32_t>(page.data(), 34, 0);

    // --- BTR / page header (big-endian) ---
    write_be<uint16_t>(page.data(), 38, 2);     // n_dir_slots
    write_be<uint16_t>(page.data(), 42, level);  // BTR_N_LEVELS / page_level
    write_be<uint64_t>(page.data(), 44, index_id); // index_id

    // --- INFIMUM record at offset 54 ---
    constexpr uint16_t INFIMUM_OFF = PAGE_INFIMUM_REC_OFFSET_ABS;  // 54
    page[INFIMUM_OFF + 2] = REC_STATUS_INFIMUM;  // byte2 = rec_type = 2

    // --- Place records ---
    uint16_t current_off = 100;  // start user records after the fixed area
    uint16_t prev_off = INFIMUM_OFF;
    int heap_no = 1;

    for (const auto& key : record_keys) {
        uint16_t rec_off = current_off;
        int16_t rel_off = rec_off - prev_off;

        // Write next_record_offset into previous record (big-endian, signed 16-bit)
        write_be<uint16_t>(page.data(), prev_off + 3,
                          static_cast<uint16_t>(static_cast<int16_t>(rel_off)));

        // Record header: byte2 = (heap_no << 3) | rec_type
        // rec_type = 0 (ordinary)
        page[rec_off + 2] = static_cast<uint8_t>((heap_no << 3) & 0xF8);

        // Key data starts after 5-byte header
        std::copy(key.begin(), key.end(), page.begin() + rec_off + 5);

        prev_off = rec_off;
        current_off = rec_off + 5 + static_cast<uint16_t>(key.size());
        heap_no++;
    }

    // --- SUPREMUM record ---
    uint16_t supremum_off = current_off;
    page[supremum_off + 2] = REC_STATUS_SUPREMUM;  // rec_type = 3

    // Link last user record -> SUPREMUM
    int16_t rel_to_supremum = supremum_off - prev_off;
    write_be<uint16_t>(page.data(), prev_off + 3,
                      static_cast<uint16_t>(static_cast<int16_t>(rel_to_supremum)));

    // SUPREMUM's next_record_offset = 0 (end of chain)
    write_be<uint16_t>(page.data(), supremum_off + 3, 0);

    // Set heap_top
    write_be<uint16_t>(page.data(), 40, supremum_off + 5);

    return page;
}

/**
 * @brief Create a page with a single record
 */
static std::vector<uint8_t> create_page_with_single_record(
    uint32_t page_no, uint16_t level, uint64_t index_id,
    const std::string& key, size_t page_size = 16384) {
    return create_page_with_records(page_no, level, index_id, {key},
                                    FIL_NULL, FIL_NULL, page_size);
}

/**
 * @brief Create a test TableSchema with id (LONGLONG) and name (VARCHAR) columns
 */
static TableSchema create_test_schema() {
    TableSchema schema;
    schema.table_name = "test_table";
    schema.row_format = row_formats::DYNAMIC;

    ColumnInfo id_col;
    id_col.name = "id";
    id_col.mysql_type_id = mysql_types::LONGLONG;
    id_col.is_signed = true;
    id_col.is_nullable = false;
    schema.columns[1] = id_col;

    ColumnInfo name_col;
    name_col.name = "name";
    name_col.mysql_type_id = mysql_types::VARCHAR;
    name_col.is_signed = false;
    name_col.is_nullable = true;
    schema.columns[2] = name_col;

    return schema;
}

/**
 * @brief Read FIL_PAGE_PREV from a page (big-endian)
 */
static uint32_t read_page_prev(const std::vector<uint8_t>& page) {
    return read_be<uint32_t>(page.data(), FIL_PAGE_PREV_ABS);
}

/**
 * @brief Read FIL_PAGE_NEXT from a page (big-endian)
 */
static uint32_t read_page_next(const std::vector<uint8_t>& page) {
    return read_be<uint32_t>(page.data(), FIL_PAGE_NEXT_ABS);
}

// ==================== Test 1: Extract min/max key from leaf page (basic) ====================

TEST(SecondaryIndexBTreeRepairTest, ExtractMinMaxKey_Basic) {
    auto page = create_page_with_records(1, 0, 100, {"AAA", "BBB"});
    SecondaryIndexBTreeRepairer repairer(16384);

    auto range = repairer.extract_key_range_from_raw(page.data());
    ASSERT_TRUE(range.has_value());
    EXPECT_TRUE(range->is_leaf);
    EXPECT_EQ(range->level, 0);
    // extract_min_key_bytes / extract_max_key_bytes return up to 256 bytes
    // of raw data after the record header (for key comparison)
    ASSERT_EQ(range->min_key.size(), 256);
    ASSERT_EQ(range->max_key.size(), 256);
    EXPECT_EQ(range->min_key[0], 'A');
    EXPECT_EQ(range->min_key[1], 'A');
    EXPECT_EQ(range->min_key[2], 'A');
    EXPECT_EQ(range->max_key[0], 'B');
    EXPECT_EQ(range->max_key[1], 'B');
    EXPECT_EQ(range->max_key[2], 'B');
}

// ==================== Test 2: Extract min/max key from page with single record ====================

TEST(SecondaryIndexBTreeRepairTest, ExtractMinMaxKey_SingleRecord) {
    std::string key = "XYZ";
    auto page = create_page_with_single_record(1, 0, 100, key);
    SecondaryIndexBTreeRepairer repairer(16384);

    auto range = repairer.extract_key_range_from_raw(page.data());
    ASSERT_TRUE(range.has_value());
    EXPECT_TRUE(range->is_leaf);
    ASSERT_EQ(range->min_key.size(), 256);
    ASSERT_EQ(range->max_key.size(), 256);
    EXPECT_EQ(range->min_key[0], 'X');
    EXPECT_EQ(range->max_key[0], 'X');
    // For a single record, min_key == max_key
    EXPECT_EQ(range->min_key, range->max_key);
}

// ==================== Test 3: Sort pages by key range (known order) ====================

TEST(SecondaryIndexBTreeRepairTest, SortByKeyRange_KnownOrder) {
    std::map<uint32_t, std::vector<uint8_t>> page_data;
    page_data[1] = create_page_with_records(1, 0, 100, {"AAA", "BBB"});
    page_data[2] = create_page_with_records(2, 0, 100, {"CCC", "DDD"});
    page_data[3] = create_page_with_records(3, 0, 100, {"EEE", "FFF"});

    SecondaryIndexBTreeRepairer repairer(16384);
    std::vector<uint32_t> page_nos = {1, 2, 3};
    repairer.set_page_list(page_nos, page_data);

    auto key_ranges = repairer.extract_key_ranges();
    ASSERT_EQ(key_ranges.size(), 3);

    // Already in correct order
    bool ordered = repairer.verify_content_order(key_ranges);
    EXPECT_TRUE(ordered);
}

// ==================== Test 4: Sort pages by key range (reverse order input) ====================

TEST(SecondaryIndexBTreeRepairTest, SortByKeyRange_ReverseOrder) {
    std::map<uint32_t, std::vector<uint8_t>> page_data;
    page_data[1] = create_page_with_records(1, 0, 100, {"AAA", "BBB"});
    page_data[2] = create_page_with_records(2, 0, 100, {"CCC", "DDD"});
    page_data[3] = create_page_with_records(3, 0, 100, {"EEE", "FFF"});

    SecondaryIndexBTreeRepairer repairer(16384);
    std::vector<uint32_t> page_nos = {3, 2, 1};  // reverse order
    repairer.set_page_list(page_nos, page_data);

    auto key_ranges = repairer.extract_key_ranges();

    // Sort by min_key
    std::sort(key_ranges.begin(), key_ranges.end(),
        [](const PageKeyRange& a, const PageKeyRange& b) {
            return a.min_key < b.min_key;
        });

    // Verify content order after sorting
    bool ordered = repairer.verify_content_order(key_ranges);
    EXPECT_TRUE(ordered);

    // Verify correct order: page 1 (AAA), page 2 (CCC), page 3 (EEE)
    EXPECT_EQ(key_ranges[0].page_no, 1u);
    EXPECT_EQ(key_ranges[1].page_no, 2u);
    EXPECT_EQ(key_ranges[2].page_no, 3u);
}

// ==================== Test 5: Sort pages by key range (random order input) ====================

TEST(SecondaryIndexBTreeRepairTest, SortByKeyRange_RandomOrder) {
    std::map<uint32_t, std::vector<uint8_t>> page_data;
    page_data[5] = create_page_with_records(5, 0, 100, {"MMM", "NNN"});
    page_data[2] = create_page_with_records(2, 0, 100, {"CCC", "DDD"});
    page_data[8] = create_page_with_records(8, 0, 100, {"AAA", "BBB"});
    page_data[3] = create_page_with_records(3, 0, 100, {"PPP", "QQQ"});

    SecondaryIndexBTreeRepairer repairer(16384);
    std::vector<uint32_t> page_nos = {5, 2, 8, 3};
    repairer.set_page_list(page_nos, page_data);

    auto key_ranges = repairer.extract_key_ranges();

    // Sort by min_key
    std::sort(key_ranges.begin(), key_ranges.end(),
        [](const PageKeyRange& a, const PageKeyRange& b) {
            return a.min_key < b.min_key;
        });

    // Verify correct order: AAA, CCC, MMM, PPP
    ASSERT_EQ(key_ranges.size(), 4);
    EXPECT_EQ(key_ranges[0].page_no, 8u);  // AAA
    EXPECT_EQ(key_ranges[1].page_no, 2u);  // CCC
    EXPECT_EQ(key_ranges[2].page_no, 5u);  // MMM
    EXPECT_EQ(key_ranges[3].page_no, 3u);  // PPP
}

// ==================== Test 6: Verify content order (correct order passes) ====================

TEST(SecondaryIndexBTreeRepairTest, VerifyContentOrder_CorrectOrder) {
    SecondaryIndexBTreeRepairer repairer(16384);

    std::vector<PageKeyRange> ranges;
    ranges.push_back({1, 0, {'A','A','A'}, {'B','B','B'}, true});
    ranges.push_back({2, 0, {'C','C','C'}, {'D','D','D'}, true});
    ranges.push_back({3, 0, {'E','E','E'}, {'F','F','F'}, true});

    EXPECT_TRUE(repairer.verify_content_order(ranges));
}

// ==================== Test 7: Verify content order (overlapping ranges detected) ====================

TEST(SecondaryIndexBTreeRepairTest, VerifyContentOrder_OverlappingDetected) {
    SecondaryIndexBTreeRepairer repairer(16384);

    // Page 2's max_key "DDD" > page 3's min_key "CCC" → overlap
    std::vector<PageKeyRange> ranges;
    ranges.push_back({1, 0, {'A','A','A'}, {'C','C','C'}, true});
    ranges.push_back({2, 0, {'B','B','B'}, {'D','D','D'}, true});

    EXPECT_FALSE(repairer.verify_content_order(ranges));
}

// ==================== Test 8: Verify content order (skewed ranges) ====================

TEST(SecondaryIndexBTreeRepairTest, VerifyContentOrder_SkewedRanges) {
    SecondaryIndexBTreeRepairer repairer(16384);

    // Pages in wrong order, but after sorting: page2 (AAA-BBB), page1 (CCC-DDD)
    // No overlap since BBB < CCC
    std::vector<PageKeyRange> ranges;
    ranges.push_back({1, 0, {'C','C','C'}, {'D','D','D'}, true});
    ranges.push_back({2, 0, {'A','A','A'}, {'B','B','B'}, true});

    // After sorting, order should be correct (no overlap)
    EXPECT_TRUE(repairer.verify_content_order(ranges));
}

// ==================== Test 9: Detect overlapping ranges ====================

TEST(SecondaryIndexBTreeRepairTest, DetectOverlappingRanges) {
    SecondaryIndexBTreeRepairer repairer(16384);

    std::vector<PageKeyRange> ranges;
    ranges.push_back({1, 0, {'A','A','A'}, {'C','C','C'}, true});
    ranges.push_back({2, 0, {'B','B','B'}, {'D','D','D'}, true});

    auto overlaps = repairer.detect_overlapping_ranges(ranges);
    ASSERT_EQ(overlaps.size(), 1);
    EXPECT_EQ(overlaps[0].first, 1u);
    EXPECT_EQ(overlaps[0].second, 2u);
}

// ==================== Test 10: Detect overlapping ranges (no overlap) ====================

TEST(SecondaryIndexBTreeRepairTest, DetectOverlappingRanges_NoOverlap) {
    SecondaryIndexBTreeRepairer repairer(16384);

    std::vector<PageKeyRange> ranges;
    ranges.push_back({1, 0, {'A','A','A'}, {'B','B','B'}, true});
    ranges.push_back({2, 0, {'C','C','C'}, {'D','D','D'}, true});

    auto overlaps = repairer.detect_overlapping_ranges(ranges);
    EXPECT_TRUE(overlaps.empty());
}

// ==================== Test 11: Non-leaf page sorting (by level then min_key) ====================

TEST(SecondaryIndexBTreeRepairTest, NonLeafPageSorting) {
    std::map<uint32_t, std::vector<uint8_t>> page_data;
    page_data[1] = create_page_with_records(1, 2, 100, {});  // level 2, no user records
    page_data[2] = create_page_with_records(2, 1, 100, {});  // level 1
    page_data[3] = create_page_with_records(3, 0, 100, {"AAA", "BBB"});  // leaf

    SecondaryIndexBTreeRepairer repairer(16384);
    std::vector<uint32_t> page_nos = {1, 2, 3};
    repairer.set_page_list(page_nos, page_data);

    auto key_ranges = repairer.extract_key_ranges();
    ASSERT_EQ(key_ranges.size(), 3);

    // Non-leaf pages have is_leaf=false
    for (const auto& kr : key_ranges) {
        if (kr.level > 0) {
            EXPECT_FALSE(kr.is_leaf);
        }
    }
}

// ==================== Test 12: Mixed leaf/non-leaf pages (correct prioritization) ====================

TEST(SecondaryIndexBTreeRepairTest, MixedLeafNonLeafPages) {
    std::map<uint32_t, std::vector<uint8_t>> page_data;
    page_data[1] = create_page_with_records(1, 1, 100, {});  // internal, level 1
    page_data[2] = create_page_with_records(2, 0, 100, {"AAA", "BBB"});  // leaf
    page_data[3] = create_page_with_records(3, 0, 100, {"CCC", "DDD"});  // leaf

    SecondaryIndexBTreeRepairer repairer(16384);
    std::vector<uint32_t> page_nos = {1, 2, 3};
    repairer.set_page_list(page_nos, page_data);

    auto key_ranges = repairer.extract_key_ranges();

    // Separate leaf and internal
    std::vector<PageKeyRange> leaf_ranges;
    std::vector<PageKeyRange> internal_ranges;
    for (const auto& kr : key_ranges) {
        if (kr.is_leaf) leaf_ranges.push_back(kr);
        else internal_ranges.push_back(kr);
    }

    EXPECT_EQ(leaf_ranges.size(), 2);
    EXPECT_EQ(internal_ranges.size(), 1);
}

// ==================== Test 13: Repair chain by content (full chain rebuild) ====================

TEST(SecondaryIndexBTreeRepairTest, RepairChainByContent) {
    std::map<uint32_t, std::vector<uint8_t>> page_data;
    // Page 3 has the smallest key range → should be first in chain
    page_data[3] = create_page_with_records(3, 0, 100, {"AAA", "BBB"});
    // Page 1 has middle key range → should be second
    page_data[1] = create_page_with_records(1, 0, 100, {"CCC", "DDD"});
    // Page 2 has largest key range → should be third
    page_data[2] = create_page_with_records(2, 0, 100, {"EEE", "FFF"});

    SecondaryIndexBTreeRepairer repairer(16384);
    std::vector<uint32_t> page_nos = {3, 1, 2};
    repairer.set_page_list(page_nos, page_data);

    bool result = repairer.repair_chain_by_content(page_nos);
    EXPECT_TRUE(result);

    // Verify stats
    const auto& stats = repairer.get_stats();
    EXPECT_EQ(stats.pages_relinked, 3);
    EXPECT_EQ(stats.chain_breaks_fixed, 1);
    EXPECT_EQ(stats.leaf_pages, 3);
    EXPECT_EQ(stats.pages_analyzed, 3);
}

// ==================== Test 14: Repair chain with corrupted page (graceful handling) ====================

TEST(SecondaryIndexBTreeRepairTest, RepairChainWithCorruptedPage) {
    std::map<uint32_t, std::vector<uint8_t>> page_data;
    // Valid page
    page_data[1] = create_page_with_records(1, 0, 100, {"AAA", "BBB"});
    // Corrupted page (too small)
    page_data[2] = std::vector<uint8_t>(100, 0);
    // Valid page
    page_data[3] = create_page_with_records(3, 0, 100, {"CCC", "DDD"});

    SecondaryIndexBTreeRepairer repairer(16384);
    std::vector<uint32_t> page_nos = {1, 2, 3};
    repairer.set_page_list(page_nos, page_data);

    // Should not crash or throw
    bool result = repairer.repair_chain_by_content(page_nos);
    // Only 2 valid pages (page 2 is too small to be INDEX)
    const auto& stats = repairer.get_stats();
    EXPECT_EQ(stats.leaf_pages, 2);
}

// ==================== Test 15: Verify FIL_PAGE_PREV/NEXT after repair (chain integrity) ====================

TEST(SecondaryIndexBTreeRepairTest, ChainIntegrityAfterRepair) {
    // Create pages and manually call relink_pages_by_content to verify pointers
    std::vector<uint32_t> page_nos = {3, 1, 2};
    auto page1_data = create_page_with_records(1, 0, 100, {"CCC", "DDD"});
    auto page2_data = create_page_with_records(2, 0, 100, {"EEE", "FFF"});
    auto page3_data = create_page_with_records(3, 0, 100, {"AAA", "BBB"});

    // Build sorted_pages in content order: page3 (AAA), page1 (CCC), page2 (EEE)
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> sorted_pages;
    sorted_pages.emplace_back(3, page3_data);
    sorted_pages.emplace_back(1, page1_data);
    sorted_pages.emplace_back(2, page2_data);

    SecondaryIndexBTreeRepairer repairer(16384);
    size_t relinked = repairer.relink_pages_by_content(sorted_pages);
    EXPECT_EQ(relinked, 3);

    // Verify chain: page3 → page1 → page2
    // page3 (first): prev=FIL_NULL, next=1
    EXPECT_EQ(read_page_prev(sorted_pages[0].second), FIL_NULL);
    EXPECT_EQ(read_page_next(sorted_pages[0].second), 1u);

    // page1 (middle): prev=3, next=2
    EXPECT_EQ(read_page_prev(sorted_pages[1].second), 3u);
    EXPECT_EQ(read_page_next(sorted_pages[1].second), 2u);

    // page2 (last): prev=1, next=FIL_NULL
    EXPECT_EQ(read_page_prev(sorted_pages[2].second), 1u);
    EXPECT_EQ(read_page_next(sorted_pages[2].second), FIL_NULL);
}

// ==================== Test 16: Integration with PageChainRebuilder ====================

TEST(SecondaryIndexBTreeRepairTest, PageChainRebuilderIntegration) {
    // Create ScannedPage list
    std::vector<ScannedPage> scanned_pages;
    ScannedPage sp1{}, sp2{}, sp3{};
    sp1.page_no = 1; sp1.type = FIL_PAGE_INDEX; sp1.level = 0; sp1.is_valid = true;
    sp2.page_no = 2; sp2.type = FIL_PAGE_INDEX; sp2.level = 0; sp2.is_valid = true;
    sp3.page_no = 3; sp3.type = FIL_PAGE_INDEX; sp3.level = 0; sp3.is_valid = true;
    scanned_pages.push_back(sp1);
    scanned_pages.push_back(sp2);
    scanned_pages.push_back(sp3);

    // Create page data map (wrong order: page1 has "EEE", page3 has "AAA")
    std::map<uint32_t, std::vector<uint8_t>> page_data;
    page_data[1] = create_page_with_records(1, 0, 100, {"EEE", "FFF"});
    page_data[2] = create_page_with_records(2, 0, 100, {"CCC", "DDD"});
    page_data[3] = create_page_with_records(3, 0, 100, {"AAA", "BBB"});

    PageChainRebuilder rebuilder(16384);
    rebuilder.rebuild_chain(scanned_pages);
    rebuilder.set_schema(create_test_schema());

    bool result = rebuilder.fix_chain_breaks_content_based(page_data);
    EXPECT_TRUE(result);
    EXPECT_GT(rebuilder.stats().fixed_chains, 0);
}

// ==================== Test 17: Integration with BPlusTreeTraversal ====================

TEST(SecondaryIndexBTreeRepairTest, BPlusTreeTraversalIntegration) {
    // This test verifies the fix_chain_breaks_content_based method on
    // BPlusTreeTraversal by checking that it doesn't crash and returns results.
    // Full integration testing requires a real IBD file.

    std::vector<ScannedPage> pages;
    ScannedPage sp{};
    sp.page_no = 1; sp.type = FIL_PAGE_INDEX; sp.level = 0;
    sp.is_valid = true; sp.prev_page = FIL_NULL; sp.next_page = FIL_NULL;
    pages.push_back(sp);

    std::map<uint32_t, std::vector<uint8_t>> page_data;
    page_data[1] = create_page_with_records(1, 0, 100, {"AAA", "BBB"});

    // Verify fix_chain_breaks_content_based exists and can be called
    // (the method was added to BPlusTreeTraversal via the modifications)
    BPlusTreeTraversal traversal(pages, create_test_schema());
    // BPlusTreeTraversal::fix_chain_breaks_content_based modifies the internal
    // pages_ vector's prev/next pointers based on content order
    // We just verify it doesn't throw on a single-page case
    EXPECT_NO_THROW(traversal.fix_chain_breaks_content_based(page_data));
}

// ==================== Test 18: Empty page list (graceful handling) ====================

TEST(SecondaryIndexBTreeRepairTest, EmptyPageList) {
    SecondaryIndexBTreeRepairer repairer(16384);
    std::vector<uint32_t> empty_nos;

    bool result = repairer.repair_chain_by_content(empty_nos);
    EXPECT_FALSE(result);

    auto key_ranges = repairer.extract_key_ranges();
    EXPECT_TRUE(key_ranges.empty());
}

// ==================== Test 19: Single page (no-op) ====================

TEST(SecondaryIndexBTreeRepairTest, SinglePageNoop) {
    std::map<uint32_t, std::vector<uint8_t>> page_data;
    page_data[1] = create_page_with_records(1, 0, 100, {"AAA", "BBB"});

    SecondaryIndexBTreeRepairer repairer(16384);
    std::vector<uint32_t> page_nos = {1};
    repairer.set_page_list(page_nos, page_data);

    bool result = repairer.repair_chain_by_content(page_nos);
    // Single page: repair_chain_by_content returns false (no-op)
    EXPECT_FALSE(result);

    const auto& stats = repairer.get_stats();
    EXPECT_EQ(stats.pages_relinked, 0);
}

// ==================== Test 20: Schema-aware vs schema-unaware key extraction ====================

TEST(SecondaryIndexBTreeRepairTest, KeyExtractionConsistency) {
    // Verify key extraction works the same way regardless of schema setting
    auto page = create_page_with_records(1, 0, 100, {"KEYDATA"});

    // Without schema
    SecondaryIndexBTreeRepairer repairer_no_schema(16384);
    auto range_no_schema = repairer_no_schema.extract_key_range_from_raw(page.data());
    ASSERT_TRUE(range_no_schema.has_value());

    // With schema
    SecondaryIndexBTreeRepairer repairer_with_schema(16384);
    repairer_with_schema.set_schema(create_test_schema());
    auto range_with_schema = repairer_with_schema.extract_key_range_from_raw(page.data());
    ASSERT_TRUE(range_with_schema.has_value());

    // Both should extract the same raw key bytes
    EXPECT_EQ(range_no_schema->min_key, range_with_schema->min_key);
    EXPECT_EQ(range_no_schema->max_key, range_with_schema->max_key);
    EXPECT_EQ(range_no_schema->is_leaf, range_with_schema->is_leaf);
    EXPECT_EQ(range_no_schema->level, range_with_schema->level);
}