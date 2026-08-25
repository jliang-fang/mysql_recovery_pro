#include <gtest/gtest.h>
#include "dbrecovery/recovery/salvage_engine.hpp"
#include "dbrecovery/salvage/evidence_serializer.hpp"
#include <cstdio>

using namespace dbrecovery::recovery;

TEST(L3L4Isolation, NotInCsvExport) {
    SalvagedRow r;
    r.salvage_level = SalvageLevel::FRAGMENT;
    r.fields = {"frag"}; r.confidence = 0.4;
    SalvagedRow s;
    s.salvage_level = SalvageLevel::STRING_ONLY;
    s.fields = {"str"}; s.confidence = 0.3;
    std::string p1 = "/tmp/l3_l4_iso_csv1.csv";
    std::remove(p1.c_str());
    EXPECT_FALSE(SalvageEngine::export_csv(p1, {r}));
    std::remove(p1.c_str());
    EXPECT_FALSE(SalvageEngine::export_csv(p1, {s}));
    std::remove(p1.c_str());
    EXPECT_FALSE(SalvageEngine::export_csv(p1, {r, s}));
    std::remove(p1.c_str());
}

TEST(L3L4Isolation, InEvidenceJson) {
    dbrecovery::salvage::SalvageEvidenceSerializer ser;
    SalvagedRow frag;
    frag.salvage_level = SalvageLevel::FRAGMENT;
    frag.recovery_method = "pattern_match"; frag.confidence = 0.4;
    frag.evidence.level = dbrecovery::common::DamageLevel::L3_SEVERE;
    frag.evidence.strategy = "pattern_match"; frag.fields = {"evidence@test.org"};
    std::string j = ser.to_json({frag}, {});
    EXPECT_NE(j.find("fragment_count"), std::string::npos);
    EXPECT_NE(j.find("non_importable_warning"), std::string::npos);
}

TEST(L3L4Isolation, IncludeFragmentsFlagStillNotInCsv) {
    SalvageEngine::Config cfg;
    cfg.min_confidence = 0.0;
    cfg.enable_pattern_match = true; cfg.enable_string_extract = true;
    cfg.enable_signature_scan = false;
    cfg.include_evidence_fragments = true;
    SalvageEngine eng(cfg);
    std::vector<uint8_t> p(4096, 0);
    std::string email = "iso@test.org";
    memcpy(p.data()+100, email.data(), email.size());
    dbrecovery::common::DamageLevel lvl = dbrecovery::common::DamageLevel::L3_SEVERE;
    DamageReport dr; dr.level = lvl;
    std::vector<std::pair<const uint8_t*,size_t>> pages = {{p.data(), p.size()}};
    auto res = eng.salvage_pages(pages);
    for (auto &row : res.rows) EXPECT_EQ(row.salvage_level, SalvageLevel::STRUCTURED);
    for (auto &f : res.evidence_fragments) EXPECT_NE(f.salvage_level, SalvageLevel::STRUCTURED);
    std::string p2 = "/tmp/l3_l4_iso_csv2.csv";
    std::remove(p2.c_str());
    if (!res.evidence_fragments.empty()) {
        EXPECT_FALSE(SalvageEngine::export_csv(p2, res.evidence_fragments));
        std::remove(p2.c_str());
    }
    if (!res.rows.empty()) EXPECT_TRUE(SalvageEngine::export_csv(p2, res.rows));
    std::remove(p2.c_str());
}

TEST(L3L4Isolation, ExportRejected) {
    SalvagedRow structured; structured.salvage_level = SalvageLevel::STRUCTURED;
    structured.fields = {"a","b"}; structured.confidence = 0.9; structured.field_count = 2;
    SalvagedRow frag; frag.salvage_level = SalvageLevel::FRAGMENT;
    frag.fields = {"x"}; frag.confidence = 0.4;
    std::string pp = "/tmp/l3_l4_iso_reject.csv";
    std::remove(pp.c_str());
    std::vector<SalvagedRow> mixed = {structured, frag};
    EXPECT_FALSE(SalvageEngine::export_csv(pp, mixed));
    std::remove(pp.c_str());
}
