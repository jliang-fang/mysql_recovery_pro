#include <gtest/gtest.h>
#include "dbrecovery/innodb/secondary_index_rescuer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// Helper: typed_encode matching the rescuer's private implementation (Phase 5b)
static std::string test_typed_encode(const std::string& col_name, const std::string& value) {
    uint8_t type_byte = 0;
    for (char c : col_name) { type_byte = static_cast<uint8_t>((type_byte * 31) + c); }
    std::string encoded;
    encoded.push_back(static_cast<char>(type_byte));
    if (value.size() < 256) {
        encoded.push_back(static_cast<char>(value.size()));
    } else {
        encoded.push_back(static_cast<char>(0xFF));
        uint32_t len = static_cast<uint32_t>(value.size());
        for (int i = 3; i >= 0; --i) {
            encoded.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
        }
    }
    encoded += value;
    return encoded;
}

// Helper: build a complete secondary index leaf page with user records
// Returns a 16KB page buffer
// NOTE: canonical layout — FIL header fields are stored BIG-ENDIAN:
//   page_type at 24, PAGE_LEVEL at 64, PAGE_INDEX_ID at 66 (8 bytes).
static std::vector<uint8_t> create_secondary_leaf_page(
    const std::vector<std::pair<uint32_t, uint32_t>>& records,
    uint32_t& out_first_rec_offset,
    uint64_t space_id = 0,
    uint64_t index_id = 0) {

    const size_t page_size = 16384;
    std::vector<uint8_t> page(page_size, 0);

    // --- FIL header (38 bytes) ---
    // Offset 24: page type (big-endian)
    write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    // Offset 34: space id (big-endian)
    write_be<uint32_t>(page.data(), FIL_PAGE_SPACE_ID, static_cast<uint32_t>(space_id));

    // --- Page header (at offset 38) ---
    // Set n_heap format bit (bit 15) to indicate COMPACT/DYNAMIC format
    page[42] |= 0x80;

    // Offset 64: PAGE_LEVEL = 0 (leaf, big-endian)
    write_be<uint16_t>(page.data(), PAGE_LEVEL_ABS, 0);
    // Offset 66: PAGE_INDEX_ID (8 bytes, big-endian)
    write_be<uint64_t>(page.data(), PAGE_INDEX_ID_ABS, index_id);

    // Each record layout: [5 bytes header] + [1 byte n_fields nibble] + [var-len list] + [NULL bitmap] + [field data]
    // We'll carefully place records after INFIMUM+SUPREMUM

    // INFIMUM at offset 54 (13 bytes), SUPREMUM at offset 67 (13 bytes)
    // First user record: start at offset 80
    uint32_t cur_off = 80;

    // INFIMUM record header: set next_offset (absolute offset) to first user record
    // INFIMUM occupies offset 54-66 (13 bytes)
    // INFIMUM's next_offset is at offset 55, 2 bytes LE
    write_le<uint16_t>(page.data(), 55, cur_off);  // next_off = first user record offset

    // SUPREMUM status byte would live at offset 67, but PAGE_INDEX_ID
    // occupies bytes 66-73 — writing here would corrupt it. The parser
    // terminates at next_offset=0 on the last user record and never
    // reaches SUPREMUM, so this write is omitted.

    // Build user records
    for (size_t i = 0; i < records.size(); i++) {
        uint32_t pos = cur_off;
        const auto& [col1_val, pk_val] = records[i];

        // n_fields = 2 (1 sec col + 1 pk col)
        // info_bits: high 5 bits = 0 (n_fields high bits), low 3 bits = 0 (ordinary)
        page[pos] = 0x00;

        // next_offset (bytes 1-2, LE): absolute offset of next record
        uint32_t next_off = (i + 1 < records.size()) ? cur_off + 15 : 0;
        write_le<uint16_t>(page.data(), pos + 1, next_off);

        // heap_no (bytes 3-4, LE): 2 + i (user records start at heap_no 2)
        write_le<uint16_t>(page.data(), pos + 3, static_cast<uint16_t>(2 + i));

        // byte 5: high nibble = n_fields low 4 bits = 2
        page[pos + 5] = 0x20;

        // No var-len fields (all fixed-length INT)
        // NULL bitmap: 1 byte, (2+7)/8 = 1, all 0 = not null
        page[pos + 6] = 0x00;

        // Field data starts at pos + 7
        // Field 1: col1 (INT, 4 bytes LE)
        write_le<uint32_t>(page.data(), pos + 7, col1_val);
        // Field 2: pk_col (INT, 4 bytes LE)
        write_le<uint32_t>(page.data(), pos + 11, pk_val);

        cur_off += 15;  // 5 + 1 + 0 + 1 + 8 = 15 bytes per record
    }

    // Set heap_top at offset 40 (big-endian)
    write_be<uint16_t>(page.data(), PAGE_HEAP_TOP_OFFSET + FIL_HEADER_SIZE, cur_off + 10);

    out_first_rec_offset = 80;
    return page;
}

