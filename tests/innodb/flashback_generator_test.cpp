#include <gtest/gtest.h>
#include "dbrecovery/innodb/flashback_generator.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

namespace dbrecovery {
namespace innodb {

// ==================== Helper: Build a test schema ====================

static catalog::TableSchema make_test_schema() {
    catalog::TableSchema schema;
    schema.table_name = "test_table";
    schema.table_id = 42;
    schema.row_format = 2; // DYNAMIC

    // INT pk
    catalog::ColumnInfo pk_col;
    pk_col.name = "id";
    pk_col.mysql_type_id = mysql_types::LONG;
    pk_col.is_nullable = false;
    pk_col.column_length = 4;
    pk_col.physical_pos = 0;
    pk_col.ordinal = 1;

    // VARCHAR name
    catalog::ColumnInfo name_col;
    name_col.name = "name";
    name_col.mysql_type_id = mysql_types::VARCHAR;
    name_col.is_nullable = true;
    name_col.column_length = 100;
    name_col.physical_pos = 1;
    name_col.ordinal = 2;

    // INT age
    catalog::ColumnInfo age_col;
    age_col.name = "age";
    age_col.mysql_type_id = mysql_types::SHORT;
    age_col.is_nullable = true;
    age_col.column_length = 2;
    age_col.physical_pos = 2;
    age_col.ordinal = 3;

    schema.columns[1] = pk_col;
    schema.columns[2] = name_col;
    schema.columns[3] = age_col;

    catalog::IndexInfo pk_idx;
    pk_idx.name = "PRIMARY";
    pk_idx.is_primary = true;
    pk_idx.is_unique = true;
    pk_idx.columns.push_back("id");
    schema.indexes.push_back(pk_idx);

    return schema;
}

// ==================== FlashbackConfig Tests ====================

TEST(FlashbackGeneratorTest, ConfigDefaultValues) {
    FlashbackConfig config;

    EXPECT_EQ(config.mode, FlashbackMode::DELETE);
    EXPECT_EQ(config.output_format, FlashbackOutputFormat::SQL);
    EXPECT_FALSE(config.start_time.has_value());
    EXPECT_FALSE(config.end_time.has_value());
    EXPECT_FALSE(config.start_lsn.has_value());
    EXPECT_FALSE(config.end_lsn.has_value());
    EXPECT_FALSE(config.trx_id.has_value());
    EXPECT_EQ(config.max_records, 10000u);
    EXPECT_EQ(config.max_chain_depth, 1000u);
    EXPECT_FALSE(config.include_incomplete);
    EXPECT_TRUE(config.human_readable);
    EXPECT_TRUE(config.pretty_json);
    EXPECT_TRUE(config.include_metadata);
}

TEST(FlashbackGeneratorTest, ConfigCustomValues) {
    FlashbackConfig config;
    config.mode = FlashbackMode::POINT_IN_TIME;
    config.output_format = FlashbackOutputFormat::JSON;
    config.max_records = 500;
    config.max_chain_depth = 200;
    config.include_incomplete = true;
    config.human_readable = false;
    config.pretty_json = false;
    config.include_metadata = false;

    EXPECT_EQ(config.mode, FlashbackMode::POINT_IN_TIME);
    EXPECT_EQ(config.output_format, FlashbackOutputFormat::JSON);
    EXPECT_EQ(config.max_records, 500u);
    EXPECT_EQ(config.max_chain_depth, 200u);
    EXPECT_TRUE(config.include_incomplete);
    EXPECT_FALSE(config.human_readable);
    EXPECT_FALSE(config.pretty_json);
    EXPECT_FALSE(config.include_metadata);
}

TEST(FlashbackGeneratorTest, ConfigTimeFilters) {
    FlashbackConfig config;
    auto now = std::chrono::system_clock::now();
    config.start_time = now;
    config.end_time = now + std::chrono::hours(1);

    ASSERT_TRUE(config.start_time.has_value());
    ASSERT_TRUE(config.end_time.has_value());
    EXPECT_EQ(*config.start_time, now);
    EXPECT_EQ(*config.end_time, now + std::chrono::hours(1));
}

TEST(FlashbackGeneratorTest, ConfigLSNFilters) {
    FlashbackConfig config;
    config.start_lsn = 1000;
    config.end_lsn = 2000;

    ASSERT_TRUE(config.start_lsn.has_value());
    ASSERT_TRUE(config.end_lsn.has_value());
    EXPECT_EQ(*config.start_lsn, 1000u);
    EXPECT_EQ(*config.end_lsn, 2000u);
}

TEST(FlashbackGeneratorTest, ConfigTrxIdFilter) {
    FlashbackConfig config;
    config.trx_id = 12345;

    ASSERT_TRUE(config.trx_id.has_value());
    EXPECT_EQ(*config.trx_id, 12345u);
}

// ==================== FlashbackMode Enum Tests ====================

TEST(FlashbackGeneratorTest, FlashbackModeValues) {
    EXPECT_NE(static_cast<int>(FlashbackMode::DELETE),
              static_cast<int>(FlashbackMode::UPDATE));
    EXPECT_NE(static_cast<int>(FlashbackMode::DELETE),
              static_cast<int>(FlashbackMode::POINT_IN_TIME));
    EXPECT_NE(static_cast<int>(FlashbackMode::DELETE),
              static_cast<int>(FlashbackMode::TRANSACTION));
    EXPECT_NE(static_cast<int>(FlashbackMode::DELETE),
              static_cast<int>(FlashbackMode::FULL_CHAIN));
}

// ==================== FlashbackOutputFormat Enum Tests ====================

TEST(FlashbackGeneratorTest, FlashbackOutputFormatValues) {
    EXPECT_NE(static_cast<int>(FlashbackOutputFormat::SQL),
              static_cast<int>(FlashbackOutputFormat::CSV));
    EXPECT_NE(static_cast<int>(FlashbackOutputFormat::SQL),
              static_cast<int>(FlashbackOutputFormat::JSON));
}

// ==================== Construction & Configuration Tests ====================

TEST(FlashbackGeneratorTest, DefaultConstruction) {
    FlashbackGenerator gen;

    const auto& config = gen.config();
    EXPECT_EQ(config.mode, FlashbackMode::DELETE);
    EXPECT_EQ(config.output_format, FlashbackOutputFormat::SQL);
    EXPECT_EQ(config.max_records, 10000u);

    const auto& stats = gen.get_stats();
    EXPECT_EQ(stats.total_records, 0u);
    EXPECT_EQ(stats.complete_records, 0u);
    EXPECT_EQ(stats.partial_records, 0u);
    EXPECT_EQ(stats.avg_completeness, 0.0);
    EXPECT_EQ(stats.execution_time_ms, 0.0);
}

TEST(FlashbackGeneratorTest, SetAndGetConfig) {
    FlashbackGenerator gen;
    auto& config = gen.config();
    config.mode = FlashbackMode::POINT_IN_TIME;
    config.output_format = FlashbackOutputFormat::CSV;
    config.max_records = 100;
    config.include_incomplete = true;

    FlashbackConfig new_config;
    new_config.mode = FlashbackMode::UPDATE;
    new_config.output_format = FlashbackOutputFormat::JSON;
    new_config.max_records = 777;
    gen.set_config(new_config);

    const auto& retrieved = gen.config();
    EXPECT_EQ(retrieved.mode, FlashbackMode::UPDATE);
    EXPECT_EQ(retrieved.output_format, FlashbackOutputFormat::JSON);
    EXPECT_EQ(retrieved.max_records, 777u);
}

TEST(FlashbackGeneratorTest, ResetStats) {
    FlashbackGenerator gen;
    gen.reset_stats();
    const auto& stats = gen.get_stats();
    EXPECT_EQ(stats.total_records, 0u);
    EXPECT_EQ(stats.complete_records, 0u);
    EXPECT_EQ(stats.partial_records, 0u);
    EXPECT_EQ(stats.avg_completeness, 0.0);
    EXPECT_EQ(stats.execution_time_ms, 0.0);
}

TEST(FlashbackGeneratorTest, SetPageSize) {
    FlashbackGenerator gen;
    // Should not throw
    EXPECT_NO_THROW(gen.set_page_size(8192));
    EXPECT_NO_THROW(gen.set_page_size(16384));
    EXPECT_NO_THROW(gen.set_page_size(32768));
}

TEST(FlashbackGeneratorTest, SetIbdataPath) {
    FlashbackGenerator gen;
    // Should not throw (path is just stored, not opened)
    EXPECT_NO_THROW(gen.set_ibdata_path("/tmp/test_ibdata"));
}

// ==================== FlashbackRecord Tests ====================

TEST(FlashbackGeneratorTest, FlashbackRecordDefaultConstruction) {
    FlashbackRecord rec;

    EXPECT_EQ(rec.operation, FlashbackRecord::INSERT);
    EXPECT_TRUE(rec.fields.empty());
    EXPECT_TRUE(rec.field_map.empty());
    EXPECT_EQ(rec.trx_id, 0u);
    EXPECT_EQ(rec.undo_no, 0u);
    EXPECT_FALSE(rec.timestamp.has_value());
    EXPECT_FALSE(rec.lsn.has_value());
    EXPECT_EQ(rec.completeness, 0.0);
    EXPECT_EQ(rec.n_columns_total, 0u);
    EXPECT_EQ(rec.n_columns_found, 0u);
    EXPECT_FALSE(rec.is_complete);
    EXPECT_TRUE(rec.source_type.empty());
}

TEST(FlashbackGeneratorTest, FlashbackRecordFieldsAssignment) {
    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 100;
    rec.undo_no = 5;
    rec.completeness = 1.0;
    rec.n_columns_total = 3;
    rec.n_columns_found = 3;
    rec.is_complete = true;
    rec.source_type = "DELETE_MARK";

    DecodedField f1;
    f1.column_name = "id";
    f1.human_value = "42";
    f1.is_decoded = true;
    f1.confidence = 1.0;

    DecodedField f2;
    f2.column_name = "name";
    f2.human_value = "Alice";
    f2.is_decoded = true;
    f2.confidence = 1.0;

    rec.fields.push_back(f1);
    rec.fields.push_back(f2);
    rec.field_map["id"] = "42";
    rec.field_map["name"] = "Alice";

    EXPECT_EQ(rec.operation, FlashbackRecord::DELETE_OP);
    EXPECT_EQ(rec.trx_id, 100u);
    EXPECT_EQ(rec.undo_no, 5u);
    EXPECT_EQ(rec.completeness, 1.0);
    EXPECT_EQ(rec.n_columns_total, 3u);
    EXPECT_EQ(rec.n_columns_found, 3u);
    EXPECT_TRUE(rec.is_complete);
    EXPECT_EQ(rec.source_type, "DELETE_MARK");
    EXPECT_EQ(rec.fields.size(), 2u);
    EXPECT_EQ(rec.field_map["id"], "42");
    EXPECT_EQ(rec.field_map["name"], "Alice");
}

TEST(FlashbackGeneratorTest, FlashbackRecordOperationTypes) {
    FlashbackRecord rec;

    rec.operation = FlashbackRecord::INSERT;
    EXPECT_EQ(rec.operation, FlashbackRecord::INSERT);

    rec.operation = FlashbackRecord::UPDATE;
    EXPECT_EQ(rec.operation, FlashbackRecord::UPDATE);

    rec.operation = FlashbackRecord::DELETE_OP;
    EXPECT_EQ(rec.operation, FlashbackRecord::DELETE_OP);
}

TEST(FlashbackGeneratorTest, FlashbackRecordTimestamp) {
    FlashbackRecord rec;
    auto now = std::chrono::system_clock::now();
    rec.timestamp = now;

    ASSERT_TRUE(rec.timestamp.has_value());
    EXPECT_EQ(*rec.timestamp, now);
}

TEST(FlashbackGeneratorTest, FlashbackRecordLSN) {
    FlashbackRecord rec;
    rec.lsn = 123456789;

    ASSERT_TRUE(rec.lsn.has_value());
    EXPECT_EQ(*rec.lsn, 123456789u);
}

TEST(FlashbackGeneratorTest, FlashbackRecordCompletenessScoring) {
    FlashbackRecord rec;
    rec.n_columns_total = 5;
    rec.n_columns_found = 3;
    rec.completeness = 3.0 / 5.0;
    rec.is_complete = false;

    EXPECT_EQ(rec.completeness, 0.6);
    EXPECT_FALSE(rec.is_complete);
}

TEST(FlashbackGeneratorTest, FlashbackRecordFullCompleteness) {
    FlashbackRecord rec;
    rec.n_columns_total = 4;
    rec.n_columns_found = 4;
    rec.completeness = 1.0;
    rec.is_complete = true;

    EXPECT_EQ(rec.completeness, 1.0);
    EXPECT_TRUE(rec.is_complete);
}

// ==================== FlashbackRecord::to_sql Tests ====================

TEST(FlashbackGeneratorTest, ToSqlDeleteOp) {
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;

    DecodedField f1;
    f1.column_name = "id";
    f1.human_value = "42";
    f1.is_null = false;
    f1.is_decoded = true;

    DecodedField f2;
    f2.column_name = "name";
    f2.human_value = "Alice";
    f2.is_null = false;
    f2.is_decoded = true;

    DecodedField f3;
    f3.column_name = "age";
    f3.human_value = "30";
    f3.is_null = false;
    f3.is_decoded = true;

    rec.fields.push_back(f1);
    rec.fields.push_back(f2);
    rec.fields.push_back(f3);

    std::string sql = rec.to_sql(schema);
    EXPECT_NE(sql.find("INSERT INTO test_table"), std::string::npos);
    EXPECT_NE(sql.find("`id`"), std::string::npos);
    EXPECT_NE(sql.find("`name`"), std::string::npos);
    EXPECT_NE(sql.find("`age`"), std::string::npos);
    EXPECT_NE(sql.find("42"), std::string::npos);
    EXPECT_NE(sql.find("'Alice'"), std::string::npos);
    EXPECT_NE(sql.find("30"), std::string::npos);
    EXPECT_NE(sql.find(";"), std::string::npos);
}

TEST(FlashbackGeneratorTest, ToSqlDeleteOpWithNull) {
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;

    DecodedField f1;
    f1.column_name = "id";
    f1.human_value = "1";
    f1.is_null = false;
    f1.is_decoded = true;

    DecodedField f2;
    f2.column_name = "name";
    f2.human_value = "NULL";
    f2.is_null = true;
    f2.is_decoded = true;

    rec.fields.push_back(f1);
    rec.fields.push_back(f2);

    std::string sql = rec.to_sql(schema);
    EXPECT_NE(sql.find("NULL"), std::string::npos);
}

TEST(FlashbackGeneratorTest, ToSqlWithEscaping) {
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;

    DecodedField f1;
    f1.column_name = "id";
    f1.human_value = "1";
    f1.is_null = false;
    f1.is_decoded = true;

    DecodedField f2;
    f2.column_name = "name";
    f2.human_value = "O'Brien";
    f2.is_null = false;
    f2.is_decoded = true;

    rec.fields.push_back(f1);
    rec.fields.push_back(f2);

    std::string sql = rec.to_sql(schema);
    // Single quote should be escaped as ''
    EXPECT_NE(sql.find("O''Brien"), std::string::npos);
}

TEST(FlashbackGeneratorTest, ToSqlUpdateOp) {
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::UPDATE;

    DecodedField f1;
    f1.column_name = "name";
    f1.human_value = "Bob";
    f1.is_decoded = true;

    rec.fields.push_back(f1);

    std::string sql = rec.to_sql(schema);
    EXPECT_NE(sql.find("UPDATE"), std::string::npos);
}

TEST(FlashbackGeneratorTest, ToSqlInsertOp) {
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::INSERT;

    DecodedField f1;
    f1.column_name = "id";
    f1.human_value = "1";
    f1.is_decoded = true;

    rec.fields.push_back(f1);

    std::string sql = rec.to_sql(schema);
    EXPECT_NE(sql.find("DELETE FROM"), std::string::npos);
}

// ==================== FlashbackRecord::to_csv Tests ====================

TEST(FlashbackGeneratorTest, ToCsvDeleteOp) {
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 100;
    rec.undo_no = 5;
    rec.completeness = 1.0;
    rec.is_complete = true;

    DecodedField f1;
    f1.column_name = "id";
    f1.human_value = "42";
    f1.is_decoded = true;

    DecodedField f2;
    f2.column_name = "name";
    f2.human_value = "Alice";
    f2.is_decoded = true;

    rec.fields.push_back(f1);
    rec.fields.push_back(f2);
    rec.field_map["id"] = "42";
    rec.field_map["name"] = "Alice";

    std::string csv = rec.to_csv(schema);
    EXPECT_NE(csv.find("DELETE"), std::string::npos);
    EXPECT_NE(csv.find("100"), std::string::npos);
    EXPECT_NE(csv.find("5"), std::string::npos);
    EXPECT_NE(csv.find("42"), std::string::npos);
    EXPECT_NE(csv.find("Alice"), std::string::npos);
    EXPECT_NE(csv.find("1.0000"), std::string::npos);
    EXPECT_NE(csv.find("1"), std::string::npos); // is_complete
}

TEST(FlashbackGeneratorTest, ToCsvWithFieldMap) {
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 0;
    rec.undo_no = 0;
    rec.completeness = 0.5;
    rec.is_complete = false;
    rec.field_map["id"] = "99";
    rec.field_map["name"] = "Bob";

    std::string csv = rec.to_csv(schema);
    EXPECT_NE(csv.find("99"), std::string::npos);
    EXPECT_NE(csv.find("Bob"), std::string::npos);
    EXPECT_NE(csv.find("0.5000"), std::string::npos);
    EXPECT_NE(csv.find("0"), std::string::npos); // is_complete = false
}

// ==================== FlashbackRecord::to_json Tests ====================

TEST(FlashbackGeneratorTest, ToJsonDeleteOp) {
    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 100;
    rec.undo_no = 5;
    rec.completeness = 1.0;
    rec.is_complete = true;

    DecodedField f1;
    f1.column_name = "id";
    f1.human_value = "42";
    f1.is_null = false;
    f1.is_decoded = true;

    DecodedField f2;
    f2.column_name = "name";
    f2.human_value = "Alice";
    f2.is_null = false;
    f2.is_decoded = true;

    rec.fields.push_back(f1);
    rec.fields.push_back(f2);

    std::string json = rec.to_json(false);
    EXPECT_NE(json.find("\"operation\": \"DELETE\""), std::string::npos);
    EXPECT_NE(json.find("\"trx_id\": 100"), std::string::npos);
    EXPECT_NE(json.find("\"undo_no\": 5"), std::string::npos);
    EXPECT_NE(json.find("\"id\": \"42\""), std::string::npos);
    EXPECT_NE(json.find("\"name\": \"Alice\""), std::string::npos);
    EXPECT_NE(json.find("\"completeness\": 1.0000"), std::string::npos);
    EXPECT_NE(json.find("\"is_complete\": true"), std::string::npos);
}

TEST(FlashbackGeneratorTest, ToJsonWithNull) {
    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 1;
    rec.undo_no = 0;
    rec.completeness = 0.5;
    rec.is_complete = false;

    DecodedField f1;
    f1.column_name = "name";
    f1.human_value = "NULL";
    f1.is_null = true;
    f1.is_decoded = true;

    rec.fields.push_back(f1);

    std::string json = rec.to_json(false);
    EXPECT_NE(json.find("\"name\": null"), std::string::npos);
    EXPECT_NE(json.find("\"is_complete\": false"), std::string::npos);
}

TEST(FlashbackGeneratorTest, ToJsonPretty) {
    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 1;
    rec.undo_no = 0;
    rec.completeness = 1.0;
    rec.is_complete = true;

    DecodedField f1;
    f1.column_name = "id";
    f1.human_value = "1";
    f1.is_null = false;
    f1.is_decoded = true;

    rec.fields.push_back(f1);

    std::string pretty = rec.to_json(true);
    // Pretty JSON should have newlines
    EXPECT_NE(pretty.find('\n'), std::string::npos);
    EXPECT_NE(pretty.find("  "), std::string::npos);

    std::string compact = rec.to_json(false);
    // Compact JSON should not have newlines
    EXPECT_EQ(compact.find('\n'), std::string::npos);
}

// ==================== to_sql_helper Tests ====================

TEST(FlashbackGeneratorTest, ToSqlHelperMultipleRecords) {
    auto schema = make_test_schema();

    FlashbackRecord rec1;
    rec1.operation = FlashbackRecord::DELETE_OP;
    DecodedField f1;
    f1.column_name = "id";
    f1.human_value = "1";
    f1.is_null = false;
    f1.is_decoded = true;
    rec1.fields.push_back(f1);

    FlashbackRecord rec2;
    rec2.operation = FlashbackRecord::DELETE_OP;
    DecodedField f2;
    f2.column_name = "id";
    f2.human_value = "2";
    f2.is_null = false;
    f2.is_decoded = true;
    rec2.fields.push_back(f2);

    std::string sql = FlashbackRecord::to_sql_helper({rec1, rec2}, schema);
    EXPECT_NE(sql.find("INSERT INTO test_table"), std::string::npos);
    // Should have two statements (two lines)
    size_t newline_count = 0;
    for (char c : sql) if (c == '\n') newline_count++;
    EXPECT_GE(newline_count, 2u);
}

TEST(FlashbackGeneratorTest, ToSqlHelperEmpty) {
    auto schema = make_test_schema();
    std::string sql = FlashbackRecord::to_sql_helper({}, schema);
    EXPECT_TRUE(sql.empty());
}

// ==================== decode_field_value Tests ====================

TEST(FlashbackGeneratorTest, DecodeFieldValueTiny) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::TINY;

