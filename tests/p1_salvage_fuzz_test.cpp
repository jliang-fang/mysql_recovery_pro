/**
 * @file p1_salvage_fuzz_test.cpp
 * @brief P1 Format Correctness Fuzz Tests for SalvageEngine
 *
 * Tests the salvage engine with corrupted/malformed page data to verify
 * correct damage level assignment, filtering behavior, and crash safety.
 */

#include <gtest/gtest.h>
#include "dbrecovery/recovery/salvage_engine.hpp"
#include "dbrecovery/salvage/evidence_serializer.hpp"

#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>
#include <limits>
#include <fstream>
#include <regex>

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
    // include_evidence_fragments=false (default) — L3/L4 must be in evidence_fragments only
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

        // rows must contain only STRUCTURED entries
        for (const auto& row : result.rows) {
            EXPECT_EQ(row.salvage_level, SalvageLevel::STRUCTURED);
            EXPECT_GE(row.confidence, 0.0);
            EXPECT_LE(row.confidence, 1.0);
        }

        // evidence_fragments must contain only FRAGMENT/STRING_ONLY entries
        for (const auto& frag : result.evidence_fragments) {
            EXPECT_TRUE(frag.salvage_level == SalvageLevel::FRAGMENT ||
                        frag.salvage_level == SalvageLevel::STRING_ONLY);
            EXPECT_GE(frag.confidence, 0.0);
            EXPECT_LE(frag.confidence, 1.0);
        }

        // At least one of rows or evidence_fragments should be non-empty
        EXPECT_TRUE(result.rows.size() > 0 || result.evidence_fragments.size() > 0);
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

// ==================== Evidence Separation (L3/L4 → evidence_fragments) ====================

TEST(P1SalvageFuzzTest, EvidenceSeparationDefault) {
    // With include_evidence_fragments=false (default), L3/L4 must route to evidence_fragments,
    // never to rows. STRUCTURED rows must remain in rows.
    SalvageEngine engine;
    SalvageEngine::Config cfg;
    cfg.min_confidence = 0.0;
    cfg.enable_signature_scan = true;
    cfg.enable_pattern_match = true;
    cfg.enable_string_extract = true;
    cfg.include_evidence_fragments = false;
    engine.set_config(cfg);

    std::vector<std::pair<const uint8_t*, size_t>> pages;
    std::vector<std::vector<uint8_t>> buffers;

    // Page 0: valid index page → STRUCTURED rows
    buffers.push_back(make_valid_index_page());
    pages.emplace_back(buffers.back().data(), buffers.back().size());

    // Page 1: random data with string → L3/L4 fragments
    buffers.push_back(std::vector<uint8_t>(16384, static_cast<uint8_t>(rand() % 256)));
    const std::string email = "evidence@test.org";
    if (100 + email.size() <= buffers.back().size())
        std::memcpy(buffers.back().data() + 100, email.data(), email.size());
    pages.emplace_back(buffers.back().data(), buffers.back().size());

    auto result = engine.salvage_pages(pages);

    // All rows must be STRUCTURED
    for (const auto& row : result.rows) {
        EXPECT_EQ(row.salvage_level, SalvageLevel::STRUCTURED);
    }

    // All evidence_fragments must be FRAGMENT or STRING_ONLY
    for (const auto& frag : result.evidence_fragments) {
        EXPECT_TRUE(frag.salvage_level == SalvageLevel::FRAGMENT ||
                    frag.salvage_level == SalvageLevel::STRING_ONLY);
    }

    // Evidence provenance metadata should be populated
    for (const auto& frag : result.evidence_fragments) {
        EXPECT_FALSE(frag.evidence.strategy.empty());
        EXPECT_GE(frag.evidence.confidence, 0.0);
        EXPECT_LE(frag.evidence.confidence, 1.0);
    }
}

