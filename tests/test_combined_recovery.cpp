#include <gtest/gtest.h>
#include "dbrecovery/tools/unified_recovery.hpp"
#include "dbrecovery/recovery/salvage_engine.hpp"
#include "dbrecovery/recovery/strategy_recommender.hpp"
#include "dbrecovery/innodb/ddl_change_detector.hpp"
#include "dbrecovery/common/damage_level.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace dbrecovery;

static void write_be16_combined(std::vector<uint8_t>& p, size_t offset, uint16_t value) {
    p[offset] = static_cast<uint8_t>(value >> 8);
    p[offset + 1] = static_cast<uint8_t>(value);
}

static void write_be32_combined(std::vector<uint8_t>& p, size_t offset, uint32_t value) {
    p[offset] = static_cast<uint8_t>(value >> 24);
    p[offset + 1] = static_cast<uint8_t>(value >> 16);
    p[offset + 2] = static_cast<uint8_t>(value >> 8);
    p[offset + 3] = static_cast<uint8_t>(value);
}

static void write_binary_combined(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

static std::vector<uint8_t> make_compressed_layout_combined(bool valid_flags) {
    std::vector<uint8_t> data(16384 * 8, 0);
    write_be16_combined(data, 24, 17848);
    write_be32_combined(data, 42, valid_flags ? 2u : 10u);
    if (valid_flags) {
        write_be16_combined(data, 8192 + 24, 17855);
    }
    return data;
}

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


TEST(CombinedRecovery, CompressedUnsupportedBlocksUnifiedPipeline) {
    const std::string tmp = "/tmp/combined_compressed_unsupported.ibd";
    write_binary_combined(tmp, make_compressed_layout_combined(true));
    const auto r = tools::run_unified_recovery(
        tmp, "/tmp/combined_compressed_unsupported_out", "compressed_t", "csv",
        16384, "", "", 0, "", false, false, false);
    EXPECT_EQ(r.compression_status, "RECOGNIZED_UNSUPPORTED");
    EXPECT_EQ(r.total_recovered, 0u);
    EXPECT_EQ(r.result_classification, "forensic_evidence");
    EXPECT_FALSE(r.export_success);
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.decoder_validated);
    EXPECT_FALSE(r.compression_rejection_reason.empty());
    EXPECT_EQ(r.export_rejection_reason, "compressed_status_RECOGNIZED_UNSUPPORTED");
    std::filesystem::remove(tmp);
}

TEST(CombinedRecovery, CompressedMetadataFailureBlocksUnifiedPipeline) {
    const std::string tmp = "/tmp/combined_compressed_metadata.ibd";
    write_binary_combined(tmp, make_compressed_layout_combined(false));
    const auto r = tools::run_unified_recovery(
        tmp, "/tmp/combined_compressed_metadata_out", "compressed_bad", "csv",
        16384, "", "", 0, "", false, false, false);
    EXPECT_EQ(r.compression_status, "CORRUPTED_METADATA");
    EXPECT_EQ(r.total_recovered, 0u);
    EXPECT_EQ(r.result_classification, "forensic_evidence");
    EXPECT_FALSE(r.export_success);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.export_rejection_reason, "compressed_status_CORRUPTED_METADATA");
    std::filesystem::remove(tmp);
}
TEST(CombinedRecovery, CompressedMalformedBlocksUnifiedPipeline) {
    const auto r = tools::run_unified_recovery(
        "/tmp/combined_missing_input.ibd", "/tmp/combined_malformed_out",
        "compressed_missing", "csv", 16384, "", "", 0, "", false, false, false);
    EXPECT_EQ(r.compression_status, "MALFORMED");
    EXPECT_EQ(r.total_recovered, 0u);
    EXPECT_EQ(r.result_classification, "forensic_evidence");
    EXPECT_FALSE(r.export_success);
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.compression_rejection_reason.empty());
    EXPECT_EQ(r.export_rejection_reason, "compressed_status_MALFORMED");
}

