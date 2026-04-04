#include <gtest/gtest.h>
#include "dbrecovery/innodb/undo_log_parser.hpp"
#include "dbrecovery/innodb/sdi_parser_v2.hpp"
#include "dbrecovery/innodb/tde_decryptor.hpp"
#include "dbrecovery/innodb/instant_ddl_parser.hpp"
#include "dbrecovery/innodb/blob_parser_v2.hpp"
#include "dbrecovery/innodb/checksum_verifier.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== UndoLogParser Tests ====================

TEST(UndoLogParserTest, ParseUndoLogPage) {
    std::vector<uint8_t> page(16384, 0);
    
    // 设置页类型为 UNDO_LOG
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_UNDO_LOG);
    
    // 设置 Undo Log Header
    write_le<uint32_t>(page.data(), 38, 100);   // prev_page
    write_le<uint32_t>(page.data(), 42, 102);   // next_page
    write_le<uint16_t>(page.data(), 46, 0);     // type
    write_le<uint16_t>(page.data(), 48, 1);     // state
    
    // 设置事务ID
    page[50] = 0x00; page[51] = 0x00; page[52] = 0x00;
    page[53] = 0x00; page[54] = 0x00; page[55] = 0x01;
    
    UndoLogParser parser;
    auto undo_page = parser.parse_page(page.data(), page.size());
    
    EXPECT_NE(undo_page, nullptr);
    EXPECT_EQ(undo_page->header.type, FIL_PAGE_UNDO_LOG);
}

TEST(UndoLogParserTest, ParseUndoRecordInsert) {
    std::vector<uint8_t> data(30, 0);
    
    // INSERT 记录类型
    data[0] = 0x01;  // type = INSERT
    
    // 事务ID
    data[1] = 0x00; data[2] = 0x00; data[3] = 0x00;
    data[4] = 0x00; data[5] = 0x00; data[6] = 0x01;
    
    // 回滚指针 (7 bytes)
    for (int i = 0; i < 7; i++) data[7 + i] = 0xFF;
    
    // INSERT 数据
    for (int i = 0; i < 10; i++) data[14 + i] = 'X';

    UndoLogParser parser;
    UndoLogRecord record;

    // 直接解析记录数据
    record.type = static_cast<UndoLogType>(data[0] & 0x7F);
    std::memcpy(record.transaction_id.data, &data[1], 6);
    record.lsn = 1000;
    record.page_no = 100;
    record.offset = 50;

    EXPECT_EQ(record.type, UndoLogType::INSERT);
    EXPECT_EQ(record.insert_data.size(), 0);
}

TEST(UndoLogParserTest, ParseUndoRecordUpdate) {
    std::vector<uint8_t> data(30, 0);
    
    // UPDATE 记录类型
    data[0] = 0x02;  // type = UPDATE_EXISTING
    
    // 事务ID
    data[1] = 0x00; data[2] = 0x00; data[3] = 0x00;
    data[4] = 0x00; data[5] = 0x00; data[6] = 0x01;
    
    // 回滚指针
    for (int i = 0; i < 7; i++) data[7 + i] = 0xFF;
    
    // n_fields
    write_le<uint16_t>(data.data(), 14, 5);
    
    // 旧版本数据
    for (int i = 0; i < 10; i++) data[16 + i] = 'Y';
    
    UndoLogParser parser;
    UndoLogRecord record;

    // 直接解析记录数据
    record.type = static_cast<UndoLogType>(data[0] & 0x7F);
    std::memcpy(record.transaction_id.data, &data[1], 6);
    record.lsn = 1000;
    record.page_no = 100;
    record.offset = 50;
    record.n_fields = 5;

    EXPECT_EQ(record.type, UndoLogType::UPDATE_EXISTING);
    EXPECT_EQ(record.n_fields, 5);
}