    std::vector<uint8_t> pos = {42};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(pos, col), "42");

    std::vector<uint8_t> neg = {static_cast<uint8_t>(-1)}; // -1 in two's complement
    EXPECT_EQ(FlashbackGenerator::decode_field_value(neg, col), "-1");

    std::vector<uint8_t> max_val = {127};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(max_val, col), "127");

    std::vector<uint8_t> min_val = {128}; // -128
    EXPECT_EQ(FlashbackGenerator::decode_field_value(min_val, col), "-128");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueTinyEmpty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::TINY;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueShort) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::SHORT;

    std::vector<uint8_t> val = {0x2A, 0x00}; // 42 LE
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "42");

    std::vector<uint8_t> neg = {0xFF, 0xFF}; // -1 LE
    EXPECT_EQ(FlashbackGenerator::decode_field_value(neg, col), "-1");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueShortEmpty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::SHORT;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueLong) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::LONG;

    std::vector<uint8_t> val = {0x2A, 0x00, 0x00, 0x00}; // 42 LE
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "42");

    std::vector<uint8_t> neg = {0xFF, 0xFF, 0xFF, 0xFF}; // -1 LE
    EXPECT_EQ(FlashbackGenerator::decode_field_value(neg, col), "-1");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueLongEmpty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::LONG;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueLongLong) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::LONGLONG;

    std::vector<uint8_t> val = {0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // 42 LE
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "42");

    std::vector<uint8_t> neg = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // -1 LE
    EXPECT_EQ(FlashbackGenerator::decode_field_value(neg, col), "-1");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueLongLongEmpty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::LONGLONG;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueInt24) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::INT24;

    // 42 as 3-byte LE
    std::vector<uint8_t> val = {0x2A, 0x00, 0x00};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "42");

    // -1 as 3-byte LE
    std::vector<uint8_t> neg = {0xFF, 0xFF, 0xFF};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(neg, col), "-1");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueInt24Empty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::INT24;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueFloat) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::FLOAT;

    float f = 3.14f;
    std::vector<uint8_t> val(4);
    memcpy(val.data(), &f, 4);
    std::string decoded = FlashbackGenerator::decode_field_value(val, col);
    EXPECT_NE(decoded.find("3.14"), std::string::npos);
}

