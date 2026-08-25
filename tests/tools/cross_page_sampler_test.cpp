// CrossPageSampler tests (Phase 4 - Slice C)
//
// Covers:
//   - compute_field_stats: empty, integer, mixed, fixed-length
//   - sample_pages: empty, single page, non-INDEX, multi-version DDL
//   - FieldStats correctness (null_ratio, len_mean, ascii_ratio, etc.)

#include <gtest/gtest.h>
#include "dbrecovery/tools/cross_page_sampler.hpp"

#include <cstring>
#include <vector>

namespace {

using dbrecovery::tools::CrossPageSampler;
using dbrecovery::tools::FieldStats;

// ==================== compute_field_stats tests ====================

// CPS-FS-01: Empty input returns empty stats
TEST(CrossPageSamplerTest, ComputeFieldStatsEmpty) {
    auto stats = CrossPageSampler::compute_field_stats({});
    EXPECT_EQ(stats.sample_count, 0u);
    EXPECT_EQ(stats.null_ratio, 0.0);
    EXPECT_EQ(stats.len_min, 0.0);
    EXPECT_EQ(stats.len_max, 0.0);
}

// CPS-FS-02: Integer values detected
TEST(CrossPageSamplerTest, ComputeFieldStatsIntegers) {
    std::vector<std::vector<uint8_t>> values;
    for (int i = 1; i <= 10; i++) {
        std::string s = std::to_string(i);
        values.emplace_back(s.begin(), s.end());
    }
    auto stats = CrossPageSampler::compute_field_stats(values);
    EXPECT_EQ(stats.sample_count, 10u);
    EXPECT_EQ(stats.null_ratio, 0.0);
    EXPECT_GT(stats.integer_ratio, 0.9);
    EXPECT_GT(stats.digit_ratio, 0.9);
    EXPECT_NEAR(stats.len_mean, 1.5, 0.6);  // 1,2,3,...,10 -> avg length ~1.5
    EXPECT_GT(stats.distinct_count, 0u);
}

// CPS-FS-03: Mixed types (integers + strings) detected correctly
TEST(CrossPageSamplerTest, ComputeFieldStatsMixed) {
    std::vector<std::vector<uint8_t>> values;
    // 5 integers
    for (int i = 1; i <= 5; i++) {
        std::string s = std::to_string(i);
        values.emplace_back(s.begin(), s.end());
    }
    // 5 text strings
    for (int i = 0; i < 5; i++) {
        std::string s = "hello";
        values.emplace_back(s.begin(), s.end());
    }
    auto stats = CrossPageSampler::compute_field_stats(values);
    EXPECT_EQ(stats.sample_count, 10u);
    // Half are integers
    EXPECT_NEAR(stats.integer_ratio, 0.5, 0.1);
    // Half are alpha
    EXPECT_NEAR(stats.alpha_ratio, 0.5, 0.1);
    EXPECT_GT(stats.ascii_ratio, 0.9);
}

// CPS-FS-04: Fixed-length values detected
TEST(CrossPageSamplerTest, ComputeFieldStatsFixedLength) {
    std::vector<std::vector<uint8_t>> values;
    for (int i = 0; i < 5; i++) {
        std::vector<uint8_t> v = {0x01, 0x02, 0x03, 0x04};
        values.push_back(v);
    }
    auto stats = CrossPageSampler::compute_field_stats(values);
    EXPECT_TRUE(stats.is_fixed_length);
    EXPECT_EQ(stats.fixed_size, 4u);
    EXPECT_EQ(stats.len_min, 4.0);
    EXPECT_EQ(stats.len_max, 4.0);
    EXPECT_EQ(stats.len_stddev, 0.0);
}

// CPS-FS-05: Null/empty values handled correctly
TEST(CrossPageSamplerTest, ComputeFieldStatsWithNulls) {
    std::vector<std::vector<uint8_t>> values;
    values.emplace_back();  // empty = null
    values.emplace_back();  // empty = null
    std::string s = "test";
    values.emplace_back(s.begin(), s.end());
    auto stats = CrossPageSampler::compute_field_stats(values);
    EXPECT_EQ(stats.sample_count, 3u);
    EXPECT_NEAR(stats.null_ratio, 2.0 / 3.0, 1e-6);
    EXPECT_NEAR(stats.empty_ratio, 2.0 / 3.0, 1e-6);
    EXPECT_NEAR(stats.len_mean, 4.0 / 3.0, 1e-6);
}

// CPS-FS-06: Character-class ratios detected
TEST(CrossPageSamplerTest, ComputeFieldStatsCharClass) {
    std::vector<std::vector<uint8_t>> values;
    std::string email = "user@example.com";
    values.emplace_back(email.begin(), email.end());
    std::string url = "https://example.com/path";
    values.emplace_back(url.begin(), url.end());
    auto stats = CrossPageSampler::compute_field_stats(values);
    EXPECT_EQ(stats.sample_count, 2u);
    EXPECT_GT(stats.has_at_ratio, 0.0);   // email has @
    EXPECT_GT(stats.has_slash_ratio, 0.0); // url has /
    EXPECT_GT(stats.has_dot_ratio, 0.0);   // both have .
    EXPECT_GT(stats.has_colon_ratio, 0.0); // url has :
}

// ==================== sample_pages tests ====================

// Mock INDEX page builder (COMPACT format)
// Mirrors the helper in ddl_change_detector_test.cpp
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

