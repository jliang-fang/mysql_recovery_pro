#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "dbrecovery/catalog/schema_type_conversion.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace {

using dbrecovery::catalog::ColumnInfo;
using dbrecovery::catalog::IndexColumnBuildRequest;
using dbrecovery::catalog::TypeIdDomain;
using dbrecovery::catalog::build_index_column;
using dbrecovery::catalog::build_index_columns;
using dbrecovery::catalog::convert_column_type_id;
using dbrecovery::catalog::validate_parser_type_id;
using dbrecovery::catalog::TypeConversionResult;

// ---------------------------------------------------------------------------
// MySQL-server-domain mapping coverage
// ---------------------------------------------------------------------------

TEST(SchemaTypeConversion, MysqlServerDomainKnownIds) {
    struct Case { uint32_t server_id; uint8_t expected_parser; };
    const std::vector<Case> cases = {
        {0,   dbrecovery::innodb::mysql_types::DECIMAL},
        {1,   dbrecovery::innodb::mysql_types::TINY},
        {2,   dbrecovery::innodb::mysql_types::SHORT},
        {3,   dbrecovery::innodb::mysql_types::LONG},
        {4,   dbrecovery::innodb::mysql_types::FLOAT},
        {5,   dbrecovery::innodb::mysql_types::DOUBLE},
        {7,   dbrecovery::innodb::mysql_types::TIMESTAMP2},
        {8,   dbrecovery::innodb::mysql_types::LONGLONG},
        {9,   dbrecovery::innodb::mysql_types::INT24},
        {10,  dbrecovery::innodb::mysql_types::NEWDATE},
        {11,  dbrecovery::innodb::mysql_types::TIME2},
        {12,  dbrecovery::innodb::mysql_types::DATETIME2},
        {13,  dbrecovery::innodb::mysql_types::YEAR},
        {14,  dbrecovery::innodb::mysql_types::NEWDATE},
        {15,  dbrecovery::innodb::mysql_types::VARCHAR},
        {16,  dbrecovery::innodb::mysql_types::BIT},
        {17,  dbrecovery::innodb::mysql_types::TIMESTAMP2},
        {18,  dbrecovery::innodb::mysql_types::DATETIME2},
        {19,  dbrecovery::innodb::mysql_types::TIME2},
        {245, dbrecovery::innodb::mysql_types::JSON},
        {246, dbrecovery::innodb::mysql_types::NEWDECIMAL},
        {247, dbrecovery::innodb::mysql_types::ENUM},
        {248, dbrecovery::innodb::mysql_types::SET},
        {249, dbrecovery::innodb::mysql_types::TINY_BLOB},
        {250, dbrecovery::innodb::mysql_types::MEDIUM_BLOB},
        {251, dbrecovery::innodb::mysql_types::LONG_BLOB},
        {252, dbrecovery::innodb::mysql_types::BLOB},
        {253, dbrecovery::innodb::mysql_types::VARCHAR},
        {254, dbrecovery::innodb::mysql_types::STRING},
        {255, dbrecovery::innodb::mysql_types::GEOMETRY},
    };

    for (const auto& c : cases) {
        const auto result = convert_column_type_id(c.server_id, TypeIdDomain::kMysqlServer);
        ASSERT_TRUE(result.ok) << "server id " << c.server_id
                               << " rejected: " << result.diagnostic;
        EXPECT_EQ(result.parser_type, c.expected_parser)
            << "server id " << c.server_id << " mapped to wrong parser id";
    }
}

TEST(SchemaTypeConversion, MysqlServerDomainRejectsNullAndUnknown) {
    for (uint32_t id : {6u, 20u, 200u, 244u}) {
        const auto result = convert_column_type_id(id, TypeIdDomain::kMysqlServer);
        EXPECT_FALSE(result.ok) << "server id " << id << " should be rejected";
        EXPECT_FALSE(result.diagnostic.empty());
    }
}

// ---------------------------------------------------------------------------
// Parser-domain validation coverage
// ---------------------------------------------------------------------------

TEST(SchemaTypeConversion, ParserDomainAcceptsKnownIds) {
    for (uint32_t id = 1; id <= 32; ++id) {
        // Only IDs that are actually valid column types are accepted.
        // TYPE_NULL (7) is reserved and not a valid column type.
        const auto result = validate_parser_type_id(id);
        if (id == 7) {
            EXPECT_FALSE(result.ok) << "parser id " << id << " unexpectedly accepted";
        } else {
            EXPECT_TRUE(result.ok) << "parser id " << id << " rejected: " << result.diagnostic;
            EXPECT_EQ(result.parser_type, id);
        }
    }
}