// Forward declaration for the identity-taking overload used below
static std::string create_temp_sec_ibd(
    const std::vector<std::pair<uint32_t, uint32_t>>& records,
    uint64_t space_id, uint64_t index_id,
    uint32_t page_no = 3,
    const std::string& suffix = "");

// Helper: create a temp .ibd file with a single secondary index page (no identity)
static std::string create_temp_sec_ibd(
    const std::vector<std::pair<uint32_t, uint32_t>>& records,
    uint32_t page_no = 3,
    const std::string& suffix = "") {

    return create_temp_sec_ibd(records, 0, 0, page_no, suffix);
}

// Helper: create a temp .ibd file with a single secondary index page,
// with space_id/index_id identity on the leaf page
static std::string create_temp_sec_ibd(
    const std::vector<std::pair<uint32_t, uint32_t>>& records,
    uint64_t space_id, uint64_t index_id,
    uint32_t page_no,
    const std::string& suffix) {

    std::string path = "/tmp/test_sec_index" + suffix + ".bin";
    const size_t page_size = 16384;

    // Allocate file with enough pages
    std::vector<uint8_t> file(page_size * (page_no + 1), 0);

    // Create the secondary index leaf page
    uint32_t first_rec_off;
    auto page_data = create_secondary_leaf_page(records, first_rec_off, space_id, index_id);

    // Copy page data into the file at the specified page_no
    std::memcpy(file.data() + page_no * page_size, page_data.data(), page_size);

    // Write non-leaf page at page 2 (to test filtering)
    // Level 1 page (canonical big-endian layout)
    write_be<uint16_t>(file.data(), 2 * page_size + FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    write_be<uint16_t>(file.data(), 2 * page_size + PAGE_LEVEL_ABS, 1);  // level 1, not leaf

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), file.size());
    out.close();
    return path;
}

// Helper: build a basic schema with 1 secondary index + 1 PK column
static catalog::TableSchema make_test_schema() {
    catalog::TableSchema schema;
    schema.table_name = "test_table";
    schema.row_format = 2;  // DYNAMIC

    // Phase 5b.1: authoritative identity fields required for strict gate
    schema.space_id = 42;
    schema.schema_hash = "a1b2c3d4e5f67890";
    schema.schema_source = "test_unit";
    schema.trust_level = "AUTHORITATIVE";

    // Column 1: sec_col (INT, type=4, fixed 4 bytes)
    catalog::ColumnInfo col1;
    col1.name = "sec_col";
    col1.mysql_type_id = 4;  // LONG/INT
    col1.is_nullable = true;
    col1.column_length = 4;
    col1.physical_pos = 0;
    col1.ordinal = 1;

    // Column 2: pk_col (INT, type=4, fixed 4 bytes)
    catalog::ColumnInfo col2;
    col2.name = "pk_col";
    col2.mysql_type_id = 4;  // LONG/INT
    col2.is_nullable = true;
    col2.column_length = 4;
    col2.physical_pos = 1;
    col2.ordinal = 2;

    schema.columns[1] = col1;
    schema.columns[2] = col2;

    // Secondary index
    catalog::IndexInfo sec_idx;
    sec_idx.name = "idx_sec";
    sec_idx.is_primary = false;
    sec_idx.is_unique = false;
    sec_idx.index_id = 101;
    sec_idx.columns.push_back("sec_col");

    // Primary key
    catalog::IndexInfo pk_idx;
    pk_idx.name = "PRIMARY";
    pk_idx.is_primary = true;
    pk_idx.is_unique = true;
    pk_idx.columns.push_back("pk_col");

    schema.indexes.push_back(sec_idx);
    schema.indexes.push_back(pk_idx);

    return schema;
}