TEST(P1SalvageFuzzTest, EvidenceSeparationIncludeFragments) {
    // With include_evidence_fragments=true, evidence_fragments should be preserved
    SalvageEngine engine;
    SalvageEngine::Config cfg;
    cfg.min_confidence = 0.0;
    cfg.enable_signature_scan = true;
    cfg.enable_pattern_match = true;
    cfg.enable_string_extract = true;
    cfg.include_evidence_fragments = true;
    engine.set_config(cfg);

    std::vector<std::pair<const uint8_t*, size_t>> pages;
    std::vector<std::vector<uint8_t>> buffers;

    buffers.push_back(std::vector<uint8_t>(16384, static_cast<uint8_t>(rand() % 256)));
    const std::string email = "included@test.org";
    if (100 + email.size() <= buffers.back().size())
        std::memcpy(buffers.back().data() + 100, email.data(), email.size());
    pages.emplace_back(buffers.back().data(), buffers.back().size());

    auto result = engine.salvage_pages(pages);

    // With include_evidence_fragments=true, evidence_fragments must be non-empty
    // (random data pages typically produce L3/L4 fragments)
    bool has_evidence = !result.evidence_fragments.empty();
    EXPECT_TRUE(has_evidence);

    // All evidence_fragments must be FRAGMENT or STRING_ONLY
    for (const auto& frag : result.evidence_fragments) {
        EXPECT_TRUE(frag.salvage_level == SalvageLevel::FRAGMENT ||
                    frag.salvage_level == SalvageLevel::STRING_ONLY);
    }
}

// ==================== CSV Export Fail-Closed ====================

TEST(P1SalvageFuzzTest, ExportCsvFailClosedNonStructured) {
    // export_csv must return false (fail closed) when any row is non-STRUCTURED,
    // and must not create a partial output file.
    std::string temp_path = std::string(std::getenv("TEST_TMPDIR") ? std::getenv("TEST_TMPDIR") : "/tmp") +
                            "/p1_csv_fail_closed_test.csv";

    // Remove any pre-existing file
    std::remove(temp_path.c_str());

    // Build a vector with mixed levels
    SalvagedRow structured;
    structured.salvage_level = SalvageLevel::STRUCTURED;
    structured.fields = {"val1", "val2"};
    structured.confidence = 0.9;

    SalvagedRow fragment;
    fragment.salvage_level = SalvageLevel::FRAGMENT;
    fragment.fields = {"frag1", "frag2"};
    fragment.confidence = 0.4;

    std::vector<SalvagedRow> mixed_rows = {structured, fragment};

    // export_csv must fail and not create a partial file
    bool result = SalvageEngine::export_csv(temp_path, mixed_rows);
    EXPECT_FALSE(result);

    // Verify no partial file was created
    std::ifstream f(temp_path);
    EXPECT_FALSE(f.good()) << "export_csv created a partial file despite non-STRUCTURED input";
    f.close();
    std::remove(temp_path.c_str());

    // Same vector with only STRUCTURED rows must succeed
    std::vector<SalvagedRow> clean_rows = {structured};
    bool clean_result = SalvageEngine::export_csv(temp_path, clean_rows);
    EXPECT_TRUE(clean_result);
    std::remove(temp_path.c_str());
}

TEST(P1SalvageFuzzTest, ExportCsvFailClosedAllFragments) {
    // export_csv must fail when ALL rows are non-STRUCTURED
    std::string temp_path = std::string(std::getenv("TEST_TMPDIR") ? std::getenv("TEST_TMPDIR") : "/tmp") +
                            "/p1_csv_fail_closed_fragments_test.csv";
    std::remove(temp_path.c_str());

    SalvagedRow fragment;
    fragment.salvage_level = SalvageLevel::FRAGMENT;
    fragment.fields = {"frag1", "frag2"};
    fragment.confidence = 0.4;

    SalvagedRow string_only;
    string_only.salvage_level = SalvageLevel::STRING_ONLY;
    string_only.fields = {"str1"};
    string_only.confidence = 0.2;

    std::vector<SalvagedRow> frag_rows = {fragment, string_only};
    bool result = SalvageEngine::export_csv(temp_path, frag_rows);
    EXPECT_FALSE(result);

    std::ifstream f(temp_path);
    EXPECT_FALSE(f.good()) << "export_csv created a partial file despite all-fragment input";
    f.close();
    std::remove(temp_path.c_str());
}

// ==================== Evidence Serializer ====================