TEST(SchemaTypeConversion, ParserDomainRejectsOutOfRange) {
    for (uint32_t id : {0u, 33u, 255u, 256u, 1000u}) {
        const auto result = validate_parser_type_id(id);
        EXPECT_FALSE(result.ok) << "parser id " << id << " should be rejected";
        EXPECT_FALSE(result.diagnostic.empty());
    }
}

// ---------------------------------------------------------------------------
// build_index_column integration
// ---------------------------------------------------------------------------

ColumnInfo make_column(uint32_t parser_type, const std::string& name = "c") {
    ColumnInfo col;
    col.name = name;
    col.mysql_type_id = parser_type;
    col.column_length = 11;
    col.charset_id = 33;
    col.is_nullable = true;
    col.is_signed = true;
    return col;
}

TEST(BuildIndexColumn, PopulatesAllFieldsOnSuccess) {
    auto col = make_column(dbrecovery::innodb::mysql_types::VARCHAR, "username");
    col.column_length = 100;
    col.charset_id = 45;               // utf8mb4
    col.is_nullable = false;
    col.is_signed = false;             // unsigned
    col.numeric_precision = 10;
    col.numeric_scale = 2;
    col.datetime_precision = 3;
    col.enum_values = {"a", "b"};
    col.ordinal = 5;
    col.physical_pos = 7;

    std::vector<std::string> diagnostics;
    auto converted = build_index_column(col, 5, 0,
                                        TypeIdDomain::kInnodbParser,
                                        diagnostics);
    ASSERT_TRUE(converted.has_value());
    EXPECT_TRUE(diagnostics.empty());
    EXPECT_EQ(converted->name, "username");
    EXPECT_EQ(converted->col_type, dbrecovery::innodb::mysql_types::VARCHAR);
    EXPECT_EQ(converted->col_length, 100u);
    EXPECT_FALSE(converted->is_nullable);
    EXPECT_TRUE(converted->is_unsigned);
    EXPECT_EQ(converted->char_set_id, 45u);
    EXPECT_EQ(converted->precision, 10u);
    EXPECT_EQ(converted->scale, 2u);
    EXPECT_EQ(converted->datetime_precision, 3u);
    EXPECT_EQ(converted->mb_maxlen, 4u);
    EXPECT_EQ(converted->enum_values.size(), 2u);
    EXPECT_EQ(converted->ordinal_position, 5u);
    EXPECT_EQ(converted->physical_pos, 7u);
}

TEST(BuildIndexColumn, RejectsInvalidTypeAndSkipsColumn) {
    auto col = make_column(0xFF, "bad_col");
    std::vector<std::string> diagnostics;
    auto converted = build_index_column(col, 1, 0,
                                        TypeIdDomain::kInnodbParser,
                                        diagnostics);
    EXPECT_FALSE(converted.has_value());
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics[0].find("bad_col"), std::string::npos);
}

TEST(BuildIndexColumn, RejectsOversizedTypeAndSkipsColumn) {
    auto col = make_column(300, "oversized");
    std::vector<std::string> diagnostics;
    auto converted = build_index_column(col, 1, 0,
                                        TypeIdDomain::kInnodbParser,
                                        diagnostics);
    EXPECT_FALSE(converted.has_value());
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics[0].find("oversized"), std::string::npos);
}

TEST(BuildIndexColumn, RejectsUnmappedParserId) {
    auto col = make_column(7, "type_null");  // TYPE_NULL not a column type
    std::vector<std::string> diagnostics;
    auto converted = build_index_column(col, 1, 0,
                                        TypeIdDomain::kInnodbParser,
                                        diagnostics);
    EXPECT_FALSE(converted.has_value());
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics[0].find("type_null"), std::string::npos);
}

TEST(BuildIndexColumn, PhysicalPosOverrideApplied) {
    auto col = make_column(dbrecovery::innodb::mysql_types::LONG);
    std::vector<std::string> diagnostics;
    auto converted = build_index_column(col, 1, 42,
                                        TypeIdDomain::kInnodbParser,
                                        diagnostics);
    ASSERT_TRUE(converted.has_value());
    EXPECT_EQ(converted->physical_pos, 42u);
}

TEST(BuildIndexColumn, BinaryCharsetResetsMbMaxlen) {
    auto col = make_column(dbrecovery::innodb::mysql_types::STRING);
    col.charset_id = 63;               // binary
    std::vector<std::string> diagnostics;
    auto converted = build_index_column(col, 1, 0,
                                        TypeIdDomain::kInnodbParser,
                                        diagnostics);
    ASSERT_TRUE(converted.has_value());
    EXPECT_TRUE(converted->is_binary);
    EXPECT_EQ(converted->mb_maxlen, 1u);
}