TEST(FlashbackGeneratorTest, DecodeFieldValueFloatEmpty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::FLOAT;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueDouble) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::DOUBLE;

    double d = 3.1415926535;
    std::vector<uint8_t> val(8);
    memcpy(val.data(), &d, 8);
    std::string decoded = FlashbackGenerator::decode_field_value(val, col);
    EXPECT_NE(decoded.find("3.14159"), std::string::npos);
}

TEST(FlashbackGeneratorTest, DecodeFieldValueDoubleEmpty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::DOUBLE;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueDate) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::DATE;

    // 2024-01-15: packed = (2024 << 9) | (1 << 5) | 15
    uint32_t packed = (2024 << 9) | (1 << 5) | 15;
    std::vector<uint8_t> val(3);
    val[0] = (packed >> 16) & 0xFF;
    val[1] = (packed >> 8) & 0xFF;
    val[2] = packed & 0xFF;
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "2024-01-15");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueDateEmpty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::DATE;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueTime) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::TIME;

    // 10:30:45 packed as HHMMSS decimal
    uint32_t packed = 103045;
    std::vector<uint8_t> val(3);
    val[0] = (packed >> 16) & 0xFF;
    val[1] = (packed >> 8) & 0xFF;
    val[2] = packed & 0xFF;
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "10:30:45");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueTimeEmpty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::TIME;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueTime2) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::TIME2;

    // 10:30:45: base = 103045
    uint32_t base = 103045;
    std::vector<uint8_t> val(3);
    val[0] = base & 0xFF;
    val[1] = (base >> 8) & 0xFF;
    val[2] = (base >> 16) & 0xFF;
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "10:30:45");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueTime2Empty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::TIME2;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueDateTime) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::DATETIME;

    // 2024-01-15 10:30:45 as big-endian uint64
    uint64_t packed = 20240115103045ULL;
    std::vector<uint8_t> val(8);
    for (int i = 7; i >= 0; i--) {
        val[i] = packed & 0xFF;
        packed >>= 8;
    }
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "2024-01-15 10:30:45");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueDateTimeEmpty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::DATETIME;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueDateTime2) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::DATETIME2;

    // 5 bytes big-endian = 300 → 00:03:00
    std::vector<uint8_t> val = {0x00, 0x00, 0x00, 0x01, 0x2C};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "0000-00-00 00:03:00");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueDateTime2Empty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::DATETIME2;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueTimestamp2) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::TIMESTAMP2;

    // 1705311045 = 2024-01-15 10:30:45 UTC (approximately)
    std::vector<uint8_t> val(4);
    uint32_t ts = 1705311045;
    val[0] = (ts >> 24) & 0xFF;
    val[1] = (ts >> 16) & 0xFF;
    val[2] = (ts >> 8) & 0xFF;
    val[3] = ts & 0xFF;
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "1705311045");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueTimestamp2Empty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::TIMESTAMP2;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueYear) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::YEAR;

    std::vector<uint8_t> val = {124}; // 1900 + 124 = 2024
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "2024");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueYearEmpty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::YEAR;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueEnum) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::ENUM;
    col.enum_values = {"red", "green", "blue"};

    // Index 1 = "red" (1-byte)
    std::vector<uint8_t> val1 = {1};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val1, col), "red");

    // Index 2 = "green" (2-byte)
    std::vector<uint8_t> val2 = {2, 0};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val2, col), "green");

    // Index 0 = empty
    std::vector<uint8_t> val0 = {0};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val0, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueEnumEmpty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::ENUM;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueSet) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::SET;
    col.set_values = {"a", "b", "c"};

    // Bit 0 = "a", Bit 1 = "b"
    std::vector<uint8_t> val = {0x03, 0x00};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "a,b");

    // Bit 0 only = "a"
    std::vector<uint8_t> val2 = {0x01, 0x00};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val2, col), "a");

    // Empty set
    std::vector<uint8_t> val3 = {0x00, 0x00};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val3, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueString) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::VARCHAR;

    std::vector<uint8_t> val = {'H', 'e', 'l', 'l', 'o'};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "Hello");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueStringWithNonPrintable) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::VARCHAR;

    // Non-printable bytes should produce hex output
    std::vector<uint8_t> val = {0x00, 0x01, 0x02};
    std::string decoded = FlashbackGenerator::decode_field_value(val, col);
    // Should be hex (non-printable)
    EXPECT_NE(decoded, std::string("\x00\x01\x02", 3));
}

