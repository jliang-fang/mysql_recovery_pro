#include <gtest/gtest.h>
#include "dbrecovery/recovery/salvage_engine.hpp"
#include "dbrecovery/common/damage_level.hpp"

using namespace dbrecovery::recovery;

static std::vector<uint8_t> make_clean_index_page_sa() {
    std::vector<uint8_t> p(16384, 0);
    p[0]=0x12; p[1]=0x34; p[2]=0x56; p[3]=0x78;
    p[4]=0x00; p[5]=0x00; p[6]=0x00; p[7]=0x03;
    p[16]=0x00; p[17]=0x00; p[18]=0x00; p[19]=0x01; p[20]=0x23; p[21]=0x45; p[22]=0x67; p[23]=0x89;
    p[24]=0x45; p[25]=0xBF;
    p[34]=0x00; p[35]=0x00; p[36]=0x00; p[37]=0x02;
    p[54]=0x02; p[55]=0x00;
    p[16376]=0x12; p[16377]=0x34; p[16378]=0x56; p[16379]=0x78;
    p[16368]=0x00; p[16369]=0x00; p[16370]=0x00; p[16371]=0x01;
    p[16372]=0x23; p[16373]=0x45; p[16374]=0x67; p[16375]=0x89;
    return p;
}

// A non-leaf page whose free space between PAGE_HEAP_TOP and the page
// directory holds stale bytes but whose own checksum is intact is NOT damaged.
// InnoDB reuses that gap after a split; a real 1m/8.4 tablespace contains such
// a page (page 37, 217 non-zero bytes, valid checksum). Treating it as damage
// set the page-integrity gate, demoted all 1,000,000 parsed rows to evidence
// fragments and exported zero rows.
TEST(SalvageAccuracy, NonLeafStaleFreeSpaceWithValidChecksumIsNotDamage) {
    auto page = make_clean_index_page_sa();
    // level = 1 (non-leaf), heap_top = 200, n_dir_slots = 2
    page[64]=0x00; page[65]=0x01;
    page[38]=0x00; page[39]=0x02;
    page[40]=0x00; page[41]=0xC8;
    for (size_t i = 200; i < 16364; ++i) page[i] = 0xAB;
    // keep the page's own checksum self-consistent so it stays "intact"
    page[16368]=page[0]; page[16369]=page[1]; page[16370]=page[2]; page[16371]=page[3];

    auto dmg = SalvageEngine::assess_damage(page.data(), page.size());
    EXPECT_FALSE(dmg.free_space_corrupt)
        << "stale free space on a checksum-intact page must not be reported as damage";
    EXPECT_LT(static_cast<int>(dmg.level),
              static_cast<int>(dbrecovery::common::DamageLevel::L3_SEVERE))
        << "stale free space must never escalate a page to severe damage";
}

// The same bytes on a page whose checksum is actually broken are still
// reported: the exemption is for checksum-intact pages only.
TEST(SalvageAccuracy, NonLeafFreeSpaceCorruptionIsReportedWhenChecksumFails) {
    auto page = make_clean_index_page_sa();
    page[64]=0x00; page[65]=0x01;
    page[38]=0x00; page[39]=0x02;
    page[40]=0x00; page[41]=0xC8;
    for (size_t i = 200; i < 16364; ++i) page[i] = 0xAB;
    // Declare CRC32C (so the 5.6 folded-LSN bypass cannot apply) and store a
    // checksum that does not match the page contents.
    page[0]=0xDE; page[1]=0xAD; page[2]=0xBE; page[3]=0xEF;
    page[16368]=0xDE; page[16369]=0xAD; page[16370]=0xBE; page[16371]=0xEF;
    page[16380]=0xEB; page[16381]=0xDA; page[16382]=0xCD; page[16383]=0xDB;

    auto dmg = SalvageEngine::assess_damage(page.data(), page.size());
    EXPECT_FALSE(dmg.checksum_valid);
    EXPECT_TRUE(dmg.free_space_corrupt);
}

TEST(SalvageAccuracy, CleanPageZeroFalsePositive) {
    SalvageEngine::Config cfg; cfg.min_confidence=0.3; cfg.include_evidence_fragments=false;
    SalvageEngine eng(cfg);
    auto page = make_clean_index_page_sa();
    auto dmg = SalvageEngine::assess_damage(page.data(), page.size());
    EXPECT_EQ(dmg.level, dbrecovery::common::DamageLevel::NONE);
    std::vector<std::pair<const uint8_t*,size_t>> pages = {{page.data(), page.size()}};
    auto r = eng.salvage_pages(pages);
    EXPECT_TRUE(r.rows.empty());
    EXPECT_TRUE(r.evidence_fragments.empty());
    EXPECT_FALSE(r.success);
}

TEST(SalvageAccuracy, CorruptPageRecall) {
    SalvageEngine::Config cfg; cfg.min_confidence=0.0; cfg.enable_signature_scan=true; cfg.enable_pattern_match=true; cfg.enable_string_extract=true;
    SalvageEngine eng(cfg);
    std::vector<uint8_t> p(16384, 0);
    p[24]=0xFF; p[25]=0xFF;
    std::fill(p.begin()+200, p.begin()+600, static_cast<uint8_t>('A'));
    std::string email="recall@test.org";
    memcpy(p.data()+300, email.data(), email.size());
    auto dmg = SalvageEngine::assess_damage(p.data(), p.size());
    EXPECT_GE(static_cast<int>(dmg.level), static_cast<int>(dbrecovery::common::DamageLevel::L3_SEVERE));
    auto rows = eng.salvage_page(p.data(), p.size(), dmg);
    EXPECT_FALSE(rows.empty());
}

TEST(SalvageAccuracy, ConfidenceThresholdFiltering) {
    SalvageEngine::Config cfg; cfg.min_confidence=0.9; cfg.enable_string_extract=true; cfg.enable_pattern_match=true; cfg.enable_signature_scan=true;
    SalvageEngine eng(cfg);
    std::vector<uint8_t> p(4096, 0);
    std::fill(p.begin()+10, p.begin()+100, static_cast<uint8_t>('B'));
    dbrecovery::common::DamageLevel lvl = dbrecovery::common::DamageLevel::L4_CRITICAL;
    DamageReport dr; dr.level=lvl;
    auto rows = eng.salvage_page(p.data(), p.size(), dr);
    for (auto &r: rows) EXPECT_GE(r.confidence, 0.9);
}

TEST(SalvageAccuracy, L3L4NotInRows) {
    SalvageEngine::Config cfg; cfg.min_confidence=0.0; cfg.enable_signature_scan=false; cfg.enable_pattern_match=true; cfg.enable_string_extract=true;
    SalvageEngine eng(cfg);
    std::vector<uint8_t> p(4096, 0);
    p[24] = 0xFF;
    p[25] = 0xFF;
    std::string url="https://example.com";
    memcpy(p.data()+50, url.data(), url.size());
    DamageReport dr; dr.level=dbrecovery::common::DamageLevel::L3_SEVERE;
    auto rows = eng.salvage_page(p.data(), p.size(), dr);
    for (auto &r: rows) EXPECT_NE(r.salvage_level, SalvageLevel::STRUCTURED);
    std::vector<std::pair<const uint8_t*,size_t>> pages={{p.data(), p.size()}};
    auto res = eng.salvage_pages(pages);
    EXPECT_TRUE(res.success);
    EXPECT_TRUE(res.rows.empty());
    EXPECT_FALSE(res.evidence_fragments.empty());
    for (auto &r: res.rows) EXPECT_EQ(r.salvage_level, SalvageLevel::STRUCTURED);
}
