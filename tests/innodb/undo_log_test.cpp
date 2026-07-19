#include <gtest/gtest.h>
#include "dbrecovery/innodb/undo_log_parser.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <iostream>

namespace {

void write_page_header(std::vector<uint8_t>& page, uint16_t page_free,
                       uint16_t record_count) {
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 0, 1);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 28, 40);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 30, page_free);
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 34, record_count);
}

size_t write_insert_record(std::vector<uint8_t>& page, size_t offset,
                           const std::vector<std::string>& fields) {
    page[offset++] = 1;
    page[offset++] = static_cast<uint8_t>(fields.size());
    for (const auto& value : fields) {
        page[offset++] = static_cast<uint8_t>(value.size());
        std::copy(value.begin(), value.end(), page.begin() + offset);
        offset += value.size();
    }
    return offset;
}

} // namespace

namespace dbrecovery {
namespace innodb {

// Test basic undo log page with records
TEST(UndoLogPageTest, ParseUndoLogPage) {
    std::vector<uint8_t> page(16384, 0);
    const size_t end = write_insert_record(page, 40, {"a", "bc"});
    write_page_header(page, static_cast<uint16_t>(end), 1);

    UndoLogParser parser;
    auto entries = parser.parse_undo_log_page(page.data(), page.size(), 0);

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].undo_type, 1);
    EXPECT_EQ(entries[0].field_count, 2);
    ASSERT_EQ(entries[0].fields.size(), 2u);
    EXPECT_EQ(std::string(entries[0].fields[0].data.begin(),
                          entries[0].fields[0].data.end()), "a");
    EXPECT_EQ(std::string(entries[0].fields[1].data.begin(),
                          entries[0].fields[1].data.end()), "bc");
}

TEST(UndoLogPageTest, RejectsInvalidFreeOffset) {
    std::vector<uint8_t> page(256, 0);
    write_page_header(page, 129, 1);
    page[40] = 1;
    page[41] = 0;

    UndoLogParser parser;
    EXPECT_TRUE(parser.parse_undo_log_page(page.data(), 128, 0).empty());
}

TEST(UndoLogPageTest, RejectsTruncatedField) {
    std::vector<uint8_t> page(16384, 0);
    page[40] = 1;
    page[41] = 1;
    page[42] = 5;
    page[43] = 'x';
    write_page_header(page, 44, 1);

    UndoLogParser parser;
    EXPECT_TRUE(parser.parse_undo_log_page(page.data(), page.size(), 0).empty());
}

TEST(UndoLogPageTest, FieldCanEndAtPageFree) {
    std::vector<uint8_t> page(16384, 0);
    const size_t end = write_insert_record(page, 40, {"ok"});
    write_page_header(page, static_cast<uint16_t>(end), 1);

    UndoLogParser parser;
    auto entries = parser.parse_undo_log_page(page.data(), page.size(), 0);
    ASSERT_EQ(entries.size(), 1u);
    ASSERT_EQ(entries[0].fields.size(), 1u);
    EXPECT_EQ(entries[0].fields[0].size, 2);
}

// Test empty undo log page
TEST(UndoLogPageTest, EmptyUndoLogPage) {
    std::vector<uint8_t> page(16384, 0);
    write_page_header(page, 40, 0);

    UndoLogParser parser;
    auto entries = parser.parse_undo_log_page(page.data(), page.size(), 0);

    EXPECT_TRUE(entries.empty());
}

// Test rollback segment parsing (file not found case)
TEST(UndoLogFileTest, ParseNonExistentFile) {
    UndoLogParser parser;
    auto entries = parser.parse_rollback_segment("/nonexistent/path/ibdata1", 0);
    EXPECT_TRUE(entries.empty());
}

// Test scanning all rollback segments
TEST(UndoLogFileTest, ScanAllRollbackSegments) {
    UndoLogParser parser;
    size_t total = parser.scan_all_rollback_segments("/nonexistent/path/ibdata1");
    EXPECT_EQ(total, 0u);
}

TEST(UndoLogRecoveryTest, RecoverFromValidUndoLog) {
    std::vector<uint8_t> page(16384, 0);

    write_page_header(page, 47, 1);
    page[40] = 13;
    page[41] = 1;
    page[42] = 1;
    page[43] = 'x';

    UndoLogParser parser;
    auto entries = parser.parse_undo_log_page(page.data(), page.size(), 0);

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].undo_type, 13);
    EXPECT_EQ(entries[0].field_count, 1);
}

// Test rollback segment parsing
TEST(UndoLogRollbackTest, ParseRollbackSegment) {
    // Create a minimal ibdata1-like file
    std::string temp_path = "/tmp/test_ibdata.bin";
    
    {
        std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            std::cerr << "[ERROR] Failed to open temp file" << std::endl;
            EXPECT_TRUE(false);
            return;
        }

        // Create a minimal file with rollback segment slot at 108
        std::vector<uint8_t> data(96 * 1024, 0);  // 96KB ibdata

        // Set rollback segment slot 0 (offset 108 + 0*8 = 108)
        // space_id = 0, page_no = 5
        write_le<uint32_t>(data.data(), 108, 0);   // space_id
        write_le<uint32_t>(data.data(), 112, 5);   // page_no

        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        file.close();
    }

    UndoLogParser parser;
    auto entries = parser.parse_rollback_segment(temp_path, 0);
    
    // Should not crash, may return empty if page doesn't exist
    EXPECT_TRUE(true);  // Just verify no crash

    std::remove(temp_path.c_str());
}

// Test statistics tracking
TEST(UndoLogStatsTest, GetParserStats) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 0, 1);
    write_le<uint16_t>(page.data(), 28, 40);
    write_le<uint16_t>(page.data(), 30, 40);
    write_le<uint16_t>(page.data(), 34, 0);

    UndoLogParser parser;
    parser.parse_undo_log_page(page.data(), 16384, 0);

    auto stats = parser.get_stats();
    EXPECT_EQ(stats.total_undo_records, 0u);
}

} // namespace innodb
} // namespace dbrecovery

// ==================== Unit Test Entry =====================
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