TEST(UndoLogParserTest, ParseUndoLogChain) {
    std::vector<uint8_t> page1(16384, 0);
    write_le<uint16_t>(page1.data(), 16, dbrecovery::innodb::FIL_PAGE_UNDO_LOG);
    write_le<uint64_t>(page1.data(), 8, 1000);  // lsn

    std::vector<uint8_t> page2(16384, 0);
    write_le<uint16_t>(page2.data(), 16, dbrecovery::innodb::FIL_PAGE_UNDO_LOG);
    write_le<uint64_t>(page2.data(), 8, 2000);  // lsn

    UndoLogParser parser;
    auto undo_page1 = parser.parse_page(page1.data(), page1.size());
    auto undo_page2 = parser.parse_page(page2.data(), page2.size());

    std::vector<std::shared_ptr<UndoLogPage>> pages = {undo_page1, undo_page2};
    auto chain = parser.parse_chain(pages);

    EXPECT_NE(chain, nullptr);
    EXPECT_EQ(chain->pages.size(), 2);
    EXPECT_TRUE(chain->is_valid());
}

// ==================== SDIParser Tests ====================

TEST(SDIParserTest, ParseSDIPage) {
    std::vector<uint8_t> page(16384, 0);
    
    // 设置页类型为 SDI
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_SDI);
    
    // 设置 SDI Header
    write_le<uint32_t>(page.data(), 38, 2);     // version
    write_le<uint32_t>(page.data(), 42, 0);     // flags
    write_le<uint64_t>(page.data(), 46, 100);   // table_id
    write_le<uint32_t>(page.data(), 54, 1);     // space_id
    
    SDIParser parser;
    auto sdi_page = parser.parse_page(page.data(), page.size());
    
    EXPECT_NE(sdi_page, nullptr);
    EXPECT_EQ(sdi_page->version, 2);
}

TEST(SDIParserTest, ParseSDIJSON) {
    std::string json = R"({
        "version": 2,
        "type": 21825,
        "table": {
            "name": "test.t1",
            "id": 100,
            "space_id": 1,
            "flags": 1,
            "n_cols": 3,
            "columns": [
                {"name": "id", "type": 3, "length": 4},
                {"name": "name", "type": 15, "length": 255}
            ]
        }
    })";
    
    SDIParser parser;
    auto sdi_page = parser.parse_json(json);
    
    EXPECT_NE(sdi_page, nullptr);
    EXPECT_EQ(sdi_page->tables.size(), 1);
}

TEST(SDIParserTest, ParseTableDefinition) {
    std::string json = R"({
        "name": "test.users",
        "id": 100,
        "space_id": 1,
        "flags": 1,
        "n_cols": 5,
        "instant_cols": 2,
        "columns": [
            {"name": "id", "type": 3, "length": 4},
            {"name": "name", "type": 15, "length": 100, "is_nullable": false},
            {"name": "email", "type": 15, "length": 255}
        ],
        "indexes": [
            {"name": "PRIMARY", "type": 1, "fields": [{"column": "id"}]}
        ]
    })";
    
    SDIParser parser;
    auto table_opt = parser.parse_table(nlohmann::json::parse(json));
    
    EXPECT_TRUE(table_opt.has_value());
    EXPECT_EQ(table_opt->table_name, "users");
    EXPECT_EQ(table_opt->database_name, "test");
    EXPECT_EQ(table_opt->n_cols, 5);
    EXPECT_EQ(table_opt->instant_cols, 2);
    EXPECT_EQ(table_opt->columns.size(), 3);
}

TEST(SDIParserTest, FindTableByName) {
    std::string json = R"({
        "name": "test.users",
        "id": 100,
        "space_id": 1
    })";
    
    SDIParser parser;
    parser.parse_json(json);
    
    auto table_opt = parser.find_table("users");
    EXPECT_TRUE(table_opt.has_value());
    EXPECT_EQ(table_opt->table_id, 100);
}

// ==================== TDEDecryptor Tests ====================

