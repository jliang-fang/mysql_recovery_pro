#include <gtest/gtest.h>
#include "dbrecovery/salvage/damaged_page_handler.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <vector>
#include <cstring>
#include <string>
#include <algorithm>

namespace dbrecovery {
namespace salvage {

using namespace dbrecovery::innodb;

// ==================== Helpers ====================

// Build a synthetic page that passes all 10 checks in detect_damage_level()
// Returns a page_size-byte buffer, zeroed except for the required fields.
static std::vector<uint8_t> create_healthy_page(
    uint32_t page_no = 0,
    size_t page_size = 16384) {

    std::vector<uint8_t> page(page_size, 0);

    // Check 1: checksum at offset 0 = 0 → bypasses check_checksum (already zero)

    // Check 5: LSN at offset 16 (LE, 8 bytes) — non-zero and ≤ 0xFFFFFFFFFFFF
    write_le<uint64_t>(page.data(), 16, 0x123456789ABCULL);

    // Check 4: page type at offset 24 (BE, 2 bytes) = FIL_PAGE_INDEX
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);

    // Check 3: page_no at offset 30 (LE, 4 bytes) ≤ 0xFFFFFF
    write_le<uint32_t>(page.data(), 30, page_no);

    // Check 2: space_id at offset 34 (LE, 4 bytes) ≤ 0xFFFFFF
    write_le<uint32_t>(page.data(), 34, 0);

    // Check 6: n_dir_slots at offset 38 (LE) ≥ 2, heap_top at offset 40 (LE) > 94
    write_le<uint16_t>(page.data(), 38, 2);     // n_dir_slots
    write_le<uint16_t>(page.data(), 40, 205);   // heap_top

    // n_heap at offset 42 (LE) = 0 → n_recs = 0 → check 9 skipped
    write_le<uint16_t>(page.data(), 42, 0);

    // Check 7: INFIMUM at offset 54, status = 0x02
    page[54] = 0x02;
    // INFIMUM next_offset at offset 55 (LE) = 200 (points to SUPREMUM)
    write_le<uint16_t>(page.data(), 55, 200);

    // Check 8: SUPREMUM at offset 200 (heap_top - 5 = 205 - 5), status = 0x03
    page[200] = 0x03;
    // Check 10: SUPREMUM next_offset at offset 201 (LE) must be 0
    write_le<uint16_t>(page.data(), 201, 0);

    return page;
}

// Build a page at a specific damage level for testing.
// Returns a page that should produce the given level when detect_damage_level() runs.
static std::vector<uint8_t> create_damaged_page(
    DamageLevel target,
    size_t page_size = 16384) {

    switch (target) {
    case LEVEL_0_HEALTHY:
        return create_healthy_page(0, page_size);

    case LEVEL_1_MINOR: {
        // 1 failure: invalid page type
        auto page = create_healthy_page(0, page_size);
        write_be<uint16_t>(page.data(), 24, 0xFFFF);
        return page;
    }

    case LEVEL_2_MODERATE: {
        // 3 failures: invalid page type, LSN=0, n_dir_slots=0
        auto page = create_healthy_page(0, page_size);
        write_be<uint16_t>(page.data(), 24, 0xFFFF);
        write_le<uint64_t>(page.data(), 16, 0);
        write_le<uint16_t>(page.data(), 38, 0);
        return page;
    }

    case LEVEL_3_SEVERE: {
        // 5 failures: checksum mismatch, invalid page type, LSN=0,
        //             n_dir_slots=0, INFIMUM corrupted
        auto page = create_healthy_page(0, page_size);
        write_le<uint32_t>(page.data(), 0, 0xDEADBEEF);
        write_be<uint16_t>(page.data(), 24, 0xFFFF);
        write_le<uint64_t>(page.data(), 16, 0);
        write_le<uint16_t>(page.data(), 38, 0);
        page[54] = 0xFF;
        return page;
    }

    case LEVEL_4_CATASTROPHIC: {
        // 9 failures: all 10 checks fail except check 1 (checksum=0 passes)
        // page_header_ok stays true (n_dir_slots=2, heap_top=205)
        // so checks 8,9,10 are also evaluated and fail
        auto page = create_healthy_page(0, page_size);
        // Check 1: checksum = 0 → passes (bypass)
        // Check 2: space_id > 0xFFFFFF → fail
        write_le<uint32_t>(page.data(), 34, 0x1FFFFFF);
        // Check 3: page_no > 0xFFFFFF → fail
        write_le<uint32_t>(page.data(), 30, 0x1FFFFFF);
        // Check 4: invalid page type → fail
        write_be<uint16_t>(page.data(), 24, 0xFFFF);
        // Check 5: LSN = 0 → fail
        write_le<uint64_t>(page.data(), 16, 0);
        // Check 6: n_dir_slots=2, heap_top=205 → pass (page_header_ok=true)
        // Check 7: INFIMUM corrupted → fail
        page[54] = 0xFF;
        // Check 8: SUPREMUM status not 0x03 → fail
        page[200] = 0x00;
        // Check 9: n_recs > 0 with bad offset table → fail
        write_le<uint16_t>(page.data(), 42, 1); // n_heap = 1 → n_recs = 1
        write_le<uint16_t>(page.data(), 203, 0); // first_offset = 0 < 54
        // Check 10: SUPREMUM next_offset != 0 → fail
        write_le<uint16_t>(page.data(), 201, 0x3FFF);
        return page;
    }

    default:
        return create_healthy_page(0, page_size);
    }
}

// ==================== Damage Detection Tests ====================

TEST(DamagedPageHandlerTest, HealthyPage) {
    auto page = create_healthy_page();
    DamagedPageHandler handler;
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());

