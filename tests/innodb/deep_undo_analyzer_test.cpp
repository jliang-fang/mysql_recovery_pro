#include <gtest/gtest.h>
#include "dbrecovery/innodb/deep_undo_analyzer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstring>
#include <chrono>

// Undo page header constants (from deep_undo_analyzer.cpp, not exposed in header)
constexpr uint16_t TRX_UNDO_PAGE_START    = 28;
constexpr uint16_t TRX_UNDO_PAGE_FREE     = 30;
constexpr uint16_t TRX_UNDO_PAGE_N_RECS   = 34;
constexpr uint16_t TRX_UNDO_PAGE_HDR_SIZE = 40;

namespace dbrecovery {
namespace innodb {

// ==================== Helper Functions ====================

static std::string create_temp_file(const std::vector<uint8_t>& data,
                                     const std::string& suffix = "") {
    std::string path = "/tmp/test_deep_undo" + suffix + ".bin";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    out.close();
    return path;
}

// Create ibdata with pages of specified types at given page numbers
static std::string create_ibdata_with_pages(
    const std::vector<std::pair<uint32_t, uint16_t>>& pages,
    size_t page_size = 16384) {
    uint32_t max_page = 0;
    for (const auto& [pg, _] : pages)
        if (pg > max_page) max_page = pg;
    std::vector<uint8_t> file(page_size * (static_cast<size_t>(max_page) + 1), 0);
    for (const auto& [pg, type] : pages) {
        write_be<uint16_t>(file.data(), pg * page_size + 24, type);
    }
    return create_temp_file(file);
}

// Create a single undo page with given record data
// Layout: FIL header + undo page header + records
static std::vector<uint8_t> make_undo_page(
    const std::vector<std::vector<uint8_t>>& record_datas,
    size_t page_size = 16384) {
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
    return page;
}

// Build raw field data with length prefix
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

// Build INSERT undo record
static std::vector<uint8_t> make_insert_rec(const std::vector<std::vector<uint8_t>>& fields) {
    std::vector<uint8_t> rec;
    rec.push_back(1);  // TRX_UNDO_INSERT
    rec.push_back(static_cast<uint8_t>(fields.size()));
    for (const auto& f : fields) {
        auto fld = make_field(f);
        rec.insert(rec.end(), fld.begin(), fld.end());
    }
    return rec;
}

// Build UPDATE undo record (MySQL 8.0+ format with 7-byte roll_ptr)
static std::vector<uint8_t> make_update_rec(
    uint32_t page_no, uint16_t page_offset, uint64_t trx_id,
    uint64_t prev_roll_ptr, const std::vector<std::vector<uint8_t>>& fields) {
    std::vector<uint8_t> rec;
    rec.push_back(2);   // TRX_UNDO_UPDATE
    rec.push_back(static_cast<uint8_t>(fields.size()));

    // page_no (4 LE)
    for (size_t i = 0; i < 4; i++)
        rec.push_back(static_cast<uint8_t>((page_no >> (i * 8)) & 0xFF));
    // page_offset (2 LE)
    for (size_t i = 0; i < 2; i++)
        rec.push_back(static_cast<uint8_t>((page_offset >> (i * 8)) & 0xFF));
    // info_bits
    rec.push_back(0);
    // trx_id (5 LE)
    for (size_t i = 0; i < 5; i++)
        rec.push_back(static_cast<uint8_t>((trx_id >> (i * 8)) & 0xFF));
    // prev_roll_ptr (7 bytes BE)
    auto decoded = RollPtrDecoder::decode(prev_roll_ptr);
    uint8_t storage[7] = {0};
    RollPtrDecoder::encode_storage(storage, decoded);
    rec.insert(rec.end(), storage, storage + 7);

    for (const auto& f : fields) {
        auto fld = make_field(f);
        rec.insert(rec.end(), fld.begin(), fld.end());
    }
    return rec;
}

// Build DELETE_MARK undo record (TRX_UNDO_DEL_REC = 13)
static std::vector<uint8_t> make_delete_rec(
    uint32_t page_no, uint16_t page_offset, uint64_t trx_id,
    uint64_t prev_roll_ptr, const std::vector<std::vector<uint8_t>>& fields) {
    std::vector<uint8_t> rec;
    rec.push_back(13);  // TRX_UNDO_DEL_REC
    rec.push_back(static_cast<uint8_t>(fields.size()));
    for (size_t i = 0; i < 4; i++)
        rec.push_back(static_cast<uint8_t>((page_no >> (i * 8)) & 0xFF));
    for (size_t i = 0; i < 2; i++)
        rec.push_back(static_cast<uint8_t>((page_offset >> (i * 8)) & 0xFF));
    rec.push_back(0);
    for (size_t i = 0; i < 5; i++)
        rec.push_back(static_cast<uint8_t>((trx_id >> (i * 8)) & 0xFF));
    auto decoded = RollPtrDecoder::decode(prev_roll_ptr);
    uint8_t storage[7] = {0};
    RollPtrDecoder::encode_storage(storage, decoded);
    rec.insert(rec.end(), storage, storage + 7);
    for (const auto& f : fields) {
        auto fld = make_field(f);
        rec.insert(rec.end(), fld.begin(), fld.end());
    }
    return rec;
}

// ==================== UndoFieldValue Tests ====================

TEST(DeepUndoAnalyzerTest, UndoFieldValueDefault) {
    UndoFieldValue fv;
    EXPECT_EQ(fv.field_no, 0u);
    EXPECT_TRUE(fv.old_value.empty());
    EXPECT_FALSE(fv.is_null);
    EXPECT_FALSE(fv.is_extern);
}

TEST(DeepUndoAnalyzerTest, UndoFieldValueToHex) {
    UndoFieldValue fv;
    fv.field_no = 1;
    fv.old_value = {0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(fv.to_hex(), "deadbeef");
}

TEST(DeepUndoAnalyzerTest, UndoFieldValueToHexNull) {
    UndoFieldValue fv;
    fv.is_null = true;
    EXPECT_EQ(fv.to_hex(), "NULL");
}

TEST(DeepUndoAnalyzerTest, UndoFieldValueToHexEmpty) {
    UndoFieldValue fv;
    fv.old_value = {};
    EXPECT_EQ(fv.to_hex(), "");
}

TEST(DeepUndoAnalyzerTest, UndoFieldValueToHexSingleByte) {
    UndoFieldValue fv;
    fv.old_value = {0x0F};
    EXPECT_EQ(fv.to_hex(), "0f");
}

// ==================== UndoRecordV2 Tests ====================

TEST(DeepUndoAnalyzerTest, UndoRecordV2Default) {
    UndoRecordV2 rec;
    EXPECT_EQ(rec.undo_page_no, 0u);
    EXPECT_EQ(rec.undo_page_offset, 0u);
    EXPECT_EQ(rec.trx_id, 0u);
    EXPECT_EQ(rec.undo_no, 0u);
    EXPECT_EQ(rec.type, UndoRecordV2::UPDATE);
    EXPECT_EQ(rec.table_id, 0u);
    EXPECT_EQ(rec.index_id, 0u);
    EXPECT_EQ(rec.roll_ptr, 0u);
    EXPECT_EQ(rec.prev_undo_ptr, 0u);
    EXPECT_FALSE(rec.commit_lsn.has_value());
    EXPECT_FALSE(rec.approx_timestamp.has_value());
}

TEST(DeepUndoAnalyzerTest, UndoRecordV2TypeStr) {
    UndoRecordV2 rec;
    rec.type = UndoRecordV2::INSERT;
    EXPECT_EQ(rec.type_str(), "INSERT");
    rec.type = UndoRecordV2::UPDATE;
    EXPECT_EQ(rec.type_str(), "UPDATE");
    rec.type = UndoRecordV2::DELETE_MARK;
    EXPECT_EQ(rec.type_str(), "DELETE_MARK");
    rec.type = UndoRecordV2::DELETE;
    EXPECT_EQ(rec.type_str(), "DELETE");
    rec.type = static_cast<UndoRecordV2::Type>(99);
    EXPECT_EQ(rec.type_str(), "UNKNOWN");
}

TEST(DeepUndoAnalyzerTest, UndoRecordV2IsInsert) {
    UndoRecordV2 rec;
    rec.type = UndoRecordV2::INSERT;
    EXPECT_TRUE(rec.is_insert());
    EXPECT_FALSE(rec.is_delete());
    EXPECT_FALSE(rec.is_update());
}

TEST(DeepUndoAnalyzerTest, UndoRecordV2IsDelete) {
    UndoRecordV2 rec;
    rec.type = UndoRecordV2::DELETE_MARK;
    EXPECT_TRUE(rec.is_delete());
    EXPECT_FALSE(rec.is_insert());
    rec.type = UndoRecordV2::DELETE;
    EXPECT_TRUE(rec.is_delete());
}

TEST(DeepUndoAnalyzerTest, UndoRecordV2IsUpdate) {
    UndoRecordV2 rec;
    rec.type = UndoRecordV2::UPDATE;
    EXPECT_TRUE(rec.is_update());
    EXPECT_FALSE(rec.is_insert());
    EXPECT_FALSE(rec.is_delete());
}

TEST(DeepUndoAnalyzerTest, UndoRecordV2WithTimestamps) {
    UndoRecordV2 rec;
    rec.commit_lsn = 12345;
    rec.approx_timestamp = std::chrono::system_clock::time_point(
        std::chrono::nanoseconds(1000));
    ASSERT_TRUE(rec.commit_lsn.has_value());
    EXPECT_EQ(*rec.commit_lsn, 12345u);
    ASSERT_TRUE(rec.approx_timestamp.has_value());
}

TEST(DeepUndoAnalyzerTest, UndoRecordV2Fields) {
    UndoRecordV2 rec;
    rec.trx_id = 42;
    rec.undo_no = 7;
    rec.table_id = 100;
    rec.index_id = 200;
    rec.page_no = 300;
    rec.page_offset = 400;
    rec.roll_ptr = 0xAB12345678ULL;
    EXPECT_EQ(rec.trx_id, 42u);
    EXPECT_EQ(rec.undo_no, 7u);
    EXPECT_EQ(rec.table_id, 100u);
    EXPECT_EQ(rec.index_id, 200u);
    EXPECT_EQ(rec.page_no, 300u);
    EXPECT_EQ(rec.page_offset, 400u);
    EXPECT_EQ(rec.roll_ptr, 0xAB12345678ULL);
}

// ==================== RowVersionHistory Tests ====================

TEST(DeepUndoAnalyzerTest, RowVersionHistoryDefault) {
    RowVersionHistory hist;
    EXPECT_TRUE(hist.current_data.empty());
    EXPECT_FALSE(hist.current_is_deleted);
    EXPECT_TRUE(hist.undo_chain.empty());
    EXPECT_EQ(hist.index_id, 0u);
    EXPECT_TRUE(hist.primary_key.empty());
    EXPECT_EQ(hist.n_versions, 0u);
    EXPECT_FALSE(hist.truncated);
}

TEST(DeepUndoAnalyzerTest, RowVersionHistoryChainLength) {
    RowVersionHistory hist;
    EXPECT_EQ(hist.chain_length(), 0u);
    hist.undo_chain.push_back(UndoRecordV2{});
    EXPECT_EQ(hist.chain_length(), 1u);
    hist.undo_chain.push_back(UndoRecordV2{});
    EXPECT_EQ(hist.chain_length(), 2u);
}

TEST(DeepUndoAnalyzerTest, RowVersionHistoryGetVersionAtEmptyChain) {
    RowVersionHistory hist;
    hist.current_data = {1, 2, 3};
    hist.current_is_deleted = false;
    auto ver = hist.get_version_at(std::chrono::system_clock::now());
    ASSERT_TRUE(ver.has_value());
    ASSERT_EQ(ver->size(), 3u);
    EXPECT_EQ((*ver)[0], 1);
}

TEST(DeepUndoAnalyzerTest, RowVersionHistoryGetVersionAtDeleted) {
    RowVersionHistory hist;
    hist.current_is_deleted = true;
    auto ver = hist.get_version_at(std::chrono::system_clock::now());
    EXPECT_FALSE(ver.has_value());
}

TEST(DeepUndoAnalyzerTest, RowVersionHistoryGetVersionAtWithChain) {
    RowVersionHistory hist;
    hist.current_data = {9, 9, 9};

    UndoRecordV2 older;
    older.approx_timestamp = std::chrono::system_clock::time_point(
        std::chrono::nanoseconds(100));
    older.old_fields.push_back({});
    older.old_fields.back().old_value = {1, 2, 3};

    UndoRecordV2 newer;
    newer.approx_timestamp = std::chrono::system_clock::time_point(
        std::chrono::nanoseconds(200));
    newer.old_fields.push_back({});
    newer.old_fields.back().old_value = {4, 5, 6};

    // oldest-first ordering
    hist.undo_chain.push_back(std::move(older));
    hist.undo_chain.push_back(std::move(newer));

    // Query at time 150 → should return older version (100 <= 150)
    auto ver = hist.get_version_at(
        std::chrono::system_clock::time_point(std::chrono::nanoseconds(150)));
    ASSERT_TRUE(ver.has_value());
    ASSERT_EQ(ver->size(), 3u);
    EXPECT_EQ((*ver)[0], 1);  // older version data
}

TEST(DeepUndoAnalyzerTest, RowVersionHistoryGetVersionAtLaterTime) {
    RowVersionHistory hist;
    UndoRecordV2 older;
    older.approx_timestamp = std::chrono::system_clock::time_point(
        std::chrono::nanoseconds(100));
    older.old_fields.push_back({});
    older.old_fields.back().old_value = {1, 2, 3};

    UndoRecordV2 newer;
    newer.approx_timestamp = std::chrono::system_clock::time_point(
        std::chrono::nanoseconds(200));
    newer.old_fields.push_back({});
    newer.old_fields.back().old_value = {4, 5, 6};

    hist.undo_chain.push_back(std::move(older));
    hist.undo_chain.push_back(std::move(newer));

    // Query at time 250 → we iterate rbegin to rend: first check newer (200 <= 250), match
    auto ver = hist.get_version_at(
        std::chrono::system_clock::time_point(std::chrono::nanoseconds(250)));
    ASSERT_TRUE(ver.has_value());
    EXPECT_EQ((*ver)[0], 4);  // newer version data
}

TEST(DeepUndoAnalyzerTest, RowVersionHistoryGetVersionAtBeforeAll) {
    RowVersionHistory hist;
    hist.current_data = {7, 7, 7};
    UndoRecordV2 rec;
    rec.approx_timestamp = std::chrono::system_clock::time_point(
        std::chrono::nanoseconds(200));
    hist.undo_chain.push_back(std::move(rec));

    // Query at time 50 → no version matches, falls back to current_data
    auto ver = hist.get_version_at(
        std::chrono::system_clock::time_point(std::chrono::nanoseconds(50)));
    ASSERT_TRUE(ver.has_value());
    EXPECT_EQ((*ver)[0], 7);
}

TEST(DeepUndoAnalyzerTest, RowVersionHistoryToInsertSqlNoChain) {
    catalog::TableSchema schema;
    schema.table_name = "test_table";
    catalog::ColumnInfo col;
    col.name = "id";
    col.mysql_type_id = 4;
    col.column_length = 4;
    schema.columns[1] = col;

    RowVersionHistory hist;
    std::string sql = hist.to_insert_sql(schema);
    EXPECT_FALSE(sql.empty());
    EXPECT_NE(sql.find("INSERT INTO test_table"), std::string::npos);
    EXPECT_NE(sql.find("DEFAULT"), std::string::npos);
}

TEST(DeepUndoAnalyzerTest, RowVersionHistoryToInsertSqlWithChain) {
    catalog::TableSchema schema;
    schema.table_name = "t";
    catalog::ColumnInfo col;
    col.name = "val";
    col.mysql_type_id = 4;
    col.column_length = 4;
    schema.columns[1] = col;

    UndoRecordV2 rec;
    rec.old_fields.push_back({});
    // field_no=1 is DB_ROLL_PTR (skipped), field_no=2 maps to ord=1 (first user column)
    auto& fv = rec.old_fields.back();
    fv.field_no = 2;
    fv.old_value = {42, 0, 0, 0};

    RowVersionHistory hist;
    hist.undo_chain.push_back(std::move(rec));

    std::string sql = hist.to_insert_sql(schema);
    EXPECT_NE(sql.find("42"), std::string::npos);
}

// ==================== FlashbackResult Tests ====================

TEST(DeepUndoAnalyzerTest, FlashbackResultDefault) {
    FlashbackResult fr;
    EXPECT_TRUE(fr.reverse_sql.empty());
    EXPECT_EQ(fr.trx_id, 0u);
    EXPECT_EQ(fr.undo_no, 0u);
    EXPECT_FALSE(fr.timestamp.has_value());
    EXPECT_DOUBLE_EQ(fr.confidence, 0.0);
    EXPECT_FALSE(fr.is_complete);
}

TEST(DeepUndoAnalyzerTest, FlashbackResultWithValues) {
    FlashbackResult fr;
    fr.reverse_sql = "INSERT INTO t VALUES (1)";
    fr.trx_id = 42;
    fr.undo_no = 7;
    fr.confidence = 0.95;
    fr.is_complete = true;
    fr.timestamp = std::chrono::system_clock::now();
    EXPECT_EQ(fr.reverse_sql, "INSERT INTO t VALUES (1)");
    EXPECT_EQ(fr.trx_id, 42u);
    EXPECT_EQ(fr.undo_no, 7u);
    EXPECT_DOUBLE_EQ(fr.confidence, 0.95);
    EXPECT_TRUE(fr.is_complete);
    EXPECT_TRUE(fr.timestamp.has_value());
}

// ==================== FlashbackFilter Tests ====================

TEST(DeepUndoAnalyzerTest, FlashbackFilterDefault) {
    FlashbackFilter filter;
    EXPECT_FALSE(filter.start_lsn.has_value());
    EXPECT_FALSE(filter.end_lsn.has_value());
    EXPECT_FALSE(filter.trx_id.has_value());
    EXPECT_FALSE(filter.start_time.has_value());
    EXPECT_FALSE(filter.end_time.has_value());
    EXPECT_EQ(filter.max_records, 10000u);
    EXPECT_FALSE(filter.include_incomplete);
}

TEST(DeepUndoAnalyzerTest, FlashbackFilterWithValues) {
    FlashbackFilter filter;
    filter.start_lsn = 1000;
    filter.end_lsn = 2000;
    filter.trx_id = 42;
    filter.max_records = 500;
    filter.include_incomplete = true;
    ASSERT_TRUE(filter.start_lsn.has_value());
    EXPECT_EQ(*filter.start_lsn, 1000u);
    ASSERT_TRUE(filter.end_lsn.has_value());
    EXPECT_EQ(*filter.end_lsn, 2000u);
    ASSERT_TRUE(filter.trx_id.has_value());
    EXPECT_EQ(*filter.trx_id, 42u);
    EXPECT_EQ(filter.max_records, 500u);
    EXPECT_TRUE(filter.include_incomplete);
}

// ==================== UndoPageManager Tests ====================

TEST(DeepUndoAnalyzerTest, UndoPageManagerDefault) {
    UndoPageManager mgr;
    EXPECT_FALSE(mgr.is_open());
    EXPECT_EQ(mgr.page_size(), 16384u);
    EXPECT_EQ(mgr.get_page_count(), 0u);
}

TEST(DeepUndoAnalyzerTest, UndoPageManagerSetDataFile) {
    // Create a small ibdata file
    std::vector<uint8_t> data(16384 * 5, 0);
    std::string path = create_temp_file(data, "_mgr");

    UndoPageManager mgr;
    mgr.set_data_file(path);
    EXPECT_EQ(mgr.page_size(), 16384u);

    // First read opens the file
    auto page = mgr.read_undo_page(0);
    EXPECT_FALSE(page.empty());
    EXPECT_EQ(page.size(), 16384u);
    EXPECT_TRUE(mgr.is_open());
    EXPECT_EQ(mgr.get_page_count(), 5u);

    std::remove(path.c_str());
}

TEST(DeepUndoAnalyzerTest, UndoPageManagerReadPageOutOfBounds) {
    std::vector<uint8_t> data(16384 * 2, 0);
    std::string path = create_temp_file(data, "_oob");

    UndoPageManager mgr;
    mgr.set_data_file(path);
    auto page = mgr.read_undo_page(10);
    EXPECT_TRUE(page.empty());

    std::remove(path.c_str());
}

TEST(DeepUndoAnalyzerTest, UndoPageManagerReadPageNonExistentFile) {
    UndoPageManager mgr;
    mgr.set_data_file("/nonexistent/path/ibdata1");
    auto page = mgr.read_undo_page(0);
    EXPECT_TRUE(page.empty());
    EXPECT_FALSE(mgr.is_open());
}

TEST(DeepUndoAnalyzerTest, UndoPageManagerReadPageEmptyFile) {
    std::string path = create_temp_file({}, "_empty");
    UndoPageManager mgr;
    mgr.set_data_file(path);
    auto page = mgr.read_undo_page(0);
    EXPECT_TRUE(page.empty());
    std::remove(path.c_str());
}

TEST(DeepUndoAnalyzerTest, UndoPageManagerMultiPage) {
    std::vector<uint8_t> data(16384 * 10, 0);
    // Mark page 0 with a known pattern
    data[0] = 0xAB;
    data[16384] = 0xCD;  // first byte of page 1
    std::string path = create_temp_file(data, "_multi");

    UndoPageManager mgr;
    mgr.set_data_file(path);

    auto p0 = mgr.read_undo_page(0);
    ASSERT_FALSE(p0.empty());
    EXPECT_EQ(p0[0], 0xAB);

    auto p1 = mgr.read_undo_page(1);
    ASSERT_FALSE(p1.empty());
    EXPECT_EQ(p1[0], 0xCD);

    auto p9 = mgr.read_undo_page(9);
    ASSERT_FALSE(p9.empty());

    std::remove(path.c_str());
}

TEST(DeepUndoAnalyzerTest, UndoPageManagerCustomPageSize) {
    std::vector<uint8_t> data(8192 * 3, 0);
    std::string path = create_temp_file(data, "_cust");

    UndoPageManager mgr(8192);
    mgr.set_data_file(path);
    EXPECT_EQ(mgr.page_size(), 8192u);

    auto page = mgr.read_undo_page(0);
    ASSERT_FALSE(page.empty());
    EXPECT_EQ(page.size(), 8192u);
    EXPECT_EQ(mgr.get_page_count(), 3u);

    std::remove(path.c_str());
}

// ==================== UndoRecordV2Parser Tests ====================

TEST(DeepUndoAnalyzerTest, ParserReadField) {
    UndoRecordV2Parser parser;
    // Field with length < 128: format = [len_byte] [data...]
    std::vector<uint8_t> data = {0x03, 0x01, 0x02, 0x03};

    auto result = parser.read_field(data.data(), data.size(), 0);
    ASSERT_TRUE(result.has_value());
    auto& [field_data, consumed] = *result;
    ASSERT_EQ(field_data.size(), 3u);
    EXPECT_EQ(field_data[0], 0x01);
    EXPECT_EQ(field_data[1], 0x02);
    EXPECT_EQ(field_data[2], 0x03);
    EXPECT_EQ(consumed, 4u);  // 1 prefix + 3 data
}

TEST(DeepUndoAnalyzerTest, ParserReadFieldLongLength) {
    UndoRecordV2Parser parser;
    // Field with length >= 128: format = [0x80 | len_high] [len_low] [data...]
    // Length = 0x80 = 128
    std::vector<uint8_t> data(130, 0);
    data[0] = 0x80 | 0x00;  // len_high = 0
    data[1] = 0x80;         // len_low = 128
    data[2] = 0xAA;         // first data byte

    auto result = parser.read_field(data.data(), data.size(), 0);
    ASSERT_TRUE(result.has_value());
    auto& [field_data, consumed] = *result;
    EXPECT_EQ(field_data.size(), 128u);
    EXPECT_EQ(field_data[0], 0xAA);
    EXPECT_EQ(consumed, 130u);  // 2 prefix + 128 data
}

TEST(DeepUndoAnalyzerTest, ParserReadFieldBoundary) {
    UndoRecordV2Parser parser;
    // Offset at end of data
    std::vector<uint8_t> data = {0x01, 0xFF};
    auto result = parser.read_field(data.data(), data.size(), 2);
    EXPECT_FALSE(result.has_value());
}

TEST(DeepUndoAnalyzerTest, ParserReadFieldTruncated) {
    UndoRecordV2Parser parser;
    // 2-byte prefix claims length > remaining data
    std::vector<uint8_t> data = {0x80, 0x0A, 0x01, 0x02};
    // Prefix claims 10 bytes, but only 2 remain
    auto result = parser.read_field(data.data(), data.size(), 0);
    EXPECT_FALSE(result.has_value());
}

TEST(DeepUndoAnalyzerTest, ParserExtractPrevPtrFromField) {
    std::vector<uint8_t> field_data(6);
    write_le<uint32_t>(field_data.data(), 0, 0x000FEDCB);
    write_le<uint16_t>(field_data.data(), 4, 0x1234);

    uint64_t prev_ptr = UndoRecordV2Parser::extract_prev_ptr_from_field(field_data);
    auto decoded = RollPtrDecoder::decode(prev_ptr);
    EXPECT_EQ(decoded.page_no, 0x000FEDCBu);
    EXPECT_EQ(decoded.offset, 0x1234u);
}

TEST(DeepUndoAnalyzerTest, ParserExtractPrevPtrFromFieldEmpty) {
    std::vector<uint8_t> field_data;
    uint64_t prev_ptr = UndoRecordV2Parser::extract_prev_ptr_from_field(field_data);
    EXPECT_EQ(prev_ptr, 0u);
}

TEST(DeepUndoAnalyzerTest, ParserParseNullData) {
    UndoRecordV2Parser parser;
    auto result = parser.parse(nullptr, 16384, 40, nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST(DeepUndoAnalyzerTest, ParserParseOffsetBeyondPage) {
    UndoRecordV2Parser parser;
    std::vector<uint8_t> page(16384, 0);
    auto result = parser.parse(page.data(), page.size(), 20000, nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST(DeepUndoAnalyzerTest, ParserParseNonUndoPage) {
    UndoRecordV2Parser parser;
    std::vector<uint8_t> page(16384, 0);
    // Page type is not UNDO_LOG (default 0)
    auto result = parser.parse(page.data(), page.size(), TRX_UNDO_PAGE_HDR_SIZE, nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST(DeepUndoAnalyzerTest, ParserParseInsertRecord) {
    UndoRecordV2Parser parser;

    // Create an undo page with one INSERT record
    auto pk_field = make_field({0x01, 0x02, 0x03, 0x04});
    auto val_field = make_field({0x2A, 0x00, 0x00, 0x00});
    std::vector<uint8_t> rec_data;
    rec_data.push_back(1);   // TRX_UNDO_INSERT
    rec_data.push_back(2);   // field_count = 2
    rec_data.insert(rec_data.end(), pk_field.begin(), pk_field.end());
    rec_data.insert(rec_data.end(), val_field.begin(), val_field.end());

    auto page = make_undo_page({rec_data});
    auto result = parser.parse(page.data(), page.size(), TRX_UNDO_PAGE_HDR_SIZE, nullptr);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->type, UndoRecordV2::INSERT);
    EXPECT_EQ(result->undo_page_offset, TRX_UNDO_PAGE_HDR_SIZE);
    EXPECT_EQ(result->old_fields.size(), 2u);
    EXPECT_EQ(result->page_no, 0u);  // INSERT records have no page_no
}

TEST(DeepUndoAnalyzerTest, ParserParseUpdateRecord) {
    UndoRecordV2Parser parser;

    // Create an undo page with one UPDATE record
    uint64_t prev_ptr = RollPtrDecoder::encode(10, 50, 0);
    auto rec = make_update_rec(100, 200, 42, prev_ptr, {
        {0x0A, 0x00, 0x00, 0x00},  // old value field 1
        {0x14, 0x00, 0x00, 0x00}   // old value field 2
    });

    auto page = make_undo_page({rec});
    auto result = parser.parse(page.data(), page.size(), TRX_UNDO_PAGE_HDR_SIZE, nullptr);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->type, UndoRecordV2::UPDATE);
    EXPECT_EQ(result->page_no, 100u);
    EXPECT_EQ(result->page_offset, 200u);
    EXPECT_EQ(result->trx_id, 42u);
    EXPECT_EQ(result->prev_undo_ptr, prev_ptr);
    EXPECT_EQ(result->old_fields.size(), 2u);
}

TEST(DeepUndoAnalyzerTest, ParserParseDeleteRecord) {
    UndoRecordV2Parser parser;

    uint64_t prev_ptr = RollPtrDecoder::encode(5, 25, 0);
    auto rec = make_delete_rec(300, 400, 99, prev_ptr, {
        {0xFF, 0xFF, 0xFF, 0xFF}
    });

    auto page = make_undo_page({rec});
    auto result = parser.parse(page.data(), page.size(), TRX_UNDO_PAGE_HDR_SIZE, nullptr);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->type, UndoRecordV2::DELETE_MARK);
    EXPECT_EQ(result->page_no, 300u);
    EXPECT_EQ(result->page_offset, 400u);
    EXPECT_EQ(result->trx_id, 99u);
    EXPECT_EQ(result->old_fields.size(), 1u);
}

TEST(DeepUndoAnalyzerTest, ParserParseInvalidType) {
    UndoRecordV2Parser parser;
    std::vector<uint8_t> rec = {0xFF, 0x01};  // invalid type
    auto page = make_undo_page({rec});
    auto result = parser.parse(page.data(), page.size(), TRX_UNDO_PAGE_HDR_SIZE, nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST(DeepUndoAnalyzerTest, ParserParseTooManyFields) {
    UndoRecordV2Parser parser;
    std::vector<uint8_t> rec = {0x01, 51};  // field_count = 51 > 50
    auto page = make_undo_page({rec});
    auto result = parser.parse(page.data(), page.size(), TRX_UNDO_PAGE_HDR_SIZE, nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST(DeepUndoAnalyzerTest, ParserParseOffsetBeforeHeader) {
    UndoRecordV2Parser parser;
    auto page = make_undo_page({});
    // Offset before TRX_UNDO_PAGE_HDR_SIZE
    auto result = parser.parse(page.data(), page.size(), 10, nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST(DeepUndoAnalyzerTest, ParserParseOffsetBeyondPageFree) {
    UndoRecordV2Parser parser;
    auto page = make_undo_page({});
    // page_free is at TRX_UNDO_PAGE_HDR_SIZE (40) since no records
    // Offset >= page_free
    auto result = parser.parse(page.data(), page.size(), 50, nullptr);
    EXPECT_FALSE(result.has_value());
}

// ==================== DeepUndoAnalyzer Tests ====================

TEST(DeepUndoAnalyzerTest, Constructor) {
    DeepUndoAnalyzer analyzer;
    const auto& stats = analyzer.get_stats();
    EXPECT_EQ(stats.chains_traced, 0u);
    EXPECT_EQ(stats.total_undo_records, 0u);
    EXPECT_EQ(stats.broken_chains, 0u);
    EXPECT_EQ(stats.flashback_statements, 0u);
    EXPECT_EQ(stats.pages_scanned, 0u);
    EXPECT_DOUBLE_EQ(stats.analysis_time_ms, 0.0);
}

TEST(DeepUndoAnalyzerTest, SetIbdataPath) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path("/path/to/ibdata1");
    // No crash, path is stored internally
}

TEST(DeepUndoAnalyzerTest, SetPageSize) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_page_size(8192);
    // No crash, page size is stored internally
}

TEST(DeepUndoAnalyzerTest, SetMaxChainDepth) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_max_chain_depth(500);
    // No crash, depth is stored internally
}

TEST(DeepUndoAnalyzerTest, SetUndoPageIndex) {
    DeepUndoAnalyzer analyzer;
    UndoPageIndex index;
    analyzer.set_undo_page_index(&index);
    // No crash, pointer is stored
}

TEST(DeepUndoAnalyzerTest, SetUndoPageIndexNull) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_undo_page_index(nullptr);
    // No crash, null pointer is handled
}

TEST(DeepUndoAnalyzerTest, ExtractTableIdFromFields) {
    std::vector<UndoFieldValue> fields;
    UndoFieldValue fv;
    fv.old_value = {0x64, 0x00, 0x00, 0x00};  // 100 LE
    fields.push_back(std::move(fv));

    uint32_t tid = DeepUndoAnalyzer::extract_table_id(fields);
    EXPECT_EQ(tid, 100u);
}

TEST(DeepUndoAnalyzerTest, ExtractTableIdEmptyFields) {
    std::vector<UndoFieldValue> fields;
    uint32_t tid = DeepUndoAnalyzer::extract_table_id(fields);
    EXPECT_EQ(tid, 0u);
}

TEST(DeepUndoAnalyzerTest, ExtractTableIdShortField) {
    std::vector<UndoFieldValue> fields;
    UndoFieldValue fv;
    fv.old_value = {0x01, 0x02};  // only 2 bytes
    fields.push_back(std::move(fv));

    uint32_t tid = DeepUndoAnalyzer::extract_table_id(fields);
    EXPECT_EQ(tid, 0u);
}

TEST(DeepUndoAnalyzerTest, ExtractTableIdLargeValue) {
    std::vector<UndoFieldValue> fields;
    UndoFieldValue fv;
    fv.old_value = {0xEF, 0xBE, 0xAD, 0xDE, 0x00, 0x00};  // 0xDEADBEEF LE
    fields.push_back(std::move(fv));

    uint32_t tid = DeepUndoAnalyzer::extract_table_id(fields);
    EXPECT_EQ(tid, 0xDEADBEEFu);
}

TEST(DeepUndoAnalyzerTest, StatsReset) {
    DeepUndoAnalyzer analyzer;
    analyzer.reset_stats();
    const auto& stats = analyzer.get_stats();
    EXPECT_EQ(stats.chains_traced, 0u);
}

TEST(DeepUndoAnalyzerTest, ScanUndoPagesNonExistentFile) {
    // Set ibdata path to non-existent file
    // Currently scan_undo_pages opens ibdata1 via page_manager_, let's just test
    // that it handles the empty result gracefully
    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path("/nonexistent/ibdata1");
    auto pages = analyzer.scan_undo_pages();
    EXPECT_TRUE(pages.empty());
}

TEST(DeepUndoAnalyzerTest, EnsureIndexBuilt) {
    DeepUndoAnalyzer analyzer;
    // No external index set, so it will try to build internal index
    // Since ibdata_path is empty, the build will fail gracefully
    analyzer.ensure_index_built();
    // No crash — internal index build fails silently
}

TEST(DeepUndoAnalyzerTest, TraceVersionChainZeroRollPtr) {
    // When roll_ptr=0, should return empty chain
    DeepUndoAnalyzer analyzer;
    catalog::TableSchema schema;
    schema.table_name = "test";

    auto history = analyzer.trace_version_chain({1, 2, 3}, 0, schema, 100);
    EXPECT_EQ(history.undo_chain.size(), 0u);
    EXPECT_EQ(history.n_versions, 1u);  // 1 + 0 = 1
    EXPECT_EQ(history.index_id, 100u);
    ASSERT_EQ(history.current_data.size(), 3u);
    EXPECT_EQ(history.current_data[0], 1);
}

TEST(DeepUndoAnalyzerTest, TraceVersionChainNoIbdata) {
    // With roll_ptr != 0 but no ibdata file, the chain should break
    DeepUndoAnalyzer analyzer;
    catalog::TableSchema schema;
    schema.table_name = "test";

    // encode(page_no=100, offset=50, flags=0)
    uint64_t rp = RollPtrDecoder::encode(100, 50, 0);
    auto history = analyzer.trace_version_chain({}, rp, schema, 200);
    // Chain should break because page 100 can't be read
    EXPECT_EQ(history.undo_chain.size(), 0u);
    EXPECT_EQ(history.index_id, 200u);
    const auto& stats = analyzer.get_stats();
    EXPECT_EQ(stats.broken_chains, 1u);
}

TEST(DeepUndoAnalyzerTest, FlashbackDeletesNoFilter) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path("/nonexistent/ibdata1");
    catalog::TableSchema schema;
    schema.table_name = "test";
    schema.table_id = 1;

    auto results = analyzer.flashback_deletes(0, schema, {});
    EXPECT_TRUE(results.empty());
}

TEST(DeepUndoAnalyzerTest, FlashbackDeletesWithTrxFilter) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path("/nonexistent/ibdata1");
    catalog::TableSchema schema;
    schema.table_name = "test";
    schema.table_id = 1;

    FlashbackFilter filter;
    filter.trx_id = 42;
    auto results = analyzer.flashback_deletes(0, schema, filter);
    EXPECT_TRUE(results.empty());
}

TEST(DeepUndoAnalyzerTest, FlashbackDeletesWithLsnFilter) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path("/nonexistent/ibdata1");
    catalog::TableSchema schema;
    schema.table_name = "test";

    FlashbackFilter filter;
    filter.start_lsn = 1000;
    filter.end_lsn = 2000;
    auto results = analyzer.flashback_deletes(0, schema, filter);
    EXPECT_TRUE(results.empty());
}

TEST(DeepUndoAnalyzerTest, FlashbackDeletesWithTimeFilter) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path("/nonexistent/ibdata1");
    catalog::TableSchema schema;
    schema.table_name = "test";

    FlashbackFilter filter;
    filter.start_time = std::chrono::system_clock::now();
    filter.end_time = std::chrono::system_clock::now();
    auto results = analyzer.flashback_deletes(0, schema, filter);
    EXPECT_TRUE(results.empty());
}

TEST(DeepUndoAnalyzerTest, FlashbackDeletesMaxRecords) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path("/nonexistent/ibdata1");
    catalog::TableSchema schema;
    schema.table_name = "test";

    FlashbackFilter filter;
    filter.max_records = 0;
    auto results = analyzer.flashback_deletes(0, schema, filter);
    EXPECT_TRUE(results.empty());
}

TEST(DeepUndoAnalyzerTest, FlashbackDeletesIncludeIncomplete) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path("/nonexistent/ibdata1");
    catalog::TableSchema schema;
    schema.table_name = "test";

    FlashbackFilter filter;
    filter.include_incomplete = true;
    auto results = analyzer.flashback_deletes(0, schema, filter);
    EXPECT_TRUE(results.empty());
}

TEST(DeepUndoAnalyzerTest, RebuildAllHistoriesEmpty) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path("/nonexistent/ibdata1");
    catalog::TableSchema schema;
    schema.table_name = "test";

