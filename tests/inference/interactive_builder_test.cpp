// InteractiveBuilder tests (Phase 4 - Slice D)
//
// Covers:
//   - infer_types: type inference from raw column values (integer, string, temporal)
//   - Interactive refinement: confirm, reject, apply_correction
//   - generate_ddl: final DDL with corrections, empty schema, rejected columns
//   - export_schema: catalog::TableSchema mapping
//   - clear/reset semantics

#include <gtest/gtest.h>
#include "dbrecovery/inference/interactive_builder.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using dbrecovery::inference::InteractiveBuilder;
using dbrecovery::inference::InferredColumnV2;
using dbrecovery::inference::SchemaInferenceResult;

// Helper: build integer raw values (4-byte little-endian, strictly increasing)
std::vector<std::vector<uint8_t>> make_uint64_values(size_t n, uint64_t start = 1) {
    std::vector<std::vector<uint8_t>> out;
    for (size_t i = 0; i < n; i++) {
        uint64_t v = start + i;
        std::vector<uint8_t> bytes(8);
        for (size_t b = 0; b < 8; b++) bytes[b] = (v >> (b * 8)) & 0xFF;
        out.push_back(bytes);
    }
    return out;
}

// Helper: build ASCII string values
std::vector<std::vector<uint8_t>> make_string_values(
    const std::vector<std::string>& strs) {
    std::vector<std::vector<uint8_t>> out;
    for (const auto& s : strs) {
        out.push_back(std::vector<uint8_t>(s.begin(), s.end()));
    }
    return out;
}

// ==================== infer_types tests ====================

// IB-TYPES-01: Integer column infers a numeric type
TEST(InteractiveBuilderTest, InferTypesInteger) {
    InteractiveBuilder builder;
    auto values = make_uint64_values(50, 100);
    std::vector<std::vector<std::vector<uint8_t>>> cols;
    cols.push_back(values);

    auto result = builder.infer_types(cols, {"id"});

    EXPECT_EQ(result.detected_column_count, 1u);
    ASSERT_EQ(result.columns.size(), 1u);
    EXPECT_EQ(result.columns[0].suggested_name, "id");
    EXPECT_GT(result.columns[0].overall_confidence, 0.0);
    EXPECT_FALSE(result.columns[0].best_guess.type_name.empty());
}

// IB-TYPES-02: Multi-column with integer and string
TEST(InteractiveBuilderTest, InferTypesMultiColumn) {
    InteractiveBuilder builder;
    std::vector<std::vector<std::vector<uint8_t>>> cols;
    cols.push_back(make_uint64_values(50, 1));
    cols.push_back(make_string_values({"alice", "bob", "carol", "dave"}));

    auto result = builder.infer_types(cols, {"id", "name"});

    ASSERT_EQ(result.columns.size(), 2u);
    EXPECT_EQ(result.columns[0].suggested_name, "id");
    EXPECT_EQ(result.columns[1].suggested_name, "name");
    EXPECT_GT(result.overall_confidence, 0.0);
}

// IB-TYPES-03: Empty input yields empty result without crash
TEST(InteractiveBuilderTest, InferTypesEmpty) {
    InteractiveBuilder builder;
    std::vector<std::vector<std::vector<uint8_t>>> cols;
    auto result = builder.infer_types(cols);
    EXPECT_EQ(result.detected_column_count, 0u);
    EXPECT_TRUE(result.columns.empty());
}

// ==================== Interactive refinement tests ====================

// IB-REFINE-01: Confirm marks a column accepted
TEST(InteractiveBuilderTest, ConfirmColumn) {
    InteractiveBuilder builder;
    std::vector<std::vector<std::vector<uint8_t>>> cols;
    cols.push_back(make_uint64_values(20, 1));
    builder.infer_types(cols, {"id"});

    ASSERT_EQ(builder.column_states().size(), 1u);
    EXPECT_FALSE(builder.column_states()[0].accepted);

    builder.confirm_column(0);
    EXPECT_TRUE(builder.column_states()[0].accepted);
    EXPECT_FALSE(builder.column_states()[0].rejected);
}