// ==================== extract_from_secondary_index Tests ====================

TEST(SecondaryIndexRescuerTest, ExtractFromSecIndex) {
    auto schema = make_test_schema();
    // Use matching index_id so identity is verified
    std::string path = create_temp_sec_ibd({{42, 999}, {100, 888}}, 42, 101, 3, "");

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    ASSERT_TRUE(rescuer.set_target_secondary_index("idx_sec"));
    auto result = rescuer.extract_from_secondary_index(path);

    // Should have 2 PK entries
    EXPECT_EQ(result.size(), 2u);

    // Use typed encoding for PK key lookups (Phase 5b)
    std::string pk999 = test_typed_encode("pk_col", "999");
    std::string pk888 = test_typed_encode("pk_col", "888");

    // Check PK 999: sec_col = 42
    auto it = result.find(pk999);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->second["sec_col"], "42");
    EXPECT_EQ(it->second["pk_col"], "999");

    // Check PK 888: sec_col = 100
    it = result.find(pk888);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->second["sec_col"], "100");
    EXPECT_EQ(it->second["pk_col"], "888");

    // Verify identity stats (Phase 5b)
    const auto& stats = rescuer.get_stats();
    EXPECT_GT(stats.identity_verified_pages, 0u);
    EXPECT_EQ(stats.identity_mismatch_pages, 0u);

    std::remove(path.c_str());
}

TEST(SecondaryIndexRescuerTest, ExtractFromSecIndexNoSecIndexInSchema) {
    // Schema with only PK, no secondary index
    catalog::TableSchema schema;
    schema.table_name = "test_table";
    catalog::ColumnInfo col;
    col.name = "pk_col";
    col.mysql_type_id = 4;
    schema.columns[1] = col;
    catalog::IndexInfo pk;
    pk.name = "PRIMARY";
    pk.is_primary = true;
    pk.columns.push_back("pk_col");
    schema.indexes.push_back(pk);

    std::string path = create_temp_sec_ibd({{42, 999}});

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    auto result = rescuer.extract_from_secondary_index(path);

    EXPECT_TRUE(result.empty());

    std::remove(path.c_str());
}

TEST(SecondaryIndexRescuerTest, ExtractFromSecIndexNonExistentFile) {
    auto schema = make_test_schema();

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    ASSERT_TRUE(rescuer.set_target_secondary_index("idx_sec"));
    auto result = rescuer.extract_from_secondary_index("/nonexistent/path.ibd");

    EXPECT_TRUE(result.empty());
}

TEST(SecondaryIndexRescuerTest, ExtractFromSecIndexNoLeafPages) {
    // Non-leaf pages should be filtered out
    std::string path = "/tmp/test_sec_nonleaf.bin";
    const size_t page_size = 16384;
    std::vector<uint8_t> file(page_size * 5, 0);

    // Page 3: INDEX but level 1 (non-leaf), should be skipped (canonical BE layout)
    write_be<uint16_t>(file.data(), 3 * page_size + FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    write_be<uint16_t>(file.data(), 3 * page_size + PAGE_LEVEL_ABS, 1);  // level 1

    // Page 4: UNDO_LOG page, should be skipped (canonical BE layout)
    write_be<uint16_t>(file.data(), 4 * page_size + FIL_PAGE_TYPE_ABS, FIL_PAGE_UNDO_LOG);

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data()), file.size());
    }

    auto schema = make_test_schema();
    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    ASSERT_TRUE(rescuer.set_target_secondary_index("idx_sec"));
    auto result = rescuer.extract_from_secondary_index(path);

    EXPECT_TRUE(result.empty());

    std::remove(path.c_str());
}

// ==================== merge_indexes Tests ====================

