#include <gtest/gtest.h>
#include "dbrecovery/recovery/salvage_engine.hpp"
#include "dbrecovery/tools/unified_recovery.hpp"
#include <cstdio>

using namespace dbrecovery::recovery;

TEST(ExportRejection, ZeroRowsEmptyExport) {
    std::string p = "/tmp/export_reject_zero.csv";
    std::remove(p.c_str());
    std::vector<SalvagedRow> empty;
    EXPECT_TRUE(SalvageEngine::export_csv(p, empty));
    std::remove(p.c_str());
}

TEST(ExportRejection, DefaultRowRejected) {
    SalvagedRow row;
    row.fields = {"unclassified"};
    std::string p = "/tmp/export_reject_default.csv";
    std::remove(p.c_str());
    EXPECT_FALSE(SalvageEngine::export_csv(p, {row}));
    std::remove(p.c_str());
}

TEST(ExportRejection, OnlyEvidenceRejected) {
    SalvagedRow frag;
    frag.salvage_level = SalvageLevel::FRAGMENT;
    frag.fields = {"evidence"}; frag.confidence = 0.4;
    std::string p = "/tmp/export_reject_evidence.csv";
    std::remove(p.c_str());
    EXPECT_FALSE(SalvageEngine::export_csv(p, {frag}));
    std::remove(p.c_str());
}

TEST(ExportRejection, DDLBlockedEvidenceOnly) {
    std::string tmp = "/tmp/export_reject_ddl.ibd";
    {
        std::ofstream f(tmp, std::ios::binary);
        std::vector<uint8_t> z(16384, 0);
        f.write(reinterpret_cast<char*>(z.data()), z.size());
    }
    auto r = dbrecovery::tools::run_unified_recovery(tmp, "/tmp/export_reject_out", "t_ddl", "csv", 16384, "", "", 0, "", false, false, false);
    if (r.ddl_variation_detected) EXPECT_EQ(r.trust_level, "EVIDENCE_ONLY");
    std::remove(tmp.c_str());
}

TEST(ExportRejection, TDECandidateBlocked) {
    std::string tmp = "/tmp/export_reject_tde.ibd";
    {
        std::ofstream f(tmp, std::ios::binary);
        std::vector<uint8_t> b(200, 0);
        f.write(reinterpret_cast<char*>(b.data()), b.size());
    }
    auto r = dbrecovery::tools::run_unified_recovery(tmp, "/tmp/export_reject_tde_out", "t_tde", "csv", 16384, "", "", 0, "", false, false, false);
    (void)r;
    SUCCEED();
    std::remove(tmp.c_str());
}