    auto histories = analyzer.rebuild_all_histories(0, schema, 100);
    EXPECT_TRUE(histories.empty());
}

TEST(DeepUndoAnalyzerTest, RebuildAllHistoriesLsnRange) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path("/nonexistent/ibdata1");
    catalog::TableSchema schema;
    schema.table_name = "test";

    auto histories = analyzer.rebuild_all_histories(0, schema, 100,
        std::make_pair(1000, 2000));
    EXPECT_TRUE(histories.empty());
}

TEST(DeepUndoAnalyzerTest, HeuristicReconstructChainsEmpty) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path("/nonexistent/ibdata1");
    catalog::TableSchema schema;
    schema.table_name = "test";

    auto chains = analyzer.heuristic_reconstruct_chains(0, schema);
    EXPECT_TRUE(chains.empty());
}

TEST(DeepUndoAnalyzerTest, ScanUndoPagesWithIbdata) {
    // Create ibdata with UNDO_LOG pages
    std::string path = create_ibdata_with_pages({
        {5, FIL_PAGE_UNDO_LOG},
        {10, FIL_PAGE_UNDO_LOG},
    });

    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path(path);
    UndoPageIndex index;
    index.build(path, 16384, 0);
    analyzer.set_undo_page_index(&index);

    auto pages = analyzer.scan_undo_pages();
    // Pages should be found but may be empty since they're allocated (zero-filled)
    // The page_manager reads them but the index tells us which pages are UNDO_LOG
    EXPECT_EQ(pages.size(), 2u);

    const auto& stats = analyzer.get_stats();
    EXPECT_EQ(stats.pages_scanned, 2u);

    std::remove(path.c_str());
}

