/**
 * @file test_interactive_schema.cpp
 * @brief Unit tests for InteractiveSchemaBuilder
 */

#include <gtest/gtest.h>
#include <sstream>
#include "dbrecovery/tools/interactive_schema_builder.hpp"
#include "dbrecovery/innodb/schema_inferrer.hpp"

using namespace dbrecovery;

// ==================== Helper: Build a v2 SchemaInferenceResult ====================

static inference::SchemaInferenceResult make_test_schema() {
    inference::SchemaInferenceResult result;
    result.proposed_table_name = "test_table";
    result.detected_column_count = 3;
    result.overall_confidence = 0.95;
    result.inference_method = "statistical";

    // Column 1: id (INT, PK, AUTO_INCREMENT, NOT NULL)
    inference::InferredColumnV2 col1;
    col1.ordinal_position = 1;
    col1.suggested_name = "id";
    col1.is_nullable = false;
    col1.is_primary_key_candidate = true;
    col1.is_auto_increment_candidate = true;
    col1.overall_confidence = 0.98;
    inference::TypeCandidate tc1;
    tc1.type_name = "INT";
    tc1.innodb_type_id = 4;
    tc1.confidence = 0.98;
    tc1.reasoning = "Integer pattern detected";
    col1.candidates.push_back(tc1);
    col1.best_guess = tc1;
    result.columns.push_back(col1);

    // Column 2: name (VARCHAR(255), NULLABLE)
    inference::InferredColumnV2 col2;
    col2.ordinal_position = 2;
    col2.suggested_name = "name";
    col2.is_nullable = true;
    col2.is_primary_key_candidate = false;
    col2.is_auto_increment_candidate = false;
    col2.overall_confidence = 0.93;
    inference::TypeCandidate tc2;
    tc2.type_name = "VARCHAR";
    tc2.innodb_type_id = 12;
    tc2.confidence = 0.93;
    tc2.reasoning = "Variable-length string";
    col2.candidates.push_back(tc2);
    col2.best_guess = tc2;
    result.columns.push_back(col2);

    // Column 3: created_at (DATETIME, NOT NULL)
    inference::InferredColumnV2 col3;
    col3.ordinal_position = 3;
    col3.suggested_name = "created_at";
    col3.is_nullable = false;
    col3.is_primary_key_candidate = false;
    col3.is_auto_increment_candidate = false;
    col3.overall_confidence = 0.91;
    inference::TypeCandidate tc3;
    tc3.type_name = "DATETIME";
    tc3.innodb_type_id = 19;
    tc3.confidence = 0.91;
    tc3.reasoning = "Temporal pattern";
    col3.candidates.push_back(tc3);
    col3.best_guess = tc3;
    result.columns.push_back(col3);

    return result;
}

// ==================== Test 1: Accept all columns ====================
TEST(InteractiveSchemaTest, AcceptAllColumns) {
    auto schema = make_test_schema();
    tools::InteractiveSchemaBuilder builder;

    // Simulate accepting all columns by setting the schema and calling helpers directly
    builder.set_schema_result(schema);

    // Verify all columns are initially accepted
    auto accepted = builder.get_accepted_schema();
    EXPECT_EQ(accepted.columns.size(), 3);
    EXPECT_EQ(accepted.proposed_table_name, "test_table");
}

// ==================== Test 2: Edit column name ====================
TEST(InteractiveSchemaTest, EditColumnName) {
    auto schema = make_test_schema();
    tools::InteractiveSchemaBuilder builder;
    builder.set_schema_result(schema);

    // Edit column 2's name (index 1, "name" -> "full_name")
    builder.handle_edit_column(1, "full_name", 12);

    auto accepted = builder.get_accepted_schema();
    ASSERT_EQ(accepted.columns.size(), 3);
    EXPECT_EQ(accepted.columns[1].suggested_name, "full_name");
}

// ==================== Test 3: Edit column type ====================
TEST(InteractiveSchemaTest, EditColumnType) {
    auto schema = make_test_schema();
    tools::InteractiveSchemaBuilder builder;
    builder.set_schema_result(schema);

    // Edit column 3's type_id (DATETIME=19 -> TIMESTAMP=17)
    builder.handle_edit_column(2, "", 17);

    auto accepted = builder.get_accepted_schema();
    ASSERT_EQ(accepted.columns.size(), 3);
    EXPECT_EQ(accepted.columns[2].best_guess.innodb_type_id, 17);
    EXPECT_EQ(accepted.columns[2].suggested_name, "created_at");  // name unchanged
}

// ==================== Test 4: Reject column ====================
TEST(InteractiveSchemaTest, RejectColumn) {
    auto schema = make_test_schema();
    tools::InteractiveSchemaBuilder builder;
    builder.set_schema_result(schema);

    // Reject column 3 (created_at, index 2)
    builder.handle_reject_column(2);

    auto accepted = builder.get_accepted_schema();
    ASSERT_EQ(accepted.columns.size(), 2);
    EXPECT_EQ(accepted.columns[0].suggested_name, "id");
    EXPECT_EQ(accepted.columns[1].suggested_name, "name");
}