TEST(BuildIndexColumn, BlobFamilyIsAlwaysBigForTwoByteLengthPrefix) {
    // InnoDB switches a BLOB/TEXT/JSON column to the 2-byte length prefix as
    // soon as the value exceeds 127 bytes, even though TINYBLOB's own byte
    // maximum (255) would fit in one byte.  Deriving is_big from the byte
    // length alone made a 255-byte TINYBLOB decode as exactly 128 bytes and
    // desynchronised every column after it, which is what fabricated the
    // bogus 20-byte "off-page pointers" in the t_text artifact.
    auto tinyblob = make_column(dbrecovery::innodb::mysql_types::TINY_BLOB, "c_tinyblob");
    tinyblob.column_length = 255;
    tinyblob.charset_id = 63;          // binary
    std::vector<std::string> diagnostics;
    auto converted = build_index_column(tinyblob, 1, 0,
                                        TypeIdDomain::kInnodbParser, diagnostics);
    ASSERT_TRUE(converted.has_value());
    EXPECT_TRUE(converted->is_big) << "TINYBLOB must use the 2-byte length prefix";

    // A non-BLOB type with the same 255-byte maximum keeps the 1-byte prefix.
    auto varbinary = make_column(dbrecovery::innodb::mysql_types::VARCHAR, "c_varbinary");
    varbinary.column_length = 255;
    varbinary.charset_id = 63;
    converted = build_index_column(varbinary, 2, 0,
                                   TypeIdDomain::kInnodbParser, diagnostics);
    ASSERT_TRUE(converted.has_value());
    EXPECT_FALSE(converted->is_big) << "VARBINARY(255) is not part of the BLOB family";

    // A VARCHAR whose byte maximum exceeds 255 stays big (64 * utf8mb4 = 256).
    auto wide = make_column(dbrecovery::innodb::mysql_types::VARCHAR, "c_varchar");
    wide.column_length = 64;
    wide.charset_id = 45;
    converted = build_index_column(wide, 3, 0,
                                   TypeIdDomain::kInnodbParser, diagnostics);
    ASSERT_TRUE(converted.has_value());
    EXPECT_TRUE(converted->is_big);

    // TEXT and JSON are BLOB-family too, at any declared byte length.
    for (uint32_t type : {dbrecovery::innodb::mysql_types::BLOB,
                          dbrecovery::innodb::mysql_types::JSON}) {
        auto col = make_column(type, "family");
        col.column_length = 11;
        converted = build_index_column(col, 4, 0,
                                       TypeIdDomain::kInnodbParser, diagnostics);
        ASSERT_TRUE(converted.has_value());
        EXPECT_TRUE(converted->is_big) << "parser type " << type << " must be big";
    }
}

TEST(BuildIndexColumns, PublishesNothingWhenAnyColumnIsInvalid) {
    std::vector<IndexColumnBuildRequest> requests;
    requests.push_back({
        make_column(dbrecovery::innodb::mysql_types::LONG, "valid"), 1, 1});
    requests.push_back({make_column(0xFF, "invalid"), 2, 2});

    const auto result = build_index_columns(
        requests, TypeIdDomain::kInnodbParser);

    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.columns.empty());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_NE(result.diagnostics.front().find("invalid"), std::string::npos);
}

TEST(BuildIndexColumns, AggregatesAllConversionDiagnostics) {
    std::vector<IndexColumnBuildRequest> requests;
    requests.push_back({make_column(7, "null_type"), 1, 1});
    requests.push_back({make_column(0xFF, "unknown_type"), 2, 2});

    const auto result = build_index_columns(
        requests, TypeIdDomain::kInnodbParser);

    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.columns.empty());
    ASSERT_EQ(result.diagnostics.size(), 2u);
    EXPECT_NE(result.diagnostics[0].find("null_type"), std::string::npos);
    EXPECT_NE(result.diagnostics[1].find("unknown_type"), std::string::npos);
}

TEST(BuildIndexColumns, ConvertsServerDomainJsonAtomically) {
    std::vector<IndexColumnBuildRequest> requests;
    requests.push_back({make_column(245, "json"), 1, 1});

    const auto result = build_index_columns(
        requests, TypeIdDomain::kMysqlServer);

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.columns.size(), 1u);
    EXPECT_EQ(result.columns.front().col_type,
              dbrecovery::innodb::mysql_types::JSON);
}

// ---------------------------------------------------------------------------
// JSON type mapping regression
// ---------------------------------------------------------------------------

TEST(SchemaTypeConversion, JsonServerIdMapsToParserJson) {
    const auto result = convert_column_type_id(245, TypeIdDomain::kMysqlServer);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.parser_type, dbrecovery::innodb::mysql_types::JSON);
}

TEST(SchemaTypeConversion, JsonParserIdRoundTrips) {
    const auto result = validate_parser_type_id(dbrecovery::innodb::mysql_types::JSON);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.parser_type, dbrecovery::innodb::mysql_types::JSON);
}

} // namespace