TEST(DeepUndoAnalyzerTest, ScanUndoPagesNoIndex) {
    std::string path = create_ibdata_with_pages({
        {5, FIL_PAGE_UNDO_LOG},
    });

    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path(path);
    // No index set — ensure_index_built will try to build internal index
    // But since ibdata_path is set, it should work
    auto pages = analyzer.scan_undo_pages();
    // May or may not find pages depending on internal index build
    // Just verify no crash

    std::remove(path.c_str());
}

// ==================== Edge Cases ====================

TEST(DeepUndoAnalyzerTest, UndoFieldValueExtern) {
    UndoFieldValue fv;
    fv.is_extern = true;
    fv.old_value = {0x01, 0x02, 0x03};
    EXPECT_TRUE(fv.is_extern);
    EXPECT_EQ(fv.to_hex(), "010203");
}

TEST(DeepUndoAnalyzerTest, UndoRecordV2EmptyOldFields) {
    UndoRecordV2 rec;
    EXPECT_TRUE(rec.old_fields.empty());
    EXPECT_TRUE(rec.primary_key_raw.empty());
}

TEST(DeepUndoAnalyzerTest, RowVersionHistoryTruncated) {
    RowVersionHistory hist;
    hist.truncated = true;
    EXPECT_TRUE(hist.truncated);
}

