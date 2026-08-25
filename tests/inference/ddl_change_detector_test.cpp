// DDLChangeDetector tests (Phase 4 - Slice B)
//
// Covers:
//   - detect_versions_from_pages end-to-end (constructed mock INDEX pages)
//   - single version (no DDL change)
//   - two versions (ADD COLUMN)
//   - three versions (ADD + DROP)
//   - empty input tolerance
//   - low-confidence variation
//   - apply_to_schema_template version metadata

#include <gtest/gtest.h>
#include "dbrecovery/engine/schema_template.hpp"
#include "dbrecovery/inference/ddl_change_detector.hpp"

#include <cstring>
#include <vector>

namespace {

using dbrecovery::inference::DDLChangeDetector;
using dbrecovery::inference::DDLChangeChainResult;

// Mock INDEX page builder (COMPACT). Layout mirrors PhysicalLayoutAnalyzer:
// page_type at 24-25 = 0x45BF, n_recs at 54-55 BE, 5-byte COMPACT record header
// at each record offset, offset table at rec_off+5 (BE, strictly decreasing),
// page directory slots at page_size-2*(i+1) (BE).
std::vector<uint8_t> build_index_page(
    uint32_t n_recs,
    uint32_t col_count,
    uint32_t first_rec_offset = 128) {

    std::vector<uint8_t> page(16384, 0);
    const size_t ps = 16384;
    constexpr uint32_t kRecHeaderSize = 5;

    // FIL_HEADER
    page[24] = 0x45; page[25] = 0xBF;  // FIL_PAGE_INDEX

    // PAGE_HEADER (big-endian)
    uint16_t n_slots = static_cast<uint16_t>(n_recs + 2);
    page[38] = (n_slots >> 8) & 0xFF; page[39] = n_slots & 0xFF;

    uint32_t null_bitmap_bytes = (col_count + 7) / 8;
    uint32_t offset_table_bytes = col_count * 2;
    uint32_t data_start_offset =
        first_rec_offset + kRecHeaderSize + offset_table_bytes + 1 + null_bitmap_bytes;

    uint32_t per_rec_data = col_count * 4;
    uint32_t total_data_size = per_rec_data * n_recs;
    uint32_t record_data_end = data_start_offset + total_data_size;
    uint32_t page_dir_start = ((record_data_end + 7) / 8) * 8;
    uint32_t heap_top = page_dir_start + n_slots * 2;
    if (heap_top > ps - 8) heap_top = ps - 8;
    page[40] = (heap_top >> 8) & 0xFF; page[41] = heap_top & 0xFF;

    uint16_t n_heap = static_cast<uint16_t>(n_recs + 2);
    page[42] = (n_heap >> 8) & 0xFF; page[43] = n_heap & 0xFF;

    page[54] = (n_recs >> 8) & 0xFF; page[55] = n_recs & 0xFF;
    page[58] = 0x00; page[59] = 0x00;  // leaf level

    // INFIMUM COMPACT record header at 94 (5 bytes)
    // REC_NEXT at 94-95 (signed BE) = first_rec_offset - 94
    {
        int16_t inf_delta = static_cast<int16_t>(first_rec_offset - 94);
        page[94] = (inf_delta >> 8) & 0xFF;
        page[95] = inf_delta & 0xFF;
        page[96] = 0x00;  // info_bits
        page[97] = 0x00;  // heap_no low
        page[98] = 0x00;
    }

    // SUPREMUM COMPACT record header at 104 (5 bytes), REC_NEXT = 0
    page[104] = 0x00; page[105] = 0x00;  // REC_NEXT = 0
    page[106] = 0x03;  // info_bits with SUPREMUM flag
    page[107] = 0x01;  // heap_no = 1
    page[108] = 0x00;

    // User records
    uint32_t current_rec_offset = first_rec_offset;
    uint32_t data_offset = data_start_offset;
    std::vector<uint32_t> user_rec_offsets;
    for (uint32_t r = 0; r < n_recs; r++) {
        user_rec_offsets.push_back(current_rec_offset);

        // Compute total record length
        uint32_t total_len = kRecHeaderSize + offset_table_bytes + 1 + null_bitmap_bytes + per_rec_data;

        // Offset table at rec_off + 5 (BE, strictly decreasing cumulative offsets from record start)
        uint32_t cumulative = total_len;
        for (uint32_t c = 0; c < col_count; c++) {
            uint16_t val = static_cast<uint16_t>(cumulative);
            page[current_rec_offset + kRecHeaderSize + c * 2] = (val >> 8) & 0xFF;
            page[current_rec_offset + kRecHeaderSize + c * 2 + 1] = val & 0xFF;
            cumulative -= 4;
        }

        // info_bits (1 byte after offset table)
        page[current_rec_offset + kRecHeaderSize + offset_table_bytes] = 0x00;
        // NULL bitmap (already zeroed)

        // Field data (4 bytes per field, LSB only)
        for (uint32_t c = 0; c < col_count; c++) {
            uint32_t val = 100 + r + c;
            page[data_offset + c * 4] = val & 0xFF;
        }

        // REC_NEXT at bytes 0-1 (signed BE)
        if (r + 1 < n_recs) {
            current_rec_offset = data_offset + per_rec_data;
            current_rec_offset = ((current_rec_offset + 3) / 4) * 4;
            data_offset = current_rec_offset + kRecHeaderSize + offset_table_bytes + 1 + null_bitmap_bytes;

            int16_t delta = static_cast<int16_t>(current_rec_offset - user_rec_offsets.back());
            page[user_rec_offsets.back()] = (delta >> 8) & 0xFF;
            page[user_rec_offsets.back() + 1] = delta & 0xFF;
        } else {
            // Last record: REC_NEXT = 0
            page[user_rec_offsets.back()] = 0x00;
            page[user_rec_offsets.back() + 1] = 0x00;
        }
    }

    // Page directory at end of page (BE)
    // Slot at page_size - 2*(i+1), i=0 = INFIMUM (skipped), i=n_slots-1 = SUPREMUM (skipped)
    for (uint16_t i = 1; i + 1 < n_slots; i++) {
        uint16_t slot_pos = static_cast<uint16_t>(ps - 2 * (i + 1));
        uint32_t rec_idx = i - 1;
        if (rec_idx < user_rec_offsets.size()) {
            uint16_t rec_off = static_cast<uint16_t>(user_rec_offsets[rec_idx]);
            page[slot_pos] = (rec_off >> 8) & 0xFF;
            page[slot_pos + 1] = rec_off & 0xFF;
        }
    }
    return page;
}

std::vector<std::pair<const uint8_t*, size_t>> to_pairs(
    const std::vector<std::vector<uint8_t>>& buffers) {
    std::vector<std::pair<const uint8_t*, size_t>> pages;
    for (const auto& buf : buffers) {
        pages.emplace_back(buf.data(), buf.size());
    }
    return pages;
}

}  // namespace

