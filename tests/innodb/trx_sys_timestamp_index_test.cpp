#include <gtest/gtest.h>
#include "dbrecovery/innodb/trx_sys_timestamp_index.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <fstream>
#include <vector>
#include <cstdio>

namespace dbrecovery {
namespace innodb {

// Helper: create a realistic ibdata1 with TRX_SYS page and rseg header pages
// Returns the temp file path
static std::string create_temp_ibdata_with_trx_sys(
    size_t page_size = 16384,
    uint32_t n_active_rsegs = 3,
    const std::vector<uint64_t>& trx_ids = {100, 200, 300},
    const std::vector<uint64_t>& commit_lsns = {50000, 60000, 70000}) {

    std::string path = "/tmp/test_trx_sys_index.bin";

    // TRX_SYS is page 5. We'll place rseg header pages at pages 6, 7, 8, ...
    // TRX_SYS page: rseg slots at offset 108, 8 bytes each (LE space_id + page_no)
    // 128 slots, but only n_active_rsegs are populated

    uint32_t n_rsegs = std::min(n_active_rsegs, 128u);
    uint32_t rseg_start_page = 6;  // First rseg header page

    // Total file size: enough for TRX_SYS page + rseg header pages
    uint32_t max_page = rseg_start_page + n_rsegs;
    std::vector<uint8_t> file(page_size * (max_page + 1), 0);

    // --- TRX_SYS page (page 5) ---
    // Set FIL page type to a valid type (not explicitly checked, but good practice)
    write_be<uint16_t>(file.data(), 5 * page_size + 24, 0x0000);  // ALLOCATED is fine

    // Write rseg slots at offset 108
    for (uint32_t i = 0; i < n_rsegs; i++) {
        size_t slot_offset = 5 * page_size + 108 + i * 8;
        write_le<uint32_t>(file.data(), slot_offset, 0);                                 // space_id = 0
        write_le<uint32_t>(file.data(), slot_offset + 4, rseg_start_page + i);           // page_no
    }

    // --- Rseg header pages ---
    for (uint32_t i = 0; i < n_rsegs; i++) {
        size_t page_offset = static_cast<size_t>(rseg_start_page + i) * page_size;
        // Set page type
        write_be<uint16_t>(file.data(), page_offset + 24, 0x0000);

        // Undo segment header is at FIL_PAGE_DATA(38) + TRX_UNDO_SEG_HDR(32) = offset 70
        // Layout (MySQL 8.0+):
        //   +0:  TRX_UNDO_STATE (2 bytes) = 1 (active)
        //   +4:  TRX_UNDO_LAST_COMMIT (8 bytes, LE) = commit_lsn
        //   +12: TRX_UNDO_TRX_ID (6 bytes, LE) = trx_id
        size_t seg_hdr = page_offset + 38 + 32;
        write_le<uint16_t>(file.data(), seg_hdr, 1);                                    // TRX_UNDO_STATE = active
        write_le<uint64_t>(file.data(), seg_hdr + 4, commit_lsns[i]);                   // TRX_UNDO_LAST_COMMIT
        // TRX_UNDO_TRX_ID: 6 bytes LE
        write_le<uint32_t>(file.data(), seg_hdr + 12, static_cast<uint32_t>(trx_ids[i] & 0xFFFFFFFF));
        write_le<uint16_t>(file.data(), seg_hdr + 16, static_cast<uint16_t>((trx_ids[i] >> 32) & 0xFFFF));
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), file.size());
    out.close();
    return path;
}

// ==================== Build Tests ====================

TEST(TrxSysTimestampIndexTest, BuildWithActiveTransactions) {
    std::string path = create_temp_ibdata_with_trx_sys(16384, 3, {100, 200, 300}, {50000, 60000, 70000});

    TrxSysTimestampIndex index;
    bool result = index.build(path, 16384);

    EXPECT_TRUE(result);
    EXPECT_TRUE(index.is_built());
    EXPECT_EQ(index.size(), 3u);

    // Verify lookups
    auto entry = index.lookup(100);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->trx_id, 100u);
    EXPECT_EQ(entry->commit_lsn, 50000u);
    EXPECT_EQ(entry->rseg_id, 0u);

    entry = index.lookup(200);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->trx_id, 200u);
    EXPECT_EQ(entry->commit_lsn, 60000u);

    entry = index.lookup(300);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->trx_id, 300u);
    EXPECT_EQ(entry->commit_lsn, 70000u);

    std::remove(path.c_str());
}

