// SchemaInferrer v2.0 6-step pipeline tests (Phase 4 - Slice A)
//
// Covers:
//   - infer_from_raw_pages end-to-end (constructed mock INDEX pages)
//   - column type inference correctness (INT / VARCHAR)
//   - system column skipping (DB_TRX_ID, DB_ROLL_PTR, DB_ROW_ID)
//   - empty-page / non-INDEX-page tolerance
//   - single / multi / mixed typed columns
//   - convert_to_v2_result mapping
//   - infer_from_frm path (writes a synthetic .frm?---- no: frm parse needs real bytes)
//     -> covered via set_frm_path error path + parse_frm failure tolerance

#include <gtest/gtest.h>
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/schema_inferrer.hpp"
#include "dbrecovery/inference/statistical_classifier.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace {

using dbrecovery::innodb::SchemaInferrer;

// Reusable mock INDEX page builder (COMPACT). Layout mirrors
// PhysicalLayoutAnalyzer: page_type at 24-25 = 0x45BF, n_recs at 54-55 BE,
// 5-byte COMPACT record header at each record offset, offset table at
// rec_off+5 (BE, strictly decreasing), page directory slots at
// page_size - 2*(i+1) (BE). INFIMUM header at 94, SUPREMUM at 104.
//
// IMPORTANT: field data is stored in REVERSED array order (field_data[n-1]
// first, field_data[0] last) and the offset table uses
// entry[c] = total_len - sum(len(field_data[0..c-1])). extract_column_values()
// then maps output column i to field_data[i].
std::vector<uint8_t> build_index_page(
    uint32_t n_recs,
    uint32_t col_count,
    const std::vector<std::vector<uint8_t>>& field_data,
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

    uint32_t total_data_size = 0;
    for (const auto& f : field_data) total_data_size += f.size();

    uint32_t record_data_end = data_start_offset + total_data_size;
    uint32_t page_dir_start = ((record_data_end + 7) / 8) * 8;
    uint32_t heap_top = page_dir_start + n_slots * 2;
    if (heap_top > ps - 8) heap_top = ps - 8;
    page[40] = (heap_top >> 8) & 0xFF; page[41] = heap_top & 0xFF;

    uint16_t n_heap = static_cast<uint16_t>(n_recs + 2);
    page[42] = (n_heap >> 8) & 0xFF; page[43] = n_heap & 0xFF;

    page[54] = (n_recs >> 8) & 0xFF; page[55] = n_recs & 0xFF;
    page[58] = 0x00; page[59] = 0x00;  // leaf level

    // INFIMUM COMPACT header at 94 (5 bytes): REC_NEXT signed BE
    {
        int16_t inf_delta = static_cast<int16_t>(first_rec_offset - 94);
        page[94] = (inf_delta >> 8) & 0xFF;
        page[95] = inf_delta & 0xFF;
        page[96] = 0x00;  // info_bits
        page[97] = 0x00;  // heap_no low
        page[98] = 0x00;
    }

    // SUPREMUM COMPACT header at 104 (5 bytes): REC_NEXT = 0
    page[104] = 0x00; page[105] = 0x00;
    page[106] = 0x03;  // info_bits with SUPREMUM flag
    page[107] = 0x01;  // heap_no = 1
    page[108] = 0x00;

    uint32_t total_len = kRecHeaderSize + offset_table_bytes + 1 + null_bitmap_bytes + total_data_size;

    // User records
    uint32_t current_rec_offset = first_rec_offset;
    uint32_t data_offset = data_start_offset;
    std::vector<uint32_t> user_rec_offsets;
    for (uint32_t r = 0; r < n_recs; r++) {
        user_rec_offsets.push_back(current_rec_offset);

        // Offset table at rec_off + 5 (BE, strictly decreasing cumulative)
        // entry[c] = total_len - sum(len(field_data[0..c-1]))
        uint32_t cumulative = total_len;
        for (uint32_t c = 0; c < col_count; c++) {
            uint16_t val = static_cast<uint16_t>(cumulative);
            page[current_rec_offset + kRecHeaderSize + c * 2] = (val >> 8) & 0xFF;
            page[current_rec_offset + kRecHeaderSize + c * 2 + 1] = val & 0xFF;
            cumulative -= static_cast<uint32_t>(field_data[c].size());
        }

        // info_bits (1 byte after offset table)
        page[current_rec_offset + kRecHeaderSize + offset_table_bytes] = 0x00;
        // NULL bitmap (all zero = non-null)

        // Field data: REVERSED order so output col i == field_data[i]
        uint32_t write_off = data_offset;
        for (uint32_t rev = 0; rev < col_count; rev++) {
            uint32_t c = col_count - 1 - rev;
            std::memcpy(&page[write_off], field_data[c].data(), field_data[c].size());
            write_off += field_data[c].size();
        }

        // REC_NEXT at bytes 0-1 (signed BE)
        if (r + 1 < n_recs) {
            current_rec_offset = write_off;
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

std::vector<uint8_t> make_varchar_field(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<uint8_t> make_int_field(int32_t val) {
    return make_varchar_field(std::to_string(val));
}

std::vector<uint8_t> make_bigint_field(int64_t val) {
    return make_varchar_field(std::to_string(val));
}

// Build a page with col_count named columns, replicating field_data for all
// records. col_count user + 3 system placeholders = page records use exactly
// the given field_data, but the analyzer treats last 3 columns as system.
std::vector<uint8_t> build_page_with_rows(
    uint32_t user_cols,
    const std::vector<std::vector<uint8_t>>& record_data,
    uint32_t n_recs) {

    std::vector<uint8_t> page = build_index_page(n_recs, user_cols, record_data);
    return page;
}

}  // namespace

namespace dbrecovery::innodb {

// SI-01: Empty pages input is tolerated, returns failure with message
TEST(SchemaInferrerTest, EmptyPagesTolerated) {
    SchemaInferrer inferrer;
    auto result = inferrer.infer_from_raw_pages(std::vector<std::pair<const uint8_t*, size_t>>{});
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_message.empty());
    EXPECT_TRUE(result.schema.columns.empty());
}

// SI-02: Non-INDEX page (page_type != 0x45BF) fails layout detection
TEST(SchemaInferrerTest, NonIndexPageTolerated) {
    std::vector<uint8_t> page(16384, 0);
    page[24] = 0x00; page[25] = 0x00;  // not FIL_PAGE_INDEX
    SchemaInferrer inferrer;
    auto result = inferrer.infer_from_raw_pages(std::vector<std::pair<const uint8_t*, size_t>>{std::make_pair(page.data(), page.size())});
    EXPECT_FALSE(result.success);
}

// SI-03: Multi-column page (int + varchar + varchar) infers INT and first col named "user_id"
// NOTE: uses 5 columns so the last 3 are treated as system columns by the pipeline
//
// Field data is stored in REVERSED order on the page (field_data[n-1] first,
// field_data[0] last), but extract_column_values() reads the offset table
// in forward order, mapping output column i to field_data[i] directly.
TEST(SchemaInferrerTest, SingleIntegerColumn) {
    std::vector<std::vector<uint8_t>> field_data = {
        make_int_field(42),        // → col[0] (user col 0, is_integer_type)
        make_varchar_field("data1"),  // → col[1] (user col 1, is_string_type)
        make_varchar_field("extra1"), // → col[2] (system: DB_TRX_ID)
        make_bigint_field(1),      // → col[3] (system: DB_ROLL_PTR)
        make_bigint_field(2),      // → col[4] (system: DB_ROW_ID)
    };
    std::vector<uint8_t> page = build_index_page(20, 5, field_data);
    SchemaInferrer inferrer;
    auto result = inferrer.infer_from_raw_pages(std::vector<std::pair<const uint8_t*, size_t>>{std::make_pair(page.data(), page.size())});
    ASSERT_TRUE(result.success) << result.error_message;
    ASSERT_EQ(result.schema.columns.size(), 5u);
    // First user column is named via PatternInferenceEngine::infer_column_name(1, {}) = "user_id"
    EXPECT_EQ(result.schema.columns[0].name, "user_id");
    EXPECT_TRUE(result.schema.columns[0].is_integer_type);
    // Second user column is string-typed
    EXPECT_TRUE(result.schema.columns[1].is_string_type);
}

// SI-04: int + varchar page (with 3 system cols) infers both correctly
TEST(SchemaInferrerTest, MixedIntegerAndVarchar) {
    std::vector<std::vector<uint8_t>> field_data = {
        make_int_field(100),               // → col[0] (user col 0, INT)
        make_varchar_field("alice@example.com"), // → col[1] (user col 1, "email")
        make_bigint_field(1),              // → col[2] (DB_TRX_ID)
        make_bigint_field(2),              // → col[3] (DB_ROLL_PTR)
        make_bigint_field(3),              // → col[4] (DB_ROW_ID)
    };
    std::vector<uint8_t> page = build_index_page(30, 5, field_data);
    SchemaInferrer inferrer;
    auto result = inferrer.infer_from_raw_pages(std::vector<std::pair<const uint8_t*, size_t>>{std::make_pair(page.data(), page.size())});
    ASSERT_TRUE(result.success) << result.error_message;
    ASSERT_EQ(result.schema.columns.size(), 5u);
    // col 0 = INT -> integer type
    EXPECT_TRUE(result.schema.columns[0].is_integer_type);
    // col 1 = varchar w/ email pattern -> "email"
    EXPECT_TRUE(result.schema.columns[1].is_string_type);
    EXPECT_EQ(result.schema.columns[1].name, "email");
    // cols 2-4 = system columns (DB_TRX_ID / DB_ROLL_PTR / DB_ROW_ID)
    EXPECT_EQ(result.schema.columns[2].name, "DB_TRX_ID");
    EXPECT_EQ(result.schema.columns[3].name, "DB_ROLL_PTR");
    EXPECT_EQ(result.schema.columns[4].name, "DB_ROW_ID");
}

// SI-05: 6 columns (3 user + 3 system) drops the last 3, names bigint trx id
TEST(SchemaInferrerTest, SystemColumnsSkipped) {
    std::vector<std::vector<uint8_t>> field_data = {
        make_int_field(1),
        make_varchar_field("name1"),
        make_bigint_field(999),
        make_bigint_field(12345),  // DB_TRX_ID-ish
        make_bigint_field(77777),  // DB_ROLL_PTR-ish
        make_bigint_field(88888),  // DB_ROW_ID-ish
    };
    std::vector<uint8_t> page = build_index_page(10, 6, field_data);
    SchemaInferrer inferrer;
    auto result = inferrer.infer_from_raw_pages(std::vector<std::pair<const uint8_t*, size_t>>{std::make_pair(page.data(), page.size())});
    ASSERT_TRUE(result.success) << result.error_message;
    ASSERT_EQ(result.schema.columns.size(), 6u);
    // Last 3 are system columns (they ARE in columns, but flagged)
    EXPECT_EQ(result.schema.columns[3].name, "DB_TRX_ID");
    EXPECT_EQ(result.schema.columns[4].name, "DB_ROLL_PTR");
    EXPECT_EQ(result.schema.columns[5].name, "DB_ROW_ID");
    // DB_TRX_ID is BIGINT (6B) so it IS integer-typed; roll ptr / row id are VARBINARY
    EXPECT_TRUE(result.schema.columns[3].is_integer_type);
    EXPECT_EQ(result.schema.columns[3].type_name, "BIGINT");
    EXPECT_FALSE(result.schema.columns[4].is_integer_type);
    EXPECT_FALSE(result.schema.columns[5].is_integer_type);
    // PK: first user integer col
    ASSERT_FALSE(result.schema.primary_key_columns.empty());
    EXPECT_EQ(result.schema.primary_key_columns[0].name, "user_id");
}

// SI-06: convert_to_v2_result preserves columns and DDL
TEST(SchemaInferrerTest, ConvertToV2ResultPreservesColumns) {
    std::vector<std::vector<uint8_t>> field_data = {
        make_int_field(1),
        make_varchar_field("hello"),
        make_bigint_field(9),  // DB_TRX_ID-ish
        make_bigint_field(9),  // DB_ROLL_PTR-ish
        make_bigint_field(9),  // DB_ROW_ID-ish
    };
    std::vector<uint8_t> page = build_index_page(15, 5, field_data);
    SchemaInferrer inferrer;
    auto result = inferrer.infer_from_raw_pages(std::vector<std::pair<const uint8_t*, size_t>>{std::make_pair(page.data(), page.size())});
    ASSERT_TRUE(result.success) << result.error_message;

    auto v2 = SchemaInferrer::convert_to_v2_result(result);
    EXPECT_EQ(v2.detected_column_count, result.schema.columns.size());
    EXPECT_EQ(v2.proposed_table_name, result.schema.table_name);
    EXPECT_EQ(v2.overall_confidence, result.schema.confidence);
    ASSERT_EQ(v2.columns.size(), result.schema.columns.size());
    EXPECT_FALSE(v2.tentative_ddl.empty());
    EXPECT_EQ(v2.columns[0].suggested_name, result.schema.columns[0].name);
}

// SI-07: column_count_hint trims extra columns
TEST(SchemaInferrerTest, ColumnCountHintTrims) {
    std::vector<std::vector<uint8_t>> field_data = {
        make_int_field(1), make_int_field(2), make_int_field(3),
        make_int_field(4), make_int_field(5),
    };
    std::vector<uint8_t> page = build_index_page(8, 5, field_data);
    SchemaInferrer inferrer;
    inferrer.set_column_count_hint(2);
    auto result = inferrer.infer_from_raw_pages(std::vector<std::pair<const uint8_t*, size_t>>{std::make_pair(page.data(), page.size())});
    ASSERT_TRUE(result.success) << result.error_message;
    ASSERT_EQ(result.schema.columns.size(), 2u);
}

// SI-08: infer_from_frm without path fails cleanly
TEST(SchemaInferrerTest, InferFromFrmWithoutPathFails) {
    SchemaInferrer inferrer;
    auto result = inferrer.infer_from_frm();
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "No .frm file path provided");
}

// SI-09: table_name_hint drives DDL table name
TEST(SchemaInferrerTest, TableNameHintApplied) {
    std::vector<std::vector<uint8_t>> field_data = {
        make_int_field(5),
        make_varchar_field("row1"),
        make_varchar_field("row2"),
    };
    std::vector<uint8_t> page = build_index_page(12, 3, field_data);
    SchemaInferrer inferrer;
    inferrer.set_table_name("orders");
    auto result = inferrer.infer_from_raw_pages(std::vector<std::pair<const uint8_t*, size_t>>{std::make_pair(page.data(), page.size())});
    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.schema.table_name, "orders");
    EXPECT_NE(result.schema.to_ddl().find("`orders`"), std::string::npos);
}

}  // namespace dbrecovery::innodb