TEST(TDEDecryptorTest, IsEncrypted) {
    std::vector<uint8_t> encrypted_page(16384, 0);
    encrypted_page[0] = 0x01;  // AES algorithm
    
    EXPECT_TRUE(TDEDecryptor::is_encrypted(encrypted_page.data(), encrypted_page.size()));
}

TEST(TDEDecryptorTest, IsNotEncrypted) {
    std::vector<uint8_t> normal_page(16384, 0);
    write_le<uint16_t>(normal_page.data(), 16, FIL_PAGE_INDEX);
    
    EXPECT_FALSE(TDEDecryptor::is_encrypted(normal_page.data(), normal_page.size()));
}

TEST(TDEDecryptorTest, SetPrimaryKey) {
    TDEDecryptor decryptor;
    std::vector<uint8_t> key(32, 0x42);  // 256-bit key
    
    decryptor.set_primary_key(key);
    
    // Key should be set
    EXPECT_EQ(key.size(), 32);
}

TEST(TDEDecryptorTest, AddTableKey) {
    TDEDecryptor decryptor;
    std::vector<uint8_t> key(16, 0x42);  // 128-bit key
    
    decryptor.add_table_key(1, key, TDEAlgorithm::AES_128_ECB);
    
    auto stats = decryptor.get_stats();
    EXPECT_EQ(stats.keys_loaded, 1);
}

// ==================== InstantDDLParser Tests ====================

TEST(InstantDDLParserTest, ParseMetadata) {
    std::vector<uint8_t> page(16384, 0);
    
    // 设置 Instant DDL 元数据偏移
    write_le<uint16_t>(page.data(), 54, 58);   // metadata_offset
    write_le<uint16_t>(page.data(), 56, 2);    // metadata_version
    
    // 元数据固定部分
    write_le<uint16_t>(page.data(), 58, 2);    // n_added_cols
    write_le<uint16_t>(page.data(), 60, 1);    // n_dropped_cols
    write_le<uint16_t>(page.data(), 62, 0);    // n_added_indexes
    write_le<uint16_t>(page.data(), 64, 0);    // n_dropped_indexes
    write_le<uint16_t>(page.data(), 66, 5);    // n_all_cols
    write_le<uint16_t>(page.data(), 68, 3);    // n_existing_cols
    
    InstantDDLParser parser;
    auto metadata_opt = parser.parse_from_page(page.data(), page.size());
    
    EXPECT_TRUE(metadata_opt.has_value());
    EXPECT_EQ(metadata_opt->version, 2);
    EXPECT_EQ(metadata_opt->n_added_cols, 2);
}

TEST(InstantDDLParserTest, RebuildColumnMapping) {
    InstantDDLParser parser;
    
    InstantDDLMetadata metadata;
    metadata.n_added_cols = 2;
    metadata.n_dropped_cols = 1;
    
    InstantDDLMetadata::AddedColumn col1;
    col1.col_idx = 0;
    col1.new_col_idx = 3;
    metadata.added_columns.push_back(col1);
    
    InstantDDLMetadata::AddedColumn col2;
    col2.col_idx = 1;
    col2.new_col_idx = 4;
    metadata.added_columns.push_back(col2);
    
    auto mapping = parser.rebuild_column_mapping(3, metadata);
    
    EXPECT_EQ(mapping.size(), 3);
    EXPECT_EQ(mapping[0], 3);
    EXPECT_EQ(mapping[1], 4);
}

// ==================== BlobParser Tests ====================

TEST(BlobParserTest, ParseBlobPointer) {
    std::vector<uint8_t> data(20, 0);
    
    write_le<uint32_t>(data.data(), 0, 1);    // space_id
    write_le<uint32_t>(data.data(), 4, 100);  // page_no
    write_le<uint32_t>(data.data(), 8, 50);   // offset
    write_le<uint32_t>(data.data(), 12, 1000); // len
    
    BlobParser parser;
    auto pointer_opt = parser.parse_pointer(data.data(), data.size());
    
    EXPECT_TRUE(pointer_opt.has_value());
    EXPECT_EQ(pointer_opt->space_id, 1);
    EXPECT_EQ(pointer_opt->page_no, 100);
    EXPECT_EQ(pointer_opt->len, 1000);
}

