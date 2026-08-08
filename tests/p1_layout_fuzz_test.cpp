/**
 * @file p1_layout_fuzz_test.cpp
 * @brief P1 Format Correctness Fuzz Tests for PhysicalLayoutAnalyzer
 *
 * Tests the PhysicalLayoutAnalyzer and StatisticalTypeClassifier with
 * malformed page data to verify graceful handling and crash safety.
 */

#include <gtest/gtest.h>
#include "dbrecovery/inference/physical_layout_analyzer.hpp"

#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>
#include <cmath>
#include <limits>

namespace dbrecovery {
namespace test {

using namespace dbrecovery::inference;

// ==================== Helpers ====================

/** Build a minimal valid InnoDB INDEX page with a few records. */
static std::vector<uint8_t> make_index_page_with_records(
    uint16_t n_recs,
    const std::vector<std::vector<uint8_t>>& field_data)
{
    std::vector<uint8_t> page(16384, 0);

    // FIL header: FIL_PAGE_INDEX = 0x45BF
    page[0] = 0x12; page[1] = 0x34; page[2] = 0x56; page[3] = 0x78;
    page[4] = 0x00; page[5] = 0x00; page[6] = 0x00; page[7] = 0x01;
    page[16] = 0x00; page[17] = 0x00; page[18] = 0x00; page[19] = 0x01;
    page[20] = 0x23; page[21] = 0x45; page[22] = 0x67; page[23] = 0x89;
    page[24] = 0x45; page[25] = 0xBF;  // FIL_PAGE_INDEX
    page[34] = 0x00; page[35] = 0x00; page[36] = 0x00; page[37] = 0x01;

    // Compute total record data size
    size_t total_data = 0;
    size_t max_field_count = 0;
    for (const auto& fv : field_data) {
        total_data += fv.size();
        max_field_count = std::max(max_field_count, fv.size());
    }

    // Layout: offset_table (2 bytes per field) + info_bits(1) + null_bitmap + field_data
    uint32_t n_fields = static_cast<uint32_t>(field_data.size());
    uint32_t null_bytes = (n_fields + 7) / 8;
    uint32_t per_rec_overhead = n_fields * 2 + 1 + null_bytes;
    uint32_t total_rec_size = static_cast<uint32_t>(per_rec_overhead + total_data);

    // Place records starting at offset 120
    uint16_t rec_start = 120;
    uint16_t heap_top = static_cast<uint16_t>(rec_start + n_recs * total_rec_size + 100);

    if (heap_top > page.size() - 100) heap_top = page.size() - 100;

    page[40] = static_cast<uint8_t>(heap_top & 0xFF);
    page[41] = static_cast<uint8_t>((heap_top >> 8) & 0xFF);
    page[54] = static_cast<uint8_t>(n_recs & 0xFF);
    page[55] = static_cast<uint8_t>((n_recs >> 8) & 0xFF);

    // INFIMUM at offset 99, points to first user record
    page[100] = static_cast<uint8_t>(rec_start & 0xFF);
    page[101] = static_cast<uint8_t>((rec_start >> 8) & 0xFF);

    // Write records
    uint16_t current_off = rec_start;
    std::vector<uint16_t> rec_offsets;

    for (int r = 0; r < n_recs; r++) {
        rec_offsets.push_back(current_off);

        // Build offset table: offsets from end of record
        // total_len, then field boundaries (strictly decreasing)
        uint16_t data_offset = per_rec_overhead; // offset from start of record to data area

        // Compute field sizes
        std::vector<uint16_t> field_sizes;
        uint16_t running = 0;
        for (const auto& fv : field_data) {
            uint16_t sz = static_cast<uint16_t>(fv.size());
            field_sizes.push_back(sz);
            running += sz;
        }

        uint16_t total_len = static_cast<uint16_t>(per_rec_overhead + running);

        // Write offset table: first entry = total_len, then cumulative from end
        uint16_t cumulative = total_len;
        page[current_off] = static_cast<uint8_t>(cumulative & 0xFF);
        page[current_off + 1] = static_cast<uint8_t>((cumulative >> 8) & 0xFF);
        cumulative -= field_sizes[0];

        for (uint32_t f = 1; f < n_fields; f++) {
            page[current_off + f * 2] = static_cast<uint8_t>(cumulative & 0xFF);
            page[current_off + f * 2 + 1] = static_cast<uint8_t>((cumulative >> 8) & 0xFF);
            if (f < n_fields - 1) {
                cumulative -= field_sizes[f];
            }
        }

        // info_bits
        page[current_off + n_fields * 2] = 0x00;
        // null bitmap
        for (uint32_t nb = 0; nb < null_bytes; nb++) {
            page[current_off + n_fields * 2 + 1 + nb] = 0x00;
        }

        // Write field data
        uint16_t data_pos = current_off + per_rec_overhead;
        for (const auto& fv : field_data) {
            if (data_pos + fv.size() <= page.size()) {
                std::memcpy(page.data() + data_pos, fv.data(), fv.size());
                data_pos += fv.size();
            }
        }

        current_off += total_len;
    }

    // Page directory
    uint16_t n_slots = static_cast<uint16_t>(2 + n_recs); // INFIMUM + records + SUPREMUM
    uint16_t dir_end = static_cast<uint16_t>(page.size() - 2);

    page[dir_end] = static_cast<uint8_t>(n_slots & 0xFF);
    page[dir_end + 1] = static_cast<uint8_t>((n_slots >> 8) & 0xFF);

    // Slot 0 = INFIMUM
    uint16_t slot_pos = static_cast<uint16_t>(dir_end - n_slots * 2);
    page[slot_pos] = 0x63; page[slot_pos + 1] = 0x00;

    // Record slots
    for (int r = 0; r < n_recs; r++) {
        slot_pos = static_cast<uint16_t>(dir_end - (n_slots - 1 - r) * 2);
        page[slot_pos] = static_cast<uint8_t>(rec_offsets[r] & 0xFF);
        page[slot_pos + 1] = static_cast<uint8_t>((rec_offsets[r] >> 8) & 0xFF);
    }

    // Last slot = SUPREMUM
    slot_pos = static_cast<uint16_t>(dir_end - 2);
    page[slot_pos] = static_cast<uint8_t>(heap_top & 0xFF);
    page[slot_pos + 1] = static_cast<uint8_t>((heap_top >> 8) & 0xFF);

    return page;
}

/** Build a simple INDEX page with N records, each with 4 fields of varying sizes. */
static std::vector<uint8_t> make_simple_index_page(int n_recs) {
    std::vector<std::vector<uint8_t>> fields;
    // 4 fields: 4 bytes, 4 bytes, 8 bytes, 4 bytes
    fields.push_back({0x00, 0x00, 0x00, 0x01});  // int
    fields.push_back({0x00, 0x00, 0x00, 0x02});  // int
    fields.push_back({0x3F, 0xF1, 0x99, 0x99, 0x99, 0x99, 0x99, 0x9A}); // double
    fields.push_back({0x00, 0x00, 0x00, 0x03});  // int

    return make_index_page_with_records(static_cast<uint16_t>(n_recs), fields);
}

// ==================== Corrupted Page Directory Fuzz ====================

TEST(P1LayoutFuzzTest, CorruptedPageDirectory) {
    PhysicalLayoutAnalyzer analyzer;

    for (int iter = 0; iter < 100; iter++) {
        auto page = make_simple_index_page(3);

        // Corrupt page directory slot offsets to point beyond page boundaries
        // Page directory is at end of page; slot values are 2-byte LE offsets
        size_t dir_region = page.size() - 10;
        size_t corr_offset = dir_region + static_cast<size_t>(rand() % 8);
        page[corr_offset] = static_cast<uint8_t>(rand() % 256);
        page[corr_offset + 1] = static_cast<uint8_t>(rand() % 256);

        auto info = analyzer.analyze_page(page.data(), page.size());
        // Should not crash. Result may be empty or valid.
        EXPECT_TRUE(!info.has_value() || info->total_columns <= 100);
    }
}

// ==================== Corrupted Offset Table Fuzz ====================

TEST(P1LayoutFuzzTest, CorruptedOffsetTable) {
    PhysicalLayoutAnalyzer analyzer;

    for (int iter = 0; iter < 100; iter++) {
        auto page = make_simple_index_page(3);

        // Corrupt the offset table entries within records
        // Records start at offset 120; offset table is at start of each record
        size_t rec_region_start = 120;
        size_t rec_region_end = 400;
        size_t corr_offset = rec_region_start + static_cast<size_t>(rand() % (rec_region_end - rec_region_start));

        // Write random bytes as offset table values
        page[corr_offset] = static_cast<uint8_t>(rand() % 256);
        if (corr_offset + 1 < page.size()) {
            page[corr_offset + 1] = static_cast<uint8_t>(rand() % 256);
        }

        auto info = analyzer.analyze_page(page.data(), page.size());
        // Should not crash
        EXPECT_TRUE(!info.has_value() || info->total_columns <= 100);
    }
}

// ==================== NULL Bitmap Corruption Fuzz ====================

TEST(P1LayoutFuzzTest, NullBitmapCorruption) {
    PhysicalLayoutAnalyzer analyzer;

    for (int iter = 0; iter < 100; iter++) {
        auto page = make_simple_index_page(3);

        // Corrupt the null bitmap area (right after offset table + info_bits)
        // For 4 fields: offset table (8 bytes) + info_bits (1) = 9 bytes from record start
        // null bitmap = ceil(4/8) = 1 byte at offset 9 from record start
        size_t rec_start = 120;
        // Corrupt the null bitmap byte for each record
        for (int r = 0; r < 3; r++) {
            size_t null_bitmap_pos = rec_start + r * 20 + 9; // rough position
            if (null_bitmap_pos < page.size()) {
                page[null_bitmap_pos] = static_cast<uint8_t>(rand() % 256);
            }
        }

        auto info = analyzer.analyze_page(page.data(), page.size());
        // Should not crash
        EXPECT_TRUE(!info.has_value() || info->total_columns <= 100);
    }
}

// ==================== Mixed Record Types ====================

TEST(P1LayoutFuzzTest, MixedRecordTypes) {
    StatisticalTypeClassifier classifier;

    for (int iter = 0; iter < 100; iter++) {
        // Create ColumnFeatures with mixed characteristics
        ColumnFeatures f;
        f.sample_count = static_cast<uint32_t>(10 + rand() % 100);
        f.null_ratio = static_cast<double>(rand() % 100) / 100.0;
        f.zero_ratio = static_cast<double>(rand() % 100) / 100.0;
        f.empty_ratio = static_cast<double>(rand() % 100) / 100.0;
        f.len_min = static_cast<double>(rand() % 20);
        f.len_max = static_cast<double>(rand() % 100 + f.len_min);
        f.len_mean = (f.len_min + f.len_max) / 2.0;
        f.len_stddev = static_cast<double>(rand() % 10);
        f.ascii_ratio = static_cast<double>(rand() % 100) / 100.0;
        f.digit_ratio = static_cast<double>(rand() % 100) / 100.0;
        f.entropy = static_cast<double>(rand() % 80) / 10.0;
        f.is_fixed_length = (rand() % 2 == 0);
        f.fixed_size = static_cast<uint32_t>(rand() % 20);

        auto candidates = classifier.classify(f);

        // Should always produce non-empty results
        EXPECT_FALSE(candidates.empty());

        // Each candidate should have confidence in [0, 1]
        double total_confidence = 0.0;
        for (const auto& c : candidates) {
            EXPECT_GE(c.confidence, 0.0);
            EXPECT_LE(c.confidence, 1.0);
            total_confidence += c.confidence;
        }

        // Candidates should be sorted by confidence descending
        for (size_t i = 1; i < candidates.size(); i++) {
            EXPECT_GE(candidates[i-1].confidence, candidates[i].confidence);
        }
    }
}

// ==================== Empty Slot Table ====================

TEST(P1LayoutFuzzTest, EmptySlotTable) {
    PhysicalLayoutAnalyzer analyzer;

    for (int iter = 0; iter < 100; iter++) {
        auto page = make_simple_index_page(0); // No records

        auto info = analyzer.analyze_page(page.data(), page.size());
        // Empty page should return empty info, not crash
        EXPECT_FALSE(info.has_value());
    }

    // Also test with zeroed-out page directory
    {
        auto page = make_simple_index_page(3);
        // Zero out n_slots at end of page
        page[page.size() - 2] = 0x00;
        page[page.size() - 1] = 0x00;

        auto info = analyzer.analyze_page(page.data(), page.size());
        EXPECT_FALSE(info.has_value()); // Should be nullopt or gracefully handled
    }
}

// ==================== Maximum Record Count ====================

TEST(P1LayoutFuzzTest, MaximumRecordCount) {
    PhysicalLayoutAnalyzer analyzer;

    // Create a page with many small records (stress test)
    {
        std::vector<std::vector<uint8_t>> fields;
        fields.push_back({0x01}); // 1-byte field
        fields.push_back({0x02}); // 1-byte field

        auto page = make_index_page_with_records(200, fields);

        auto info = analyzer.analyze_page(page.data(), page.size());
        // Should not crash or OOM
        EXPECT_TRUE(!info.has_value() || info->total_columns == 2);
    }

    // Create page with 500 very small records (boundary)
    {
        std::vector<std::vector<uint8_t>> fields;
        fields.push_back({0x01}); // 1-byte field

        auto page = make_index_page_with_records(500, fields);

        auto info = analyzer.analyze_page(page.data(), page.size());
        // Should not crash
        EXPECT_TRUE(!info.has_value() || info->total_columns <= 10);
    }
}

// ==================== Extreme Page Sizes ====================

TEST(P1LayoutFuzzTest, ExtremePageSizes) {
    PhysicalLayoutAnalyzer analyzer;

    // Very small pages
    for (size_t sz : {32, 64, 100, 512}) {
        std::vector<uint8_t> tiny(sz, static_cast<uint8_t>(rand() % 256));
        auto info = analyzer.analyze_page(tiny.data(), tiny.size());
        EXPECT_FALSE(info.has_value()); // Too small to be valid
    }

    // Very large page size (64KB)
    {
        std::vector<uint8_t> large(65536, 0);
        // Set valid INDEX page type header
        large[24] = 0x45; large[25] = 0xBF;
        large[54] = 0x01; large[55] = 0x00; // n_recs = 1

        auto info = analyzer.analyze_page(large.data(), large.size());
        // Should not crash; may return empty or partial info
        EXPECT_TRUE(!info.has_value() || info->total_columns <= 100);
    }
}

// ==================== Null/Empty Data ====================

TEST(P1LayoutFuzzTest, NullEmptyData) {
    PhysicalLayoutAnalyzer analyzer;
    StatisticalTypeClassifier classifier;
    FeatureExtractor extractor;

    // Null data
    auto info = analyzer.analyze_page(nullptr, 16384);
    EXPECT_FALSE(info.has_value());

    // Empty data
    info = analyzer.analyze_page(nullptr, 0);
    EXPECT_FALSE(info.has_value());

    // Empty feature extraction
    std::vector<std::vector<uint8_t>> empty_samples;
    auto features = extractor.extract(empty_samples);
    EXPECT_EQ(features.sample_count, 0);

    // Classify empty features — the classifier may return default candidates
    // (e.g., VARCHAR) for empty/unknown data, since there's no evidence to
    // rule out any type. Verify results are within valid range.
    auto candidates = classifier.classify(features);
    for (const auto& c : candidates) {
        EXPECT_GE(c.confidence, 0.0);
        EXPECT_LE(c.confidence, 1.0);
    }
}

// ==================== Analyze multiple pages with corruption ====================

TEST(P1LayoutFuzzTest, MultiPageCorruption) {
    PhysicalLayoutAnalyzer analyzer;

    for (int iter = 0; iter < 50; iter++) {
        std::vector<std::pair<const uint8_t*, size_t>> pages;

        for (int p = 0; p < 5; p++) {
            auto page = make_simple_index_page(3);

            // Corrupt random parts of some pages
            if (rand() % 2 == 0) {
                size_t corr_off = static_cast<size_t>(rand() % page.size());
                page[corr_off % page.size()] = static_cast<uint8_t>(rand() % 256);
            }

            pages.emplace_back(page.data(), page.size());
        }

        auto info = analyzer.analyze_multiple_pages(pages);
        // Should not crash
        EXPECT_TRUE(info.total_columns == 0 || info.total_columns <= 100);
    }
}

} // namespace test
} // namespace dbrecovery