TEST(SecondaryIndexRescuerTest, MergeIndexes) {
    auto schema = make_test_schema();

    std::string pk999 = test_typed_encode("pk_col", "999");
    std::string pk888 = test_typed_encode("pk_col", "888");

    // Two index data sources: first has sec_col for PK 999, second has sec_col for PK 888
    std::map<std::string, std::map<std::string, std::string>> idx1;
    idx1[pk999]["sec_col"] = "42";
    idx1[pk999]["pk_col"] = "999";

    std::map<std::string, std::map<std::string, std::string>> idx2;
    idx2[pk888]["sec_col"] = "100";
    idx2[pk888]["pk_col"] = "888";

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    auto rows = rescuer.merge_indexes({idx1, idx2});

    EXPECT_EQ(rows.size(), 2u);
}

TEST(SecondaryIndexRescuerTest, MergeIndexesEmpty) {
    auto schema = make_test_schema();

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    auto rows = rescuer.merge_indexes({});

    EXPECT_TRUE(rows.empty());
}

TEST(SecondaryIndexRescuerTest, MergeIndexesNoOverlap) {
    auto schema = make_test_schema();

    std::string pk111 = test_typed_encode("pk_col", "111");
    std::string pk222 = test_typed_encode("pk_col", "222");

    std::map<std::string, std::map<std::string, std::string>> idx1;
    idx1[pk111]["sec_col"] = "10";
    idx1[pk111]["pk_col"] = "111";

    std::map<std::string, std::map<std::string, std::string>> idx2;
    idx2[pk222]["sec_col"] = "20";
    idx2[pk222]["pk_col"] = "222";

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    auto rows = rescuer.merge_indexes({idx1, idx2});

    EXPECT_EQ(rows.size(), 2u);
}

// ==================== rescue_from_indexes Tests ====================

TEST(SecondaryIndexRescuerTest, RescueFromIndexes) {
    auto schema = make_test_schema();
    // Use matching index_id so identity is verified (Phase 5b)
    std::string path = create_temp_sec_ibd({{42, 999}}, 42, 101, 3, "");

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    ASSERT_TRUE(rescuer.set_target_secondary_index("idx_sec"));
    auto rows = rescuer.rescue_from_indexes({path});

    EXPECT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].fields["sec_col"], "42");
    EXPECT_EQ(rows[0].fields["pk_col"], "999");
    EXPECT_EQ(rows[0].source, "secondary_index_rescue");
    EXPECT_GT(rows[0].completeness, 0.0);
    // Identity should be verified with matching index_id (Phase 5b)
    EXPECT_TRUE(rows[0].identity_verified);

    // Verify stats
    const auto& stats = rescuer.get_stats();
    EXPECT_GT(stats.secondary_pages_scanned, 0u);
    EXPECT_GT(stats.secondary_leaf_pages, 0u);
    EXPECT_GT(stats.secondary_records, 0u);
    EXPECT_GT(stats.rescued_rows, 0u);
    EXPECT_GT(stats.rescue_time_ms, 0.0);
    EXPECT_GT(stats.identity_verified_pages, 0u);
    EXPECT_EQ(stats.identity_mismatch_pages, 0u);

    std::remove(path.c_str());
}

TEST(SecondaryIndexRescuerTest, RescueFromIndexesEmptyPaths) {
    auto schema = make_test_schema();
    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    ASSERT_TRUE(rescuer.set_target_secondary_index("idx_sec"));
    auto rows = rescuer.rescue_from_indexes({});

    EXPECT_TRUE(rows.empty());
}

TEST(SecondaryIndexRescuerTest, RescueFromIndexesMultipleFiles) {
    auto schema = make_test_schema();
    // Both files with matching index_id
    std::string path1 = create_temp_sec_ibd({{42, 999}}, 42, 101, 3, "a");
    std::string path2 = create_temp_sec_ibd({{100, 888}}, 42, 101, 4, "b");

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    ASSERT_TRUE(rescuer.set_target_secondary_index("idx_sec"));
    auto rows = rescuer.rescue_from_indexes({path1, path2});

    EXPECT_EQ(rows.size(), 2u);

    std::remove(path1.c_str());
    std::remove(path2.c_str());
}

// ==================== Stats Tests ====================