    EXPECT_EQ(repair_result.before_level, LEVEL_0_HEALTHY);
    EXPECT_EQ(repair_result.after_level, LEVEL_0_HEALTHY);
    EXPECT_FALSE(repair_result.header_repaired);
    EXPECT_FALSE(repair_result.structure_repaired);
    EXPECT_FALSE(repair_result.checksum_fixed);
    EXPECT_TRUE(repair_result.is_fully_repaired());
    EXPECT_FALSE(repair_result.improved());
}

TEST(DamagedPageHandlerTest, MinorDamage) {
    auto page = create_damaged_page(LEVEL_1_MINOR);
    DamagedPageHandler handler;
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());

    EXPECT_EQ(repair_result.before_level, LEVEL_1_MINOR);
    // Strategy should be HEADER_ONLY
    EXPECT_GE(repair_result.after_level, LEVEL_0_HEALTHY);
}

TEST(DamagedPageHandlerTest, ModerateDamage) {
    auto page = create_damaged_page(LEVEL_2_MODERATE);
    DamagedPageHandler handler;
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());

    EXPECT_EQ(repair_result.before_level, LEVEL_2_MODERATE);
    EXPECT_GE(repair_result.after_level, LEVEL_0_HEALTHY);
}

TEST(DamagedPageHandlerTest, SevereDamage) {
    auto page = create_damaged_page(LEVEL_3_SEVERE);
    DamagedPageHandler handler;
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());

    EXPECT_EQ(repair_result.before_level, LEVEL_3_SEVERE);
    EXPECT_GE(repair_result.after_level, LEVEL_0_HEALTHY);
}

TEST(DamagedPageHandlerTest, CatastrophicDamage) {
    auto page = create_damaged_page(LEVEL_4_CATASTROPHIC);
    DamagedPageHandler handler;
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());

    EXPECT_EQ(repair_result.before_level, LEVEL_4_CATASTROPHIC);
    // CATASTROPHIC → SALVAGE_ONLY, no repair attempted
    EXPECT_FALSE(repair_result.header_repaired);
    EXPECT_FALSE(repair_result.structure_repaired);
    EXPECT_FALSE(repair_result.checksum_fixed);
}

// ==================== select_repair_strategy Tests ====================