TEST(FlashbackGeneratorTest, DecodeFieldValueBlob) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::BLOB;

    std::vector<uint8_t> val = {0xDE, 0xAD, 0xBE, 0xEF};
    std::string decoded = FlashbackGenerator::decode_field_value(val, col);
    EXPECT_EQ(decoded, "deadbeef");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueJson) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::JSON;

    std::vector<uint8_t> val = {'{', '"', 'k', 'e', 'y', '"', ':', ' ', '"', 'v', '"', '}'};
    std::string decoded = FlashbackGenerator::decode_field_value(val, col);
    EXPECT_NE(decoded.find("key"), std::string::npos);
    EXPECT_NE(decoded.find("v"), std::string::npos);
}

TEST(FlashbackGeneratorTest, DecodeFieldValueBit) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::BIT;

    std::vector<uint8_t> val = {0x01};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "1");

    std::vector<uint8_t> val2 = {0x0F};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val2, col), "15");

    std::vector<uint8_t> val3 = {0x01, 0x00};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val3, col), "256");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueBitEmpty) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::BIT;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueNewDecimal) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::NEWDECIMAL;

    std::vector<uint8_t> val = {0x01, 0x23};
    std::string decoded = FlashbackGenerator::decode_field_value(val, col);
    // NEWDECIMAL outputs hex since full decoding is complex
    EXPECT_EQ(decoded, "0123");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueUnknownType) {
    catalog::ColumnInfo col;
    col.mysql_type_id = 255; // Unknown type

    std::vector<uint8_t> val = {0xAB, 0xCD};
    std::string decoded = FlashbackGenerator::decode_field_value(val, col);
    EXPECT_EQ(decoded, "abcd");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueRawBytesMode) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::LONG;

    std::vector<uint8_t> val = {0x2A, 0x00, 0x00, 0x00};
    // human_readable = false should return hex
    std::string decoded = FlashbackGenerator::decode_field_value(val, col, false);
    EXPECT_EQ(decoded, "2a000000");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueEmptyBytes) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::VARCHAR;
    EXPECT_EQ(FlashbackGenerator::decode_field_value({}, col), "");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueStringTypeVarString) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::VAR_STRING;

    std::vector<uint8_t> val = {'T', 'e', 's', 't'};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "Test");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueStringTypeString) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::STRING;

    std::vector<uint8_t> val = {'C', 'H', 'A', 'R'};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "CHAR");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueTinyBlob) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::TINY_BLOB;
    std::vector<uint8_t> val = {0x01, 0x02};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "0102");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueMediumBlob) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::MEDIUM_BLOB;
    std::vector<uint8_t> val = {0xAB};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "ab");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueLongBlob) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::LONG_BLOB;
    std::vector<uint8_t> val = {0xFF, 0x00};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "ff00");
}

// ==================== bytes_to_hex Tests ====================