TEST(SecondaryIndexRescuerTest, StatsInitial) {
    SecondaryIndexRescuer rescuer;
    const auto& stats = rescuer.get_stats();
    EXPECT_EQ(stats.secondary_pages_scanned, 0u);
    EXPECT_EQ(stats.secondary_leaf_pages, 0u);
    EXPECT_EQ(stats.secondary_records, 0u);
    EXPECT_EQ(stats.rescued_rows, 0u);
    EXPECT_EQ(stats.merged_rows, 0u);
    EXPECT_EQ(stats.rescue_time_ms, 0.0);
    // Phase 5b identity/conflict stats
    EXPECT_EQ(stats.identity_verified_pages, 0u);
    EXPECT_EQ(stats.identity_mismatch_pages, 0u);
    EXPECT_EQ(stats.merge_conflicts, 0u);
}

TEST(SecondaryIndexRescuerTest, ExtractUpdatesStats) {
    auto schema = make_test_schema();
    std::string path = create_temp_sec_ibd({{42, 999}}, 42, 101, 3, "");

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    ASSERT_TRUE(rescuer.set_target_secondary_index("idx_sec"));
    rescuer.extract_from_secondary_index(path);

    const auto& stats = rescuer.get_stats();
    EXPECT_GT(stats.secondary_pages_scanned, 0u);
    EXPECT_GT(stats.secondary_leaf_pages, 0u);
    EXPECT_GT(stats.secondary_records, 0u);

    std::remove(path.c_str());
}

// ==================== Edge Cases ====================

TEST(SecondaryIndexRescuerTest, SchemaWithoutSecondaryIndex) {
    catalog::TableSchema schema;
    schema.table_name = "test";
    catalog::ColumnInfo col;
    col.name = "pk_col";
    col.mysql_type_id = 4;
    schema.columns[1] = col;
    catalog::IndexInfo pk;
    pk.name = "PRIMARY";
    pk.is_primary = true;
    pk.columns.push_back("pk_col");
    schema.indexes.push_back(pk);

    std::string path = create_temp_sec_ibd({{42, 999}});

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    auto result = rescuer.extract_from_secondary_index(path);

    // No secondary index defined, should return empty
    EXPECT_TRUE(result.empty());

    std::remove(path.c_str());
}

TEST(SecondaryIndexRescuerTest, EmptySchema) {
    catalog::TableSchema schema;
    schema.table_name = "empty";

    std::string path = create_temp_sec_ibd({{42, 999}});

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    auto result = rescuer.extract_from_secondary_index(path);

    EXPECT_TRUE(result.empty());

    std::remove(path.c_str());
}

// ==================== Phase 5b: Identity & Conflict Tests ====================

TEST(SecondaryIndexRescuerTest, IdentityMismatchDetected) {
    // Schema expects index_id=101, but page has index_id=0
    // Phase 5b.1: strict gate rejects mismatched pages before parsing
    auto schema = make_test_schema();
    std::string path = create_temp_sec_ibd({{42, 999}}, 0, 0, 3, "");

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    ASSERT_TRUE(rescuer.set_target_secondary_index("idx_sec"));
    auto result = rescuer.extract_from_secondary_index(path);

    // Strict gate: identity mismatch → page rejected, zero structured rows
    EXPECT_EQ(result.size(), 0u);

    const auto& stats = rescuer.get_stats();
    EXPECT_EQ(stats.identity_verified_pages, 0u);
    EXPECT_GT(stats.identity_mismatch_pages, 0u);

    std::remove(path.c_str());
}

TEST(SecondaryIndexRescuerTest, IdentityMismatchMarksRowsEvidenceOnly) {
    auto schema = make_test_schema();
    // Page has index_id=0, schema expects index_id=101
    // Phase 5b.1: strict gate rejects mismatched pages → zero rows
    std::string path = create_temp_sec_ibd({{42, 999}}, 0, 0, 3, "");

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    ASSERT_TRUE(rescuer.set_target_secondary_index("idx_sec"));
    auto rows = rescuer.rescue_from_indexes({path});

    // Strict gate: zero rows from mismatched identity
    EXPECT_EQ(rows.size(), 0u);

    const auto& stats = rescuer.get_stats();
    EXPECT_EQ(stats.identity_verified_pages, 0u);
    EXPECT_GT(stats.identity_mismatch_pages, 0u);

    std::remove(path.c_str());
}

