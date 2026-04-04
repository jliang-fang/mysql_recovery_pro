/**
 * @file test_corruption_detector.cpp
 * @brief Tests for corruption detection and repair functionality
 */

#include <gtest/gtest.h>
#include "dbrecovery/repair/corruption_detector.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <vector>
#include <cstring>

using namespace dbrecovery::repair;

// Test corruption detector initialization
TEST(CorruptionDetectorTest, Initialize) {
    CorruptionDetector detector;
    EXPECT_EQ(detector.get_critical_count(), 0);
    EXPECT_EQ(detector.get_warning_count(), 0);
    EXPECT_EQ(detector.get_info_count(), 0);
}

// Test page header validation
TEST(CorruptionDetectorTest, ValidatePageHeader) {
    CorruptionDetector detector;
    detector.set_page_size(16384);

    // Create valid page
    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 50, dbrecovery::innodb::FIL_PAGE_INDEX);

    PageHealth health = detector.analyze_page(page.data(), page.size(), 0);
    EXPECT_TRUE(health.is_valid);
}

// Test invalid page type detection
TEST(CorruptionDetectorTest, DetectInvalidPageType) {
    CorruptionDetector detector;

    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 50, 0xFFFF);  // Invalid type

    auto corruptions = detector.detect_corruption(page.data(), page.size(), 0);
    EXPECT_GT(corruptions.size(), 0);
    EXPECT_EQ(corruptions[0].type, CorruptionType::PAGE_TYPE_INVALID);
}

// Test checksum mismatch detection
TEST(CorruptionDetectorTest, DetectChecksumMismatch) {
    CorruptionDetector detector;

    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 50, dbrecovery::innodb::FIL_PAGE_INDEX);
    dbrecovery::innodb::write_le<uint32_t>(page.data(), 24, 0x12345678);  // Invalid checksum

    auto corruptions = detector.detect_corruption(page.data(), page.size(), 0);
    EXPECT_GT(corruptions.size(), 0);
}

// Test INFIMUM/SUPREMUM corruption detection
TEST(CorruptionDetectorTest, DetectRecordCorruption) {
    CorruptionDetector detector;

    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 50, dbrecovery::innodb::FIL_PAGE_INDEX);

    // Set invalid INFIMUM type
    page[54] = 0x05;  // Invalid type

    auto corruptions = detector.detect_corruption(page.data(), page.size(), 0);
    EXPECT_GT(corruptions.size(), 0);
}

// Test page chain repairer
TEST(PageChainRepairerTest, RepairChain) {
    PageChainRepairer repairer;

    // Create pages with broken chain
    for (uint32_t i = 0; i < 5; ++i) {
        std::vector<uint8_t> page(16384, 0);
        dbrecovery::innodb::write_le<uint32_t>(page.data(), 20, i);  // page_no
        dbrecovery::innodb::write_le<uint16_t>(page.data(), 50, dbrecovery::innodb::FIL_PAGE_INDEX);
        repairer.add_page(i, std::move(page));
    }

    bool success = repairer.repair_chains();
    EXPECT_TRUE(success);
    EXPECT_EQ(repairer.get_stats().total_pages, 5);
}

// Test record chain repairer
TEST(RecordChainRepairerTest, DetectBrokenRecords) {
    RecordChainRepairer repairer;

    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 50, dbrecovery::innodb::FIL_PAGE_INDEX);

    // Set INFIMUM at offset 54
    page[54] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 55, 100);  // next_offset

    // Set SUPREMUM at offset 100
    page[100] = dbrecovery::innodb::REC_STATUS_SUPREMUM;

    repairer.analyze_chain(page.data(), page.size());
    auto stats = repairer.get_stats();
    EXPECT_GT(stats.total_records, 0);
}

// Test corruption statistics
TEST(CorruptionDetectorTest, GetCorruptionStats) {
    CorruptionDetector detector;

    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 50, 0xFFFF);

    detector.detect_corruption(page.data(), page.size(), 0);
    auto stats = detector.get_corruption_stats();

    EXPECT_GT(stats.size(), 0);
    EXPECT_EQ(stats[CorruptionType::PAGE_TYPE_INVALID], 1);
}

