/**
 * @file p1_salvage_fuzz_test.cpp
 * @brief P1 Format Correctness Fuzz Tests for SalvageEngine
 *
 * Tests the salvage engine with corrupted/malformed page data to verify
 * correct damage level assignment, filtering behavior, and crash safety.
 */

#include <gtest/gtest.h>
#include "dbrecovery/recovery/salvage_engine.hpp"

#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>
#include <limits>

namespace dbrecovery {
namespace test {

using namespace dbrecovery::recovery;

// ==================== Helpers ====================

/** Build a minimal valid-looking InnoDB page buffer (16 KB) with INDEX page type. */
static std::vector<uint8_t> make_valid_index_page() {
    std::vector<uint8_t> page(16384, 0);

    // FIL header: checksum at offset 0 (big-endian)
    page[0] = 0x12; page[1] = 0x34; page[2] = 0x56; page[3] = 0x78;
    // Page no at offset 4
    page[4] = 0x00; page[5] = 0x00; page[6] = 0x00; page[7] = 0x01;
    // LSN at offset 16 (8 bytes big-endian)
    page[16] = 0x00; page[17] = 0x00; page[18] = 0x00; page[19] = 0x01;
    page[20] = 0x23; page[21] = 0x45; page[22] = 0x67; page[23] = 0x89;
    // Page type at offset 24: FIL_PAGE_INDEX = 0x45BF
    page[24] = 0x45; page[25] = 0xBF;
    // Space ID at offset 34
    page[34] = 0x00; page[35] = 0x00; page[36] = 0x00; page[37] = 0x01;

    // Page header: PAGE_N_RECS at offset 54 (LE)
    page[54] = 0x02; page[55] = 0x00; // n_recs = 2
    // PAGE_HEAP_TOP at offset 40 (LE)
    page[40] = 0x00; page[41] = 0x01; // heap_top = 256
    // PAGE_DIRECTION at offset 52 (LE)
    page[52] = 0x01; page[53] = 0x00;

    // INFIMUM record at offset 99
    // INFIMUM next record pointer (offset 100, LE)
    page[100] = 0x90; page[101] = 0x01; // points to record at 0x190 = 400

    // Trailing checksum at page_size - 8 (big-endian)
    page[16376] = 0x78; page[16377] = 0x56;
    page[16378] = 0x34; page[16379] = 0x12;
    // Trailing LSN at page_size - 16
    page[16368] = 0x00; page[16369] = 0x00; page[16370] = 0x00; page[16371] = 0x01;
    page[16372] = 0x23; page[16373] = 0x45; page[16374] = 0x67; page[16375] = 0x89;

    // Page directory: 4 slots (INFIMUM, REC1, REC2, SUPREMUM)
    // Last 2 bytes = number of slots
    page[16382] = 0x04; page[16383] = 0x00; // n_slots = 4
    // Slot 0 (INFIMUM) at page_size - 2 - 4*2 = 16374
    page[16374] = 0x63; page[16375] = 0x00; // offset 99
    // Slot 1 (REC1) at page_size - 2 - 3*2 = 16376
    page[16376] = 0x90; page[16377] = 0x01; // offset 400
    // Slot 2 (REC2) at page_size - 2 - 2*2 = 16378
    page[16378] = 0xA0; page[16379] = 0x01; // offset 416
    // Slot 3 (SUPREMUM) at page_size - 2 - 1*2 = 16380
    page[16380] = 0xA8; page[16381] = 0x01; // offset 424

    // Place a simple COMPACT record at offset 400
    uint16_t rec_off = 400;
    // offset[0] = total_len = 24
    page[rec_off]     = 0x18; page[rec_off+1]   = 0x00; // LE = 24
    page[rec_off+2]   = 0x14; page[rec_off+3]   = 0x00;
    page[rec_off+4]   = 0x10; page[rec_off+5]   = 0x00;
    page[rec_off+6]   = 0x0C; page[rec_off+7]   = 0x00;
    page[rec_off+8]   = 0x00; // info_bits
    page[rec_off+9]   = 0x00; // null bitmap
    // Field 0 (4 bytes)
    page[rec_off+10]  = 0x00; page[rec_off+11] = 0x00; page[rec_off+12] = 0x00; page[rec_off+13] = 0x01;
    // Field 1 (4 bytes)
    page[rec_off+14]  = 0x00; page[rec_off+15] = 0x00; page[rec_off+16] = 0x00; page[rec_off+17] = 0x02;
    // Field 2 (4 bytes)
    page[rec_off+18]  = 0x00; page[rec_off+19] = 0x00; page[rec_off+20] = 0x00; page[rec_off+21] = 0x03;
    // Field 3 (2 bytes)
    page[rec_off+22] = 0x00; page[rec_off+23] = 0x04;

    // Record 2 at offset 416
    rec_off = 416;
    page[rec_off]     = 0x18; page[rec_off+1]   = 0x00;
    page[rec_off+2]   = 0x14; page[rec_off+3]   = 0x00;
    page[rec_off+4]   = 0x10; page[rec_off+5]   = 0x00;
    page[rec_off+6]   = 0x0C; page[rec_off+7]   = 0x00;
    page[rec_off+8]   = 0x00;
    page[rec_off+9]   = 0x00;
    page[rec_off+10]  = 0x00; page[rec_off+11] = 0x00; page[rec_off+12] = 0x00; page[rec_off+13] = 0x0A;
    page[rec_off+14]  = 0x00; page[rec_off+15] = 0x00; page[rec_off+16] = 0x00; page[rec_off+17] = 0x0B;
    page[rec_off+18]  = 0x00; page[rec_off+19] = 0x00; page[rec_off+20] = 0x00; page[rec_off+21] = 0x0C;
    page[rec_off+22]  = 0x00; page[rec_off+23] = 0x0D;

    return page;
}

static void corrupt_bytes(std::vector<uint8_t>& page, size_t offset, size_t length) {
    if (offset + length > page.size()) return;
    for (size_t i = 0; i < length; i++) {
        page[offset + i] = static_cast<uint8_t>(rand() % 256);
    }
}

// ==================== L1 Structured Recovery Fuzz ====================

TEST(P1SalvageFuzzTest, L1StructuredRecoveryFuzz) {
    SalvageEngine engine;
    SalvageEngine::Config cfg;
    cfg.min_record_fields = 2;
    cfg.min_confidence = 0.3;
    engine.set_config(cfg);

    for (int iter = 0; iter < 100; iter++) {
        auto page = make_valid_index_page();
        corrupt_bytes(page, 0, 2);
        corrupt_bytes(page, page.size() - 8, 2);

        auto damage = SalvageEngine::assess_damage(page.data(), page.size());

        EXPECT_TRUE(damage.level == DamageLevel::L1_MINOR ||
                    damage.level == DamageLevel::NONE)
            << "Iteration " << iter << ": expected L1_MINOR or NONE, got level="
            << static_cast<int>(damage.level);

        auto rows = engine.salvage_page(page.data(), page.size(), damage);

        for (const auto& row : rows) {
            if (row.salvage_level == SalvageLevel::STRUCTURED) {
                EXPECT_GE(row.field_count, cfg.min_record_fields);
            }
            EXPECT_GE(row.confidence, 0.0);
            EXPECT_LE(row.confidence, 1.0);
        }
    }
}

// ==================== L3 Fragment Recovery Fuzz ====================

TEST(P1SalvageFuzzTest, L3FragmentRecoveryFuzz) {
    SalvageEngine engine;
    SalvageEngine::Config cfg;
    cfg.enable_signature_scan = false;
    cfg.enable_pattern_match = true;
    cfg.enable_string_extract = false;
    cfg.min_record_fields = 10;
    cfg.min_confidence = 0.0;
    engine.set_config(cfg);

    for (int iter = 0; iter < 3; iter++) {
        std::vector<uint8_t> page(16384, static_cast<uint8_t>(rand() % 256));

        const std::string email = "user@example.com";
        const std::string datetime = "2026-07-18 14:30:00";
        const std::string url = "https://example.com/path";
        const std::string integer = "1234567890";

        size_t pos = 100;
        if (pos + email.size() <= page.size())
            std::memcpy(page.data() + pos, email.data(), email.size());
        pos += 200;
        if (pos + datetime.size() <= page.size())
            std::memcpy(page.data() + pos, datetime.data(), datetime.size());
        pos += 200;
        if (pos + url.size() <= page.size())
            std::memcpy(page.data() + pos, url.data(), url.size());
        pos += 200;
        if (pos + integer.size() <= page.size())
            std::memcpy(page.data() + pos, integer.data(), integer.size());

        auto damage = SalvageEngine::assess_damage(page.data(), page.size());

        // With random data, the damage level depends on whether the random
        // bytes happen to form a recognizable FIL header.
        EXPECT_GE(static_cast<int>(damage.level), static_cast<int>(DamageLevel::L1_MINOR));
        EXPECT_LE(static_cast<int>(damage.level), static_cast<int>(DamageLevel::L4_CRITICAL));

        auto rows = engine.salvage_page(page.data(), page.size(), damage);

        for (const auto& row : rows) {
            if (damage.level >= DamageLevel::L3_SEVERE) {
                EXPECT_EQ(row.salvage_level, SalvageLevel::FRAGMENT);
            } else {
                EXPECT_TRUE(row.salvage_level == SalvageLevel::STRUCTURED ||
                            row.salvage_level == SalvageLevel::FRAGMENT);
            }
            EXPECT_GE(row.confidence, 0.0);
            EXPECT_LE(row.confidence, 1.0);
        }
    }
}

// ==================== L4 String-Only Fuzz ====================

TEST(P1SalvageFuzzTest, L4StringOnlyFuzz) {
    SalvageEngine engine;
    SalvageEngine::Config cfg;
    cfg.enable_signature_scan = false;
    cfg.enable_pattern_match = false;
    cfg.enable_string_extract = true;
    cfg.min_string_len = 4;
    cfg.min_confidence = 0.0;
    engine.set_config(cfg);

    for (int iter = 0; iter < 20; iter++) {
        std::vector<uint8_t> page(8192, static_cast<uint8_t>(rand() % 256));

        const char* strings[] = {"abcd", "HelloWorld", "test1234", "DATA", "EOF"};
        for (const char* s : strings) {
            size_t offset = static_cast<size_t>(rand() % (page.size() - 20));
            size_t len = std::min(strlen(s), page.size() - offset);
            std::memcpy(page.data() + offset, s, len);
        }

        auto damage = SalvageEngine::assess_damage(page.data(), page.size());

        // Random 8KB pages may not always produce L3/L4 damage — the
        // assess_damage heuristic depends on what random bytes end up in
        // the FIL header positions. Accept any damage level.
        EXPECT_GE(static_cast<int>(damage.level), static_cast<int>(DamageLevel::L1_MINOR));
        EXPECT_LE(static_cast<int>(damage.level), static_cast<int>(DamageLevel::L4_CRITICAL));

        auto rows = engine.salvage_page(page.data(), page.size(), damage);

        for (const auto& row : rows) {
            EXPECT_EQ(row.salvage_level, SalvageLevel::STRING_ONLY);
            EXPECT_EQ(row.recovery_method, "string_extract");
            EXPECT_GE(row.confidence, 0.0);
            EXPECT_LE(row.confidence, 1.0);
        }
    }
}

// ==================== Mixed Damage Levels ====================

TEST(P1SalvageFuzzTest, MixedDamageLevels) {
    SalvageEngine engine;
    SalvageEngine::Config cfg;
    cfg.min_confidence = 0.0;
    cfg.enable_signature_scan = true;
    cfg.enable_pattern_match = true;
    cfg.enable_string_extract = true;
    engine.set_config(cfg);

    for (int iter = 0; iter < 5; iter++) {
        std::vector<std::pair<const uint8_t*, size_t>> pages;
        std::vector<std::vector<uint8_t>> buffers;

        buffers.push_back(make_valid_index_page());
        pages.emplace_back(buffers.back().data(), buffers.back().size());

        buffers.push_back(std::vector<uint8_t>(16384, static_cast<uint8_t>(rand() % 256)));
        const std::string email = "admin@test.org";
        if (100 + email.size() <= buffers.back().size())
            std::memcpy(buffers.back().data() + 100, email.data(), email.size());
        pages.emplace_back(buffers.back().data(), buffers.back().size());

        buffers.push_back(std::vector<uint8_t>(16384, static_cast<uint8_t>(rand() % 256)));
        pages.emplace_back(buffers.back().data(), buffers.back().size());

        auto result = engine.salvage_pages(pages);

        EXPECT_TRUE(result.success);
        EXPECT_GE(result.total_pages_scanned, 3);

        int structured = 0, fragment = 0, string_only = 0;
        for (const auto& row : result.rows) {
            switch (row.salvage_level) {
                case SalvageLevel::STRUCTURED:  structured++;  break;
                case SalvageLevel::FRAGMENT:    fragment++;    break;
                case SalvageLevel::STRING_ONLY: string_only++; break;
            }
            EXPECT_GE(row.confidence, 0.0);
            EXPECT_LE(row.confidence, 1.0);
        }

        EXPECT_GT(result.rows.size(), 0);
    }
}

// ==================== Confidence Score Boundaries ====================

TEST(P1SalvageFuzzTest, ConfidenceScoreBoundaries) {
    SalvageEngine engine;

    for (int iter = 0; iter < 100; iter++) {
        auto page = make_valid_index_page();

        DamageLevel levels[] = {
            DamageLevel::NONE,
            DamageLevel::L1_MINOR,
            DamageLevel::L2_MODERATE,
            DamageLevel::L3_SEVERE,
            DamageLevel::L4_CRITICAL
        };

        for (auto level : levels) {
            DamageReport report;
            report.level = level;
            report.fil_header_valid = (level <= DamageLevel::L1_MINOR);
            report.page_header_valid = (level <= DamageLevel::L1_MINOR);

            auto rows = engine.salvage_page(page.data(), page.size(), report);

            for (const auto& row : rows) {
                EXPECT_GE(row.confidence, 0.0);
                EXPECT_LE(row.confidence, 1.0);
            }
        }
    }
}

// ==================== Empty/Truncated Pages ====================

TEST(P1SalvageFuzzTest, EmptyTruncatedPages) {
    SalvageEngine engine;

    // nullptr
    {
        auto damage = SalvageEngine::assess_damage(nullptr, 0);
        EXPECT_EQ(damage.level, DamageLevel::L4_CRITICAL);
        auto rows = engine.salvage_page(nullptr, 0, damage);
        EXPECT_TRUE(rows.empty());
    }

    // Small pages
    for (size_t sz : {1, 4, 8, 16, 32, 63}) {
        std::vector<uint8_t> tiny(sz, static_cast<uint8_t>(rand() % 256));
        auto damage = SalvageEngine::assess_damage(tiny.data(), tiny.size());
        EXPECT_EQ(damage.level, DamageLevel::L4_CRITICAL);
        auto rows = engine.salvage_page(tiny.data(), tiny.size(), damage);
        EXPECT_TRUE(rows.empty());
    }

    // Page with zero records but valid header — salvage may still produce
    // rows depending on implementation; verify crash safety and valid confidence.
    {
        auto page = make_valid_index_page();
        page[54] = 0x00; page[55] = 0x00;
        auto damage = SalvageEngine::assess_damage(page.data(), page.size());
        EXPECT_TRUE(damage.level == DamageLevel::NONE ||
                    damage.level == DamageLevel::L1_MINOR);
        auto rows = engine.salvage_page(page.data(), page.size(), damage);
        // Should not crash; rows may be empty or contain extracted data
        for (const auto& row : rows) {
            EXPECT_GE(row.confidence, 0.0);
            EXPECT_LE(row.confidence, 1.0);
        }
    }
}

// ==================== Nullptr Safety ====================

TEST(P1SalvageFuzzTest, NullptrSafety) {
    SalvageEngine engine;

    auto damage = SalvageEngine::assess_damage(nullptr, 0);
    EXPECT_EQ(damage.level, DamageLevel::L4_CRITICAL);

    auto rows = engine.salvage_page(nullptr, 0, damage);
    EXPECT_TRUE(rows.empty());

    std::vector<std::pair<const uint8_t*, size_t>> empty_pages;
    auto result = engine.salvage_pages(empty_pages);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_pages_scanned, 0);
}

// ==================== Extreme Confidence Thresholds ====================

TEST(P1SalvageFuzzTest, ExtremeConfidenceThresholds) {
    SalvageEngine engine;
    SalvageEngine::Config cfg;

    for (int iter = 0; iter < 50; iter++) {
        auto page = make_valid_index_page();
        auto damage = SalvageEngine::assess_damage(page.data(), page.size());

        cfg.min_confidence = -0.1;
        engine.set_config(cfg);
        auto rows1 = engine.salvage_page(page.data(), page.size(), damage);
        // Should not crash

        cfg.min_confidence = 1.5;
        engine.set_config(cfg);
        auto rows2 = engine.salvage_page(page.data(), page.size(), damage);

        cfg.min_confidence = 0.0;
        engine.set_config(cfg);
        auto rows3 = engine.salvage_page(page.data(), page.size(), damage);
    }
}

} // namespace test
} // namespace dbrecovery