TEST(SecondaryIndexRescuerTest, ConflictReporting) {
    auto schema = make_test_schema();

    std::string pk = test_typed_encode("pk_col", "999");

    // Two indexes with same PK but different sec_col value
    std::map<std::string, std::map<std::string, std::string>> idx1;
    idx1[pk]["sec_col"] = "42";
    idx1[pk]["pk_col"] = "999";

    std::map<std::string, std::map<std::string, std::string>> idx2;
    idx2[pk]["sec_col"] = "100";  // conflicting value
    idx2[pk]["pk_col"] = "999";

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    auto rows = rescuer.merge_indexes({idx1, idx2});

    ASSERT_EQ(rows.size(), 1u);
    // Conflict should be reported as "42|100" (old|new concatenation)
    EXPECT_EQ(rows[0].fields["sec_col"], "42|100");

    const auto& stats = rescuer.get_stats();
    EXPECT_GT(stats.merge_conflicts, 0u);
}

TEST(SecondaryIndexRescuerTest, TypedEncodingUnique) {
    // Same value, different column names should produce different encodings
    std::string enc1 = test_typed_encode("col_a", "123");
    std::string enc2 = test_typed_encode("col_b", "123");
    EXPECT_NE(enc1, enc2);

    // Same column name, different values should produce different encodings
    std::string enc3 = test_typed_encode("pk_col", "123");
    std::string enc4 = test_typed_encode("pk_col", "456");
    EXPECT_NE(enc3, enc4);
}

TEST(SecondaryIndexRescuerTest, IdentityVerifiedWithMatchingIndexId) {
    auto schema = make_test_schema();
    // Page has matching index_id=101
    std::string path = create_temp_sec_ibd({{42, 999}}, 42, 101, 3, "");

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    ASSERT_TRUE(rescuer.set_target_secondary_index("idx_sec"));
    auto result = rescuer.extract_from_secondary_index(path);

    const auto& stats = rescuer.get_stats();
    EXPECT_GT(stats.identity_verified_pages, 0u);
    EXPECT_EQ(stats.identity_mismatch_pages, 0u);

    std::remove(path.c_str());
}

TEST(SecondaryIndexRescuerTest, NoIndexIdInSchemaRejectsTargetSelection) {
    // Schema has index_id=0, so set_target_secondary_index() returns false
    catalog::TableSchema schema;
    schema.table_name = "test";
    schema.row_format = 2;
    schema.space_id = 42;
    schema.schema_hash = "a1b2c3d4e5f67890";
    schema.schema_source = "test_unit";
    schema.trust_level = "AUTHORITATIVE";
    catalog::ColumnInfo col1;
    col1.name = "sec_col"; col1.mysql_type_id = 4; col1.is_nullable = true; col1.column_length = 4; col1.physical_pos = 0; col1.ordinal = 1;
    catalog::ColumnInfo col2;
    col2.name = "pk_col"; col2.mysql_type_id = 4; col2.is_nullable = true; col2.column_length = 4; col2.physical_pos = 1; col2.ordinal = 2;
    schema.columns[1] = col1;
    schema.columns[2] = col2;
    catalog::IndexInfo sec_idx;
    sec_idx.name = "idx_sec";
    sec_idx.is_primary = false;
    sec_idx.index_id = 0;  // No index_id set — prevents authoritative target
    sec_idx.columns.push_back("sec_col");
    catalog::IndexInfo pk;
    pk.name = "PRIMARY";
    pk.is_primary = true;
    pk.columns.push_back("pk_col");
    schema.indexes.push_back(sec_idx);
    schema.indexes.push_back(pk);

    std::string path = create_temp_sec_ibd({{42, 999}}, 0, 0, 3, "");

    SecondaryIndexRescuer rescuer;
    rescuer.set_schema(schema);
    // Cannot set target when index_id == 0
    EXPECT_FALSE(rescuer.set_target_secondary_index("idx_sec"));
    auto result = rescuer.extract_from_secondary_index(path);

    // Without target index, no structured extraction possible
    EXPECT_TRUE(result.empty());

    const auto& stats = rescuer.get_stats();
    EXPECT_EQ(stats.identity_verified_pages, 0u);
    EXPECT_EQ(stats.identity_mismatch_pages, 0u);

    std::remove(path.c_str());
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}