TEST(DeepUndoAnalyzerTest, RowVersionHistoryCurrentIsDeleted) {
    RowVersionHistory hist;
    hist.current_is_deleted = true;
    EXPECT_TRUE(hist.current_is_deleted);
}

TEST(DeepUndoAnalyzerTest, ParserParseMultiRecord) {
    UndoRecordV2Parser parser;

    // Create two records on one undo page
    auto rec1 = make_insert_rec({{0x01}, {0x02}});
    auto rec2 = make_insert_rec({{0x03}, {0x04}});

    auto page = make_undo_page({rec1, rec2});

    // Parse first record
    auto r1 = parser.parse(page.data(), page.size(), TRX_UNDO_PAGE_HDR_SIZE, nullptr);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->old_fields.size(), 2u);

    // Calculate consumed size and parse second record
    size_t consumed = 0;
    auto r1_with_consumed = parser.parse(
        page.data(), page.size(), TRX_UNDO_PAGE_HDR_SIZE, nullptr, &consumed);
    ASSERT_TRUE(r1_with_consumed.has_value());
    EXPECT_GT(consumed, 0u);

    auto r2 = parser.parse(
        page.data(), page.size(), TRX_UNDO_PAGE_HDR_SIZE + consumed, nullptr, &consumed);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->old_fields.size(), 2u);
}