TEST(FlashbackGeneratorTest, BytesToHex) {
    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(FlashbackGenerator::bytes_to_hex(data), "deadbeef");
}

TEST(FlashbackGeneratorTest, BytesToHexEmpty) {
    EXPECT_EQ(FlashbackGenerator::bytes_to_hex({}), "");
}

TEST(FlashbackGeneratorTest, BytesToHexSingleByte) {
    std::vector<uint8_t> data = {0x00};
    EXPECT_EQ(FlashbackGenerator::bytes_to_hex(data), "00");
}

TEST(FlashbackGeneratorTest, BytesToHexAllFF) {
    std::vector<uint8_t> data = {0xFF, 0xFF, 0xFF};
    EXPECT_EQ(FlashbackGenerator::bytes_to_hex(data), "ffffff");
}

TEST(FlashbackGeneratorTest, BytesToHexMixed) {
    std::vector<uint8_t> data = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    EXPECT_EQ(FlashbackGenerator::bytes_to_hex(data), "0123456789abcdef");
}

// ==================== field_no_to_ordinal Tests ====================

TEST(FlashbackGeneratorTest, FieldNoToOrdinalDelete) {
    // field_no=1 (DB_ROLL_PTR) should be skipped
    auto result = FlashbackGenerator::field_no_to_ordinal(1, true);
    EXPECT_FALSE(result.has_value());

    // field_no=2 maps to ordinal 0 (first user column)
    result = FlashbackGenerator::field_no_to_ordinal(2, true);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u);

    // field_no=3 maps to ordinal 1
    result = FlashbackGenerator::field_no_to_ordinal(3, true);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u);

    // field_no=10 maps to ordinal 8
    result = FlashbackGenerator::field_no_to_ordinal(10, true);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 8u);
}

TEST(FlashbackGeneratorTest, FieldNoToOrdinalNotDelete) {
    // For non-delete, field_no=1 (DB_ROLL_PTR) should also be skipped
    auto result = FlashbackGenerator::field_no_to_ordinal(1, false);
    EXPECT_FALSE(result.has_value());

    // field_no=2 maps to ordinal 0
    result = FlashbackGenerator::field_no_to_ordinal(2, false);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u);

    // field_no=5 maps to ordinal 3
    result = FlashbackGenerator::field_no_to_ordinal(5, false);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 3u);
}

TEST(FlashbackGeneratorTest, FieldNoToOrdinalZero) {
    // field_no=0 should be skipped (no valid mapping)
    auto result = FlashbackGenerator::field_no_to_ordinal(0, true);
    EXPECT_FALSE(result.has_value());

    result = FlashbackGenerator::field_no_to_ordinal(0, false);
    EXPECT_FALSE(result.has_value());
}

// ==================== DecodedField Tests ====================

TEST(FlashbackGeneratorTest, DecodedFieldDefaultConstruction) {
    DecodedField df;

    EXPECT_TRUE(df.column_name.empty());
    EXPECT_TRUE(df.human_value.empty());
    EXPECT_TRUE(df.hex_value.empty());
    EXPECT_FALSE(df.is_null);
    EXPECT_FALSE(df.is_decoded);
    EXPECT_EQ(df.confidence, 1.0);
}

TEST(FlashbackGeneratorTest, DecodedFieldWithValues) {
    DecodedField df;
    df.column_name = "name";
    df.human_value = "Alice";
    df.hex_value = "416c696365";
    df.is_null = false;
    df.is_decoded = true;
    df.confidence = 0.95;

    EXPECT_EQ(df.column_name, "name");
    EXPECT_EQ(df.human_value, "Alice");
    EXPECT_EQ(df.hex_value, "416c696365");
    EXPECT_FALSE(df.is_null);
    EXPECT_TRUE(df.is_decoded);
    EXPECT_EQ(df.confidence, 0.95);
}

// ==================== generate_output Tests ====================

TEST(FlashbackGeneratorTest, GenerateOutputEmptyRecords) {
    FlashbackGenerator gen;
    auto schema = make_test_schema();

    // With empty records, generate_output now returns empty string
    // (fail-closed: no data to export)
    gen.config().output_format = FlashbackOutputFormat::SQL;
    std::string sql = gen.generate_output({}, schema);
    EXPECT_TRUE(sql.empty());

    gen.config().output_format = FlashbackOutputFormat::CSV;
    std::string csv = gen.generate_output({}, schema);
    EXPECT_TRUE(csv.empty());

    gen.config().output_format = FlashbackOutputFormat::JSON;
    std::string json = gen.generate_output({}, schema);
    EXPECT_TRUE(json.empty());
}

TEST(FlashbackGeneratorTest, GenerateOutputSQL) {
    FlashbackGenerator gen;
    gen.config().output_format = FlashbackOutputFormat::SQL;
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    DecodedField f;
    f.column_name = "id";
    f.human_value = "1";
    f.is_null = false;
    f.is_decoded = true;
    rec.fields.push_back(f);

    std::string output = gen.generate_output({rec}, schema);
    EXPECT_NE(output.find("INSERT INTO test_table"), std::string::npos);
}

TEST(FlashbackGeneratorTest, GenerateOutputCSV) {
    FlashbackGenerator gen;
    gen.config().output_format = FlashbackOutputFormat::CSV;
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 42;
    rec.undo_no = 7;
    rec.completeness = 1.0;
    rec.is_complete = true;
    rec.field_map["id"] = "1";
    rec.field_map["name"] = "Alice";
    rec.field_map["age"] = "30";

    std::string output = gen.generate_output({rec}, schema);
    EXPECT_NE(output.find("DELETE"), std::string::npos);
    EXPECT_NE(output.find("42"), std::string::npos);
    EXPECT_NE(output.find("7"), std::string::npos);
}

TEST(FlashbackGeneratorTest, GenerateOutputJSON) {
    FlashbackGenerator gen;
    gen.config().output_format = FlashbackOutputFormat::JSON;
    gen.config().pretty_json = false;
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 42;
    rec.undo_no = 7;
    rec.completeness = 1.0;
    rec.is_complete = true;

    DecodedField f;
    f.column_name = "id";
    f.human_value = "1";
    f.is_null = false;
    f.is_decoded = true;
    rec.fields.push_back(f);

    std::string output = gen.generate_output({rec}, schema);
    EXPECT_NE(output.find("DELETE"), std::string::npos);
    EXPECT_NE(output.find("42"), std::string::npos);
    EXPECT_NE(output.find("7"), std::string::npos);
}

// ==================== Edge Cases ====================

TEST(FlashbackGeneratorTest, EmptyFieldVectorToSql) {
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;

    std::string sql = rec.to_sql(schema);
    EXPECT_NE(sql.find("INSERT INTO test_table"), std::string::npos);
}

TEST(FlashbackGeneratorTest, EmptyFieldVectorToCsv) {
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 0;
    rec.undo_no = 0;
    rec.completeness = 0.0;
    rec.is_complete = false;

    std::string csv = rec.to_csv(schema);
    EXPECT_NE(csv.find("DELETE"), std::string::npos);
    EXPECT_NE(csv.find("0.0000"), std::string::npos);
}

TEST(FlashbackGeneratorTest, EmptyFieldVectorToJson) {
    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 0;
    rec.undo_no = 0;
    rec.completeness = 0.0;
    rec.is_complete = false;

    std::string json = rec.to_json(false);
    EXPECT_NE(json.find("\"operation\": \"DELETE\""), std::string::npos);
    EXPECT_NE(json.find("\"completeness\": 0.0000"), std::string::npos);
    EXPECT_NE(json.find("\"is_complete\": false"), std::string::npos);
}

TEST(FlashbackGeneratorTest, LargeTrxId) {
    FlashbackRecord rec;
    rec.trx_id = 0xFFFFFFFFFFFFFFFFULL;
    EXPECT_EQ(rec.trx_id, 0xFFFFFFFFFFFFFFFFULL);
}