// Test clear corruptions
TEST(CorruptionDetectorTest, ClearCorruptions) {
    CorruptionDetector detector;

    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 50, 0xFFFF);

    detector.detect_corruption(page.data(), page.size(), 0);
    EXPECT_GT(detector.get_corruptions().size(), 0);

    detector.clear_corruptions();
    EXPECT_EQ(detector.get_corruptions().size(), 0);
}

// ==================== BLOB Chain Rebuilder Tests ====================

#include "dbrecovery/recovery/blob_chain_rebuilder.hpp"

using namespace dbrecovery::recovery;

TEST(BlobChainRebuilderTest, ParseBlobPointer) {
    BlobChainRebuilder builder;
    builder.set_page_size(16384);

    std::vector<uint8_t> data(20, 0);
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 0, 0);   // space_id
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 4, 100); // page_no
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 8, 0);   // offset
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 12, 50); // len
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 16, 150); // total_len

    auto pointer = builder.parse_blob_pointer(data.data(), data.size());
    EXPECT_TRUE(pointer.has_value());
    EXPECT_EQ(pointer->space_id, 0);
    EXPECT_EQ(pointer->page_no, 100);
    EXPECT_EQ(pointer->total_len, 150);
}

TEST(BlobChainRebuilderTest, AddBlobPage) {
    BlobChainRebuilder builder;
    builder.set_page_size(16384);

    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 16, dbrecovery::innodb::FIL_PAGE_TYPE_BLOB);

    bool success = builder.add_blob_page(0, 100, std::move(page));
    EXPECT_TRUE(success);
    EXPECT_EQ(builder.get_stats().total_pages, 1);
}

TEST(BlobChainRebuilderTest, HasCircularReference) {
    BlobChainRebuilder builder;

    std::vector<uint32_t> chain = {100, 200, 300, 100};  // Circular
    EXPECT_TRUE(builder.has_circular_reference(chain));

    std::vector<uint32_t> linear = {100, 200, 300, 400};
    EXPECT_FALSE(builder.has_circular_reference(linear));
}

TEST(BlobPageParserTest, ParsePage) {
    BlobPageParser parser;

    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint32_t>(page.data(), 38, 0xFFFFFFFF);  // next_page
    dbrecovery::innodb::write_le<uint32_t>(page.data(), 42, 100);  // total_length
    dbrecovery::innodb::write_le<uint32_t>(page.data(), 46, 50);   // current_length

    auto parsed = parser.parse_page(0, 100, page.data(), page.size());
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->page_no, 100);
}

// ==================== Undo Log Parser Tests ====================

#include "dbrecovery/innodb/undo_log_parser.hpp"

using namespace dbrecovery::innodb;

TEST(UndoLogParserTest, ParseUndoRecord) {
    UndoLogParser parser;
    parser.set_page_size(16384);

    std::vector<uint8_t> data(30, 0);
    dbrecovery::innodb::write_le<uint64_t>(data.data(), 0, 12345);      // trx_id
    dbrecovery::innodb::write_le<uint64_t>(data.data(), 8, 67890);      // roll_ptr
    dbrecovery::innodb::write_le<uint16_t>(data.data(), 16, 1);         // type
    dbrecovery::innodb::write_le<uint16_t>(data.data(), 18, 4);         // data_len
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 20, 999);       // data

    size_t pos = 0;
    auto record = parser.parse_record(data.data(), data.size(), pos);
    EXPECT_TRUE(record.has_value());
    EXPECT_EQ(record->u_trx_id, 12345);
    EXPECT_EQ(record->u_type, 1);
}

TEST(UndoLogParserTest, ParseUndoPage) {
    UndoLogParser parser;

    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 16, dbrecovery::innodb::FIL_PAGE_UNDO_LOG);
    dbrecovery::innodb::write_le<uint64_t>(page.data(), 38, 12345);  // trx_id

    auto parsed = parser.parse_page(page.data(), page.size(), 0);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->page_type, dbrecovery::innodb::FIL_PAGE_UNDO_LOG);
}

// ==================== Redo Log Replayer Tests ====================

#include "dbrecovery/innodb/redo_log_replayer.hpp"

using namespace dbrecovery::innodb;