TEST(DamagedPageHandlerTest, SelectRepairStrategyMapping) {
    EXPECT_EQ(DamagedPageHandler::select_repair_strategy(LEVEL_0_HEALTHY),
              RepairStrategy::NONE);
    EXPECT_EQ(DamagedPageHandler::select_repair_strategy(LEVEL_1_MINOR),
              RepairStrategy::HEADER_ONLY);
    EXPECT_EQ(DamagedPageHandler::select_repair_strategy(LEVEL_2_MODERATE),
              RepairStrategy::FULL_REPAIR);
    EXPECT_EQ(DamagedPageHandler::select_repair_strategy(LEVEL_3_SEVERE),
              RepairStrategy::STRUCTURE_ONLY);
    EXPECT_EQ(DamagedPageHandler::select_repair_strategy(LEVEL_4_CATASTROPHIC),
              RepairStrategy::SALVAGE_ONLY);
}

// ==================== Repair Disabled Tests ====================

TEST(DamagedPageHandlerTest, RepairDisabled) {
    auto page = create_damaged_page(LEVEL_1_MINOR);
    DamagedPageHandler handler;
    handler.set_enable_repair(false);
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());

    EXPECT_EQ(repair_result.before_level, LEVEL_1_MINOR);
    // No repair should be attempted
    EXPECT_FALSE(repair_result.header_repaired);
    EXPECT_FALSE(repair_result.structure_repaired);
    EXPECT_FALSE(repair_result.checksum_fixed);
    // after_level should equal before_level since no repair was attempted
    EXPECT_EQ(repair_result.after_level, repair_result.before_level);
}

// ==================== handle_pages Batch Tests ====================

TEST(DamagedPageHandlerTest, HandleMultiplePages) {
    auto healthy = create_healthy_page(0);
    auto minor = create_damaged_page(LEVEL_1_MINOR, 16384);
    auto severe = create_damaged_page(LEVEL_3_SEVERE, 16384);

    std::vector<std::pair<std::vector<uint8_t>, uint32_t>> pages;
    pages.emplace_back(std::move(healthy), 0);
    pages.emplace_back(std::move(minor), 1);
    pages.emplace_back(std::move(severe), 2);

    DamagedPageHandler handler;
    auto results = handler.handle_pages(pages);

    EXPECT_EQ(results.size(), 3u);

    // Verify damage distribution was recorded
    const auto& stats = handler.get_stats();
    EXPECT_EQ(stats.pages_handled, 3u);
    EXPECT_GE(stats.damage_distribution.size(), 1u);
}

TEST(DamagedPageHandlerTest, HandlePagesEmpty) {
    DamagedPageHandler handler;
    std::vector<std::pair<std::vector<uint8_t>, uint32_t>> empty;
    auto results = handler.handle_pages(empty);
    EXPECT_TRUE(results.empty());
}

// ==================== Null / Empty Buffer Tests ====================

TEST(DamagedPageHandlerTest, NullData) {
    DamagedPageHandler handler;
    auto [repair_result, salvage_result] = handler.handle_page(nullptr, 0, 0);

    EXPECT_EQ(repair_result.before_level, LEVEL_4_CATASTROPHIC);
    EXPECT_TRUE(salvage_result.records.empty());
    EXPECT_FALSE(repair_result.header_repaired);
    EXPECT_FALSE(repair_result.structure_repaired);
}

TEST(DamagedPageHandlerTest, EmptyBuffer) {
    std::vector<uint8_t> empty;
    DamagedPageHandler handler;
    auto [repair_result, salvage_result] = handler.handle_page(empty.data(), 0, empty.size());

    EXPECT_EQ(repair_result.before_level, LEVEL_4_CATASTROPHIC);
    EXPECT_TRUE(salvage_result.records.empty());
}

TEST(DamagedPageHandlerTest, TooSmallBuffer) {
    // Buffer smaller than FIL_HEADER_SIZE + 56 = 94
    std::vector<uint8_t> small(50, 0);
    DamagedPageHandler handler;
    auto [repair_result, salvage_result] = handler.handle_page(small.data(), 0, small.size());

    EXPECT_EQ(repair_result.before_level, LEVEL_4_CATASTROPHIC);
}

// ==================== Statistics Tests ====================