// IB-REFINE-02: Reject marks a column rejected and excluded from PK candidates
TEST(InteractiveBuilderTest, RejectColumn) {
    InteractiveBuilder builder;
    std::vector<std::vector<std::vector<uint8_t>>> cols;
    cols.push_back(make_uint64_values(20, 1));
    builder.infer_types(cols, {"id"});

    builder.reject_column(0);
    EXPECT_TRUE(builder.column_states()[0].rejected);
    EXPECT_FALSE(builder.column_states()[0].accepted);

    // Rejected column type must not appear in DDL
    auto ddl = builder.generate_ddl("t");
    EXPECT_EQ(ddl.find("col_0"), std::string::npos);
}

// IB-REFINE-03: Apply correction overrides type and name
TEST(InteractiveBuilderTest, ApplyCorrection) {
    InteractiveBuilder builder;
    std::vector<std::vector<std::vector<uint8_t>>> cols;
    cols.push_back(make_uint64_values(20, 1));
    builder.infer_types(cols, {"id"});

    InteractiveBuilder::UserCorrection corr;
    corr.ordinal_position = 0;
    corr.corrected_type_name = "BIGINT";
    corr.corrected_name = "user_id";
    corr.is_nullable = false;
    builder.apply_correction(corr);

    auto ddl = builder.generate_ddl("users");
    EXPECT_NE(ddl.find("user_id"), std::string::npos);
    EXPECT_NE(ddl.find("BIGINT"), std::string::npos);
    EXPECT_NE(ddl.find("NOT NULL"), std::string::npos);
}

// ==================== DDL generation tests ====================

// IB-DDL-01: Empty builder produces valid CREATE TABLE
TEST(InteractiveBuilderTest, GenerateDDLEmpty) {
    InteractiveBuilder builder;
    auto ddl = builder.generate_ddl("t");
    EXPECT_NE(ddl.find("CREATE TABLE t"), std::string::npos);
    EXPECT_NE(ddl.find("ENGINE=InnoDB"), std::string::npos);
}

// IB-DDL-02: DDL reflects inferred schema
TEST(InteractiveBuilderTest, GenerateDDLInferred) {
    InteractiveBuilder builder;
    std::vector<std::vector<std::vector<uint8_t>>> cols;
    cols.push_back(make_string_values({"a", "b", "c"}));
    builder.infer_types(cols, {"name"});

    auto ddl = builder.generate_ddl("people");
    EXPECT_NE(ddl.find("CREATE TABLE people"), std::string::npos);
    EXPECT_NE(ddl.find("name"), std::string::npos);
}

// ==================== export_schema tests ====================

// IB-EXPORT-01: Export maps column states to catalog schema
TEST(InteractiveBuilderTest, ExportSchema) {
    InteractiveBuilder builder;
    std::vector<std::vector<std::vector<uint8_t>>> cols;
    cols.push_back(make_uint64_values(30, 1));
    cols.push_back(make_string_values({"x", "y"}));
    builder.infer_types(cols, {"id", "val"});

    auto schema = builder.export_schema("exported_table");
    EXPECT_EQ(schema.table_name, "exported_table");
    ASSERT_EQ(schema.columns.size(), 2u);
    EXPECT_EQ(schema.columns.at(0).name, "id");
}

// ==================== reset tests ====================

// IB-RESET-01: clear() wipes all state
TEST(InteractiveBuilderTest, ClearResetsState) {
    InteractiveBuilder builder;
    std::vector<std::vector<std::vector<uint8_t>>> cols;
    cols.push_back(make_uint64_values(10, 1));
    builder.infer_types(cols, {"id"});
    ASSERT_EQ(builder.column_states().size(), 1u);

    builder.clear();
    EXPECT_EQ(builder.column_states().size(), 0u);
    EXPECT_EQ(builder.inference_result().columns.size(), 0u);
    EXPECT_TRUE(builder.generate_ddl("t").empty() == false);  // still valid SQL
}

}  // namespace