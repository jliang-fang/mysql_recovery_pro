#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "dbrecovery/report/recovery_report.hpp"
#include "dbrecovery/validate/unified_validator.hpp"

using namespace dbrecovery;
using namespace dbrecovery::report;
using namespace dbrecovery::validate;

class RecoveryReportTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* env_tmp = std::getenv("DBREC_TEST_TMPDIR");
        if (env_tmp && *env_tmp) {
            test_dir_ = std::filesystem::path(env_tmp) / "report_test";
        } else {
            test_dir_ = std::filesystem::temp_directory_path() / "dbrec_report_test";
        }
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }

    RecoveryReport make_sample_report() {
        ReportBuilder builder;
        builder.set_recovery_type("DROP_TABLE")
               .set_mysql_version("8.0.35")
               .set_status("SUCCESS");

        InputInfo in;
        in.source_type = "IBD";
        in.source_path = "/data/test_table.ibd";
        in.source_size_bytes = 1048576;
        in.has_schema = true;
        in.schema_source = "SDI";
        in.has_undo_logs = true;
        in.files_processed = {"test_table.ibd"};
        builder.set_input(in);

        StrategyInfo st;
        st.primary_strategy = "PAGE_PARSER";
        st.strategies_attempted = {"PAGE_PARSER", "CARVER"};
        st.fallback_strategies = {"RAW_CARVER"};
        st.parameters["page_size"] = "16384";
        builder.set_strategy(st);

        DataStats ds;
        ds.total_pages_scanned = 64;
        ds.index_pages_found = 40;
        ds.blob_pages_found = 4;
        ds.corrupted_pages = 2;
        ds.skipped_pages = 0;
        ds.total_records_attempted = 1000;
        ds.records_fully_recovered = 980;
        ds.records_partially_recovered = 10;
        ds.records_failed = 10;
        ds.recovery_rate_percent = 98.0;
        ds.blob_records_recovered = 4;
        ds.history_versions_recovered = 15;
        ds.flashback_statements_generated = 15;
        ds.records_by_type["PRIMARY"] = 980;
        builder.set_data(ds);

        DamageDetail dmg;
        dmg.page_no = 12;
        dmg.damage_type = "checksum_mismatch";
        dmg.description = "Header checksum does not match payload";
        builder.add_damage(dmg);

        builder.add_failure("DELETED_OVERWRITTEN", 10, 1.0, "Check undo tablespaces for historical versions");
        builder.add_ddl("CREATE TABLE `test_table` (`id` int PRIMARY KEY, `data` varchar(100));");

        OutputInfo out;
        out.output_directory = test_dir_.string();
        out.output_files = {"recovered_data.sql", "recovered_data.csv"};
        out.total_output_size_bytes = 204800;
        out.format = "SQL,CSV";
        builder.set_output(out);

        TimingInfo tm;
        tm.scan_time_seconds = 0.12;
        tm.parse_time_seconds = 0.45;
        tm.export_time_seconds = 0.08;
        tm.total_time_seconds = 0.65;
        builder.set_timing(tm);

        return builder.build();
    }

    std::filesystem::path test_dir_;
};

TEST_F(RecoveryReportTest, BuildAndGenerateReportId) {
    RecoveryReport report = make_sample_report();
    EXPECT_FALSE(report.report_id.empty());
    EXPECT_EQ(report.recovery_type, "DROP_TABLE");
    EXPECT_EQ(report.mysql_version, "8.0.35");
    EXPECT_EQ(report.overall_status, "SUCCESS");
}

TEST_F(RecoveryReportTest, ToJsonGeneratesValidJson) {
    RecoveryReport report = make_sample_report();
    std::string json_str = report.to_json();
    EXPECT_FALSE(json_str.empty());

    nlohmann::json parsed;
    EXPECT_NO_THROW({
        parsed = nlohmann::json::parse(json_str);
    });

    EXPECT_EQ(parsed["recovery_type"], "DROP_TABLE");
    EXPECT_EQ(parsed["mysql_version"], "8.0.35");
    EXPECT_EQ(parsed["overall_status"], "SUCCESS");
    EXPECT_EQ(parsed["data"]["records_fully_recovered"], 980);
    EXPECT_EQ(parsed["data"]["total_records_attempted"], 1000);
    EXPECT_DOUBLE_EQ(parsed["data"]["recovery_rate_percent"], 98.0);
    EXPECT_EQ(parsed["input"]["has_schema"], true);
    EXPECT_EQ(parsed["damage"]["pages_with_checksum_mismatch"], 1);
    EXPECT_EQ(parsed["failures"]["categories"].size(), 1);
}

TEST_F(RecoveryReportTest, ToTextContainsKeyInformation) {
    RecoveryReport report = make_sample_report();
    std::string text = report.to_text();
    EXPECT_FALSE(text.empty());

    EXPECT_NE(text.find("DROP_TABLE"), std::string::npos);
    EXPECT_NE(text.find("8.0.35"), std::string::npos);
    EXPECT_NE(text.find("SUCCESS"), std::string::npos);
    EXPECT_NE(text.find("980"), std::string::npos);
    EXPECT_NE(text.find("CREATE TABLE"), std::string::npos);
}