TEST(DeepUndoAnalyzerTest, ParserParseWithConsumedSize) {
    UndoRecordV2Parser parser;
    auto rec = make_insert_rec({{0xAA}});
    auto page = make_undo_page({rec});

    size_t consumed = 0;
    auto result = parser.parse(
        page.data(), page.size(), TRX_UNDO_PAGE_HDR_SIZE, nullptr, &consumed);
    ASSERT_TRUE(result.has_value());
    EXPECT_GT(consumed, 0u);
}

TEST(DeepUndoAnalyzerTest, DeepUndoAnalyzerMultipleConfig) {
    DeepUndoAnalyzer analyzer;
    analyzer.set_page_size(4096);
    analyzer.set_max_chain_depth(50);
    analyzer.set_ibdata_path("/tmp/test_ibdata");
    // No crash from multiple config calls
    analyzer.set_page_size(8192);
    analyzer.set_max_chain_depth(100);
}

TEST(DeepUndoAnalyzerTest, DeepUndoAnalyzerResetStats) {
    DeepUndoAnalyzer analyzer;
    // Trace with non-zero roll_ptr to increment broken_chains
    catalog::TableSchema schema;
    uint64_t rp = RollPtrDecoder::encode(999, 999, 0);
    analyzer.trace_version_chain({}, rp, schema, 0);
    EXPECT_EQ(analyzer.get_stats().broken_chains, 1u);

    analyzer.reset_stats();
    EXPECT_EQ(analyzer.get_stats().broken_chains, 0u);
}