TEST(CombinedRecovery, CompressedPhysicalMismatchBlocksUnifiedPipeline) {
    const std::string tmp = "/tmp/combined_compressed_physical_mismatch.ibd";
    std::vector<uint8_t> data(16384 * 8, 0);
    write_be16_combined(data, 24, 17848);
    write_be32_combined(data, 42, 2u);
    write_be16_combined(data, 16384 + 24, 17855);
    write_binary_combined(tmp, data);

    const auto r = tools::run_unified_recovery(
        tmp, "/tmp/combined_compressed_physical_mismatch_out", "compressed_mismatch",
        "csv", 16384, "", "", 0, "", false, false, false);
    EXPECT_EQ(r.compression_status, "PHYSICAL_LENGTH_MISMATCH");
    EXPECT_EQ(r.total_recovered, 0u);
    EXPECT_EQ(r.result_classification, "forensic_evidence");
    EXPECT_FALSE(r.export_success);
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.compression_rejection_reason.empty());
    EXPECT_EQ(r.export_rejection_reason, "compressed_status_PHYSICAL_LENGTH_MISMATCH");
    std::filesystem::remove(tmp);
}

TEST(CombinedRecovery, CompressedEncryptedBlocksUnifiedPipeline) {
    const std::string tmp = "/tmp/combined_compressed_encrypted.ibd";
    std::vector<uint8_t> data(16384 * 2, 0);
    write_be16_combined(data, 24, 17848);
    write_be32_combined(data, 42, 2u);
    write_be16_combined(data, 8192 + 24, 17847);
    write_binary_combined(tmp, data);

    const auto r = tools::run_unified_recovery(
        tmp, "/tmp/combined_compressed_encrypted_out", "compressed_encrypted",
        "csv", 16384, "", "", 0, "", false, false, false);
    EXPECT_EQ(r.compression_status, "COMPRESSED_AND_ENCRYPTED");
    EXPECT_EQ(r.total_recovered, 0u);
    EXPECT_EQ(r.result_classification, "forensic_evidence");
    EXPECT_FALSE(r.export_success);
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.compression_rejection_reason.empty());
    EXPECT_EQ(r.export_rejection_reason, "compressed_status_COMPRESSED_AND_ENCRYPTED");
    std::filesystem::remove(tmp);
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
    cfg.page_size = 16384; cfg.min_confidence = 0.3; cfg.include_evidence_fragments = false;
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

TEST(CombinedRecovery, LegacyResultIsCanonicalV2AndNonImportable) {
    const nlohmann::json legacy = {
        {"format_version", 1},
        {"table_name", "legacy_table"},
        {"success", true},
        {"classification", "structured_recovery"},
        {"structured_rows", 7},
        {"damage_level", "L2_MODERATE"},
        {"importable", true},
        {"mode", "undo_history"},
        {"mysql_version", "8.4"},
        {"source_inputs", {"system_tablespace", "/tmp/ibdata1"}}
    };

    const auto contract = tools::result_contract_from_legacy(
        legacy, "fallback_table", "/tmp/source.ibd");
    const auto output = tools::result_contract_object(contract);

    EXPECT_EQ(output.at("format_version"), 2);
    EXPECT_FALSE(output.at("importable").get<bool>());
    EXPECT_EQ(output.at("table_name"), "legacy_table");
    EXPECT_EQ(output.at("structured_rows"), 7);
    EXPECT_EQ(output.at("damage_level"), "L2_MODERATE");
    EXPECT_FALSE(output.contains("mode"));
    EXPECT_FALSE(output.contains("mysql_version"));
    EXPECT_FALSE(output.contains("source_inputs"));
}

TEST(CombinedRecovery, LegacyResultRejectsNegativeCounters) {
    const nlohmann::json legacy = {
        {"structured_rows", -1},
        {"total_pages_scanned", -2},
        {"damaged_pages", -3},
        {"checksum_mismatches", -4},
        {"header_damage_pages", -5}
    };

    const auto contract = tools::result_contract_from_legacy(
        legacy, "fallback_table", "/tmp/source.ibd");

    EXPECT_EQ(contract.structured_rows, 0u);
    EXPECT_EQ(contract.total_pages_scanned, 0u);
    EXPECT_EQ(contract.damaged_pages, 0u);
    EXPECT_EQ(contract.checksum_mismatches, 0u);
    EXPECT_EQ(contract.header_damage_pages, 0u);
}