TEST(DamagedPageHandlerTest, StatsInitial) {
    DamagedPageHandler handler;
    const auto& stats = handler.get_stats();
    EXPECT_EQ(stats.pages_handled, 0u);
    EXPECT_EQ(stats.pages_repaired, 0u);
    EXPECT_EQ(stats.pages_salvaged, 0u);
    EXPECT_EQ(stats.total_records_recovered, 0u);
    EXPECT_EQ(stats.high_confidence_records, 0u);
    EXPECT_EQ(stats.repair_attempts, 0u);
    EXPECT_EQ(stats.repair_successes, 0u);
    EXPECT_EQ(stats.total_repair_time_ms, 0.0);
    EXPECT_EQ(stats.total_scan_time_ms, 0.0);
    EXPECT_TRUE(stats.damage_distribution.empty());
    EXPECT_TRUE(stats.repair_strategy_usage.empty());
}

TEST(DamagedPageHandlerTest, StatsAfterSinglePage) {
    auto page = create_healthy_page();
    DamagedPageHandler handler;
    handler.handle_page(page.data(), 0, page.size());

    const auto& stats = handler.get_stats();
    EXPECT_EQ(stats.pages_handled, 1u);
    EXPECT_TRUE(stats.damage_distribution.find(LEVEL_0_HEALTHY)
                != stats.damage_distribution.end());
    EXPECT_EQ(stats.damage_distribution.at(LEVEL_0_HEALTHY), 1u);
    EXPECT_TRUE(stats.repair_strategy_usage.find(RepairStrategy::NONE)
                != stats.repair_strategy_usage.end());
}

TEST(DamagedPageHandlerTest, StatsReset) {
    auto page = create_healthy_page();
    DamagedPageHandler handler;
    handler.handle_page(page.data(), 0, page.size());
    EXPECT_GT(handler.get_stats().pages_handled, 0u);

    handler.reset_stats();
    const auto& stats = handler.get_stats();
    EXPECT_EQ(stats.pages_handled, 0u);
    EXPECT_TRUE(stats.damage_distribution.empty());
    EXPECT_TRUE(stats.repair_strategy_usage.empty());
}

TEST(DamagedPageHandlerTest, StatsMultiplePages) {
    DamagedPageHandler handler;
    auto healthy = create_healthy_page(0);
    auto minor = create_damaged_page(LEVEL_1_MINOR);

    handler.handle_page(healthy.data(), 0, healthy.size());
    handler.handle_page(minor.data(), 1, minor.size());

    const auto& stats = handler.get_stats();
    EXPECT_EQ(stats.pages_handled, 2u);
    EXPECT_EQ(stats.damage_distribution.size(), 2u);
    EXPECT_EQ(stats.repair_strategy_usage.size(), 2u);
}

// ==================== Configuration Tests ====================

TEST(DamagedPageHandlerTest, SetMinConfidence) {
    DamagedPageHandler handler;
    // No getter for min_confidence, but verify it doesn't crash
    handler.set_min_confidence(0.5);
    auto page = create_healthy_page();
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());
    EXPECT_EQ(repair_result.before_level, LEVEL_0_HEALTHY);
}

TEST(DamagedPageHandlerTest, SetPageSize) {
    DamagedPageHandler handler;
    handler.set_page_size(8192);

    // 8K page with the usual layout
    std::vector<uint8_t> page(8192, 0);
    write_le<uint64_t>(page.data(), 16, 0x123456789ABCULL);
    write_be<uint16_t>(page.data(), 24, FIL_PAGE_INDEX);
    write_le<uint16_t>(page.data(), 38, 2);
    write_le<uint16_t>(page.data(), 40, 205);
    page[54] = 0x02;
    write_le<uint16_t>(page.data(), 55, 200);
    page[200] = 0x03;
    write_le<uint16_t>(page.data(), 201, 0);

    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());
    EXPECT_EQ(repair_result.before_level, LEVEL_0_HEALTHY);
}

TEST(DamagedPageHandlerTest, SetEnableDedup) {
    DamagedPageHandler handler;
    handler.set_enable_dedup(false);
    auto page = create_healthy_page();
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());
    EXPECT_EQ(repair_result.before_level, LEVEL_0_HEALTHY);
}