// ==================== trace_version_chain Integration Helpers ====================

// Build a 6-byte field-data roll pointer (4B page_no LE + 2B offset LE).
// All-zero bytes encode "no previous version" (null).
static std::vector<uint8_t> make_roll_ptr_field(uint32_t page_no, uint16_t offset) {
    std::vector<uint8_t> f(6, 0);
    write_le<uint32_t>(f.data(), 0, page_no);
    write_le<uint16_t>(f.data(), 4, offset);
    return f;
}

// Layout a chain of UPDATE undo records on a single undo page, oldest-first.
// Each record's first field is the old DB_ROLL_PTR pointing to the previous
// (older) record; the oldest record's first field is null (all zeros).
// Returns {page, first_record_offset_of_newest}.
struct ChainLayout {
    uint32_t page_no = 0;
    uint16_t newest_offset = 0;
    uint16_t free = 0;
};

static ChainLayout build_version_chain_page(
    std::vector<uint8_t>& out_page,
    size_t page_size = 16384) {
    std::vector<uint8_t> page(page_size, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_UNDO_LOG);

    uint16_t free_off = TRX_UNDO_PAGE_HDR_SIZE;
    // (next_ptr_page, next_ptr_off) for each record's old DB_ROLL_PTR field.
    // We build oldest-first, so each record points to the record placed before it.
    std::vector<std::pair<uint32_t, uint16_t>> chain_ptrs = {
        {0, 0},        // oldest: no previous
        {0, 0},        // placeholder, patched below
        {0, 0},        // placeholder, patched below
    };
    // Record offsets as they will land in the page (patched after layout).
    uint16_t offsets[3] = {0, 0, 0};

    for (int i = 0; i < 3; i++) {
        offsets[i] = free_off;
        // Old value column field (an INT, 4 bytes) — value differs per record.
        std::vector<uint8_t> val_field = {static_cast<uint8_t>(10 * (i + 1)), 0, 0, 0};
        // First field: old DB_ROLL_PTR. Patched for records 1,2 after layout.
        std::vector<uint8_t> ptr_field = make_roll_ptr_field(chain_ptrs[i].first,
                                                             chain_ptrs[i].second);
        auto rec = make_update_rec( /*page_no*/100, /*page_offset*/200,
            /*trx_id*/100 + static_cast<uint64_t>(i),
            /*prev_roll_ptr*/0, {ptr_field, val_field});
        if (free_off + rec.size() > page_size) break;
        std::memcpy(page.data() + free_off, rec.data(), rec.size());
        free_off += static_cast<uint16_t>(rec.size());
    }
    // Patch the middle (newer) records' first field to point to the previous record.
    // Record 2 (newest) -> record 1, record 1 -> record 0.
    auto patch_ptr = [&](int idx, uint32_t page_no, uint16_t target_off) {
        auto ptr_field = make_roll_ptr_field(page_no, target_off);
        // Header is 21 bytes (2 + 4 + 2 + 1 + 5 + 7), then 1-byte length prefix for first field.
        size_t field_off = static_cast<size_t>(offsets[idx]) + 22;
        std::memcpy(page.data() + field_off, ptr_field.data(), ptr_field.size());
    };
    patch_ptr(2, 5, offsets[1]);
    patch_ptr(1, 5, offsets[0]);

    write_le<uint16_t>(page.data(), TRX_UNDO_PAGE_START, TRX_UNDO_PAGE_HDR_SIZE);
    write_le<uint16_t>(page.data(), TRX_UNDO_PAGE_FREE, free_off);
    write_le<uint16_t>(page.data(), TRX_UNDO_PAGE_N_RECS, 3);
    out_page = std::move(page);
    ChainLayout layout;
    layout.page_no = 5;
    layout.newest_offset = offsets[2];
    layout.free = free_off;
    return layout;
}