TEST(FlashbackGeneratorTest, MaxUndoNo) {
    FlashbackRecord rec;
    rec.undo_no = 0xFFFFFFFFFFFFFFFFULL;
    EXPECT_EQ(rec.undo_no, 0xFFFFFFFFFFFFFFFFULL);
}

TEST(FlashbackGeneratorTest, DecodeFieldValueShortTruncated) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::SHORT;
    // Only 1 byte when 2 are needed
    std::vector<uint8_t> val = {0x01};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "?");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueLongTruncated) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::LONG;
    std::vector<uint8_t> val = {0x01, 0x02};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "?");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueFloatTruncated) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::FLOAT;
    std::vector<uint8_t> val = {0x01, 0x02};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "?");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueDoubleTruncated) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::DOUBLE;
    std::vector<uint8_t> val = {0x01, 0x02, 0x03};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "?");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueEnumOutOfRange) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::ENUM;
    col.enum_values = {"red", "green"};

    // Index 5 is out of range for 2 values, should return the index
    std::vector<uint8_t> val = {5};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "5");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueEnumTwoByteIndex) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::ENUM;
    col.enum_values = {"a", "b", "c", "d"};

    // 2-byte index = 3 -> "c"
    std::vector<uint8_t> val = {3, 0};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "c");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueSetMultipleBytes) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::SET;
    col.set_values = {"a", "b", "c", "d", "e", "f", "g", "h", "i"};

    // 2 bytes: bit 0 = "a", bit 8 = "i"
    std::vector<uint8_t> val = {0x01, 0x01};
    std::string decoded = FlashbackGenerator::decode_field_value(val, col);
    EXPECT_EQ(decoded, "a,i");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueStringTabNewline) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::VARCHAR;

    // Tab and newline are considered printable
    std::vector<uint8_t> val = {'H', 'e', 'l', 'l', 'o', '\t', 'W', 'o', 'r', 'l', 'd', '\n'};
    std::string decoded = FlashbackGenerator::decode_field_value(val, col);
    EXPECT_EQ(decoded, "Hello\tWorld\n");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueJsonPrintable) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::JSON;

    // JSON with tab and newline should be printable
    std::vector<uint8_t> val = {'{', '\n', ' ', ' ', '"', 'a', '"', ':', ' ', '1', '\n', '}'};
    std::string decoded = FlashbackGenerator::decode_field_value(val, col);
    EXPECT_NE(decoded.find('{'), std::string::npos);
    EXPECT_NE(decoded.find('"'), std::string::npos);
}

TEST(FlashbackGeneratorTest, DecodeFieldValueJsonNonPrintable) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::JSON;

    // Non-printable JSON should output hex
    std::vector<uint8_t> val = {0x00, 0x01, 0x02};
    std::string decoded = FlashbackGenerator::decode_field_value(val, col);
    EXPECT_EQ(decoded, "000102");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueTime2Negative) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::TIME2;

    // Negative time: -10:30:45
    // Positive base = 103045 = 0x00019285
    // Negative: (~0x019285 & 0x7FFFFF) + 1 = 0x7FE6D7A + 1 = 0x7FE6D7B
    // Then set bit 23: 0x7FE6D7B | 0x800000 = 0xFFE6D7B
    uint32_t neg_base = 0x0FFE6D7B;  // Actually let me think more carefully.
    // Positive 103045 in 24-bit = 0x019285
    // Negative: (~0x019285) & 0x7FFFFF = 0x7E6D7A
    // + 1 = 0x7E6D7B
    // Set sign bit (bit 23): 0x7E6D7B | 0x800000 = 0xFE6D7B
    neg_base = 0x00FE6D7B;
    std::vector<uint8_t> val(3);
    val[0] = neg_base & 0xFF;
    val[1] = (neg_base >> 8) & 0xFF;
    val[2] = (neg_base >> 16) & 0xFF;
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "-10:30:45");
}

TEST(FlashbackGeneratorTest, DecodeFieldValueStringBackslash) {
    catalog::ColumnInfo col;
    col.mysql_type_id = mysql_types::VARCHAR;

    // Backslash in string should be preserved
    std::vector<uint8_t> val = {'p', 'a', 't', 'h', '\\', 'f', 'i', 'l', 'e'};
    EXPECT_EQ(FlashbackGenerator::decode_field_value(val, col), "path\\file");
}

// ==================== Stats Tests ====================

TEST(FlashbackGeneratorTest, StatsAfterReset) {
    FlashbackGenerator gen;
    gen.reset_stats();
    const auto& stats = gen.get_stats();
    EXPECT_EQ(stats.total_records, 0u);
    EXPECT_EQ(stats.complete_records, 0u);
    EXPECT_EQ(stats.partial_records, 0u);
    EXPECT_EQ(stats.avg_completeness, 0.0);
    EXPECT_EQ(stats.execution_time_ms, 0.0);
}

TEST(FlashbackGeneratorTest, StatsDefaultValues) {
    FlashbackGenerator gen;
    const auto& stats = gen.get_stats();
    EXPECT_EQ(stats.total_records, 0u);
    EXPECT_EQ(stats.complete_records, 0u);
    EXPECT_EQ(stats.partial_records, 0u);
    EXPECT_EQ(stats.avg_completeness, 0.0);
    EXPECT_EQ(stats.execution_time_ms, 0.0);
}

// ==================== FlashbackConfig Filter Combination Tests ====================

TEST(FlashbackGeneratorTest, ConfigAllFiltersSet) {
    FlashbackConfig config;
    auto now = std::chrono::system_clock::now();
    config.start_time = now;
    config.end_time = now + std::chrono::hours(2);
    config.start_lsn = 5000;
    config.end_lsn = 10000;
    config.trx_id = 12345;
    config.max_records = 100;
    config.include_incomplete = true;

    ASSERT_TRUE(config.start_time.has_value());
    ASSERT_TRUE(config.end_time.has_value());
    ASSERT_TRUE(config.start_lsn.has_value());
    ASSERT_TRUE(config.end_lsn.has_value());
    ASSERT_TRUE(config.trx_id.has_value());

    EXPECT_EQ(*config.start_time, now);
    EXPECT_EQ(*config.end_time, now + std::chrono::hours(2));
    EXPECT_EQ(*config.start_lsn, 5000u);
    EXPECT_EQ(*config.end_lsn, 10000u);
    EXPECT_EQ(*config.trx_id, 12345u);
    EXPECT_EQ(config.max_records, 100u);
    EXPECT_TRUE(config.include_incomplete);
}

TEST(FlashbackGeneratorTest, ConfigEndTimeOnly) {
    FlashbackConfig config;
    auto now = std::chrono::system_clock::now();
    config.end_time = now;

    EXPECT_FALSE(config.start_time.has_value());
    ASSERT_TRUE(config.end_time.has_value());
    EXPECT_EQ(*config.end_time, now);
}

TEST(FlashbackGeneratorTest, ConfigStartLSNOnly) {
    FlashbackConfig config;
    config.start_lsn = 1000;

    ASSERT_TRUE(config.start_lsn.has_value());
    EXPECT_FALSE(config.end_lsn.has_value());
    EXPECT_EQ(*config.start_lsn, 1000u);
}

// ==================== FlashbackMode/OutputFormat Coverage Tests ====================

TEST(FlashbackGeneratorTest, FlashbackModeAllValues) {
    // Verify all enum values are distinct
    std::set<int> seen;
    seen.insert(static_cast<int>(FlashbackMode::DELETE));
    seen.insert(static_cast<int>(FlashbackMode::UPDATE));
    seen.insert(static_cast<int>(FlashbackMode::POINT_IN_TIME));
    seen.insert(static_cast<int>(FlashbackMode::TRANSACTION));
    seen.insert(static_cast<int>(FlashbackMode::FULL_CHAIN));
    EXPECT_EQ(seen.size(), 5u);
}