TEST(RedoLogReplayerTest, ApplyUpdateRecord) {
    RedoLogReplayer replayer;
    replayer.set_page_size(16384);

    // Create a page
    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 50, dbrecovery::innodb::FIL_PAGE_INDEX);
    replayer.add_page(0, std::move(page));

    // Create update record
    RedoLogRecord record;
    record.r_type = MLOG_2BYTES;
    record.r_space_id = 0;
    record.r_page_no = 0;

    // Add update data: offset(2) + value(2)
    std::vector<uint8_t> data(4);
    dbrecovery::innodb::write_le<uint16_t>(data.data(), 0, 100);  // offset
    dbrecovery::innodb::write_le<uint16_t>(data.data(), 2, 0x1234); // value
    record.r_data = std::move(data);

    bool success = replayer.apply_redo_record(0, record);
    EXPECT_TRUE(success);
}

TEST(RedoLogReplayerTest, ApplyDeleteRecord) {
    RedoLogReplayer replayer;
    replayer.set_page_size(16384);

    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 50, dbrecovery::innodb::FIL_PAGE_INDEX);
    replayer.add_page(0, std::move(page));

    RedoLogRecord record;
    record.r_type = MLOG_REC_DELETE;
    record.r_space_id = 0;
    record.r_page_no = 0;

    std::vector<uint8_t> data(2);
    dbrecovery::innodb::write_le<uint16_t>(data.data(), 0, 54);  // offset
    record.r_data = std::move(data);

    bool success = replayer.apply_redo_record(0, record);
    EXPECT_TRUE(success);
}

TEST(RedoLogFilterTest, FilterBySpaceId) {
    RedoLogFilter filter;
    filter.set_space_id(100);

    std::vector<RedoLogRecord> records;
    RedoLogRecord r1, r2;
    r1.r_space_id = 100;
    r2.r_space_id = 200;
    records.push_back(r1);
    records.push_back(r2);

    auto filtered = filter.filter(records);
    EXPECT_EQ(filtered.size(), 1);
    EXPECT_EQ(filtered[0].r_space_id, 100);
}

TEST(RedoLogAnalyzerTest, AnalyzeFile) {
    RedoLogAnalyzer analyzer;
    // Note: This would require a real redo log file for testing
    // For now, just test the interface
    auto result = analyzer.get_analysis();
    EXPECT_EQ(result.total_records, 0);
}

// ==================== TDE Decryptor Tests ====================

#include "dbrecovery/innodb/tde_decryptor.hpp"

using namespace dbrecovery::innodb;

TEST(TDEDecryptorTest, IsEncrypted) {
    std::vector<uint8_t> page(16384, 0);
    page[0] = 0x01;  // TDE algorithm marker

    bool encrypted = TDEDecryptor::is_encrypted(page.data(), page.size());
    EXPECT_TRUE(encrypted);
}

TEST(TDEDecryptorTest, NotEncrypted) {
    std::vector<uint8_t> page(16384, 0);
    page[0] = 0xFF;  // Not a valid TDE algorithm

    bool encrypted = TDEDecryptor::is_encrypted(page.data(), page.size());
    EXPECT_FALSE(encrypted);
}

TEST(TDEDecryptorTest, AddTableKey) {
    TDEDecryptor decryptor;
    std::vector<uint8_t> key(32, 0x42);  // 256-bit key

    decryptor.add_table_key(1, key, TDEAlgorithm::AES_256_CBC);
    auto stats = decryptor.get_stats();
    EXPECT_EQ(stats.keys_loaded, 1);
}

// ==================== Page Chain Repairer Tests ====================

TEST(PageChainRepairerTest, GetStats) {
    PageChainRepairer repairer;

    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 50, dbrecovery::innodb::FIL_PAGE_INDEX);
    repairer.add_page(0, std::move(page));

    repairer.repair_chains();
    auto stats = repairer.get_stats();

    EXPECT_EQ(stats.total_pages, 1);
    EXPECT_EQ(stats.valid_chains, 1);
}

// ==================== Record Chain Repairer Tests ====================

TEST(RecordChainRepairerTest, RepairChain) {
    RecordChainRepairer repairer;

    std::vector<uint8_t> page(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 50, dbrecovery::innodb::FIL_PAGE_INDEX);

    // Set up valid record chain
    page[54] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 55, 100);

    page[100] = dbrecovery::innodb::REC_STATUS_SUPREMUM;

    repairer.analyze_chain(page.data(), page.size());
    auto stats = repairer.get_stats();

    EXPECT_GT(stats.total_records, 0);
}