// ==================== trace_version_chain Integration Tests ====================

TEST(DeepUndoAnalyzerTest, TraceVersionChainThreeVersionWalk) {
    // Build a single undo page with a 3-record chain (oldest -> newest).
    std::vector<uint8_t> page;
    auto layout = build_version_chain_page(page);

    // ibdata1 with page 5 = the undo page.
    uint32_t max_page = 5;
    std::vector<uint8_t> file(16384 * 6, 0);
    std::memcpy(file.data() + 5 * 16384, page.data(), page.size());
    std::string path = create_temp_file(file, "_chain_walk");

    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path(path);
    analyzer.set_page_size(16384);

    catalog::TableSchema schema;
    schema.table_name = "t";
    schema.table_id = 1;

    // Current clustered-index record; roll_ptr points to the NEWEST undo record.
    std::vector<uint8_t> current = {0xAA, 0xBB};
    uint64_t roll_ptr = RollPtrDecoder::encode(layout.page_no, layout.newest_offset, 0);

    auto history = analyzer.trace_version_chain(current, roll_ptr, schema, 42);

    EXPECT_EQ(history.index_id, 42u);
    ASSERT_EQ(history.current_data.size(), 2u);
    EXPECT_EQ(history.current_data[0], 0xAA);
    // undo_chain is ordered oldest-first.
    ASSERT_EQ(history.undo_chain.size(), 3u);
    EXPECT_EQ(history.n_versions, 4u);
    EXPECT_FALSE(history.truncated);

    // Oldest record first: trx_id = 100 (oldest), then 101, then 102 (newest).
    EXPECT_EQ(history.undo_chain[0].trx_id, 100u);
    EXPECT_EQ(history.undo_chain[1].trx_id, 101u);
    EXPECT_EQ(history.undo_chain[2].trx_id, 102u);

    // Each record carries 2 fields: old roll ptr + old value.
    ASSERT_EQ(history.undo_chain[0].old_fields.size(), 2u);
    EXPECT_EQ(history.undo_chain[0].old_fields[1].old_value[0], 10u);
    EXPECT_EQ(history.undo_chain[1].old_fields[1].old_value[0], 20u);
    EXPECT_EQ(history.undo_chain[2].old_fields[1].old_value[0], 30u);

    // Oldest record's old roll ptr is null (no previous version).
    EXPECT_EQ(history.undo_chain[0].old_fields[0].old_value.size(), 6u);
    EXPECT_EQ(history.undo_chain[0].old_fields[0].old_value[0], 0u);

    const auto& stats = analyzer.get_stats();
    EXPECT_EQ(stats.chains_traced, 1u);
    EXPECT_EQ(stats.broken_chains, 0u);
    EXPECT_EQ(stats.total_undo_records, 3u);

    std::remove(path.c_str());
}

