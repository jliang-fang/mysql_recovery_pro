#include <gtest/gtest.h>
#include "dbrecovery/recovery/salvage_engine.hpp"
#include "dbrecovery/tools/unified_recovery.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

using namespace dbrecovery::recovery;

namespace {

std::string test_tmp_path(const char* name) {
    const char* test_tmpdir = std::getenv("DBREC_TEST_TMPDIR");
    if (test_tmpdir == nullptr || *test_tmpdir == '\0') {
        throw std::runtime_error("DBREC_TEST_TMPDIR must be set");
    }
    return (std::filesystem::path(test_tmpdir) / name).string();
}

}  // namespace

TEST(ExportRejection, MalformedJsonRecordCannotProduceCsvOrSql) {
    dbrecovery::innodb::IndexRecord record;
    dbrecovery::innodb::IndexFieldValue field;
    field.is_unparsable_json = true;
    field.raw_bytes = {0x00, 0xFF};
    record.fields.push_back(std::move(field));

    EXPECT_TRUE(record.has_unparsable_json_evidence());
    EXPECT_TRUE(record.to_csv_line().empty());
    EXPECT_TRUE(record.to_sql_values().empty());
}

TEST(ExportRejection, ZeroRowsEmptyExport) {
    std::string p = test_tmp_path("export_reject_zero.csv");
    std::remove(p.c_str());
    std::vector<SalvagedRow> empty;
    EXPECT_TRUE(SalvageEngine::export_csv(p, empty));
    std::remove(p.c_str());
}

TEST(ExportRejection, DefaultRowRejected) {
    SalvagedRow row;
    row.fields = {"unclassified"};
    std::string p = test_tmp_path("export_reject_default.csv");
    std::remove(p.c_str());
    EXPECT_FALSE(SalvageEngine::export_csv(p, {row}));
    std::remove(p.c_str());
}

TEST(ExportRejection, OnlyEvidenceRejected) {
    SalvagedRow frag;
    frag.salvage_level = SalvageLevel::FRAGMENT;
    frag.fields = {"evidence"}; frag.confidence = 0.4;
    std::string p = test_tmp_path("export_reject_evidence.csv");
    std::remove(p.c_str());
    EXPECT_FALSE(SalvageEngine::export_csv(p, {frag}));
    std::remove(p.c_str());
}

TEST(ExportRejection, DDLBlockedEvidenceOnly) {
    std::string tmp = test_tmp_path("export_reject_ddl.ibd");
    {
        std::ofstream f(tmp, std::ios::binary);
        std::vector<uint8_t> z(16384, 0);
        f.write(reinterpret_cast<char*>(z.data()), z.size());
    }
    auto r = dbrecovery::tools::run_unified_recovery(tmp, test_tmp_path("export_reject_out"), "t_ddl", "csv", 16384, "", "", 0, "", false, false, false);
    if (r.ddl_variation_detected) EXPECT_EQ(r.trust_level, "EVIDENCE_ONLY");
    std::remove(tmp.c_str());
}

TEST(ExportRejection, TDECandidateBlocked) {
    const std::string input = test_tmp_path("export_reject_tde.ibd");
    const std::filesystem::path output = test_tmp_path("export_reject_tde_out");
    std::filesystem::remove(input);
    std::filesystem::remove_all(output);

    std::vector<uint8_t> page(16384, 0);
    page[24] = 0x45;
    page[25] = 0xBF;
    page[38] = 6;
    page[39] = 1;
    for (size_t i = 55; i < 83; ++i) page[i] = 0xAB;

    {
        std::ofstream file(input, std::ios::binary);
        ASSERT_TRUE(file.good());
        file.write(reinterpret_cast<const char*>(page.data()), page.size());
        ASSERT_TRUE(file.good());
    }

    const auto result = dbrecovery::tools::run_unified_recovery(
        input, output.string(), "t_tde", "all", 16384, "", "", 0,
        "", false, false, false);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.records.empty());
    EXPECT_EQ(result.trust_level, "EVIDENCE_ONLY");
    EXPECT_EQ(result.schema_source, "EVIDENCE_ONLY");
    EXPECT_EQ(result.result_classification, "forensic_evidence");
    EXPECT_EQ(result.recovery_method, "blocked_tde_candidate");
    EXPECT_EQ(result.export_rejection_reason, "tde_candidate_UNVALIDATED");
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_FALSE(std::filesystem::exists(output / "t_tde.csv"));
    EXPECT_FALSE(std::filesystem::exists(output / "t_tde.sql"));

    std::filesystem::remove(input);
    std::filesystem::remove_all(output);
}