TEST(TrxSysTimestampIndexTest, BuildNoActiveTransactions) {
    // Create ibdata with no active rseg slots
    std::string path = create_temp_ibdata_with_trx_sys(16384, 0, {}, {});

    TrxSysTimestampIndex index;
    bool result = index.build(path, 16384);

    EXPECT_TRUE(result);
    EXPECT_TRUE(index.is_built());
    EXPECT_EQ(index.size(), 0u);

    std::remove(path.c_str());
}

TEST(TrxSysTimestampIndexTest, BuildNonExistentFile) {
    TrxSysTimestampIndex index;
    bool result = index.build("/nonexistent/path/ibdata1", 16384);
    EXPECT_FALSE(result);
    EXPECT_FALSE(index.is_built());
    EXPECT_EQ(index.size(), 0u);
}

// ==================== Lookup Tests ====================

TEST(TrxSysTimestampIndexTest, LookupNotFound) {
    std::string path = create_temp_ibdata_with_trx_sys(16384, 2, {100, 200}, {50000, 60000});

    TrxSysTimestampIndex index;
    index.build(path, 16384);

    auto entry = index.lookup(999);
    EXPECT_FALSE(entry.has_value());

    std::remove(path.c_str());
}

TEST(TrxSysTimestampIndexTest, LookupBeforeBuild) {
    TrxSysTimestampIndex index;
    auto entry = index.lookup(100);
    EXPECT_FALSE(entry.has_value());
}

// ==================== Bulk Lookup Tests ====================

TEST(TrxSysTimestampIndexTest, BulkLookup) {
    std::string path = create_temp_ibdata_with_trx_sys(16384, 3, {100, 200, 300}, {50000, 60000, 70000});

    TrxSysTimestampIndex index;
    index.build(path, 16384);

    std::vector<uint64_t> trx_ids = {100, 300, 999};
    auto results = index.lookup_bulk(trx_ids);

    EXPECT_EQ(results.size(), 2u);
    EXPECT_TRUE(results.find(100) != results.end());
    EXPECT_TRUE(results.find(300) != results.end());
    EXPECT_TRUE(results.find(999) == results.end());

    EXPECT_EQ(results[100].commit_lsn, 50000u);
    EXPECT_EQ(results[300].commit_lsn, 70000u);

    std::remove(path.c_str());
}

TEST(TrxSysTimestampIndexTest, BulkLookupEmpty) {
    std::string path = create_temp_ibdata_with_trx_sys(16384, 1, {100}, {50000});

    TrxSysTimestampIndex index;
    index.build(path, 16384);

    std::vector<uint64_t> empty;
    auto results = index.lookup_bulk(empty);
    EXPECT_TRUE(results.empty());

    std::remove(path.c_str());
}

// ==================== Management Tests ====================

TEST(TrxSysTimestampIndexTest, Reset) {
    std::string path = create_temp_ibdata_with_trx_sys(16384, 2, {100, 200}, {50000, 60000});

    TrxSysTimestampIndex index;
    index.build(path, 16384);
    EXPECT_EQ(index.size(), 2u);
    EXPECT_TRUE(index.is_built());

    index.reset();
    EXPECT_EQ(index.size(), 0u);
    EXPECT_FALSE(index.is_built());

    std::remove(path.c_str());
}

TEST(TrxSysTimestampIndexTest, RebuildAfterReset) {
    std::string path = create_temp_ibdata_with_trx_sys(16384, 2, {100, 200}, {50000, 60000});

    TrxSysTimestampIndex index;
    index.build(path, 16384);
    ASSERT_EQ(index.size(), 2u);

    index.reset();
    bool result = index.build(path, 16384);
    EXPECT_TRUE(result);
    EXPECT_EQ(index.size(), 2u);

    std::remove(path.c_str());
}

// ==================== Statistics Tests ====================

TEST(TrxSysTimestampIndexTest, Stats) {
    std::string path = create_temp_ibdata_with_trx_sys(16384, 3, {100, 200, 300}, {50000, 60000, 70000});

    TrxSysTimestampIndex index;
    index.build(path, 16384);

    const auto& stats = index.stats();
    EXPECT_EQ(stats.rseg_slots_found, 3u);
    EXPECT_EQ(stats.entries_found, 3u);
    EXPECT_GT(stats.build_time_ms, 0.0);

    std::remove(path.c_str());
}

// ==================== Build from Buffer Tests ====================

