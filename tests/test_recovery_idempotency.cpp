#include <gtest/gtest.h>
#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/recovery/salvage_engine.hpp"
#include "dbrecovery/tools/unified_recovery.hpp"
#include "dbrecovery/innodb/ddl_change_detector.hpp"
#include <filesystem>

using namespace dbrecovery;

TEST(RecoveryIdempotency, IndexParserTwice) {
    dbrecovery::innodb::InnoDBIndexParser p1, p2;
    p1.set_recover_deleted(true); p1.set_bad_page_mode(2);
    p2.set_recover_deleted(true); p2.set_bad_page_mode(2);
    std::string tmp = "/tmp/idem_parser.ibd";
    {
        std::ofstream f(tmp, std::ios::binary);
        std::vector<uint8_t> z(16384, 0);
        f.write(reinterpret_cast<char*>(z.data()), z.size());
    }
    auto r1 = p1.parse_ibd_force(tmp);
    auto r2 = p2.parse_ibd_force(tmp);
    EXPECT_EQ(r1.size(), r2.size());
    std::filesystem::remove(tmp);
}

TEST(RecoveryIdempotency, SalvageEngineTwice) {
    recovery::SalvageEngine::Config cfg;
    cfg.min_confidence = 0.0; cfg.enable_string_extract = true; cfg.enable_pattern_match = true;
    recovery::SalvageEngine e1(cfg), e2(cfg);
    std::vector<uint8_t> page(4096, 0);
    std::string email="idem@test.org";
    memcpy(page.data()+50, email.data(), email.size());
    recovery::DamageReport dr; dr.level = common::DamageLevel::L3_SEVERE;
    auto a = e1.salvage_page(page.data(), page.size(), dr);
    auto b = e2.salvage_page(page.data(), page.size(), dr);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i=0;i<a.size();++i) {
        EXPECT_EQ(a[i].fields, b[i].fields);
        EXPECT_EQ(a[i].content_hash, b[i].content_hash);
    }
}

TEST(RecoveryIdempotency, UnifiedPipelineTwice) {
    std::string tmp = "/tmp/idem_unified.ibd";
    {
        std::ofstream f(tmp, std::ios::binary);
        std::vector<uint8_t> z(16384, 0);
        f.write(reinterpret_cast<char*>(z.data()), z.size());
    }
    auto r1 = tools::run_unified_recovery(tmp, "/tmp/idem_out1", "idem_tbl", "csv", 16384, "", "", 0, "", false, false, false);
    auto r2 = tools::run_unified_recovery(tmp, "/tmp/idem_out2", "idem_tbl", "csv", 16384, "", "", 0, "", false, false, false);
    EXPECT_EQ(r1.total_recovered, r2.total_recovered);
    EXPECT_EQ(r1.evidence_fragments.size(), r2.evidence_fragments.size());
    EXPECT_EQ(r1.result_classification, r2.result_classification);
    std::filesystem::remove(tmp);
}

TEST(RecoveryIdempotency, DDLDetectionTwice) {
    std::string tmp = "/tmp/idem_ddl.ibd";
    {
        std::ofstream f(tmp, std::ios::binary);
        std::vector<uint8_t> z(16384, 0);
        f.write(reinterpret_cast<char*>(z.data()), z.size());
    }
    innodb::DDLChangeChainDetector d1(16384), d2(16384);
    auto a = d1.detect(tmp);
    auto b = d2.detect(tmp);
    EXPECT_EQ(a.possible_schema_variation, b.possible_schema_variation);
    EXPECT_EQ(a.blocked_auto_decode, b.blocked_auto_decode);
    std::filesystem::remove(tmp);
}