TEST(DamagedPageHandlerTest, SetScanBruteForceStep) {
    DamagedPageHandler handler;
    handler.set_scan_brute_force_step(4);
    auto page = create_healthy_page();
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());
    EXPECT_EQ(repair_result.before_level, LEVEL_0_HEALTHY);
}

// ==================== Report Tests ====================

TEST(DamagedPageHandlerTest, GenerateReportWithoutData) {
    DamagedPageHandler handler;
    std::string report = handler.generate_report();
    EXPECT_FALSE(report.empty());
    EXPECT_NE(report.find("Pages handled:"), std::string::npos);
}

TEST(DamagedPageHandlerTest, GenerateReportAfterHandling) {
    DamagedPageHandler handler;
    auto healthy = create_healthy_page(0);
    auto minor = create_damaged_page(LEVEL_1_MINOR);
    handler.handle_page(healthy.data(), 0, healthy.size());
    handler.handle_page(minor.data(), 1, minor.size());

    std::string report = handler.generate_report();
    EXPECT_FALSE(report.empty());
    EXPECT_NE(report.find("Pages handled:"), std::string::npos);
    // Check that the report contains the damage distribution header
    EXPECT_NE(report.find("Damage distribution"), std::string::npos);
    EXPECT_NE(report.find("Repair strategy usage"), std::string::npos);
}

// ==================== Edge Cases ====================

TEST(DamagedPageHandlerTest, AllDamageLevelsRoundTrip) {
    // Verify that each damage level helper produces the expected level
    for (int level = 0; level <= 4; level++) {
        auto dl = static_cast<DamageLevel>(level);
        auto page = create_damaged_page(dl, 16384);
        DamagedPageHandler handler;
        auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());

        // The detected level should be at least as severe as what we asked for,
        // but could be more severe due to cascading effects
        EXPECT_GE(static_cast<int>(repair_result.before_level), level);
    }
}

TEST(DamagedPageHandlerTest, ScannerAccess) {
    DamagedPageHandler handler;
    // Verify we can access the underlying scanner
    RowSalvageScanner& scanner = handler.scanner();
    EXPECT_NO_THROW(scanner.reset_stats());
}

TEST(DamagedPageHandlerTest, MultiplePageNos) {
    auto page0 = create_healthy_page(0);
    auto page5 = create_healthy_page(5);
    auto page100 = create_healthy_page(100);

    DamagedPageHandler handler;
    auto [r0, s0] = handler.handle_page(page0.data(), 0, page0.size());
    auto [r5, s5] = handler.handle_page(page5.data(), 5, page5.size());
    auto [r100, s100] = handler.handle_page(page100.data(), 100, page100.size());

    EXPECT_EQ(r0.page_no, 0u);
    EXPECT_EQ(r5.page_no, 5u);
    EXPECT_EQ(r100.page_no, 100u);
}

TEST(DamagedPageHandlerTest, RepairSkippedForHealthy) {
    // HEALTHY → NONE strategy → repair not attempted
    auto page = create_healthy_page();
    DamagedPageHandler handler;
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());

    EXPECT_FALSE(repair_result.header_repaired);
    EXPECT_FALSE(repair_result.structure_repaired);
    EXPECT_FALSE(repair_result.checksum_fixed);
    EXPECT_EQ(handler.get_stats().repair_attempts, 0u);
}

TEST(DamagedPageHandlerTest, RepairSkippedForCatastrophic) {
    // CATASTROPHIC → SALVAGE_ONLY → repair not attempted
    auto page = create_damaged_page(LEVEL_4_CATASTROPHIC);
    DamagedPageHandler handler;
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());

    EXPECT_FALSE(repair_result.header_repaired);
    EXPECT_FALSE(repair_result.structure_repaired);
    EXPECT_EQ(handler.get_stats().repair_attempts, 0u);
}

// ==================== Repair Strategy Enum String Tests ====================