// ==================== Test 5: Reject + accept mix ====================
TEST(InteractiveSchemaTest, RejectAcceptMix) {
    auto schema = make_test_schema();
    tools::InteractiveSchemaBuilder builder;
    builder.set_schema_result(schema);

    // Reject column 2 (name), accept others
    builder.handle_reject_column(1);

    auto accepted = builder.get_accepted_schema();
    ASSERT_EQ(accepted.columns.size(), 2);
    EXPECT_EQ(accepted.columns[0].suggested_name, "id");
    EXPECT_EQ(accepted.columns[1].suggested_name, "created_at");
}

// ==================== Test 6: Quit mid-session ====================
TEST(InteractiveSchemaTest, QuitMidSession) {
    // Quit is handled by run_interactive() returning false
    // We test by verifying the builder state is clean
    auto schema = make_test_schema();
    tools::InteractiveSchemaBuilder builder;
    builder.set_schema_result(schema);

    auto accepted = builder.get_accepted_schema();
    EXPECT_EQ(accepted.columns.size(), 3);
}

// ==================== Test 7: Generate DDL after accept ====================
TEST(InteractiveSchemaTest, GenerateDDLAfterAccept) {
    auto schema = make_test_schema();
    tools::InteractiveSchemaBuilder builder;
    builder.set_schema_result(schema);
    // Accept all by default

    std::string ddl = builder.generate_ddl();
    EXPECT_NE(ddl.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(ddl.find("test_table"), std::string::npos);
    EXPECT_NE(ddl.find("INT"), std::string::npos);
    EXPECT_NE(ddl.find("VARCHAR"), std::string::npos);
    EXPECT_NE(ddl.find("DATETIME"), std::string::npos);
    EXPECT_NE(ddl.find("AUTO_INCREMENT"), std::string::npos);
}

// ==================== Test 8: Generate DDL with reject ====================
TEST(InteractiveSchemaTest, GenerateDDLWithReject) {
    auto schema = make_test_schema();
    tools::InteractiveSchemaBuilder builder;
    builder.set_schema_result(schema);

    // Reject column 2 (name)
    builder.handle_reject_column(1);

    std::string ddl = builder.generate_ddl();
    EXPECT_NE(ddl.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(ddl.find("test_table"), std::string::npos);
    EXPECT_NE(ddl.find("INT"), std::string::npos);
    EXPECT_EQ(ddl.find("VARCHAR"), std::string::npos);  // name was rejected
    EXPECT_NE(ddl.find("DATETIME"), std::string::npos);
}

// ==================== Test 9: Display summary format ====================
TEST(InteractiveSchemaTest, DisplaySummaryFormat) {
    auto schema = make_test_schema();
    tools::InteractiveSchemaBuilder builder;
    builder.set_schema_result(schema);

    // display_summary() just outputs to stdout, verify it doesn't crash
    // and that the accepted schema is still correct
    testing::internal::CaptureStdout();
    builder.display_summary();
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("Accepted Schema Summary"), std::string::npos);
    EXPECT_NE(output.find("test_table"), std::string::npos);
    EXPECT_NE(output.find("3/3"), std::string::npos);
    EXPECT_NE(output.find("95"), std::string::npos);  // 0.95 -> 95%
}

// ==================== Test 10: Empty schema handling ====================
TEST(InteractiveSchemaTest, EmptySchemaHandling) {
    inference::SchemaInferenceResult empty;
    empty.proposed_table_name = "empty_table";
    empty.detected_column_count = 0;

    tools::InteractiveSchemaBuilder builder;
    builder.set_schema_result(empty);

    auto accepted = builder.get_accepted_schema();
    EXPECT_EQ(accepted.columns.size(), 0);
    EXPECT_EQ(accepted.proposed_table_name, "empty_table");

    // generate_ddl should still produce valid SQL
    std::string ddl = builder.generate_ddl();
    EXPECT_NE(ddl.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(ddl.find("empty_table"), std::string::npos);
}

// ==================== Test 11: v1->v2 conversion ====================
TEST(InteractiveSchemaTest, V1ToV2Conversion) {
    // Build a v1 result
    innodb::SchemaInferrer::Result v1_result;
    v1_result.success = true;
    v1_result.schema.table_name = "converted_table";
    v1_result.schema.confidence = 0.92;
    v1_result.schema.inference_method = "statistical_v1";

    innodb::InferredColumn v1_col1;
    v1_col1.ordinal_position = 1;
    v1_col1.name = "id";
    v1_col1.type_name = "INT";
    v1_col1.type_id = 4;
    v1_col1.is_nullable = false;
    v1_col1.is_primary_key = true;
    v1_col1.is_auto_increment = true;
    v1_result.schema.columns.push_back(v1_col1);

    innodb::InferredColumn v1_col2;
    v1_col2.ordinal_position = 2;
    v1_col2.name = "name";
    v1_col2.type_name = "VARCHAR";
    v1_col2.type_id = 12;
    v1_col2.is_nullable = true;
    v1_col2.is_primary_key = false;
    v1_col2.is_auto_increment = false;
    v1_result.schema.columns.push_back(v1_col2);

    // Convert
    auto v2_result = innodb::SchemaInferrer::convert_to_v2_result(v1_result);

    EXPECT_EQ(v2_result.proposed_table_name, "converted_table");
    EXPECT_EQ(v2_result.detected_column_count, 2);
    EXPECT_DOUBLE_EQ(v2_result.overall_confidence, 0.92);
    EXPECT_EQ(v2_result.inference_method, "statistical_v1");

    ASSERT_EQ(v2_result.columns.size(), 2);
    EXPECT_EQ(v2_result.columns[0].suggested_name, "id");
    EXPECT_EQ(v2_result.columns[1].suggested_name, "name");
    EXPECT_EQ(v2_result.columns[0].best_guess.innodb_type_id, 4);
    EXPECT_EQ(v2_result.columns[1].best_guess.innodb_type_id, 12);
}

// ==================== Test 12: v1->v2 preserves nullable ====================
TEST(InteractiveSchemaTest, V1ToV2PreservesNullable) {
    innodb::SchemaInferrer::Result v1_result;
    v1_result.success = true;
    v1_result.schema.table_name = "nullable_test";
    v1_result.schema.confidence = 0.90;

    innodb::InferredColumn v1_col1;
    v1_col1.ordinal_position = 1;
    v1_col1.name = "nullable_col";
    v1_col1.type_name = "VARCHAR";
    v1_col1.type_id = 12;
    v1_col1.is_nullable = true;
    v1_result.schema.columns.push_back(v1_col1);

    innodb::InferredColumn v1_col2;
    v1_col2.ordinal_position = 2;
    v1_col2.name = "not_null_col";
    v1_col2.type_name = "INT";
    v1_col2.type_id = 4;
    v1_col2.is_nullable = false;
    v1_result.schema.columns.push_back(v1_col2);

    auto v2_result = innodb::SchemaInferrer::convert_to_v2_result(v1_result);

    ASSERT_EQ(v2_result.columns.size(), 2);
    EXPECT_TRUE(v2_result.columns[0].is_nullable);
    EXPECT_FALSE(v2_result.columns[1].is_nullable);
}

// ==================== Test 13: v1->v2 preserves auto_increment ====================
TEST(InteractiveSchemaTest, V1ToV2PreservesAutoIncrement) {
    innodb::SchemaInferrer::Result v1_result;
    v1_result.success = true;
    v1_result.schema.table_name = "auto_inc_test";
    v1_result.schema.confidence = 0.90;

    innodb::InferredColumn v1_col1;
    v1_col1.ordinal_position = 1;
    v1_col1.name = "id";
    v1_col1.type_name = "INT";
    v1_col1.type_id = 4;
    v1_col1.is_auto_increment = true;
    v1_result.schema.columns.push_back(v1_col1);

    innodb::InferredColumn v1_col2;
    v1_col2.ordinal_position = 2;
    v1_col2.name = "not_auto";
    v1_col2.type_name = "INT";
    v1_col2.type_id = 4;
    v1_col2.is_auto_increment = false;
    v1_result.schema.columns.push_back(v1_col2);

    auto v2_result = innodb::SchemaInferrer::convert_to_v2_result(v1_result);

    ASSERT_EQ(v2_result.columns.size(), 2);
    EXPECT_TRUE(v2_result.columns[0].is_auto_increment_candidate);
    EXPECT_FALSE(v2_result.columns[1].is_auto_increment_candidate);
}

// ==================== Test 14: Multiple edit cycles ====================
TEST(InteractiveSchemaTest, MultipleEditCycles) {
    auto schema = make_test_schema();
    tools::InteractiveSchemaBuilder builder;
    builder.set_schema_result(schema);

    // Edit column 1 name
    builder.handle_edit_column(0, "user_id", 4);

    // Edit column 2 name and type
    builder.handle_edit_column(1, "user_name", 12);

    // Reject column 3
    builder.handle_reject_column(2);

    auto accepted = builder.get_accepted_schema();
    ASSERT_EQ(accepted.columns.size(), 2);
    EXPECT_EQ(accepted.columns[0].suggested_name, "user_id");
    EXPECT_EQ(accepted.columns[1].suggested_name, "user_name");
    EXPECT_EQ(accepted.columns[1].best_guess.innodb_type_id, 12);
}