TEST(FlashbackGeneratorTest, FlashbackOutputFormatAllValues) {
    std::set<int> seen;
    seen.insert(static_cast<int>(FlashbackOutputFormat::SQL));
    seen.insert(static_cast<int>(FlashbackOutputFormat::CSV));
    seen.insert(static_cast<int>(FlashbackOutputFormat::JSON));
    EXPECT_EQ(seen.size(), 3u);
}

// ==================== FlashbackRecord Ops Coverage Tests ====================

TEST(FlashbackGeneratorTest, FlashbackRecordAllOps) {
    FlashbackRecord rec;

    rec.operation = FlashbackRecord::INSERT;
    EXPECT_EQ(rec.operation, FlashbackRecord::INSERT);

    rec.operation = FlashbackRecord::UPDATE;
    EXPECT_EQ(rec.operation, FlashbackRecord::UPDATE);

    rec.operation = FlashbackRecord::DELETE_OP;
    EXPECT_EQ(rec.operation, FlashbackRecord::DELETE_OP);

    // Verify all three are distinct
    EXPECT_NE(FlashbackRecord::INSERT, FlashbackRecord::UPDATE);
    EXPECT_NE(FlashbackRecord::INSERT, FlashbackRecord::DELETE_OP);
    EXPECT_NE(FlashbackRecord::UPDATE, FlashbackRecord::DELETE_OP);
}

// ==================== FlashbackRecord Output Edge Cases ====================

TEST(FlashbackGeneratorTest, ToSqlSpecialChars) {
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;

    DecodedField f1;
    f1.column_name = "id";
    f1.human_value = "1";
    f1.is_null = false;
    f1.is_decoded = true;

    DecodedField f2;
    f2.column_name = "name";
    f2.human_value = "test\"quote";
    f2.is_null = false;
    f2.is_decoded = true;

    rec.fields.push_back(f1);
    rec.fields.push_back(f2);

    std::string sql = rec.to_sql(schema);
    // SQL escaping: double quote is not escaped in SQL, but backslash is
    EXPECT_NE(sql.find("test\"quote"), std::string::npos);
}

TEST(FlashbackGeneratorTest, ToSqlBackslash) {
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;

    DecodedField f1;
    f1.column_name = "id";
    f1.human_value = "1";
    f1.is_null = false;
    f1.is_decoded = true;

    DecodedField f2;
    f2.column_name = "name";
    f2.human_value = "path\\to\\file";
    f2.is_null = false;
    f2.is_decoded = true;

    rec.fields.push_back(f1);
    rec.fields.push_back(f2);

    std::string sql = rec.to_sql(schema);
    // Backslash should be escaped as double backslash
    EXPECT_NE(sql.find("path\\\\to\\\\file"), std::string::npos);
}

// ==================== CSV Edge Cases ====================

TEST(FlashbackGeneratorTest, ToCsvWithDelimiterInValue) {
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 1;
    rec.undo_no = 0;
    rec.completeness = 1.0;
    rec.is_complete = true;
    rec.field_map["name"] = "Alice,Bob"; // comma in value

    std::string csv = rec.to_csv(schema, ',');
    // Value with comma should be quoted
    EXPECT_NE(csv.find("\"Alice,Bob\""), std::string::npos);
}

TEST(FlashbackGeneratorTest, ToCsvWithQuoteInValue) {
    auto schema = make_test_schema();

    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 1;
    rec.undo_no = 0;
    rec.completeness = 1.0;
    rec.is_complete = true;
    rec.field_map["name"] = "Alice\"Bob";

    std::string csv = rec.to_csv(schema, ',');
    EXPECT_NE(csv.find("Alice\"\"Bob"), std::string::npos);
}

// ==================== JSON Edge Cases ====================

TEST(FlashbackGeneratorTest, ToJsonSpecialChars) {
    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 1;
    rec.undo_no = 0;
    rec.completeness = 1.0;
    rec.is_complete = true;

    DecodedField f;
    f.column_name = "name";
    f.human_value = "line1\nline2";
    f.is_null = false;
    f.is_decoded = true;
    rec.fields.push_back(f);

    std::string json = rec.to_json(false);
    EXPECT_NE(json.find("line1\\nline2"), std::string::npos);
}

TEST(FlashbackGeneratorTest, ToJsonTabChar) {
    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 1;
    rec.undo_no = 0;
    rec.completeness = 1.0;
    rec.is_complete = true;

    DecodedField f;
    f.column_name = "name";
    f.human_value = "col1\tcol2";
    f.is_null = false;
    f.is_decoded = true;
    rec.fields.push_back(f);

    std::string json = rec.to_json(false);
    EXPECT_NE(json.find("col1\\tcol2"), std::string::npos);
}

TEST(FlashbackGeneratorTest, ToJsonCarriageReturn) {
    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 1;
    rec.undo_no = 0;
    rec.completeness = 1.0;
    rec.is_complete = true;

    DecodedField f;
    f.column_name = "name";
    f.human_value = "line1\r\nline2";
    f.is_null = false;
    f.is_decoded = true;
    rec.fields.push_back(f);

    std::string json = rec.to_json(false);
    EXPECT_NE(json.find("line1\\r\\nline2"), std::string::npos);
}

TEST(FlashbackGeneratorTest, ToJsonQuoteChar) {
    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 1;
    rec.undo_no = 0;
    rec.completeness = 1.0;
    rec.is_complete = true;

    DecodedField f;
    f.column_name = "name";
    f.human_value = "say \"hello\"";
    f.is_null = false;
    f.is_decoded = true;
    rec.fields.push_back(f);

    std::string json = rec.to_json(false);
    EXPECT_NE(json.find("say \\\"hello\\\""), std::string::npos);
}

TEST(FlashbackGeneratorTest, ToJsonBackslash) {
    FlashbackRecord rec;
    rec.operation = FlashbackRecord::DELETE_OP;
    rec.trx_id = 1;
    rec.undo_no = 0;
    rec.completeness = 1.0;
    rec.is_complete = true;

    DecodedField f;
    f.column_name = "name";
    f.human_value = "a\\b";
    f.is_null = false;
    f.is_decoded = true;
    rec.fields.push_back(f);

    std::string json = rec.to_json(false);
    EXPECT_NE(json.find("a\\\\b"), std::string::npos);
}

// ==================== Flashback Full Chain Safe Tests ====================
//
// These tests verify flashback_full_chain_safe() and the deprecated
// flashback_full_chain() API. They build minimal undo pages with version
// chains and check the integrity/exportable/diagnostic fields.

