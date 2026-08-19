#include <gtest/gtest.h>
#include "dbrecovery/tools/unified_recovery.hpp"
#include "dbrecovery/recovery/salvage_engine.hpp"
#include "dbrecovery/recovery/strategy_recommender.hpp"
#include "dbrecovery/innodb/ddl_change_detector.hpp"
#include "dbrecovery/common/damage_level.hpp"
#include <filesystem>
#include <fstream>

using namespace dbrecovery;

static std::vector<uint8_t> make_valid_index_page_combined() {
    std::vector<uint8_t> p(16384, 0);
    p[0]=0x12; p[1]=0x34; p[2]=0x56; p[3]=0x78;
    p[4]=0x00; p[5]=0x00; p[6]=0x00; p[7]=0x01;
    p[16]=0x00; p[17]=0x00; p[18]=0x00; p[19]=0x01; p[20]=0x23; p[21]=0x45; p[22]=0x67; p[23]=0x89;
    p[24]=0x45; p[25]=0xBF;
    p[34]=0x00; p[35]=0x00; p[36]=0x00; p[37]=0x01;
    p[54]=0x02; p[55]=0x00;
    p[40]=0x00; p[41]=0x01;
    p[100]=0x90; p[101]=0x01;
    p[16376]=0x78; p[16377]=0x56; p[16378]=0x34; p[16379]=0x12;
    p[16368]=0x00; p[16369]=0x00; p[16370]=0x00; p[16371]=0x01;
    p[16372]=0x23; p[16373]=0x45; p[16374]=0x67; p[16375]=0x89;
    return p;
}

TEST(CombinedRecovery, IndexParserToHeuristicFallback) {
    std::string tmp = "/tmp/combined_heuristic_test.ibd";
    {
        std::ofstream f(tmp, std::ios::binary);
        std::vector<uint8_t> zeros(16384, 0);
        f.write(reinterpret_cast<char*>(zeros.data()), zeros.size());
    }
    auto r = tools::run_unified_recovery(tmp, "/tmp/combined_out1", "t1", "csv", 16384, "", "", 0, "", false, false, false);
    EXPECT_EQ(r.result_classification, "forensic_evidence");
    EXPECT_EQ(r.total_recovered, 0u);
    std::filesystem::remove(tmp);
}

TEST(CombinedRecovery, IndexParserToSalvageEngine) {
    recovery::SalvageEngine::Config cfg;
    cfg.page_size = 16384; cfg.min_confidence = 0.3; cfg.include_fragments = false;
    recovery::SalvageEngine eng(cfg);
    auto page = make_valid_index_page_combined();
    page[0]=0x00; page[1]=0x00;
    auto dmg = recovery::SalvageEngine::assess_damage(page.data(), page.size());
    auto rows = eng.salvage_page(page.data(), page.size(), dmg);
    for (auto &row : rows) EXPECT_EQ(row.salvage_level, recovery::SalvageLevel::STRUCTURED);
}

TEST(CombinedRecovery, DDLVariationDetected) {
    std::string tmp = "/tmp/combined_ddl_test.ibd";
    {
        std::ofstream f(tmp, std::ios::binary);
        std::vector<uint8_t> zeros(16384, 0);
        f.write(reinterpret_cast<char*>(zeros.data()), zeros.size());
    }
    innodb::DDLChangeChainDetector det(16384);
    auto res = det.detect(tmp);
    (void)res;
    auto r = tools::run_unified_recovery(tmp, "/tmp/combined_out2", "t2", "csv", 16384, "", "", 0, "", false, false, false);
    EXPECT_FALSE(r.ddl_variation_detected && r.trust_level != "EVIDENCE_ONLY");
    std::filesystem::remove(tmp);
}

TEST(CombinedRecovery, StrategyRecommendationInformational) {
    recovery::StrategyRecommender rec;
    auto rr = rec.recommend("CORRUPTION");
    std::string txt = rr.to_text();
    EXPECT_FALSE(txt.empty());
}

TEST(CombinedRecovery, ProvenanceFieldsPresent) {
    std::string tmp = "/tmp/combined_prov_test.ibd";
    {
        std::ofstream f(tmp, std::ios::binary);
        std::vector<uint8_t> zeros(16384, 0);
        f.write(reinterpret_cast<char*>(zeros.data()), zeros.size());
    }
    auto r = tools::run_unified_recovery(tmp, "/tmp/combined_out3", "prov_tbl", "csv", 16384, "", "", 0, "", false, false, false);
    EXPECT_FALSE(r.schema_source.empty());
    EXPECT_FALSE(r.trust_level.empty());
    EXPECT_FALSE(r.recovery_method.empty());
    EXPECT_FALSE(r.result_classification.empty());
    bool ok = tools::write_unified_result_json("/tmp/combined_out3", "prov_tbl", r);
    EXPECT_TRUE(ok);
    std::ifstream jf("/tmp/combined_out3/prov_tbl_result.json");
    ASSERT_TRUE(jf.good());
    std::string content((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("schema_source"), std::string::npos);
    EXPECT_NE(content.find("schema_trust_level"), std::string::npos);
    EXPECT_NE(content.find("damage_level"), std::string::npos);
    EXPECT_NE(content.find("recovery_method"), std::string::npos);
    EXPECT_NE(content.find("ddl_variation_detected"), std::string::npos);
    std::filesystem::remove(tmp);
}