TEST(CombinedRecovery, EmptyLegacyResultHasAllCanonicalFields) {
    const auto output = tools::result_contract_object(
        tools::result_contract_from_legacy(
            nlohmann::json::object(), "fallback_table", "/tmp/source.ibd"));
    const std::vector<std::string> keys = {
        "format_version", "table_name", "success", "classification",
        "structured_rows", "evidence_fragments", "schema_source",
        "schema_trust_level", "damage_level", "recovery_method",
        "ddl_variation_detected", "export_success",
        "export_rejection_reason", "total_pages_scanned", "damaged_pages",
        "checksum_mismatches", "header_damage_pages", "importable", "reason",
        "source_ibd", "compression_status", "logical_page_size", "physical_page_size",
        "compression_classification_basis", "decoder_validated", "compression_rejection_reason"
    };

    for (const auto& key : keys) {
        EXPECT_TRUE(output.contains(key)) << key;
    }
    EXPECT_EQ(output.size(), keys.size());
    EXPECT_EQ(output.at("format_version"), 2);
    EXPECT_FALSE(output.at("importable").get<bool>());
    EXPECT_EQ(output.at("table_name"), "fallback_table");
    EXPECT_EQ(output.at("source_ibd"), "/tmp/source.ibd");
}

TEST(CombinedRecovery, RecoveryReportCanonicalFieldsMatchUnifiedResult) {
    tools::UnifiedRecoveryResult result;
    result.table_name = "report_table";
    result.source_ibd = "/tmp/report_table.ibd";
    result.success = true;
    result.result_classification = "structured_recovery";
    result.total_recovered = 3;
    result.schema_source = "SDI";
    result.trust_level = "TRUSTED";
    result.damage_level = common::DamageLevel::L1_MINOR;
    result.recovery_method = "index_parser";
    result.ddl_variation_detected = true;
    result.export_success = false;
    result.export_rejection_reason = "checksum mismatch";

    tools::PageDamageEvidence page;
    page.damage_level = common::DamageLevel::L1_MINOR;
    page.checksum_valid = false;
    page.fil_header_valid = true;
    page.page_header_valid = false;
    result.page_damage.push_back(page);

    result.report.result_contract = tools::result_contract_from_unified(
        result, result.table_name, result.source_ibd);
    const auto report = nlohmann::json::parse(result.report.to_json());
    const auto contract = tools::result_contract_object(
        tools::result_contract_from_unified(
            result, result.table_name, result.source_ibd));

    for (const auto& item : contract.items()) {
        ASSERT_TRUE(report.contains(item.key())) << item.key();
        EXPECT_EQ(report.at(item.key()), item.value()) << item.key();
    }
    EXPECT_EQ(report.at("format_version"), 2);
    EXPECT_FALSE(report.at("importable").get<bool>());
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
    EXPECT_EQ(r.table_name, "prov_tbl");
    EXPECT_EQ(r.source_ibd, tmp);
    bool ok = tools::write_unified_result_json("/tmp/combined_out3", "prov_tbl", r);
    EXPECT_TRUE(ok);
    std::ifstream jf("/tmp/combined_out3/prov_tbl_result.json");
    ASSERT_TRUE(jf.good());
    nlohmann::json output;
    jf >> output;
    EXPECT_EQ(output.at("format_version"), 2);
    EXPECT_EQ(output.at("table_name"), "prov_tbl");
    EXPECT_EQ(output.at("source_ibd"), tmp);
    EXPECT_EQ(output.at("structured_rows"), r.total_recovered);
    EXPECT_EQ(output.at("evidence_fragments"), r.evidence_fragments.size());
    EXPECT_EQ(output.at("schema_source"), r.schema_source);
    EXPECT_EQ(output.at("schema_trust_level"), r.trust_level);
    EXPECT_EQ(output.at("damage_level"), common::damage_level_to_string(r.damage_level));
    EXPECT_EQ(output.at("recovery_method"), r.recovery_method);
    EXPECT_EQ(output.at("ddl_variation_detected"), r.ddl_variation_detected);
    EXPECT_EQ(output.at("export_success"), r.export_success);
    EXPECT_EQ(output.at("importable"), false);
    const auto report_json = nlohmann::json::parse(r.report.to_json());
    for (const auto& item : output.items()) {
        ASSERT_TRUE(report_json.contains(item.key())) << item.key();
        EXPECT_EQ(report_json.at(item.key()), item.value()) << item.key();
    }
    std::filesystem::remove(tmp);
}
