#include <gtest/gtest.h>
#include "dbrecovery/innodb/undo_log_parser.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <vector>
#include <fstream>
#include <iostream>

namespace dbrecovery {
namespace innodb {

// ==================== Undo Log Page Parsing Tests ====================

// Test basic undo log page with records
TEST(UndoLogPageTest, ParseUndoLogPage) {
    // Create a minimal undo log page
    std::vector<uint8_t> page(16384, 0);

    // Set page type to UNDO_LOG
    write_le<uint16_t>(page.data(), 0, 1);  // TRX_UNDO_PAGE_TYPE = 1

    // Write undo log page header
    write_le<uint16_t>(page.data(), 28, 40);  // TRX_UNDO_PAGE_START
    write_le<uint16_t>(page.data(), 30, 80);  // TRX_UNDO_PAGE_FREE
    write_le<uint16_t>(page.data(), 34, 2);   // TRX_UNDO_PAGE_N_RECS = 2

    // Add two undo records at offset 40
    page[40] = 1;   // Record 1 type: TRX_UNDO_INSERT
    page[41] = 2;   // 2 fields
    page[42] = 2;   // Record 2 type: TRX_UNDO_DELETE
    page[43] = 1;   // 1 field

    UndoLogParser parser;
    auto entries = parser.parse_undo_log_page(page.data(), 16384, 0);

    EXPECT_FALSE(entries.empty());
    EXPECT_EQ(entries.size(), 2);
}

// Test empty undo log page
TEST(UndoLogPageTest, EmptyUndoLogPage) {
    std::vector<uint8_t> page(16384, 0);

    // No records
    write_le<uint16_t>(page.data(), 34, 0);  // TRX_UNDO_PAGE_N_RECS = 0

    UndoLogParser parser;
    auto entries = parser.parse_undo_log_page(page.data(), 16384, 0);

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

// Test page recovery with valid undo log
TEST(UndoLogRecoveryTest, RecoverFromValidUndoLog) {
    std::vector<uint8_t> page(16384, 0);

    // Set page type
    write_le<uint16_t>(page.data(), 0, 1);

    // Write undo log page header
    write_le<uint16_t>(page.data(), 28, 40);
    write_le<uint16_t>(page.data(), 30, 60);
    write_le<uint16_t>(page.data(), 34, 1);

    // Add a delete record
    page[40] = 13;  // TRX_UNDO_DEL_REC = 13
    page[41] = 2;   // 2 fields

    UndoLogParser parser;
    auto entries = parser.parse_undo_log_page(page.data(), 16384, 0);

    EXPECT_FALSE(entries.empty());
    EXPECT_EQ(entries[0].undo_type, 13);  // TRX_UNDO_DEL_REC
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