TEST(TrxSysTimestampIndexTest, BuildFromBuffer) {
    // Create ibdata with TRX_SYS page and rseg pages
    std::string path = create_temp_ibdata_with_trx_sys(16384, 2, {100, 200}, {50000, 60000});

    // Read TRX_SYS page into buffer
    std::vector<uint8_t> trx_sys_page(16384, 0);
    {
        std::ifstream file(path, std::ios::binary);
        file.seekg(5 * 16384, std::ios::beg);
        file.read(reinterpret_cast<char*>(trx_sys_page.data()), 16384);
    }

    TrxSysTimestampIndex index;
    bool result = index.build_from_buffer(trx_sys_page, path, 16384);

    EXPECT_TRUE(result);
    EXPECT_TRUE(index.is_built());
    EXPECT_EQ(index.size(), 2u);

    auto entry = index.lookup(100);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->commit_lsn, 50000u);

    std::remove(path.c_str());
}

// ==================== MySQL 5.6/5.7 Fallback Tests ====================

TEST(TrxSysTimestampIndexTest, MySQL56Fallback) {
    // MySQL 5.6/5.7: offsets are swapped (trx_id at +4, commit_lsn at +12)
    size_t page_size = 16384;
    std::string path = "/tmp/test_trx_sys_56.bin";

    uint32_t rseg_start_page = 6;
    uint32_t n_rsegs = 2;

    // TRX_SYS page 5 + 2 rseg pages
    uint32_t max_page = rseg_start_page + n_rsegs;
    std::vector<uint8_t> file(page_size * (max_page + 1), 0);

    // Write rseg slots
    for (uint32_t i = 0; i < n_rsegs; i++) {
        size_t slot_offset = 5 * page_size + 108 + i * 8;
        write_le<uint32_t>(file.data(), slot_offset, 0);
        write_le<uint32_t>(file.data(), slot_offset + 4, rseg_start_page + i);
    }

    // Write rseg header pages with 5.6/5.7 layout
    for (uint32_t i = 0; i < n_rsegs; i++) {
        size_t page_offset = static_cast<size_t>(rseg_start_page + i) * page_size;
        size_t seg_hdr = page_offset + 38 + 32;

        // MySQL 5.6/5.7: trx_id at +4, commit_lsn at +12
        write_le<uint16_t>(file.data(), seg_hdr, 1);                          // state = active
        // trx_id at +4 (6 bytes LE)
        uint64_t trx_id = 100 + i;
        write_le<uint32_t>(file.data(), seg_hdr + 4, static_cast<uint32_t>(trx_id & 0xFFFFFFFF));
        write_le<uint16_t>(file.data(), seg_hdr + 8, static_cast<uint16_t>((trx_id >> 32) & 0xFFFF));
        // commit_lsn at +12 (8 bytes LE) — set to 0 so the 8.0+ layout detection
        // path reads trx_id=0 from this position, falls through to 5.6/5.7
        uint64_t commit_lsn = 0;
        write_le<uint64_t>(file.data(), seg_hdr + 12, commit_lsn);
    }

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data()), file.size());
    }

    TrxSysTimestampIndex index;
    bool result = index.build(path, page_size);

    // Should detect the 5.6/5.7 layout via fallback
    EXPECT_TRUE(result);
    EXPECT_TRUE(index.is_built());
    EXPECT_EQ(index.size(), 2u);

    auto entry = index.lookup(100);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->trx_id, 100u);
    EXPECT_EQ(entry->commit_lsn, 0u);

    entry = index.lookup(101);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->commit_lsn, 0u);

    std::remove(path.c_str());
}

// ==================== Edge Cases ====================

TEST(TrxSysTimestampIndexTest, All128RsegSlots) {
    // Test with all 128 rseg slots populated
    size_t page_size = 16384;
    uint32_t n_rsegs = 128;
    uint32_t rseg_start = 6;

    std::vector<uint64_t> trx_ids, commit_lsns;
    for (uint32_t i = 0; i < n_rsegs; i++) {
        trx_ids.push_back(1000 + i);
        commit_lsns.push_back(100000 + i * 1000);
    }

    std::string path = create_temp_ibdata_with_trx_sys(page_size, n_rsegs, trx_ids, commit_lsns);

    TrxSysTimestampIndex index;
    bool result = index.build(path, page_size);

    EXPECT_TRUE(result);
    EXPECT_EQ(index.size(), 128u);

    // Spot check a few entries
    auto entry = index.lookup(1000);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->commit_lsn, 100000u);

    entry = index.lookup(1127);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->commit_lsn, 100000u + 127 * 1000);

    std::remove(path.c_str());
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}