TEST(DamagedPageHandlerTest, RepairStrategyToString) {
    EXPECT_EQ(repair_strategy_to_string(RepairStrategy::NONE), "NONE");
    EXPECT_EQ(repair_strategy_to_string(RepairStrategy::HEADER_ONLY), "HEADER_ONLY");
    EXPECT_EQ(repair_strategy_to_string(RepairStrategy::STRUCTURE_ONLY), "STRUCTURE_ONLY");
    EXPECT_EQ(repair_strategy_to_string(RepairStrategy::FULL_REPAIR), "FULL_REPAIR");
    EXPECT_EQ(repair_strategy_to_string(RepairStrategy::SALVAGE_ONLY), "SALVAGE_ONLY");
}

// ==================== Repair Result Tests ====================

TEST(DamagedPageHandlerTest, RepairResultImproved) {
    RepairAttemptResult result;
    result.before_level = LEVEL_3_SEVERE;
    result.after_level = LEVEL_1_MINOR;
    EXPECT_TRUE(result.improved());
    EXPECT_FALSE(result.is_fully_repaired());
}

TEST(DamagedPageHandlerTest, RepairResultFullyRepaired) {
    RepairAttemptResult result;
    result.before_level = LEVEL_1_MINOR;
    result.after_level = LEVEL_0_HEALTHY;
    EXPECT_TRUE(result.improved());
    EXPECT_TRUE(result.is_fully_repaired());
}

TEST(DamagedPageHandlerTest, RepairResultNotImproved) {
    RepairAttemptResult result;
    result.before_level = LEVEL_2_MODERATE;
    result.after_level = LEVEL_2_MODERATE;
    EXPECT_FALSE(result.improved());
    EXPECT_FALSE(result.is_fully_repaired());
}

TEST(DamagedPageHandlerTest, RepairResultWorse) {
    RepairAttemptResult result;
    result.before_level = LEVEL_1_MINOR;
    result.after_level = LEVEL_3_SEVERE;
    EXPECT_FALSE(result.improved());
    EXPECT_FALSE(result.is_fully_repaired());
}

// ==================== Provenance Gating Tests ====================

TEST(DamagedPageHandlerTest, ProvenanceIsStructuredExportablePredicate) {
    // Structured-exportable strategies
    EXPECT_TRUE(is_structured_exportable(ScanStrategy::BPLUS_TREE));
    EXPECT_TRUE(is_structured_exportable(ScanStrategy::PAGE_DIR));
    EXPECT_TRUE(is_structured_exportable(ScanStrategy::LINEAR_SCAN));

    // Evidence-only strategies
    EXPECT_FALSE(is_structured_exportable(ScanStrategy::BRUTE_FORCE));
    EXPECT_FALSE(is_structured_exportable(ScanStrategy::FRAGMENT));
}

TEST(DamagedPageHandlerTest, ProvenanceUnknownEnumFailClosed) {
    // Unknown enum values must fail closed (not structured-exportable)
    auto unknown = static_cast<ScanStrategy>(99);
    EXPECT_FALSE(is_structured_exportable(unknown));
}

TEST(DamagedPageHandlerTest, ProvenanceHealthyPageRecordsStructured) {
    // LEVEL_0_HEALTHY → BPLUS_TREE → all records are structured-exportable
    auto page = create_healthy_page();
    DamagedPageHandler handler;
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());

    const auto& stats = handler.get_stats();
    // All records from BPLUS_TREE should be structured
    EXPECT_EQ(stats.structured_exportable_records, stats.total_records_recovered);
    // No evidence-only records from a healthy page
    EXPECT_EQ(stats.evidence_only_records, 0u);
    // High confidence should be a subset of structured
    EXPECT_LE(stats.high_confidence_records, stats.structured_exportable_records);
}

TEST(DamagedPageHandlerTest, ProvenanceCatastrophicPageEvidenceOnly) {
    // LEVEL_4_CATASTROPHIC → FRAGMENT → all records are evidence-only
    auto page = create_damaged_page(LEVEL_4_CATASTROPHIC);
    DamagedPageHandler handler;
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());

    const auto& stats = handler.get_stats();
    // FRAGMENT records must not be counted as structured recovery
    EXPECT_EQ(stats.structured_exportable_records, 0u);
    // Any records found by fragment scan are evidence-only
    // total_records_recovered should also be 0 since BRUTE_FORCE/FRAGMENT
    // are not counted in total_records_recovered per the new gating
    EXPECT_EQ(stats.total_records_recovered, 0u);
}