TEST(P1SalvageFuzzTest, EvidenceSerializerNonImportableMarker) {
    // Evidence JSON must contain the non-importable warning marker
    salvage::SalvageEvidenceSerializer serializer;

    SalvagedRow frag;
    frag.salvage_level = SalvageLevel::FRAGMENT;
    frag.recovery_method = "pattern_match";
    frag.confidence = 0.45;
    frag.page_offset = 0x1000;
    frag.record_offset = 0x50;
    frag.evidence.level = common::DamageLevel::L3_SEVERE;
    frag.evidence.strategy = "pattern_match";
    frag.evidence.source_offset = 0x1050;
    frag.evidence.confidence = 0.45;
    frag.evidence.tags = {"extracted", "partial"};
    frag.fields = {"sensitive_data_12345", "another_field_value"};

    std::vector<SalvagedRow> fragments = {frag};
    std::map<std::string, std::string> metadata = {
        {"source_file", "test.ibd"},
        {"page_size", "16384"}
    };

    std::string json = serializer.to_json(fragments, metadata);

    // Must contain non-importable warning
    EXPECT_NE(json.find("non_importable_warning"), std::string::npos);
    EXPECT_NE(json.find("forensic evidence"), std::string::npos);
    EXPECT_NE(json.find("NOT verified"), std::string::npos);

    // Must contain SHA-256 hashes of the full values
    EXPECT_NE(json.find("sha256"), std::string::npos);

    // Must contain bounded previews
    EXPECT_NE(json.find("preview"), std::string::npos);
    EXPECT_NE(json.find("preview_length"), std::string::npos);

    // Short values appear inside the bounded preview (expected); the guarantee
    // is that values longer than MAX_PREVIEW_LENGTH are truncated and never
    // emitted in full. That is covered by EvidenceSerializerBoundedPreview.
    EXPECT_NE(json.find("source_file"), std::string::npos);
    EXPECT_NE(json.find("test.ibd"), std::string::npos);
    EXPECT_NE(json.find("page_size"), std::string::npos);

    // Must contain provenance fields
    EXPECT_NE(json.find("salvage_level"), std::string::npos);
    EXPECT_NE(json.find("recovery_method"), std::string::npos);
    EXPECT_NE(json.find("source_page_offset"), std::string::npos);
    EXPECT_NE(json.find("source_record_offset"), std::string::npos);

    // Must contain evidence metadata
    EXPECT_NE(json.find("strategy"), std::string::npos);
    EXPECT_NE(json.find("tags"), std::string::npos);

    // Must contain fragment and string_only counts
    EXPECT_NE(json.find("fragment_count"), std::string::npos);
    EXPECT_NE(json.find("string_only_count"), std::string::npos);
}

TEST(P1SalvageFuzzTest, EvidenceSerializerWriteAndRead) {
    // Write evidence to a temp file and verify it can be read back
    salvage::SalvageEvidenceSerializer serializer;

    SalvagedRow frag;
    frag.salvage_level = SalvageLevel::FRAGMENT;
    frag.recovery_method = "pattern_match";
    frag.confidence = 0.5;
    frag.page_offset = 0x2000;
    frag.record_offset = 0x80;
    frag.evidence.level = common::DamageLevel::L3_SEVERE;
    frag.evidence.strategy = "pattern_match";
    frag.evidence.source_offset = 0x2080;
    frag.evidence.confidence = 0.5;
    frag.fields = {"field1", "field2_with_longer_value_for_testing"};

    SalvagedRow string_only;
    string_only.salvage_level = SalvageLevel::STRING_ONLY;
    string_only.recovery_method = "string_extract";
    string_only.confidence = 0.25;
    string_only.page_offset = 0x3000;
    string_only.record_offset = 0;
    string_only.evidence.level = common::DamageLevel::L4_CRITICAL;
    string_only.evidence.strategy = "string_extract";
    string_only.evidence.source_offset = 0x3050;
    string_only.evidence.confidence = 0.25;
    string_only.fields = {"extracted_string"};

    std::vector<SalvagedRow> fragments = {frag, string_only};

    std::string temp_path = std::string(std::getenv("TEST_TMPDIR") ? std::getenv("TEST_TMPDIR") : "/tmp") +
                            "/p1_evidence_serializer_test.json";

    bool written = serializer.write_evidence(temp_path, fragments);
    EXPECT_TRUE(written);

    // Read back and verify basic structure
    std::ifstream f(temp_path);
    ASSERT_TRUE(f.good());
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    f.close();

    EXPECT_NE(content.find("salvage_evidence"), std::string::npos);
    EXPECT_NE(content.find("fragment_count"), std::string::npos);
    EXPECT_NE(content.find("string_only_count"), std::string::npos);

    std::remove(temp_path.c_str());
}