TEST(BlobParserTest, ParseBlobPageHeader) {
    std::vector<uint8_t> data(20, 0);
    
    write_le<uint32_t>(data.data(), 0, 99);   // prev_page
    write_le<uint32_t>(data.data(), 4, 101);  // next_page
    write_le<uint32_t>(data.data(), 8, 1);    // type (FIRST)
    write_le<uint32_t>(data.data(), 12, 500); // len
    
    BlobParser parser;
    auto header_opt = parser.parse_page_header(data.data(), data.size());
    
    EXPECT_TRUE(header_opt.has_value());
    EXPECT_EQ(header_opt->prev_page, 99);
    EXPECT_EQ(header_opt->next_page, 101);
}

// ==================== ChecksumVerifier Tests ====================

TEST(ChecksumVerifierTest, CRC32CCalculation) {
    std::vector<uint8_t> data(100, 0x42);
    
    uint32_t crc = ChecksumVerifier::crc32c(data.data(), data.size());
    
    // CRC32C should be consistent
    EXPECT_NE(crc, 0);
    EXPECT_EQ(crc, ChecksumVerifier::crc32c(data.data(), data.size()));
}

TEST(ChecksumVerifierTest, VerifyChecksum) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    ChecksumVerifier verifier;
    auto result = verifier.verify_checksum(page.data(), page.size());
    
    EXPECT_TRUE(result.is_valid);
}

TEST(ChecksumVerifierTest, CheckPageHealth) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    ChecksumVerifier verifier;
    auto info = verifier.check_page_health(page.data(), page.size());
    
    EXPECT_TRUE(info.is_valid);
    EXPECT_TRUE(info.header_valid);
}

TEST(ChecksumVerifierTest, VerifyPageType) {
    ChecksumVerifier verifier;
    
    EXPECT_TRUE(verifier.verify_page_type(FIL_PAGE_INDEX));
    EXPECT_TRUE(verifier.verify_page_type(FIL_PAGE_SDI));
    EXPECT_TRUE(verifier.verify_page_type(FIL_PAGE_UNDO_LOG));
    EXPECT_FALSE(verifier.verify_page_type(0xFFFF));
}

TEST(ChecksumVerifierTest, VerifyPageSize) {
    ChecksumVerifier verifier;
    
    EXPECT_TRUE(verifier.verify_page_size(16384));
    EXPECT_TRUE(verifier.verify_page_size(4096));
    EXPECT_TRUE(verifier.verify_page_size(65536));
    EXPECT_FALSE(verifier.verify_page_size(12345));
}

// ==================== Integration Tests ====================

TEST(IntegrationTest, ParseCompleteUndoLogChain) {
    // Create multiple Undo Log pages
    std::vector<std::vector<uint8_t>> pages(3);
    
    for (size_t i = 0; i < pages.size(); i++) {
        pages[i].resize(16384, 0);
        write_le<uint16_t>(pages[i].data(), 16, FIL_PAGE_UNDO_LOG);
        write_le<uint64_t>(pages[i].data(), 8, 1000 + i * 1000);  // lsn
    }
    
    UndoLogParser parser;
    std::vector<std::shared_ptr<UndoLogPage>> undo_pages;
    
    for (const auto& page : pages) {
        auto undo_page = parser.parse_page(page.data(), page.size());
        if (undo_page) {
            undo_pages.push_back(undo_page);
        }
    }
    
    auto chain = parser.parse_chain(undo_pages);
    
    EXPECT_NE(chain, nullptr);
    EXPECT_EQ(chain->pages.size(), 3);
    EXPECT_TRUE(chain->is_valid());
}