TEST_F(RecoveryReportTest, ToHtmlGeneratesHtmlDocument) {
    RecoveryReport report = make_sample_report();
    std::string html = report.to_html();
    EXPECT_FALSE(html.empty());

    EXPECT_NE(html.find("<!DOCTYPE html>"), std::string::npos);
    EXPECT_NE(html.find("<html"), std::string::npos);
    EXPECT_NE(html.find("DROP_TABLE"), std::string::npos);
    EXPECT_NE(html.find("SUCCESS"), std::string::npos);
    EXPECT_NE(html.find("980"), std::string::npos);
    EXPECT_NE(html.find("</html>"), std::string::npos);
}

TEST_F(RecoveryReportTest, WriteFilesAcceptsOnlyExactDeduplicatedFormats) {
    RecoveryReport report = make_sample_report();
    std::string base_path = (test_dir_ / "exact_formats").string();

    auto files = report.write_files(base_path, " JSON,html,json,unknown,textual,text ");
    ASSERT_EQ(files.size(), 3);
    EXPECT_TRUE(std::filesystem::exists(base_path + ".json"));
    EXPECT_TRUE(std::filesystem::exists(base_path + ".html"));
    EXPECT_TRUE(std::filesystem::exists(base_path + ".txt"));
}

TEST_F(RecoveryReportTest, WriteFilesReturnsEmptyForUnwritableBasePath) {
    RecoveryReport report = make_sample_report();
    const auto blocker = test_dir_ / "regular_file";
    {
        std::ofstream file(blocker);
        ASSERT_TRUE(file);
        file << "not a directory";
    }

    auto files = report.write_files((blocker / "report").string(), "json,text,html");
    EXPECT_TRUE(files.empty());
}

TEST_F(RecoveryReportTest, WriteFilesOverwritesExistingReport) {
    RecoveryReport report = make_sample_report();
    const auto base_path = (test_dir_ / "overwrite").string();
    ASSERT_EQ(report.write_files(base_path, "text").size(), 1);

    const auto report_path = base_path + ".txt";
    std::ifstream before(report_path);
    const std::string initial((std::istreambuf_iterator<char>(before)),
                              std::istreambuf_iterator<char>());

    report.overall_status = "PARTIAL";
    ASSERT_EQ(report.write_files(base_path, "text").size(), 1);

    std::ifstream after(report_path);
    const std::string updated((std::istreambuf_iterator<char>(after)),
                              std::istreambuf_iterator<char>());
    EXPECT_NE(updated, initial);
    EXPECT_NE(updated.find("PARTIAL"), std::string::npos);
}

TEST_F(RecoveryReportTest, HtmlEscapesUserControlledReportValues) {
    RecoveryReport report = make_sample_report();
    report.report_id = "<report>&\"";
    report.input.source_path = "<input>&\"";
    report.reconstructed_ddl = {"CREATE TABLE <unsafe> & \"quoted\";"};

    const auto html = report.to_html();
    EXPECT_NE(html.find("&lt;report&gt;&amp;&quot;"), std::string::npos);
    EXPECT_NE(html.find("&lt;input&gt;&amp;&quot;"), std::string::npos);
    EXPECT_NE(html.find("CREATE TABLE &lt;unsafe&gt; &amp; &quot;quoted&quot;;"), std::string::npos);
}

// ==================== CapabilityReport & UnifiedValidator Tests ====================

TEST(UnifiedValidatorTest, ValidateMySQL80Features) {
    CapabilityReport cap = UnifiedValidator::validate_all(MySQLVersion::MYSQL_80);
    EXPECT_EQ(cap.version_id, MySQLVersion::MYSQL_80);
    EXPECT_EQ(cap.mysql_version, "MySQL 8.0");

    const auto* sdi = cap.find_feature("sdi.extraction");
    ASSERT_NE(sdi, nullptr);
    EXPECT_EQ(sdi->status, CapabilityStatus::EVIDENCE_ONLY);
    EXPECT_EQ(sdi->output_category, RecoveryOutputCategory::FORENSIC_EVIDENCE);

    const auto* l3_l4 = cap.find_feature("salvage.l3");
    ASSERT_NE(l3_l4, nullptr);
    EXPECT_EQ(l3_l4->status, CapabilityStatus::EVIDENCE_ONLY);
    EXPECT_EQ(l3_l4->output_category, RecoveryOutputCategory::FORENSIC_EVIDENCE);

    std::string json_rep = cap.to_json();
    EXPECT_FALSE(json_rep.empty());
    nlohmann::json parsed = nlohmann::json::parse(json_rep);
    EXPECT_EQ(parsed["mysql_version"], "MySQL 8.0");
    EXPECT_FALSE(parsed["features"].empty());
}

TEST(UnifiedValidatorTest, ValidateMySQL57Features) {
    CapabilityReport cap = UnifiedValidator::validate_all(MySQLVersion::MYSQL_57);
    EXPECT_EQ(cap.version_id, MySQLVersion::MYSQL_57);
    EXPECT_EQ(cap.mysql_version, "MySQL 5.7");

    const auto* sdi = cap.find_feature("sdi.extraction");
    ASSERT_NE(sdi, nullptr);
    EXPECT_EQ(sdi->status, CapabilityStatus::REJECTED);
}

TEST(UnifiedValidatorTest, ValidateMySQL90Features) {
    CapabilityReport cap = UnifiedValidator::validate_all(MySQLVersion::MYSQL_90);
    EXPECT_EQ(cap.version_id, MySQLVersion::MYSQL_90);
    EXPECT_EQ(cap.mysql_version, "MySQL 9.0");

    const auto* sdi = cap.find_feature("sdi.extraction");
    ASSERT_NE(sdi, nullptr);
    EXPECT_EQ(sdi->status, CapabilityStatus::EVIDENCE_ONLY);
}