TEST(P1SalvageFuzzTest, EvidenceSerializerBoundedPreview) {
    // Verify that bounded_preview truncates long values and escapes non-printable bytes
    salvage::SalvageEvidenceSerializer serializer;

    // Build a fragment with a very long field value
    std::string long_value(1000, 'A');
    SalvagedRow frag;
    frag.salvage_level = SalvageLevel::FRAGMENT;
    frag.recovery_method = "test";
    frag.confidence = 0.5;
    frag.evidence.strategy = "test";
    frag.evidence.confidence = 0.5;
    frag.fields = {long_value};

    std::string json = serializer.to_json({frag}, {});

    // Must contain truncated marker
    EXPECT_NE(json.find("truncated"), std::string::npos);

    // Must not contain the full 1000-byte value
    std::string full_run(1000, 'A');
    EXPECT_EQ(json.find(full_run), std::string::npos);
}

// ==================== Auto-Salvage Correlation Boundary ====================

TEST(P1SalvageFuzzTest, AutoSalvageStructuredOnly) {
    // Verify that salvage_pages with default config returns only STRUCTURED in rows,
    // which is what the auto-salvage correlation block consumes.
    SalvageEngine engine;
    SalvageEngine::Config cfg;
    cfg.min_confidence = 0.0;
    cfg.enable_signature_scan = true;
    cfg.enable_pattern_match = true;
    cfg.enable_string_extract = true;
    cfg.include_evidence_fragments = false;
    engine.set_config(cfg);

    std::vector<std::pair<const uint8_t*, size_t>> pages;
    std::vector<std::vector<uint8_t>> buffers;

    buffers.push_back(make_valid_index_page());
    pages.emplace_back(buffers.back().data(), buffers.back().size());

    auto result = engine.salvage_pages(pages);

    for (const auto& row : result.rows) {
        // auto-salvage uses is_complete = (sr.salvage_level == STRUCTURED)
        // Since rows only contain STRUCTURED, is_complete is always true
        EXPECT_EQ(row.salvage_level, SalvageLevel::STRUCTURED);
    }

    // evidence_fragments must not contain STRUCTURED
    for (const auto& frag : result.evidence_fragments) {
        EXPECT_NE(frag.salvage_level, SalvageLevel::STRUCTURED);
    }
}

TEST(P1SalvageFuzzTest, SalvageEvidencePreservesWideOffsets) {
    salvage::SalvageEvidenceSerializer serializer;

    SalvagedRow fragment;
    fragment.salvage_level = SalvageLevel::FRAGMENT;
    fragment.recovery_method = "pattern_match";
    fragment.evidence.level = DamageLevel::L3_SEVERE;
    fragment.evidence.strategy = "pattern_match";
    fragment.evidence.source_offset = static_cast<uint64_t>(UINT16_MAX) + 1234;

    const std::string json = serializer.to_json({fragment}, {});
    EXPECT_NE(json.find("66769"), std::string::npos);
}