// Undo page header constants (from deep_undo_analyzer.cpp, not exposed in header)
namespace {
constexpr uint16_t TRX_UNDO_PAGE_START    = 28;
constexpr uint16_t TRX_UNDO_PAGE_FREE     = 30;
constexpr uint16_t TRX_UNDO_PAGE_N_RECS   = 32;
constexpr uint16_t TRX_UNDO_PAGE_HDR_SIZE = 40;

static std::string create_temp_file(const std::vector<uint8_t>& data,
                                     const std::string& suffix = "") {
    std::string path = "/tmp/test_flashback_chain" + suffix + ".bin";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    out.close();
    return path;
}

static std::vector<uint8_t> make_roll_ptr_field(uint32_t page_no, uint16_t offset) {
    std::vector<uint8_t> f(6, 0);
    write_le<uint32_t>(f.data(), 0, page_no);
    write_le<uint16_t>(f.data(), 4, offset);
    return f;
}

static std::vector<uint8_t> make_field(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    if (data.size() < 128) {
        out.push_back(static_cast<uint8_t>(data.size()));
    } else {
        out.push_back(static_cast<uint8_t>(0x80 | (data.size() >> 8)));
        out.push_back(static_cast<uint8_t>(data.size() & 0xFF));
    }
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

static std::vector<uint8_t> make_update_rec(
    uint32_t page_no, uint16_t page_offset, uint64_t trx_id,
    uint64_t prev_roll_ptr, const std::vector<std::vector<uint8_t>>& fields,
    UndoFormatProfile profile = UndoFormatProfile::MYSQL8_7BYTE) {
    const size_t rp_len = (profile == UndoFormatProfile::MYSQL8_7BYTE) ? 7 : 6;
    std::vector<uint8_t> rec;
    rec.push_back(2);   // TRX_UNDO_UPDATE
    rec.push_back(static_cast<uint8_t>(fields.size()));

    for (size_t i = 0; i < 4; i++)
        rec.push_back(static_cast<uint8_t>((page_no >> (i * 8)) & 0xFF));
    for (size_t i = 0; i < 2; i++)
        rec.push_back(static_cast<uint8_t>((page_offset >> (i * 8)) & 0xFF));
    rec.push_back(0);  // info_bits
    for (size_t i = 0; i < 5; i++)
        rec.push_back(static_cast<uint8_t>((trx_id >> (i * 8)) & 0xFF));

    auto decoded = RollPtrDecoder::decode(prev_roll_ptr);
    uint8_t storage[7] = {0};
    RollPtrDecoder::encode_storage(storage, decoded);
    rec.insert(rec.end(), storage + (7 - rp_len), storage + 7);

    for (const auto& f : fields) {
        auto fld = make_field(f);
        rec.insert(rec.end(), fld.begin(), fld.end());
    }
    return rec;
}

static std::vector<uint8_t> make_undo_page(
    const std::vector<std::vector<uint8_t>>& record_datas,
    size_t page_size = 16384,
    uint32_t space_id = 0) {
    std::vector<uint8_t> page(page_size, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_UNDO_LOG);

    uint16_t start = TRX_UNDO_PAGE_HDR_SIZE;
    uint16_t free_off = start;
    for (const auto& rd : record_datas) {
        if (free_off + rd.size() > page_size) break;
        std::memcpy(page.data() + free_off, rd.data(), rd.size());
        free_off += static_cast<uint16_t>(rd.size());
    }
    write_le<uint16_t>(page.data(), TRX_UNDO_PAGE_START, start);
    write_le<uint16_t>(page.data(), TRX_UNDO_PAGE_FREE, free_off);
    write_le<uint16_t>(page.data(), TRX_UNDO_PAGE_N_RECS,
                       static_cast<uint16_t>(record_datas.size()));
    write_be<uint32_t>(page.data(), 34, space_id);
    return page;
}
} // anonymous namespace

TEST(FlashbackGeneratorTest, FlashbackFullChainSafeComplete) {
    // Build a valid 2-record chain on page 5, trace it, assert exportable.
    // Oldest record first (null roll_ptr), then newer record pointing to oldest.
    const uint32_t page_no = 5;
    const uint16_t old_off = TRX_UNDO_PAGE_HDR_SIZE;
    auto rec_old = make_update_rec(100, 200, 100, 0,
        {make_roll_ptr_field(0, 0), {0x0A, 0, 0, 0}});
    uint16_t new_off = old_off + static_cast<uint16_t>(rec_old.size());
    auto rec_new = make_update_rec(300, 400, 101,
        RollPtrDecoder::encode(page_no, old_off, 0),
        {make_roll_ptr_field(page_no, old_off), {0x14, 0, 0, 0}});

    auto page = make_undo_page({rec_old, rec_new});
    std::vector<uint8_t> file(16384 * 6, 0);
    std::memcpy(file.data() + page_no * 16384, page.data(), page.size());
    std::string path = create_temp_file(file, "_safe_complete");

    FlashbackGenerator gen;
    gen.set_ibdata_path(path);
    gen.set_page_size(16384);
    gen.set_undo_format_profile(UndoFormatProfile::MYSQL8_7BYTE);

    auto schema = make_test_schema();
    uint64_t roll_ptr = RollPtrDecoder::encode(page_no, new_off, 0);
    auto result = gen.flashback_full_chain_safe({}, roll_ptr, schema, 42);

    EXPECT_TRUE(result.exportable);
    EXPECT_EQ(result.integrity, HistoryIntegrity::COMPLETE);
    EXPECT_TRUE(result.diagnostic.empty());
    EXPECT_TRUE(result);
    EXPECT_FALSE(result.records.empty());

    std::remove(path.c_str());
}

TEST(FlashbackGeneratorTest, FlashbackFullChainSafeBrokenPage) {
    // Roll_ptr to page 0 (zero-filled, not an undo page) -> BROKEN_PAGE
    std::vector<uint8_t> file(16384, 0);
    std::string path = create_temp_file(file, "_safe_broken");

    FlashbackGenerator gen;
    gen.set_ibdata_path(path);
    gen.set_page_size(16384);

    auto schema = make_test_schema();
    uint64_t roll_ptr = RollPtrDecoder::encode(0, 40, 0);
    auto result = gen.flashback_full_chain_safe({}, roll_ptr, schema, 42);

    EXPECT_FALSE(result.exportable);
    EXPECT_EQ(result.integrity, HistoryIntegrity::BROKEN_PAGE);
    EXPECT_FALSE(result.diagnostic.empty());
    EXPECT_FALSE(result);
    EXPECT_TRUE(result.records.empty());

    std::remove(path.c_str());
}

TEST(FlashbackGeneratorTest, FlashbackFullChainSafeCycle) {
    // Self-referential roll_ptr -> CYCLE
    const uint32_t page_no = 5;
    const uint16_t rec_off = TRX_UNDO_PAGE_HDR_SIZE;
    auto rec = make_update_rec(100, 200, 101, 0,
        {make_roll_ptr_field(page_no, rec_off), {0x0A, 0, 0, 0}});
    // Patch the header to point to self
    {
        auto decoded = RollPtrDecoder::decode(RollPtrDecoder::encode(page_no, rec_off, 0));
        uint8_t storage[7] = {0};
        RollPtrDecoder::encode_storage(storage, decoded);
        // Header is at offset+14 in the record
        auto page = make_undo_page({rec});
        std::memcpy(page.data() + rec_off + 14, storage, 7);
        uint16_t free_off = rec_off + static_cast<uint16_t>(rec.size());
        write_le<uint16_t>(page.data(), TRX_UNDO_PAGE_FREE, free_off);
        write_le<uint16_t>(page.data(), TRX_UNDO_PAGE_N_RECS, 1);

        std::vector<uint8_t> file(16384 * 6, 0);
        std::memcpy(file.data() + page_no * 16384, page.data(), page.size());
        std::string path = create_temp_file(file, "_safe_cycle");

        FlashbackGenerator gen;
        gen.set_ibdata_path(path);
        gen.set_page_size(16384);
        gen.set_undo_format_profile(UndoFormatProfile::MYSQL8_7BYTE);

        auto schema = make_test_schema();
        uint64_t roll_ptr = RollPtrDecoder::encode(page_no, rec_off, 0);
        auto result = gen.flashback_full_chain_safe({}, roll_ptr, schema, 42);

        EXPECT_FALSE(result.exportable);
        EXPECT_EQ(result.integrity, HistoryIntegrity::CYCLE);
        EXPECT_FALSE(result.diagnostic.empty());
        EXPECT_FALSE(result);
        EXPECT_TRUE(result.records.empty());

        std::remove(path.c_str());
    }
}

TEST(FlashbackGeneratorTest, FlashbackFullChainDeprecatedFailClosed) {
    // Deprecated flashback_full_chain() must return empty records when
    // the chain is not safely exportable.
    std::vector<uint8_t> file(16384, 0);
    std::string path = create_temp_file(file, "_deprecated_fail");

    FlashbackGenerator gen;
    gen.set_ibdata_path(path);
    gen.set_page_size(16384);

    auto schema = make_test_schema();
    uint64_t roll_ptr = RollPtrDecoder::encode(0, 40, 0);
    auto records = gen.flashback_full_chain({}, roll_ptr, schema, 42);

    EXPECT_TRUE(records.empty());

    std::remove(path.c_str());
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}