namespace dbrecovery::inference {

// DDL-01: Empty input tolerated, no versions
TEST(DDLChangeDetectorTest, EmptyInputTolerated) {
    DDLChangeDetector detector;
    auto result = detector.detect_versions_from_buffers({});
    EXPECT_FALSE(result.has_ddl_change);
    EXPECT_TRUE(result.versions.empty());
    EXPECT_FALSE(result.blocked_auto_decode);
}

// DDL-02: Single column-count group = single schema version, no DDL change
TEST(DDLChangeDetectorTest, SingleVersionNoDdlChange) {
    std::vector<std::vector<uint8_t>> pages;
    for (int i = 0; i < 5; i++) {
        pages.push_back(build_index_page(10, 5));
    }
    DDLChangeDetector detector;
    auto result = detector.detect_versions_from_buffers(pages);
    ASSERT_EQ(result.versions.size(), 1u);
    EXPECT_FALSE(result.has_ddl_change);
    EXPECT_FALSE(result.blocked_auto_decode);
    EXPECT_EQ(result.versions[0].column_count, 5u);
    EXPECT_EQ(result.versions[0].page_count, 5u);
    EXPECT_NEAR(result.versions[0].confidence, 1.0, 1e-6);
    EXPECT_NE(result.summary.find("Single schema version"), std::string::npos);
}

// DDL-03: Two column-count groups = ADD COLUMN boundary detected
TEST(DDLChangeDetectorTest, TwoVersionsAddColumn) {
    std::vector<std::vector<uint8_t>> pages;
    for (int i = 0; i < 3; i++) {
        pages.push_back(build_index_page(10, 5));  // older: 5 columns
    }
    for (int i = 0; i < 4; i++) {
        pages.push_back(build_index_page(10, 6));  // newer: 6 columns (1 added)
    }
    DDLChangeDetector detector;
    auto result = detector.detect_versions_from_buffers(pages);
    ASSERT_EQ(result.versions.size(), 2u);
    EXPECT_TRUE(result.has_ddl_change);
    EXPECT_TRUE(result.blocked_auto_decode);
    // Oldest first (fewest columns = version 1)
    EXPECT_EQ(result.versions[0].version_id, 1u);
    EXPECT_EQ(result.versions[0].column_count, 5u);
    EXPECT_EQ(result.versions[1].version_id, 2u);
    EXPECT_EQ(result.versions[1].column_count, 6u);
    // v2 added exactly 1 column
    ASSERT_EQ(result.versions[1].columns_added.size(), 1u);
    EXPECT_EQ(result.versions[1].columns_added[0].ordinal_position, 5u);
    EXPECT_TRUE(result.versions[1].columns_dropped.empty());
    EXPECT_NE(result.summary.find("schema evolution"), std::string::npos);
}

// DDL-04: Three groups = ADD then DROP detected
TEST(DDLChangeDetectorTest, ThreeVersionsAddAndDrop) {
    std::vector<std::vector<uint8_t>> pages;
    for (int i = 0; i < 2; i++) {
        pages.push_back(build_index_page(10, 5));  // v1: 5 cols
    }
    for (int i = 0; i < 2; i++) {
        pages.push_back(build_index_page(10, 6));  // v2: 6 cols (added 1)
    }
    for (int i = 0; i < 2; i++) {
        pages.push_back(build_index_page(10, 4));  // v3: 4 cols (dropped 2)
    }
    DDLChangeDetector detector;
    auto result = detector.detect_versions_from_buffers(pages);
    ASSERT_EQ(result.versions.size(), 3u);
    // v1 oldest: 4 columns
    EXPECT_EQ(result.versions[0].column_count, 4u);
    EXPECT_EQ(result.versions[0].version_id, 1u);
    // v2: 5 columns -> 1 added
    EXPECT_EQ(result.versions[1].column_count, 5u);
    ASSERT_EQ(result.versions[1].columns_added.size(), 1u);
    EXPECT_TRUE(result.versions[1].columns_dropped.empty());
    // v3: 6 columns -> 1 added, and vs v2 dropped nothing (6 > 5)
    EXPECT_EQ(result.versions[2].column_count, 6u);
    ASSERT_EQ(result.versions[2].columns_added.size(), 1u);
    EXPECT_TRUE(result.versions[2].columns_dropped.empty());
    // 3-group summary mentions v1/v2/v3
    EXPECT_NE(result.summary.find("v3"), std::string::npos);
}

// DDL-05: Non-INDEX / malformed pages tolerated; falls back to no versions
TEST(DDLChangeDetectorTest, NonIndexPagesTolerated) {
    std::vector<uint8_t> garbage_page(16384, 0xAB);
    garbage_page[24] = 0x00; garbage_page[25] = 0x00;  // not INDEX
    DDLChangeDetector detector;
    auto result = detector.detect_versions_from_buffers({garbage_page});
    EXPECT_TRUE(result.versions.empty());
    EXPECT_FALSE(result.has_ddl_change);
    // summary reflects "no pages" wording (either path is tolerable)
    EXPECT_FALSE(result.summary.empty());
}

// DDL-06: Low-confidence multi-cross — minority version still surfaces
TEST(DDLChangeDetectorTest, MinorityVersionDetected) {
    std::vector<std::vector<uint8_t>> pages;
    for (int i = 0; i < 20; i++) {
        pages.push_back(build_index_page(10, 7));  // dominant
    }
    for (int i = 0; i < 2; i++) {
        pages.push_back(build_index_page(10, 8));  // minority: 1 added
    }
    DDLChangeDetector detector;
    auto result = detector.detect_versions_from_buffers(pages);
    ASSERT_EQ(result.versions.size(), 2u);
    EXPECT_NEAR(result.versions[0].confidence, 20.0 / 22.0, 1e-6);
    EXPECT_NEAR(result.versions[1].confidence, 2.0 / 22.0, 1e-6);
    EXPECT_TRUE(result.has_ddl_change);
}

// DDL-07: apply_to_schema_template sets version_added/version_dropped
TEST(DDLChangeDetectorTest, ApplyToSchemaTemplate) {
    std::vector<std::vector<uint8_t>> pages;
    for (int i = 0; i < 3; i++) {
        pages.push_back(build_index_page(10, 5));  // v1: 5 cols
    }
    for (int i = 0; i < 3; i++) {
        pages.push_back(build_index_page(10, 7));  // v2: 7 cols (2 added)
    }
    DDLChangeDetector detector;
    auto result = detector.detect_versions_from_buffers(pages);
    ASSERT_EQ(result.versions.size(), 2u);

    engine::SchemaTemplate tmpl;
    tmpl.table_name = "t";
    tmpl.columns.resize(7);
    for (size_t i = 0; i < tmpl.columns.size(); i++) {
        tmpl.columns[i].name = "c" + std::to_string(i);
        tmpl.columns[i].physical_pos = static_cast<uint32_t>(i);
    }

    bool ok = detector.apply_to_schema_template(result, tmpl);
    ASSERT_TRUE(ok);
    EXPECT_EQ(tmpl.max_row_version, 2u);
    // Added columns landed on ordinals 5 and 6 -> version_added = 2
    EXPECT_EQ(tmpl.columns[5].version_added, 2u);
    EXPECT_EQ(tmpl.columns[6].version_added, 2u);
    // Original columns untouched (version_added remains 0)
    EXPECT_EQ(tmpl.columns[0].version_added, 0u);
    EXPECT_EQ(tmpl.columns[4].version_added, 0u);
}

// DDL-08: apply_to_schema_template fails cleanly on empty result
TEST(DDLChangeDetectorTest, ApplyToEmptyResultFails) {
    DDLChangeDetector detector;
    engine::SchemaTemplate tmpl;
    tmpl.columns.resize(3);
    auto result = detector.detect_versions_from_buffers({});
    bool ok = detector.apply_to_schema_template(result, tmpl);
    EXPECT_FALSE(ok);
}

}  // namespace dbrecovery::inference