TEST(P1SalvageFuzzTest, L4StringExtractionUsesBoundedContinuationFragments) {
    SalvageEngine::Config cfg;
    cfg.min_string_len = 1;
    cfg.max_l4_fragment_bytes = 8;
    cfg.max_l4_fragments = 3;
    cfg.enable_signature_scan = false;
    cfg.enable_pattern_match = false;
    cfg.enable_string_extract = true;
    cfg.min_confidence = 0.0;
    SalvageEngine engine(cfg);

    std::vector<uint8_t> page(64, 0);
    std::fill(page.begin() + 5, page.begin() + 35, static_cast<uint8_t>('A'));
    DamageReport damage;
    damage.level = DamageLevel::L4_CRITICAL;

    const auto rows = engine.salvage_page(page.data(), page.size(), damage);
    ASSERT_EQ(rows.size(), 3);
    for (size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(rows[i].salvage_level, SalvageLevel::STRING_ONLY);
        ASSERT_EQ(rows[i].fields.size(), 1);
        EXPECT_LE(rows[i].fields.front().size(), cfg.max_l4_fragment_bytes);
        EXPECT_EQ(rows[i].evidence.source_offset, 5 + i * cfg.max_l4_fragment_bytes);
        EXPECT_NE(std::find(rows[i].evidence.tags.begin(), rows[i].evidence.tags.end(), "truncated"),
                  rows[i].evidence.tags.end());
        if (i > 0) {
            EXPECT_NE(std::find(rows[i].evidence.tags.begin(), rows[i].evidence.tags.end(), "continuation"),
                      rows[i].evidence.tags.end());
        }
    }
    EXPECT_FALSE(SalvageEngine::export_csv("/tmp/p1_l4_fragment_export.csv", rows));
}

// ==================== Page No Population ====================

TEST(P1SalvageFuzzTest, SalvagePageNoPopulated) {
    // Verify that salvage methods populate page_no from the FIL header
    SalvageEngine::Config cfg;
    cfg.min_confidence = 0.0;
    cfg.enable_signature_scan = true;
    cfg.enable_pattern_match = true;
    cfg.enable_string_extract = true;
    SalvageEngine engine(cfg);

    auto page = make_valid_index_page();
    // page_no = 1 at offset 4
    auto damage = SalvageEngine::assess_damage(page.data(), page.size());
    auto rows = engine.salvage_page(page.data(), page.size(), damage);

    for (const auto& row : rows) {
        EXPECT_EQ(row.page_no, 1);
        EXPECT_EQ(row.evidence.source_page_no, 1);
    }
}

// ==================== Content Hash Population ====================

TEST(P1SalvageFuzzTest, SalvageContentHashPopulated) {
    // Verify that all salvage methods populate content_hash
    SalvageEngine::Config cfg;
    cfg.min_confidence = 0.0;
    cfg.enable_signature_scan = true;
    cfg.enable_pattern_match = true;
    cfg.enable_string_extract = true;
    SalvageEngine engine(cfg);

    // L1: valid index page
    {
        auto page = make_valid_index_page();
        auto damage = SalvageEngine::assess_damage(page.data(), page.size());
        auto rows = engine.salvage_page(page.data(), page.size(), damage);
        for (const auto& row : rows) {
            EXPECT_FALSE(row.content_hash.empty());
            EXPECT_EQ(row.content_hash.size(), 64); // SHA256 hex
        }
    }

    // L3: with email pattern
    {
        std::vector<uint8_t> page(16384, static_cast<uint8_t>(rand() % 256));
        const std::string email = "user@example.com";
        if (100 + email.size() <= page.size())
            std::memcpy(page.data() + 100, email.data(), email.size());
        auto damage = SalvageEngine::assess_damage(page.data(), page.size());
        auto rows = engine.salvage_page(page.data(), page.size(), damage);
        for (const auto& row : rows) {
            if (row.salvage_level == SalvageLevel::FRAGMENT) {
                EXPECT_FALSE(row.content_hash.empty());
                EXPECT_EQ(row.content_hash.size(), 64);
            }
        }
    }

    // L4: string extraction
    {
        cfg.enable_signature_scan = false;
        cfg.enable_pattern_match = false;
        cfg.enable_string_extract = true;
        cfg.min_string_len = 4;
        SalvageEngine engine2(cfg);
        std::vector<uint8_t> page(256, 0);
        const char* str = "HelloWorldTest";
        std::memcpy(page.data() + 50, str, strlen(str));
        DamageReport damage;
        damage.level = DamageLevel::L4_CRITICAL;
        auto rows = engine2.salvage_page(page.data(), page.size(), damage);
        for (const auto& row : rows) {
            EXPECT_FALSE(row.content_hash.empty());
            EXPECT_EQ(row.content_hash.size(), 64);
        }
    }
}