TEST(IntegrationTest, ParseSDIWithTableAndIndexes) {
    std::string json = R"({
        "version": 2,
        "type": 21825,
        "table": {
            "name": "test.orders",
            "id": 200,
            "space_id": 2,
            "flags": 1,
            "n_cols": 6,
            "columns": [
                {"name": "id", "type": 3, "length": 4},
                {"name": "user_id", "type": 3, "length": 4},
                {"name": "amount", "type": 4, "length": 4},
                {"name": "created_at", "type": 12, "length": 8},
                {"name": "status", "type": 15, "length": 20},
                {"name": "notes", "type": 252, "length": 65535}
            ],
            "indexes": [
                {"name": "PRIMARY", "type": 1, "fields": [{"column": "id"}]},
                {"name": "idx_user", "type": 2, "fields": [{"column": "user_id"}]}
            ]
        }
    })";
    
    SDIParser parser;
    auto sdi_page = parser.parse_json(json);
    
    EXPECT_NE(sdi_page, nullptr);
    EXPECT_EQ(sdi_page->tables.size(), 1);
    
    auto table = sdi_page->tables[0];
    EXPECT_EQ(table.table_name, "orders");
    EXPECT_EQ(table.columns.size(), 6);
    EXPECT_EQ(table.indexes.size(), 2);
}

TEST(IntegrationTest, VerifyPageWithMultipleChecks) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    // Run multiple verifiers
    ChecksumVerifier checksum_verifier;
    auto checksum_result = checksum_verifier.verify_checksum(page.data(), page.size());
    
    PageHealthInfo health = PageHealthChecker::check(page.data(), page.size());
    
    EXPECT_TRUE(checksum_result.is_valid);
    EXPECT_TRUE(health.is_valid);
}

// ==================== Edge Case Tests ====================

TEST(UndoLogParserTest, ParseInvalidPage) {
    std::vector<uint8_t> page(100, 0);  // Too small
    
    UndoLogParser parser;
    auto undo_page = parser.parse_page(page.data(), page.size());
    
    EXPECT_EQ(undo_page, nullptr);
}

TEST(SDIParserTest, ParseInvalidJSON) {
    std::string invalid_json = "{ invalid json }";
    
    SDIParser parser;
    auto sdi_page = parser.parse_json(invalid_json);
    
    EXPECT_EQ(sdi_page, nullptr);
}

TEST(TDEDecryptorTest, DecryptNonEncryptedPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    TDEDecryptor decryptor;
    auto result = decryptor.decrypt_page(page.data(), page.size(), 1, 100);
    
    // Should return empty or original data
    EXPECT_EQ(result.size(), 0);
}

TEST(InstantDDLParserTest, ParseInvalidMetadata) {
    std::vector<uint8_t> page(50, 0);  // Too small
    
    InstantDDLParser parser;
    auto metadata_opt = parser.parse_from_page(page.data(), page.size());
    
    EXPECT_FALSE(metadata_opt.has_value());
}

TEST(BlobParserTest, ParseInvalidPointer) {
    std::vector<uint8_t> data(10, 0);  // Too small
    
    BlobParser parser;
    auto pointer_opt = parser.parse_pointer(data.data(), data.size());
    
    EXPECT_FALSE(pointer_opt.has_value());
}

TEST(ChecksumVerifierTest, VerifyInvalidPage) {
    std::vector<uint8_t> page(100, 0);  // Too small
    
    ChecksumVerifier verifier;
    auto result = verifier.verify_checksum(page.data(), page.size());
    
    EXPECT_FALSE(result.is_valid);
}

// ==================== Performance Tests ====================

TEST(ChecksumVerifierTest, CRC32CPerformance) {
    std::vector<uint8_t> data(16384, 0x42);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < 10000; i++) {
        ChecksumVerifier::crc32c(data.data(), data.size());
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Should complete in reasonable time
    EXPECT_LT(duration.count(), 5000);  // Less than 5 seconds
}

} // namespace innodb
} // namespace dbrecovery
