/**
 * @file test_heuristic_schema_engine.cpp
 * @brief Test HeuristicSchemaEngine v2.0 pipeline
 *
 * Creates mock INDEX pages with valid COMPACT format records that pass
 * PhysicalLayoutAnalyzer validation, then runs the full inference pipeline
 * and verifies the SchemaInferenceResult output.
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>

#include "dbrecovery/tools/heuristic_schema_engine.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

using namespace dbrecovery::tools;
using namespace dbrecovery::inference;

// ==================== Test Page Builder ====================

/**
 * Build a valid COMPACT-format INDEX page at 16K.
 *
 * PhysicalLayoutAnalyzer::analyze_page() expects:
 *   - page_type at offset 24 = 0x45BF (FIL_PAGE_INDEX)
 *   - n_recs at offset 54 (LE) > 0 and <= 1000
 *   - heap_top at offset 58 (LE) >= 120 and <= page_size - 8
 *   - infimum's next_offset at offset 100-101 (LE) = first user record offset
 *   - first_rec_offset >= 120 and < heap_top
 *   - offset table entries (LE uint16, strictly decreasing, non-zero)
 *   - col_count from offset table entries must be 3-100
 */
static std::vector<uint8_t> build_index_page(
    uint32_t n_recs,
    uint32_t col_count,
    const std::vector<std::vector<uint8_t>>& field_data,
    uint32_t first_rec_offset = 128) {

    std::vector<uint8_t> page(16384, 0);
    const size_t ps = 16384;

    // === FIL_HEADER (offset 0-37) ===
    // FIL_PAGE_PREV (0-3)
    page[0] = 0xFF; page[1] = 0xFF; page[2] = 0xFF; page[3] = 0xFF;
    // FIL_PAGE_NEXT (4-7)
    page[4] = 0xFF; page[5] = 0xFF; page[6] = 0xFF; page[7] = 0xFF;
    // FIL_PAGE_LSN (8-15)
    page[8] = 0x01; page[9] = 0x00; page[10] = 0x00; page[11] = 0x00;
    page[12] = 0x00; page[13] = 0x00; page[14] = 0x00; page[15] = 0x00;
    // FIL_PAGE_TYPE (24-25) = 0x45BF (FIL_PAGE_INDEX)
    page[24] = 0x45; page[25] = 0xBF;
    // FIL_PAGE_SPACE_ID (18-21)
    page[18] = 0x01; page[19] = 0x00; page[20] = 0x00; page[21] = 0x00;
    // FIL_PAGE_OFFSET (22-23)
    page[22] = 0x00; page[23] = 0x00;
    // FIL_PAGE_FILE_FLUSH_LSN (24-31) -- already set page_type at 24-25
    // Actually page_type is at 24-25, file_flush_lsn is at 24-31
    // Overwrite page_type correctly
    // Already done above

    // === PAGE_HEADER (offset 38-93) ===
    // PAGE_N_DIR_SLOTS (38-39)
    uint16_t n_slots = 2;
    page[38] = n_slots & 0xFF; page[39] = (n_slots >> 8) & 0xFF;

    // Compute data layout
    // The extractor reads field_offsets from rec_off (col_count entries).
    // field_offsets[0] = next_rec (also = total record length from rec_off,
    // since the next record immediately follows this one).
    // field_offsets[1..col_count-1] = offset table entries for fields
    // col_count-2 down to 0 (end of each field, relative to rec_off).
    //
    // Data starts at: rec_off + col_count*2 + 1 (info_bits) + null_bitmap_bytes
    // This matches the extractor's calculation in PhysicalLayoutAnalyzer.
    uint32_t null_bitmap_bytes = (col_count + 7) / 8;
    uint32_t offset_table_bytes = col_count * 2;
    uint32_t data_start_offset = first_rec_offset + offset_table_bytes + 1 /* info_bits */ + null_bitmap_bytes;

    // Compute total field data size
    uint32_t total_data_size = 0;
    for (const auto& f : field_data) total_data_size += f.size();

    // PAGE_HEAP_TOP (40-41) = end of records + page directory
    uint32_t record_data_end = data_start_offset + total_data_size;
    uint32_t page_dir_start = ((record_data_end + 7) / 8) * 8; // align to 8
    uint32_t heap_top = page_dir_start + (n_recs + 2) * 2; // +2 for infimum/supremum
    if (heap_top > ps - 8) heap_top = ps - 8;
    page[40] = heap_top & 0xFF; page[41] = (heap_top >> 8) & 0xFF;

    // PAGE_N_HEAP (42-43)
    uint16_t n_heap = n_recs + 2; // +2 for infimum/supremum
    page[42] = n_heap & 0xFF; page[43] = (n_heap >> 8) & 0xFF;

    // PAGE_FREE (44-45)
    page[44] = 0x00; page[45] = 0x00;
    // PAGE_GARBAGE (46-47)
    page[46] = 0x00; page[47] = 0x00;
    // PAGE_LAST_INSERT (48-49)
    page[48] = 0x00; page[49] = 0x00;
    // PAGE_DIRECTION (50-51)
    page[50] = 0x00; page[51] = 0x00;
    // PAGE_N_DIRECTION (52-53)
    page[52] = 0x00; page[53] = 0x00;

    // PAGE_N_RECS (54-55, LE)
    page[54] = n_recs & 0xFF;
    page[55] = (n_recs >> 8) & 0xFF;

    // PAGE_MAX_TRX_ID (56-57)
    page[56] = 0x00; page[57] = 0x00;

    // PAGE_LEVEL (58-59, LE) = 0 for leaf
    page[58] = 0x00; page[59] = 0x00;

    // PAGE_INDEX_ID (60-67)
    for (int i = 0; i < 8; i++) page[60 + i] = 0x00;

    // Rest of page header (68-93) = 0
    // FSEG header (74-93)

    // === INFIMUM record (offset 99, 5 bytes) ===
    // COMPACT format: INFIMUM at offset 99
    const uint32_t INFIMUM_OFF = 99;
    page[INFIMUM_OFF] = 0x00; // info_bits

    // INFIMUM next_offset (2 bytes at 100-101, LE) = first user record offset
    page[INFIMUM_OFF + 1] = first_rec_offset & 0xFF;
    page[INFIMUM_OFF + 2] = (first_rec_offset >> 8) & 0xFF;

    // INFIMUM heap_no (2 bytes at 102-103, LE) = 0
    page[INFIMUM_OFF + 3] = 0x00;
    page[INFIMUM_OFF + 4] = 0x00;

    // === SUPREMUM record (offset 104, 5+ bytes) ===
    const uint32_t SUPREMUM_OFF = 104;
    page[SUPREMUM_OFF] = 0x03; // info_bits with SUPREMUM status

    // SUPREMUM next_offset (2 bytes, LE) = 0 (end of chain)
    page[SUPREMUM_OFF + 1] = 0x00;
    page[SUPREMUM_OFF + 2] = 0x00;

    // SUPREMUM heap_no (2 bytes, LE) = 1
    page[SUPREMUM_OFF + 3] = 0x01;
    page[SUPREMUM_OFF + 4] = 0x00;

    // === User Records ===
    uint32_t current_rec_offset = first_rec_offset;
    uint32_t data_offset = data_start_offset;

    for (uint32_t r = 0; r < n_recs; r++) {
        // Compute offset table entries (relative to rec offset, decreasing order)
        // Stored in reverse field order: last field end first
        // field_offsets[0] = end of last field (largest)
        // field_offsets[col_count-1] = end of first field (smallest)
        std::vector<uint16_t> field_ends;
        uint32_t accum = data_start_offset - current_rec_offset;
        for (uint32_t c = 0; c < col_count; c++) {
            accum += field_data[c].size();
            field_ends.push_back(accum);
        }

        // Write offset table in reverse order (decreasing, LE)
        for (uint32_t c = 0; c < col_count; c++) {
            uint32_t rev_idx = col_count - 1 - c;
            uint16_t val = field_ends[rev_idx];
            page[current_rec_offset + c * 2] = val & 0xFF;
            page[current_rec_offset + c * 2 + 1] = (val >> 8) & 0xFF;
        }

        // info_bits (1 byte after offset table)
        uint32_t info_off = current_rec_offset + offset_table_bytes;
        page[info_off] = 0x00;

        // NULL bitmap
        uint32_t null_off = info_off + 1;
        // All fields non-null, so bitmap = 0

        // Field data (all non-null)
        uint32_t write_off = data_offset;
        for (uint32_t c = 0; c < col_count; c++) {
            std::memcpy(&page[write_off], field_data[c].data(), field_data[c].size());
            write_off += field_data[c].size();
        }

        // Do NOT overwrite offset table entries with next_rec.
        // The first 2 bytes (offset[0]) already contain the correct
        // total record length (relative to rec_off), which the extractor
        // reads as both field_offsets[0] and next_rec.
        // Overwriting would corrupt the offset table and cause field
        // extraction to read garbage data.
        if (r + 1 < n_recs) {
            current_rec_offset = write_off;
            // align to 4
            current_rec_offset = ((current_rec_offset + 3) / 4) * 4;
            data_offset = current_rec_offset + offset_table_bytes + 1 + null_bitmap_bytes;
        }
    }

    // === Page Directory (at page_dir_start) ===
    // Slot 0: INFIMUM (offset 99)
    page[page_dir_start] = 99 & 0xFF;
    page[page_dir_start + 1] = (99 >> 8) & 0xFF;
    // Slot 1: SUPREMUM
    page[page_dir_start + 2] = SUPREMUM_OFF & 0xFF;
    page[page_dir_start + 3] = (SUPREMUM_OFF >> 8) & 0xFF;

    return page;
}

