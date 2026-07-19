// Test cases for Undo Log transaction rollback
// These tests verify the modified UndoLogParser API

#include <gtest/gtest.h>
#include "dbrecovery/innodb/undo_log_parser.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Undo Log Transaction Rollback Test Cases ====================

// Test Case 1: Undo log page parsing with valid records
TEST(UndoLogRollbackTest, ValidUndoLogParsing) {
    // Create a minimal undo log page with just the basic header
    std::vector<uint8_t> page(16384, 0);

    // Write undo log page header
    // TRX_UNDO_PAGE_TYPE at offset 0
    write_le<uint16_t>(page.data(), 0, 1);  // TRX_UNDO_PAGE_TYPE = 1 (UNDO log)
    
    // TRX_UNDO_PAGE_START at offset 28 (start of undo log records)
    write_le<uint16_t>(page.data(), 28, 40);  // Start after header
    
    // TRX_UNDO_PAGE_FREE at offset 30
    write_le<uint16_t>(page.data(), 30, 60);  // Free space
    
    // TRX_UNDO_PAGE_N_RECS at offset 34
    write_le<uint16_t>(page.data(), 34, 1);  // 1 record

    // Add a simple undo record at offset 40
    // Format: type(1) + field_count(1) + length-prefixed fields
    page[40] = 1;
    page[41] = 2;
    page[42] = 1;
    page[43] = 'a';
    page[44] = 1;
    page[45] = 'b';

    write_le<uint16_t>(page.data(), 30, 46);

    UndoLogParser parser;
    auto entries = parser.parse_undo_log_page(page.data(), 16384, 0);

    EXPECT_FALSE(entries.empty());
    EXPECT_EQ(entries[0].undo_type, 1);  // TRX_UNDO_INSERT
    EXPECT_EQ(entries[0].field_count, 2);
}

// Test Case 2: Undo log page parsing with empty page
TEST(UndoLogRollbackTest, EmptyUndoLogPage) {
    std::vector<uint8_t> page(16384, 0);
    
    // Write undo log page header with no records
    write_le<uint16_t>(page.data(), 0, 1);   // TRX_UNDO_PAGE_TYPE
    write_le<uint16_t>(page.data(), 28, 40); // TRX_UNDO_PAGE_START
    write_le<uint16_t>(page.data(), 30, 40); // TRX_UNDO_PAGE_FREE (no records)
    write_le<uint16_t>(page.data(), 34, 0);  // TRX_UNDO_PAGE_N_RECS = 0

    UndoLogParser parser;
    auto entries = parser.parse_undo_log_page(page.data(), 16384, 0);

    EXPECT_TRUE(entries.empty());
}

// Test Case 3: Parse rollback segment with non-existent file
TEST(UndoLogRollbackTest, NonExistentFile) {
    UndoLogParser parser;
    auto entries = parser.parse_rollback_segment("/nonexistent/path/ibdata1", 0);
    EXPECT_TRUE(entries.empty());
}

// Test Case 4: Scan all rollback segments (no-op on non-existent file)
TEST(UndoLogRollbackTest, ScanAllRollbackSegments) {
    UndoLogParser parser;
    size_t total = parser.scan_all_rollback_segments("/nonexistent/path/ibdata1");
    EXPECT_EQ(total, 0u);
}

} // namespace innodb
} // namespace dbrecovery