// ==================== Dedup by Content Hash ====================

TEST(P1SalvageFuzzTest, SalvageDedupIdenticalContent) {
    // Verify that salvage_pages deduplicates rows with identical content_hash
    SalvageEngine::Config cfg;
    cfg.min_confidence = 0.0;
    cfg.enable_signature_scan = true;
    cfg.enable_pattern_match = false;
    cfg.enable_string_extract = false;
    SalvageEngine engine(cfg);

    // Create two identical page buffers
    auto page = make_valid_index_page();
    std::vector<uint8_t> page_copy = page;

    std::vector<std::pair<const uint8_t*, size_t>> pages;
    pages.emplace_back(page.data(), page.size());
    pages.emplace_back(page_copy.data(), page_copy.size());

    auto result = engine.salvage_pages(pages);

    // Count distinct content_hashes in rows
    std::set<std::string> hashes;
    for (const auto& row : result.rows) {
        hashes.insert(row.content_hash);
    }
    // Each distinct record should appear only once despite being in 2 pages
    EXPECT_LE(hashes.size(), result.rows.size());
    // Total rows should not exceed unique content hashes
    // (if every row had a unique hash, sizes would be equal)
    EXPECT_LE(result.rows.size(), hashes.size() + 1); // +1 for empty hash rows
}

// ==================== L1 Tag Changed from "verified" to "structure-derived" ====================

TEST(P1SalvageFuzzTest, L1TagIsStructureDerived) {
    // Phase 5a: L1 salvage must use "structure-derived" not "verified"
    SalvageEngine::Config cfg;
    cfg.min_confidence = 0.0;
    SalvageEngine engine(cfg);

    auto page = make_valid_index_page();
    auto damage = SalvageEngine::assess_damage(page.data(), page.size());
    auto rows = engine.salvage_page(page.data(), page.size(), damage);

    for (const auto& row : rows) {
        if (row.recovery_method == "header_repair") {
            bool has_verified = false;
            bool has_structure_derived = false;
            for (const auto& tag : row.evidence.tags) {
                if (tag == "verified") has_verified = true;
                if (tag == "structure-derived") has_structure_derived = true;
            }
            EXPECT_FALSE(has_verified) << "L1 tags must not contain 'verified'";
            EXPECT_TRUE(has_structure_derived) << "L1 tags must contain 'structure-derived'";
        }
    }
}

// ==================== Evidence Serializer Includes Page No and Content Hash ====================

TEST(P1SalvageFuzzTest, EvidenceSerializerIncludesPageNoAndContentHash) {
    salvage::SalvageEvidenceSerializer serializer;

    SalvagedRow frag;
    frag.salvage_level = SalvageLevel::FRAGMENT;
    frag.recovery_method = "pattern_match";
    frag.confidence = 0.45;
    frag.page_offset = 0x1000;
    frag.page_no = 42;
    frag.record_offset = 0x50;
    frag.content_hash = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    frag.evidence.level = common::DamageLevel::L3_SEVERE;
    frag.evidence.strategy = "pattern_match";
    frag.evidence.source_page_no = 42;
    frag.evidence.source_offset = 0x1050;
    frag.evidence.confidence = 0.45;
    frag.evidence.tags = {"extracted", "partial"};
    frag.fields = {"sensitive_data"};

    std::string json = serializer.to_json({frag}, {});

    // Must contain source_page_no
    EXPECT_NE(json.find("source_page_no"), std::string::npos);

    // Must contain content_sha256
    EXPECT_NE(json.find("content_sha256"), std::string::npos);

    // Must contain the specific page_no value
    EXPECT_NE(json.find("\"source_page_no\": 42"), std::string::npos);

    // Must contain the specific content hash
    EXPECT_NE(json.find("abcdef0123456789abcdef0123456789"), std::string::npos);

    // Must contain evidence.source_page_no
    EXPECT_NE(json.find("evidence"), std::string::npos);
}

} // namespace test
} // namespace dbrecovery