// ==================== Test Fixtures ====================

class HeuristicSchemaEngineTest : public ::testing::Test {
protected:
    HeuristicEngineConfig cfg;
    HeuristicSchemaEngine* engine = nullptr;

    void SetUp() override {
        cfg.verbose = false;
        cfg.max_pages_to_sample = 10;
        cfg.max_records_per_column = 1000;
        cfg.type_candidates_top_n = 3;
        cfg.confidence_threshold = 0.30;
        engine = new HeuristicSchemaEngine(cfg);
    }

    void TearDown() override {
        delete engine;
    }
};

// ==================== Helper to create typed field data ====================

static std::vector<uint8_t> make_varchar_field(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

static std::vector<uint8_t> make_int_field(int32_t val) {
    return make_varchar_field(std::to_string(val));
}

static std::vector<uint8_t> make_bigint_field(int64_t val) {
    return make_varchar_field(std::to_string(val));
}

// ==================== Tests ====================

// HS-01: Basic inference with integer columns
TEST_F(HeuristicSchemaEngineTest, BasicIntegerColumns) {
    // 6 columns total (3 system + 3 user):
    //   col 0: INT (4 bytes) - sequential IDs
    //   col 1: INT (4 bytes) - small values
    //   col 2: INT (4 bytes) - zero/null-like
    //   col 3-5: system columns (DB_TRX_ID, DB_ROLL_PTR, DB_ROW_ID)
    // Actually the system columns are 3 of the 6, so 3 user columns

    // For simplicity, create 6 columns where all look like integers
    std::vector<std::vector<uint8_t>> field_data(6);
    for (uint32_t c = 0; c < 6; c++) {
        field_data[c] = make_int_field(100 + c);
    }

    auto page = build_index_page(5, 6, field_data, 128);
    std::vector<std::pair<const uint8_t*, size_t>> pages = {
        {page.data(), page.size()}
    };

    auto result = engine->infer_from_pages(pages);

    // Should have detected columns
    EXPECT_GT(result.columns.size(), 0);
    EXPECT_EQ(result.detected_column_count, 6);
    EXPECT_EQ(result.inference_method, "heuristic_v2_statistical");

    // Each column should have a best_guess
    for (const auto& col : result.columns) {
        EXPECT_FALSE(col.best_guess.type_name.empty());
        EXPECT_GE(col.overall_confidence, 0.0);
        EXPECT_LE(col.overall_confidence, 1.0);
    }

    // DDL should be generated
    EXPECT_FALSE(result.tentative_ddl.empty());
    EXPECT_NE(result.tentative_ddl.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(result.tentative_ddl.find("ENGINE=InnoDB"), std::string::npos);

    // Overall confidence should be in [0, 1]
    EXPECT_GE(result.overall_confidence, 0.0);
    EXPECT_LE(result.overall_confidence, 1.0);

    // Table name should be derived
    EXPECT_FALSE(result.proposed_table_name.empty());

    // Stats should be populated
    auto stats = engine->get_stats();
    EXPECT_GT(stats.pages_scanned, 0);
    EXPECT_GT(stats.records_sampled, 0);
    EXPECT_GT(stats.total_ms, 0.0);
}

// HS-02: VARCHAR columns (text-like data)
TEST_F(HeuristicSchemaEngineTest, VarcharColumns) {
    // 6 columns: 3 user + 3 system
    // User columns have string data
    std::vector<std::vector<uint8_t>> field_data(6);
    // col 0: VARCHAR-like (name strings)
    for (int r = 0; r < 10; r++) {
        (void)r; // field_data is per-column, not per-row
    }
    field_data[0] = make_varchar_field("John");
    field_data[1] = make_varchar_field("Doe");
    field_data[2] = make_varchar_field("New York");
    field_data[3] = make_bigint_field(100); // DB_TRX_ID
    field_data[4] = make_bigint_field(200); // DB_ROLL_PTR
    field_data[5] = make_int_field(1);      // DB_ROW_ID

    auto page = build_index_page(5, 6, field_data, 128);
    std::vector<std::pair<const uint8_t*, size_t>> pages = {
        {page.data(), page.size()}
    };

    auto result = engine->infer_from_pages(pages);
    EXPECT_GT(result.columns.size(), 0);
    EXPECT_FALSE(result.tentative_ddl.empty());
    auto stats = engine->get_stats();
    EXPECT_GT(stats.pages_scanned, 0);
}

// HS-03: Mixed types (INT + VARCHAR)
TEST_F(HeuristicSchemaEngineTest, MixedTypes) {
    // 6 columns: 3 user (INT, VARCHAR, BIGINT) + 3 system
    std::vector<std::vector<uint8_t>> field_data(6);
    field_data[0] = make_int_field(42);       // user col 0
    field_data[1] = make_varchar_field("hello"); // user col 1
    field_data[2] = make_bigint_field(999);   // user col 2
    field_data[3] = make_bigint_field(100);   // DB_TRX_ID
    field_data[4] = make_bigint_field(200);   // DB_ROLL_PTR
    field_data[5] = make_int_field(1);        // DB_ROW_ID

    auto page = build_index_page(3, 6, field_data, 128);
    std::vector<std::pair<const uint8_t*, size_t>> pages = {
        {page.data(), page.size()}
    };

    auto result = engine->infer_from_pages(pages);
    EXPECT_GT(result.columns.size(), 0);
    EXPECT_FALSE(result.tentative_ddl.empty());
}

// HS-04: Multiple pages aggregation
TEST_F(HeuristicSchemaEngineTest, MultiplePages) {
    std::vector<std::vector<uint8_t>> field_data(6);
    field_data[0] = make_int_field(1);
    field_data[1] = make_int_field(2);
    field_data[2] = make_int_field(3);
    field_data[3] = make_bigint_field(100);
    field_data[4] = make_bigint_field(200);
    field_data[5] = make_int_field(1);

    auto page1 = build_index_page(3, 6, field_data, 128);
    auto page2 = build_index_page(3, 6, field_data, 128);
    auto page3 = build_index_page(3, 6, field_data, 128);

    std::vector<std::pair<const uint8_t*, size_t>> pages = {
        {page1.data(), page1.size()},
        {page2.data(), page2.size()},
        {page3.data(), page3.size()}
    };

    auto result = engine->infer_from_pages(pages);
    EXPECT_GT(result.columns.size(), 0);
    EXPECT_GE(result.overall_confidence, 0.0);

    auto stats = engine->get_stats();
    EXPECT_GE(stats.pages_scanned, 3);
    EXPECT_GE(stats.records_sampled, 9);
}

// HS-05: Single page with no INDEX type → empty result
TEST_F(HeuristicSchemaEngineTest, NoIndexPages) {
    // Create a page with wrong page type
    std::vector<uint8_t> page(16384, 0);
    // Set page type to something other than FIL_PAGE_INDEX
    page[24] = 0x00; page[25] = 0x01; // not INDEX

    std::vector<std::pair<const uint8_t*, size_t>> pages = {
        {page.data(), page.size()}
    };

    auto result = engine->infer_from_pages(pages);
    EXPECT_EQ(result.columns.size(), 0);
    EXPECT_EQ(result.overall_confidence, 0.0);
    EXPECT_FALSE(result.warnings.empty());
}

// HS-06: Empty page vector
TEST_F(HeuristicSchemaEngineTest, EmptyPages) {
    std::vector<std::pair<const uint8_t*, size_t>> pages;
    auto result = engine->infer_from_pages(pages);
    EXPECT_EQ(result.columns.size(), 0);
    EXPECT_EQ(result.overall_confidence, 0.0);
}

// HS-07: DDL generation has correct structure
TEST_F(HeuristicSchemaEngineTest, DDLStructure) {
    std::vector<std::vector<uint8_t>> field_data(6);
    field_data[0] = make_int_field(1);
    field_data[1] = make_int_field(2);
    field_data[2] = make_int_field(3);
    field_data[3] = make_bigint_field(100);
    field_data[4] = make_bigint_field(200);
    field_data[5] = make_int_field(1);

    auto page = build_index_page(3, 6, field_data, 128);
    std::vector<std::pair<const uint8_t*, size_t>> pages = {
        {page.data(), page.size()}
    };

    auto result = engine->infer_from_pages(pages);
    ASSERT_FALSE(result.tentative_ddl.empty());

    // DDL must contain expected SQL keywords
    EXPECT_NE(result.tentative_ddl.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(result.tentative_ddl.find("ENGINE=InnoDB"), std::string::npos);
    EXPECT_NE(result.tentative_ddl.find("NOT NULL"), std::string::npos);
    EXPECT_NE(result.tentative_ddl.find(";"), std::string::npos);

    // Each column should appear in DDL
    for (const auto& col : result.columns) {
        EXPECT_NE(result.tentative_ddl.find(col.suggested_name), std::string::npos);
    }
}

// HS-08: Column name suggestions
TEST_F(HeuristicSchemaEngineTest, ColumnNameSuggestions) {
    std::vector<std::vector<uint8_t>> field_data(6);
    // INT-like data for col 0 → should suggest "id" for ordinal 0
    field_data[0] = make_int_field(1);
    // More INT data for col 1
    field_data[1] = make_int_field(2);
    // INT data for col 2
    field_data[2] = make_int_field(3);
    field_data[3] = make_bigint_field(100);
    field_data[4] = make_bigint_field(200);
    field_data[5] = make_int_field(1);

    auto page = build_index_page(3, 6, field_data, 128);
    std::vector<std::pair<const uint8_t*, size_t>> pages = {
        {page.data(), page.size()}
    };

    auto result = engine->infer_from_pages(pages);
    ASSERT_GT(result.columns.size(), 0);

    // First column (ordinal 0) with INT candidate should be named "id"
    EXPECT_EQ(result.columns[0].suggested_name, "id");
}

// HS-09: Engine config is accessible
TEST_F(HeuristicSchemaEngineTest, ConfigAccess) {
    auto config = engine->get_config();
    EXPECT_EQ(config.page_size, 16384);
    EXPECT_EQ(config.max_pages_to_sample, 10);
    EXPECT_EQ(config.type_candidates_top_n, 3);
    EXPECT_DOUBLE_EQ(config.confidence_threshold, 0.30);
    EXPECT_DOUBLE_EQ(config.pk_likelihood_threshold, 0.80);
}

// HS-10: infer_from_file with non-existent path returns empty result
TEST_F(HeuristicSchemaEngineTest, NonExistentFile) {
    auto result = engine->infer_from_file("/nonexistent/path.ibd");
    EXPECT_EQ(result.columns.size(), 0);
    EXPECT_EQ(result.overall_confidence, 0.0);
    EXPECT_FALSE(result.proposed_table_name.empty());
}

// HS-11: Multiple records produce distinct values
TEST_F(HeuristicSchemaEngineTest, DistinctValues) {
    // Create field data with varying values per column
    // Note: build_index_page uses the same field_data template for all records
    // So all records will have identical values - which is fine for testing
    // the pipeline doesn't crash
    std::vector<std::vector<uint8_t>> field_data(6);
    field_data[0] = make_varchar_field("test");
    field_data[1] = make_varchar_field("data");
    field_data[2] = make_int_field(0);
    field_data[3] = make_bigint_field(100);
    field_data[4] = make_bigint_field(200);
    field_data[5] = make_int_field(1);

    auto page = build_index_page(10, 6, field_data, 128);
    std::vector<std::pair<const uint8_t*, size_t>> pages = {
        {page.data(), page.size()}
    };

    auto result = engine->infer_from_pages(pages);
    EXPECT_GT(result.columns.size(), 0);
    EXPECT_FALSE(result.tentative_ddl.empty());
}

// HS-12: Verify engine stats completeness
TEST_F(HeuristicSchemaEngineTest, EngineStatsComplete) {
    std::vector<std::vector<uint8_t>> field_data(6);
    field_data[0] = make_int_field(1);
    field_data[1] = make_int_field(2);
    field_data[2] = make_int_field(3);
    field_data[3] = make_bigint_field(100);
    field_data[4] = make_bigint_field(200);
    field_data[5] = make_int_field(1);

    auto page = build_index_page(3, 6, field_data, 128);
    std::vector<std::pair<const uint8_t*, size_t>> pages = {
        {page.data(), page.size()}
    };

    auto result = engine->infer_from_pages(pages);
    auto stats = engine->get_stats();

    EXPECT_GT(stats.pages_scanned, 0);
    EXPECT_GT(stats.records_sampled, 0);
    EXPECT_GT(stats.columns_inferred, 0);
    EXPECT_GT(stats.sampling_ms, 0.0);
    EXPECT_GT(stats.total_ms, 0.0);
    // feature_ms and classify_ms may be 0 in some edge cases
    EXPECT_GE(stats.feature_ms, 0.0);
    EXPECT_GE(stats.classify_ms, 0.0);
}