    // INFIMUM COMPACT record header at 94 (5 bytes): REC_NEXT signed BE
    {
        int16_t inf_delta = static_cast<int16_t>(first_rec_offset - 94);
        page[94] = (inf_delta >> 8) & 0xFF;
        page[95] = inf_delta & 0xFF;
        page[96] = 0x00; page[97] = 0x00; page[98] = 0x00;
    }

    // SUPREMUM COMPACT record header at 104: REC_NEXT = 0
    page[104] = 0x00; page[105] = 0x00;
    page[106] = 0x03;  // info_bits with SUPREMUM flag
    page[107] = 0x01; page[108] = 0x00;

    // User records
    uint32_t current_rec_offset = first_rec_offset;
    uint32_t data_offset = data_start_offset;
    std::vector<uint32_t> user_rec_offsets;
    for (uint32_t r = 0; r < n_recs; r++) {
        user_rec_offsets.push_back(current_rec_offset);

        uint32_t total_len = kRecHeaderSize + offset_table_bytes + 1 + null_bitmap_bytes + per_rec_data;

        // Offset table at rec_off + 5 (BE, strictly decreasing cumulative offsets)
        uint32_t cumulative = total_len;
        for (uint32_t c = 0; c < col_count; c++) {
            uint16_t val = static_cast<uint16_t>(cumulative);
            page[current_rec_offset + kRecHeaderSize + c * 2] = (val >> 8) & 0xFF;
            page[current_rec_offset + kRecHeaderSize + c * 2 + 1] = val & 0xFF;
            cumulative -= 4;
        }

        // info_bits (1 byte after offset table)
        page[current_rec_offset + kRecHeaderSize + offset_table_bytes] = 0x00;

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
            page[user_rec_offsets.back()] = 0x00;
            page[user_rec_offsets.back() + 1] = 0x00;
        }
    }

    // Page directory at end of page (BE)
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

// CPS-SP-01: Empty input returns empty result
TEST(CrossPageSamplerTest, SamplePagesEmpty) {
    CrossPageSampler sampler;
    auto result = sampler.sample_pages({});
    EXPECT_EQ(result.total_pages_scanned, 0u);
    EXPECT_EQ(result.detected_column_count, 0u);
    EXPECT_FALSE(result.has_ddl_change);
}

