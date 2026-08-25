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
    std::string url="https://example.com";
    memcpy(p.data()+50, url.data(), url.size());
    DamageReport dr; dr.level=dbrecovery::common::DamageLevel::L3_SEVERE;
    auto rows = eng.salvage_page(p.data(), p.size(), dr);
    for (auto &r: rows) EXPECT_NE(r.salvage_level, SalvageLevel::STRUCTURED);
    std::vector<std::pair<const uint8_t*,size_t>> pages={{p.data(), p.size()}};
    auto res = eng.salvage_pages(pages);
    for (auto &r: res.rows) EXPECT_EQ(r.salvage_level, SalvageLevel::STRUCTURED);
}
