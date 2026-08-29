/**
 * @file redo_plan_gate_test.cpp
 * @brief Phase 5f regression tests: inventory/patch-plan correctness,
 *        pre-condition gate (fail-closed), and whitelist accounting.
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>

#include "dbrecovery/innodb/redo_log_replayer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

using namespace dbrecovery;
using namespace dbrecovery::innodb;

namespace {

RedoLogRecord make_record(uint16_t type, uint32_t space_id, uint32_t page_no,
                          uint64_t lsn, const std::vector<uint8_t>& data) {
    RedoLogRecord r;
    r.r_type = type;
    r.r_space_id = space_id;
    r.r_page_no = page_no;
    r.r_lsn = lsn;
    r.r_data = data;
    return r;
}

std::vector<uint8_t> make_page(uint32_t page_no, uint32_t space_id) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint32_t>(page.data(), FIL_PAGE_OFFSET, page_no);          // offset 4
    write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX); // offset 24
    write_be<uint32_t>(page.data(), FIL_PAGE_SPACE_ID, space_id);       // offset 34
    write_page_checksum(page.data(), page.size(), ChecksumAlgo::CRC32C);
    return page;
}

}  // namespace

// ==================== Inventory (read-only, synthetic) ====================

TEST(RedoRedoPlanGateTest, InventoryGroupsBySpacePageAndCountsWhitelist) {
    RedoLogReplayer replayer;
    replayer.set_page_size(16384);

    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
    replayer.add_redo_records({
        make_record(MLOG_COMP_REC_INSERT, 10, 5, 100, data),  // whitelisted
        make_record(MLOG_COMP_REC_INSERT, 10, 5, 101, data),  // same page
        make_record(MLOG_COMP_REC_UPDATE, 10, 7, 102, data),  // different page
        make_record(99, 10, 5, 103, data),                    // NON-whitelisted
    });

    auto inv = replayer.generate_inventory();
    EXPECT_EQ(inv.total_records, 4);
    EXPECT_EQ(inv.whitelisted_records, 3);
    EXPECT_EQ(inv.non_whitelisted_records, 1);
    EXPECT_EQ(inv.unique_space_ids, 1);
    EXPECT_EQ(inv.unique_page_numbers, 2);
    ASSERT_EQ(inv.page_entries.size(), 2);

    const auto& p5 = (inv.page_entries[0].page_no == 5) ? inv.page_entries[0]
                                                        : inv.page_entries[1];
    EXPECT_EQ(p5.page_no, 5);
    EXPECT_EQ(p5.total_records, 3);
    EXPECT_EQ(p5.whitelisted_records, 2);
    EXPECT_EQ(p5.non_whitelisted_records, 1);

    EXPECT_NE(inv.global_type_distribution.find(MLOG_COMP_REC_INSERT),
              inv.global_type_distribution.end());
    EXPECT_EQ(inv.global_type_distribution.at(MLOG_COMP_REC_INSERT), 2);
    EXPECT_EQ(inv.global_type_distribution.at(99), 1);
}

TEST(RedoRedoPlanGateTest, EmptyInventoryIsEmpty) {
    RedoLogReplayer replayer;
    auto inv = replayer.generate_inventory();
    EXPECT_TRUE(inv.empty());
    EXPECT_EQ(inv.total_records, 0);
}

// ==================== Patch plan ordering and provenance ====================

TEST(RedoRedoPlanGateTest, PatchPlanSortsByLsnAndTracksProvenance) {
    RedoLogReplayer replayer;
    replayer.set_page_size(16384);

    std::vector<uint8_t> data = {0xAA};
    // Out-of-order input: LSN 300 first, then 100
    replayer.add_redo_records({
        make_record(MLOG_REC_INSERT, 1, 2, 300, data),
        make_record(MLOG_REC_INSERT, 1, 2, 100, data),
    });

    auto plan = replayer.generate_patch_plan(false);
    EXPECT_TRUE(plan.records_sorted_by_lsn);
    ASSERT_EQ(plan.entries.size(), 2);
    EXPECT_EQ(plan.entries[0].lsn, 100);
    EXPECT_EQ(plan.entries[1].lsn, 300);
    EXPECT_TRUE(plan.entries[0].whitelisted);
    EXPECT_EQ(plan.entries[0].provenance,
              "MLOG_REC_INSERT @ LSN 100 (space 1, page 2)");
    EXPECT_EQ(plan.unknown_mlog_count, 0);
}

TEST(RedoRedoPlanGateTest, NonWhitelistedMlogCounted) {
    RedoLogReplayer replayer;
    replayer.set_page_size(16384);

    std::vector<uint8_t> data = {0xAA};
    replayer.add_redo_records({
        make_record(247, 1, 2, 100, data),  // non-whitelisted
    });

    auto plan = replayer.generate_patch_plan(false);
    EXPECT_FALSE(plan.all_whitelisted);
    EXPECT_EQ(plan.unknown_mlog_count, 1);
    EXPECT_EQ(plan.entries.size(), 1);
    EXPECT_FALSE(plan.entries[0].whitelisted);
    EXPECT_EQ(plan.entries[0].mlog_name, "MLOG_UNKNOWN");
}

// ==================== Pre-condition gate (fail-closed) ====================

TEST(RedoRedoPlanGateTest, PreConditionsFailClosedWithoutBasePages) {
    RedoLogReplayer replayer;
    replayer.set_page_size(16384);

    std::vector<uint8_t> data = {0xAA};
    replayer.add_redo_records({
        make_record(MLOG_COMP_REC_INSERT, 1, 2, 100, data),
    });

    auto cond = replayer.validate_conditions();
    EXPECT_TRUE(cond.log_format_valid);
    EXPECT_TRUE(cond.lsn_monotonic);
    EXPECT_FALSE(cond.pages_available);   // no base pages loaded
    EXPECT_FALSE(cond.base_pages_identity_match);
    EXPECT_FALSE(cond.checksum_available);
    EXPECT_FALSE(cond.all_passed());

    EXPECT_FALSE(cond.to_string().empty());
}

TEST(RedoRedoPlanGateTest, ValidPageConditionsPass) {
    RedoLogReplayer replayer;
    replayer.set_page_size(16384);

    auto page = make_page(2, 1);
    replayer.add_page(2, page);

    std::vector<uint8_t> data = {0xAA, 0xBB};
    replayer.add_redo_records({
        make_record(MLOG_COMP_REC_INSERT, 1, 2, 100, data),
        make_record(MLOG_CHECKPOINT, 1, 2, 200, data),
    });

    auto cond = replayer.validate_conditions();
    EXPECT_TRUE(cond.log_format_valid);
    EXPECT_TRUE(cond.lsn_monotonic);
    EXPECT_TRUE(cond.pages_available);
    EXPECT_TRUE(cond.base_pages_identity_match);
    EXPECT_TRUE(cond.checksum_available);
    EXPECT_TRUE(cond.all_whitelisted);
    EXPECT_TRUE(cond.has_checkpoint);
    EXPECT_TRUE(cond.all_passed());
}

TEST(RedoRedoPlanGateTest, IdentityMismatchBlocksApply) {
    RedoLogReplayer replayer;
    replayer.set_page_size(16384);

    // Base page claims page_no 99 (offset 4), but records target page 2
    auto page = make_page(99, 1);
    replayer.add_page(2, page);

    std::vector<uint8_t> data = {0xAA};
    replayer.add_redo_records({
        make_record(MLOG_COMP_REC_INSERT, 1, 2, 100, data),
        make_record(MLOG_CHECKPOINT, 1, 2, 200, data),
    });

    auto cond = replayer.validate_conditions();
    EXPECT_TRUE(cond.pages_available);
    EXPECT_FALSE(cond.base_pages_identity_match);
    EXPECT_FALSE(cond.all_passed());

    // replay_safe must fail closed — the base page must remain byte-identical.
    auto page_before = replayer.get_base_pages().at(2);
    auto result = replayer.replay_safe();
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.records_applied, 0u);
    auto page_after = replayer.get_base_pages().at(2);
    EXPECT_TRUE(page_before == page_after);
}

TEST(RedoRedoPlanGateTest, NoCheckpointFailsClosed) {
    RedoLogReplayer replayer;
    replayer.set_page_size(16384);

    auto page = make_page(2, 1);
    replayer.add_page(2, page);

    std::vector<uint8_t> data = {0xAA};
    replayer.add_redo_records({
        make_record(MLOG_COMP_REC_INSERT, 1, 2, 100, data),
    });

    auto cond = replayer.validate_conditions();
    EXPECT_TRUE(cond.all_whitelisted);
    EXPECT_TRUE(cond.base_pages_identity_match);
    EXPECT_TRUE(cond.pages_available);
    EXPECT_TRUE(cond.checksum_available);
    EXPECT_FALSE(cond.has_checkpoint);   // no checkpoint record
    EXPECT_FALSE(cond.all_passed());

    auto result = replayer.replay_safe();
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.records_applied, 0u);
    EXPECT_EQ(result.records_processed, 0u);  // pre-conditions gate before any processing
    EXPECT_EQ(result.errors.size(), 0u);
}

// ==================== replay_safe write semantics ====================

TEST(RedoRedoPlanGateTest, ReplaySafeAppliesOnlyWhenAllPass) {
    RedoLogReplayer replayer;
    replayer.set_page_size(16384);

    auto page = make_page(2, 1);
    replayer.add_page(2, page);

    std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC};
    replayer.add_redo_records({
        make_record(MLOG_COMP_REC_INSERT, 1, 2, 100, data),
        make_record(MLOG_CHECKPOINT, 1, 2, 200, data),
    });

    auto cond = replayer.validate_conditions();
    EXPECT_TRUE(cond.all_passed());

    auto result = replayer.replay_safe();
    EXPECT_TRUE(result.success);
    EXPECT_GE(result.records_processed, 2);
    EXPECT_EQ(replayer.get_updated_pages().size(), 1u);
}

// ==================== Cross-tablespace identity mismatch ====================

TEST(RedoRedoPlanGateTest, CrossTablespaceIdentityMismatchRejectsReplay) {
    // Base page carries space_id=99 (page_no=2), but the redo record targets
    // r_space_id=1, r_page_no=2.  Without expected_space_id comparison the
    // old code would accept this base page because page_no matched.
    RedoLogReplayer replayer;
    replayer.set_page_size(16384);

    auto page = make_page(2, 99);   // page_no=2, space_id=99
    replayer.add_page(2, page);

    std::vector<uint8_t> data = {0xAA};
    replayer.add_redo_records({
        make_record(MLOG_COMP_REC_INSERT, 1, 2, 100, data),   // r_space_id=1 ≠ 99
        make_record(MLOG_CHECKPOINT, 1, 2, 200, data),
    });

    auto cond = replayer.validate_conditions();
    EXPECT_TRUE(cond.pages_available);
    EXPECT_FALSE(cond.base_pages_identity_match);   // space_id mismatch
    EXPECT_FALSE(cond.all_passed());

    // replay_safe must fail closed — the base page must remain byte-identical.
    auto page_before = replayer.get_base_pages().at(2);
    auto result = replayer.replay_safe();
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.records_applied, 0u);
    auto page_after = replayer.get_base_pages().at(2);
    EXPECT_TRUE(page_before == page_after);
}

TEST(RedoRedoPlanGateTest, SameTablespaceIdentityPassesReplay) {
    // Same page_no=2, space_id=99 for both base page and redo record → identity passes.
    RedoLogReplayer replayer;
    replayer.set_page_size(16384);

    auto page = make_page(2, 99);
    replayer.add_page(2, page);

    std::vector<uint8_t> data = {0xAA, 0xBB};
    replayer.add_redo_records({
        make_record(MLOG_COMP_REC_INSERT, 99, 2, 100, data),
        make_record(MLOG_CHECKPOINT, 99, 2, 200, data),
    });

    auto cond = replayer.validate_conditions();
    EXPECT_TRUE(cond.base_pages_identity_match);
    EXPECT_TRUE(cond.all_passed());

    auto result = replayer.replay_safe();
    EXPECT_TRUE(result.success);
    EXPECT_GE(result.records_processed, 2u);
}

// ==================== MLOG name helper ====================

TEST(RedoRedoPlanGateTest, MlogTypeNameStaticKnownAndUnknown) {
    EXPECT_STREQ(mlog_type_name_static(MLOG_1BYTE), "MLOG_1BYTE");
    EXPECT_STREQ(mlog_type_name_static(MLOG_CHECKPOINT), "MLOG_CHECKPOINT");
    EXPECT_STREQ(mlog_type_name_static(MLOG_REC_INSERT), "MLOG_REC_INSERT");
    EXPECT_STREQ(mlog_type_name_static(254), "MLOG_UNKNOWN");
}

// ==================== Main ====================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}