// CPS-SP-02: Single page with INDEX data
TEST(CrossPageSamplerTest, SamplePagesSinglePage) {
    auto page = build_index_page(10, 5);
    CrossPageSampler sampler;
    auto result = sampler.sample_pages(to_pairs({page}));
    EXPECT_EQ(result.total_pages_scanned, 1u);
    EXPECT_EQ(result.detected_column_count, 5u);
    EXPECT_GT(result.total_records, 0u);
    EXPECT_FALSE(result.has_ddl_change);
    // Consensus should have field_stats for 5 columns
    EXPECT_EQ(result.consensus.n_fields, 5u);
    EXPECT_EQ(result.consensus.field_stats.size(), 5u);
}

// CPS-SP-03: Non-INDEX pages are tolerated
TEST(CrossPageSamplerTest, SamplePagesNonIndex) {
    std::vector<uint8_t> garbage_page(16384, 0xAB);
    garbage_page[24] = 0x00; garbage_page[25] = 0x00;
    CrossPageSampler sampler;
    auto result = sampler.sample_pages(to_pairs({garbage_page}));
    EXPECT_EQ(result.total_pages_scanned, 1u);
    EXPECT_EQ(result.detected_column_count, 0u);
    EXPECT_TRUE(result.consensus.field_stats.empty());
}

// CPS-SP-04: Multiple pages with same column count
TEST(CrossPageSamplerTest, SamplePagesMultiplePages) {
    std::vector<std::vector<uint8_t>> pages;
    for (int i = 0; i < 3; i++) {
        pages.push_back(build_index_page(8, 5));
    }
    CrossPageSampler sampler;
    auto result = sampler.sample_pages(to_pairs(pages));
    EXPECT_EQ(result.total_pages_scanned, 3u);
    EXPECT_EQ(result.detected_column_count, 5u);
    EXPECT_EQ(result.versions.size(), 1u);
    EXPECT_FALSE(result.has_ddl_change);
    // Consensus field_stats populated
    ASSERT_EQ(result.consensus.field_stats.size(), 5u);
    // Each column should have records
    EXPECT_GT(result.consensus.field_stats[0].sample_count, 0u);
}

// CPS-SP-05: Multi-version DDL detected (different column counts)
TEST(CrossPageSamplerTest, SamplePagesMultiVersion) {
    std::vector<std::vector<uint8_t>> pages;
    for (int i = 0; i < 3; i++) {
        pages.push_back(build_index_page(8, 5));  // 5 columns
    }
    for (int i = 0; i < 2; i++) {
        pages.push_back(build_index_page(8, 6));  // 6 columns
    }
    CrossPageSampler sampler;
    auto result = sampler.sample_pages(to_pairs(pages));
    EXPECT_EQ(result.total_pages_scanned, 5u);
    EXPECT_EQ(result.detected_column_count, 5u);  // consensus = most common
    EXPECT_TRUE(result.has_ddl_change);
    EXPECT_EQ(result.versions.size(), 2u);
    // Consensus should use the dominant column count (5)
    EXPECT_EQ(result.consensus.n_fields, 5u);
}

// CPS-SP-06: Mixed INDEX and non-INDEX pages
TEST(CrossPageSamplerTest, SamplePagesMixedIndexAndNonIndex) {
    std::vector<std::vector<uint8_t>> pages;
    pages.push_back(build_index_page(6, 4));  // INDEX
    std::vector<uint8_t> garbage(16384, 0xFF);
    garbage[24] = 0x00; garbage[25] = 0x00;
    pages.push_back(garbage);                 // non-INDEX
    pages.push_back(build_index_page(6, 4));  // INDEX
    CrossPageSampler sampler;
    auto result = sampler.sample_pages(to_pairs(pages));
    EXPECT_EQ(result.total_pages_scanned, 3u);
    EXPECT_EQ(result.detected_column_count, 4u);
    EXPECT_EQ(result.consensus.n_fields, 4u);
}

}  // namespace