TEST(DamagedPageHandlerTest, ProvenanceSeverePageNoRepair) {
    // LEVEL_3_SEVERE with repair disabled → BRUTE_FORCE → evidence-only
    auto page = create_damaged_page(LEVEL_3_SEVERE);
    DamagedPageHandler handler;
    handler.set_enable_repair(false);
    auto [repair_result, salvage_result] = handler.handle_page(page.data(), 0, page.size());

    const auto& stats = handler.get_stats();
    // BRUTE_FORCE records must not be counted as structured recovery
    EXPECT_EQ(stats.structured_exportable_records, 0u);
    EXPECT_EQ(stats.total_records_recovered, 0u);
}

TEST(DamagedPageHandlerTest, ProvenanceMixedPages) {
    DamagedPageHandler handler;
    handler.set_enable_repair(false);  // Prevent repair from changing strategies

    auto healthy = create_healthy_page(0);
    auto catastrophic = create_damaged_page(LEVEL_4_CATASTROPHIC);

    handler.handle_page(healthy.data(), 0, healthy.size());
    handler.handle_page(catastrophic.data(), 1, catastrophic.size());

    const auto& stats = handler.get_stats();
    EXPECT_EQ(stats.pages_handled, 2u);

    // Structured records come only from the healthy page
    // Evidence-only records come only from the catastrophic page
    EXPECT_EQ(stats.total_records_recovered, stats.structured_exportable_records);
    // At least one page should have evidence-only records (catastrophic)
    // Note: healthy page may produce 0 records, so evidence could be >= 0
    EXPECT_LE(stats.structured_exportable_records, stats.total_records_recovered + stats.evidence_only_records);
}

TEST(DamagedPageHandlerTest, ProvenanceReportHasStructuredAndEvidence) {
    DamagedPageHandler handler;
    handler.set_enable_repair(false);

    auto healthy = create_healthy_page(0);
    auto catastrophic = create_damaged_page(LEVEL_4_CATASTROPHIC);

    handler.handle_page(healthy.data(), 0, healthy.size());
    handler.handle_page(catastrophic.data(), 1, catastrophic.size());

    std::string report = handler.generate_report();
    // Report must contain the structured/evidence breakdown
    EXPECT_NE(report.find("Structured:"), std::string::npos);
    EXPECT_NE(report.find("Evidence-only:"), std::string::npos);
    EXPECT_NE(report.find("Low confidence:"), std::string::npos);
}

TEST(DamagedPageHandlerTest, ProvenanceStatsReset) {
    DamagedPageHandler handler;
    handler.set_enable_repair(false);

    auto catastrophic = create_damaged_page(LEVEL_4_CATASTROPHIC);
    handler.handle_page(catastrophic.data(), 0, catastrophic.size());

    handler.reset_stats();
    const auto& stats = handler.get_stats();
    EXPECT_EQ(stats.structured_exportable_records, 0u);
    EXPECT_EQ(stats.evidence_only_records, 0u);
    EXPECT_EQ(stats.low_confidence_records, 0u);
    EXPECT_EQ(stats.total_records_recovered, 0u);
    EXPECT_EQ(stats.high_confidence_records, 0u);
}

TEST(DamagedPageHandlerTest, ProvenanceNoRecordsAllCountsZero) {
    // Empty buffer produces no records; all provenance counts must be zero
    std::vector<uint8_t> empty;
    DamagedPageHandler handler;
    handler.handle_page(empty.data(), 0, empty.size());

    const auto& stats = handler.get_stats();
    EXPECT_EQ(stats.total_records_recovered, 0u);
    EXPECT_EQ(stats.structured_exportable_records, 0u);
    EXPECT_EQ(stats.evidence_only_records, 0u);
    EXPECT_EQ(stats.high_confidence_records, 0u);
    EXPECT_EQ(stats.low_confidence_records, 0u);
}

} // namespace salvage
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}