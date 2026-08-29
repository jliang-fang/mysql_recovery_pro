/**
 * @file partition_table_recovery_test.cpp
 * @brief Focused tests for PartitionTableRecovery P1 state semantics
 *
 * Verifies that caller-configured partitions survive recover() (only
 * per-run records/statistics are reset) and that an INDEX page is
 * recognized only by its canonical big-endian FIL page type.
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <string>
#include <fstream>
#include <filesystem>

#include "dbrecovery/innodb/partition_table_recovery.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

using namespace dbrecovery;
using namespace dbrecovery::innodb;

namespace {

std::string make_temp_file(const std::string& name, const std::vector<uint8_t>& data) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    out.close();
    return path.string();
}

/// @brief Build an INDEX leaf page with one live user record.
/// @param canonical_be true: FIL_PAGE_INDEX (0x45BF) big-endian at
/// FIL_PAGE_TYPE_ABS; false: the same bytes in little-endian order.
/// @param space_id page-0 space_id written (canonical big-endian) at
/// FIL_PAGE_SPACE_ID when non-zero — identity gate requires the partition
/// tablespace_id to match.
std::vector<uint8_t> build_index_page(bool canonical_be, uint32_t space_id = 0) {
    std::vector<uint8_t> page(16384, 0);

    if (canonical_be) {
        write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    } else {
        // Swapped byte order: a big-endian read at FIL_PAGE_TYPE_ABS
        // yields 0xBF45, which is not a recognized FIL page type.
        page[FIL_PAGE_TYPE_ABS]     = static_cast<uint8_t>(FIL_PAGE_INDEX & 0xFF);
        page[FIL_PAGE_TYPE_ABS + 1] = static_cast<uint8_t>(FIL_PAGE_INDEX >> 8);
    }

    if (space_id != 0) {
        write_be<uint32_t>(page.data(), FIL_PAGE_SPACE_ID, space_id);
    }

    // Page header (relative to FIL_HEADER_SIZE = 38): n_dir_slots at 38,
    // heap_top at 40, n_heap at 42.
    write_le<uint16_t>(page.data(), 38, 3);     // n_dir_slots
    write_le<uint16_t>(page.data(), 40, 150);   // heap_top
    write_le<uint16_t>(page.data(), 42, 5);     // n_heap

    // INFIMUM -> deleted user record @75 -> live user record @99 -> SUPREMUM @120.
    page[54] = REC_STATUS_INFIMUM;
    write_le<uint16_t>(page.data(), 55, 75);

    page[75] = REC_INFO_DELETED_FLAG;
    write_le<uint16_t>(page.data(), 76, 99);

    page[99] = 0x00;
    write_le<uint16_t>(page.data(), 100, 120);

    page[120] = REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page.data(), 121, 0xFFFF);

    return page;
}

catalog::TableSchema make_schema() {
    catalog::TableSchema schema;
    schema.table_name = "partition_test";
    schema.row_format = 0;  // COMPACT

    catalog::ColumnInfo col;
    col.name = "id";
    col.mysql_type_id = mysql_types::LONGLONG;
    col.is_nullable = false;
    schema.columns[1] = col;
    return schema;
}

PartitionInfo make_partition(const std::string& name, uint32_t id,
                             const std::string& path) {
    PartitionInfo p;
    p.partition_name = name;
    p.partition_id = id;
    p.tablespace_id = id + 10;
    p.ibd_path = path;
    p.first_page_no = 0;
    p.last_page_no = 0;
    return p;
}

}  // namespace

// ==================== P1: configured partitions survive recover() ====================

TEST(PartitionTableRecoveryTest, ConfiguredPartitionsSurviveRecover) {
    PartitionTableRecovery engine;
    engine.add_partition(make_partition("p0", 0, "/nonexistent/p0.ibd"));
    engine.add_partition(make_partition("p1", 1, "/nonexistent/p1.ibd"));

    auto records = engine.recover();
    EXPECT_TRUE(records.empty());

    // Per-run state resets, but caller-configured partitions must persist.
    EXPECT_EQ(engine.get_partitions().size(), 2);
    EXPECT_EQ(engine.get_stats().total_partitions, 2);

    // Partitions added after a run must not be lost by the next run either.
    engine.add_partition(make_partition("p2", 2, "/nonexistent/p2.ibd"));
    engine.recover();
    EXPECT_EQ(engine.get_partitions().size(), 3);
    EXPECT_EQ(engine.get_stats().total_partitions, 3);
}

// ==================== P0: scanner populates tablespace_id from page 0 ====================

TEST(PartitionTableRecoveryTest, ScannerPopulatesTablespaceIdFromPageZero) {
    // Build an IBD whose page 0 carries FIL_PAGE_SPACE_ID = 123.
    // The scanner must read page 0 and fill PartitionInfo::tablespace_id;
    // previously it defaulted to 0, causing the identity gate to reject
    // all non-zero space-id pages.
    auto page0 = build_index_page(true, 123);
    std::string ibd_path = make_temp_file("dbrec_t1#P#p0.ibd", page0);

    // Place the file in a temp directory so the scanner can discover it.
    auto dir = std::filesystem::temp_directory_path() / "dbrec_part_scan_test";
    std::filesystem::create_directories(dir);
    auto dest = dir / "dbrec_t1#P#p0.ibd";
    std::filesystem::copy_file(ibd_path, dest, std::filesystem::copy_options::overwrite_existing);

    PartitionTableScanner scanner(16384);
    scanner.set_data_directory(dir.string());
    auto partitions = scanner.get_partitions("dbrec_t1");

    ASSERT_EQ(partitions.size(), 1);
    EXPECT_EQ(partitions[0].tablespace_id, 123u);

    std::filesystem::remove_all(dir);
}

// ==================== P1: INDEX page recognized only in canonical byte order ====================

TEST(PartitionTableRecoveryTest, IndexPageRecognizedOnlyInCanonicalByteOrder) {
    // The canonical partition p0 has tablespace_id = 10; its page must carry
    // FIL_PAGE_SPACE_ID = 10 (big-endian at offset 34) or the Phase 5d
    // identity gate rejects it.
    std::string canonical_path =
        make_temp_file("dbrec_part_canonical.ibd", build_index_page(true, 10));
    std::string swapped_path =
        make_temp_file("dbrec_part_swapped.ibd", build_index_page(false, 11));

    PartitionTableRecovery engine;
    engine.set_schema(make_schema());
    engine.add_partition(make_partition("canonical", 0, canonical_path));
    engine.add_partition(make_partition("swapped", 1, swapped_path));

    auto records = engine.recover();

    // The canonical big-endian page is recognized and parsed; the swapped
    // byte-order page must be skipped entirely.
    EXPECT_EQ(records.size(), 1);
    EXPECT_EQ(engine.get_stats().recovered_partitions, 1);
    EXPECT_EQ(engine.get_stats().failed_partitions, 1);
}

// ==================== P1: identity gate rejects mismatched space_id ====================

TEST(PartitionTableRecoveryTest, IdentityGateRejectsMismatchedSpaceId) {
    // Page carries space_id 10 but the partition declares tablespace_id 20 —
    // the index page belongs to a different tablespace and must be skipped.
    std::string wrong_spid_path =
        make_temp_file("dbrec_part_wrong_spid.ibd", build_index_page(true, 10));

    PartitionInfo p = make_partition("wrong_spid", 0, wrong_spid_path);
    p.tablespace_id = 20;  // override the id+10 default to create a mismatch

    PartitionTableRecovery engine;
    engine.set_schema(make_schema());
    engine.add_partition(p);
    auto records = engine.recover();

    EXPECT_TRUE(records.empty());
    EXPECT_EQ(engine.get_stats().recovered_partitions, 0);
    EXPECT_EQ(engine.get_stats().failed_partitions, 1);
}

// ==================== Main ====================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