TEST(DeepUndoAnalyzerTest, TraceVersionChainRollPtrPointsToCorruptPage) {
    // ibdata only has page 0 (zero-filled, not an undo page). A roll_ptr to
    // a non-undo page must break the chain and count a broken chain.
    std::vector<uint8_t> file(16384 * 1, 0);
    std::string path = create_temp_file(file, "_chain_badpage");

    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path(path);
    analyzer.set_page_size(16384);

    catalog::TableSchema schema;
    schema.table_name = "t";

    // encode(page_no=0 ...) would be null; use page 0 offset 40 which is a
    // valid pointer but the page type is 0 (not UNDO_LOG) -> chain break.
    uint64_t roll_ptr = RollPtrDecoder::encode(0, 40, 0);
    auto history = analyzer.trace_version_chain({}, roll_ptr, schema, 7);

    EXPECT_EQ(history.undo_chain.size(), 0u);
    EXPECT_EQ(history.n_versions, 1u);
    EXPECT_EQ(analyzer.get_stats().broken_chains, 1u);

    std::remove(path.c_str());
}

TEST(DeepUndoAnalyzerTest, TraceVersionChainCycleDetected) {
    // A roll_ptr that points back to itself must terminate via the visited set
    // rather than looping forever. We build a page whose single record's old
    // roll-ptr field points back to its own offset.
    std::vector<uint8_t> page(16384, 0);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_UNDO_LOG);
    uint16_t rec_off = TRX_UNDO_PAGE_HDR_SIZE;
    auto rec = make_update_rec(100, 200, 55, 0,
        {make_roll_ptr_field(5, rec_off), {0x01, 0x00, 0x00, 0x00}});
    std::memcpy(page.data() + rec_off, rec.data(), rec.size());
    uint16_t free_off = rec_off + static_cast<uint16_t>(rec.size());
    write_le<uint16_t>(page.data(), TRX_UNDO_PAGE_FREE, free_off);
    write_le<uint16_t>(page.data(), TRX_UNDO_PAGE_N_RECS, 1);

    std::vector<uint8_t> file(16384 * 6, 0);
    std::memcpy(file.data() + 5 * 16384, page.data(), page.size());
    std::string path = create_temp_file(file, "_chain_cycle");

    DeepUndoAnalyzer analyzer;
    analyzer.set_ibdata_path(path);
    analyzer.set_page_size(16384);

    catalog::TableSchema schema;
    schema.table_name = "t";

    uint64_t roll_ptr = RollPtrDecoder::encode(5, rec_off, 0);
    auto history = analyzer.trace_version_chain({}, roll_ptr, schema, 9);

    // The self-referential record is visited once; the loop must terminate.
    // undo_chain has either 1 record (if the self-pointer is extracted) or is
    // terminated by the visited set. Either way it must NOT hang or exceed 1.
    ASSERT_LE(history.undo_chain.size(), 1u);
    EXPECT_LE(history.n_versions, 2u);

    std::remove(path.c_str());
}

// ==================== FlashbackGenerator-style Output Tests ====================

TEST(DeepUndoAnalyzerTest, FlashbackResultConfidence) {
    FlashbackResult fr;
    fr.is_complete = true;
    EXPECT_TRUE(fr.is_complete);
    fr.confidence = 0.5;
    fr.is_complete = false;
    EXPECT_FALSE(fr.is_complete);
    EXPECT_DOUBLE_EQ(fr.confidence, 0.5);
}

TEST(DeepUndoAnalyzerTest, FlashbackResultTimestamp) {
    FlashbackResult fr;
    auto now = std::chrono::system_clock::now();
    fr.timestamp = now;
    ASSERT_TRUE(fr.timestamp.has_value());
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(
        *fr.timestamp - now).count();
    EXPECT_EQ(diff, 0);
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}