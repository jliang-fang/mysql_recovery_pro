#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <set>
#include <sys/wait.h>
#include <vector>


#ifndef DBRECOVERY_CLI_PATH
#error "DBRECOVERY_CLI_PATH must name the recovery CLI executable"
#endif

namespace {

namespace fs = std::filesystem;

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char c : value) {
        if (c == '\'') {
            quoted += "'\\\"'\\\"'";
        } else {
            quoted += c;
        }
    }
    return quoted + "'";
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int process_exit_code(int status) {
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

void write_tde_candidate_fixture(const fs::path& path) {
    std::vector<uint8_t> page(16384, 0);
    page[24] = 0x45;
    page[25] = 0xBF;
    page[38] = 6;
    page[39] = 1;
    for (size_t index = 55; index < 83; ++index) page[index] = 0xAB;

    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("unable to create TDE candidate fixture");
    file.write(reinterpret_cast<const char*>(page.data()), static_cast<std::streamsize>(page.size()));
    if (!file) throw std::runtime_error("unable to write TDE candidate fixture");
}

void write_empty_tablespace_fixture(const fs::path& path) {
    std::vector<uint8_t> page(16384, 0);
    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("unable to create empty tablespace fixture");
    file.write(reinterpret_cast<const char*>(page.data()), static_cast<std::streamsize>(page.size()));
    if (!file) throw std::runtime_error("unable to write empty tablespace fixture");
}

class RecoveryCliReportTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* test_tmp = std::getenv("DBREC_TEST_TMPDIR");
        if (test_tmp == nullptr || *test_tmp == '\0') {
            throw std::runtime_error("DBREC_TEST_TMPDIR must be set");
        }
        root_ = fs::path(test_tmp) / "recovery_cli_report";
        std::error_code error;
        fs::remove_all(root_, error);
        ASSERT_FALSE(error);
        fs::create_directories(root_);
    }

    void TearDown() override {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    int run_cli(const std::string& arguments, const fs::path& log_path) const {
        const std::string command = shell_quote(DBRECOVERY_CLI_PATH) + " " + arguments +
                                    " > " + shell_quote(log_path.string()) + " 2>&1";
        return std::system(command.c_str());
    }

    std::string scan_arguments(const fs::path& output_dir) const {
        return "--ibd=" + shell_quote("/app/mysql_recovery_pro/tests/data/mysql8.0.ibd") +
               " --scan --output=" + shell_quote(output_dir.string());
    }

    fs::path root_;
};

TEST_F(RecoveryCliReportTest, DefaultTextReportUsesOutputDirectoryBase) {
    const fs::path output = root_ / "default";
    fs::create_directories(output);
    const fs::path log = root_ / "default.log";

    ASSERT_EQ(run_cli(scan_arguments(output), log), 0) << read_file(log);
    EXPECT_TRUE(fs::is_regular_file(output / "recovery_report.txt"));
    EXPECT_FALSE(fs::exists(output / "recovery_report.json"));
    EXPECT_FALSE(fs::exists(output / "recovery_report.html"));
}

TEST_F(RecoveryCliReportTest, NormalizesFormatsAndReadsTextReportFromBasePath) {
    const fs::path output = root_ / "multiple";
    const fs::path base = output / "reports" / "run";
    fs::create_directories(base.parent_path());
    const fs::path generate_log = root_ / "generate.log";

    const auto generate_args = scan_arguments(output) +
                               " --report-path=" + shell_quote(base.string()) +
                               " --report-format=' JSON, html, json, text '";
    ASSERT_EQ(run_cli(generate_args, generate_log), 0) << read_file(generate_log);
    EXPECT_TRUE(fs::is_regular_file(base.string() + ".json"));
    EXPECT_TRUE(fs::is_regular_file(base.string() + ".txt"));
    EXPECT_TRUE(fs::is_regular_file(base.string() + ".html"));

    const fs::path show_log = root_ / "show.log";
    const auto show_args = "--show-last-report --output=" + shell_quote(output.string()) +
                           " --report-path=" + shell_quote(base.string());
    ASSERT_EQ(run_cli(show_args, show_log), 0) << read_file(show_log);
    EXPECT_NE(read_file(show_log).find(base.string() + ".txt"), std::string::npos);
}

TEST_F(RecoveryCliReportTest, RejectsInvalidOnlyFormatWithoutReportArtifact) {
    const fs::path output = root_ / "invalid";
    fs::create_directories(output);
    const fs::path log = root_ / "invalid.log";

    const auto arguments = scan_arguments(output) + " --report-format=pdf";
    EXPECT_NE(run_cli(arguments, log), 0);
    EXPECT_FALSE(fs::exists(output / "recovery_report.txt"));
    EXPECT_NE(read_file(log).find("--report-format must contain"), std::string::npos);
}

TEST_F(RecoveryCliReportTest, RejectsUnwritableReportBase) {
    const fs::path output = root_ / "unwritable";
    fs::create_directories(output);
    const fs::path blocker = output / "blocker";
    {
        std::ofstream file(blocker);
        ASSERT_TRUE(file);
    }
    const fs::path log = root_ / "unwritable.log";

    const auto arguments = scan_arguments(output) +
                           " --report-path=" + shell_quote((blocker / "report").string()) +
                           " --report-format=text";
    EXPECT_NE(run_cli(arguments, log), 0);
    EXPECT_NE(read_file(log).find("unable to write requested recovery report artifacts"),
              std::string::npos);
}

TEST_F(RecoveryCliReportTest, BlocksOuterExportWhenUnifiedRecoveryDeniesRows) {
    const fs::path input = root_ / "empty.ibd";
    write_empty_tablespace_fixture(input);

    for (const std::string& format : {"csv", "sql"}) {
        const fs::path output = root_ / ("unified_" + format);
        fs::create_directories(output);
        const fs::path log = root_ / ("unified_" + format + ".log");
        const auto arguments = "--ibd=" + shell_quote(input.string()) +
                               " --format=" + format +
                               " --schema='id:BIGINT:pk'" +
                               " --output=" + shell_quote(output.string());

        EXPECT_EQ(process_exit_code(run_cli(arguments, log)), 0) << read_file(log);
        EXPECT_FALSE(fs::exists(output / "recovered.csv"));
        EXPECT_FALSE(fs::exists(output / "recovered.sql"));
        EXPECT_NE(read_file(log).find("Unified recovery pipeline blocked export"),
                  std::string::npos);
        EXPECT_NE(read_file(log).find("compressed_status_MALFORMED"),
                  std::string::npos);
    }
}

TEST_F(RecoveryCliReportTest, RejectsUnvalidatedTDECandidatesWithoutStructuredArtifacts) {
    const fs::path input = root_ / "encrypted.ibd";
    write_tde_candidate_fixture(input);

    for (const std::string& format : {"csv", "sql"}) {
        const fs::path output = root_ / format;
        fs::create_directories(output);
        const fs::path log = root_ / (format + ".log");
        const auto arguments = "--ibd=" + shell_quote(input.string()) +
                               " --scan --format=" + format +
                               " --output=" + shell_quote(output.string());

        EXPECT_EQ(process_exit_code(run_cli(arguments, log)), 1) << read_file(log);
        EXPECT_FALSE(fs::exists(output / "recovered.csv"));
        EXPECT_FALSE(fs::exists(output / "recovered.sql"));
        EXPECT_FALSE(fs::exists(output / "recovery_report.txt"));
        EXPECT_FALSE(fs::exists(output / "recovery_report.json"));
        EXPECT_FALSE(fs::exists(output / "recovery_report.html"));
        EXPECT_NE(read_file(log).find("tde_candidate_UNVALIDATED"), std::string::npos);
    }
}

// --- Result-contract coverage -------------------------------------------
// The disaster-validation orchestrator gates every recovery output on the
// v2 result contract; these tests lock in the contract semantics the Python
// side relies on (see REQUIRED_CONTRACT_KEYS in
// scripts/disaster_validation/phase_a_offline_orchestrator.py).

const std::set<std::string> kRequiredContractKeys = {
    "format_version", "table_name", "success", "classification",
    "structured_rows", "evidence_fragments", "schema_source",
    "schema_trust_level", "damage_level", "recovery_method",
    "ddl_variation_detected", "export_success", "export_rejection_reason",
    "total_pages_scanned", "skipped_pages", "damaged_pages",
    "checksum_mismatches", "header_damage_pages", "importable", "reason",
    "source_ibd", "compression_status", "logical_page_size",
    "physical_page_size", "compression_classification_basis",
    "decoder_validated", "compression_rejection_reason",
};

TEST_F(RecoveryCliReportTest, MissingInputEmitsCompleteFailureContract) {
    const fs::path output = root_ / "missing_input";
    const fs::path result_path = output / "result.json";
    const fs::path log = root_ / "missing_input.log";
    const auto arguments = "--ibd=" + shell_quote((root_ / "does_not_exist.ibd").string()) +
                           " --format=csv --schema='id:BIGINT:pk'" +
                           " --output=" + shell_quote(output.string()) +
                           " --result-json=" + shell_quote(result_path.string());

    EXPECT_EQ(process_exit_code(run_cli(arguments, log)), 1) << read_file(log);
    ASSERT_TRUE(fs::is_regular_file(result_path)) << read_file(log);

    const auto doc = nlohmann::json::parse(read_file(result_path));
    for (const auto& key : kRequiredContractKeys) {
        EXPECT_TRUE(doc.contains(key)) << "missing contract key: " << key;
    }
    EXPECT_EQ(doc["format_version"], 2);
    EXPECT_FALSE(doc["success"].get<bool>());
    EXPECT_FALSE(doc["export_success"].get<bool>());
    EXPECT_FALSE(doc["importable"].get<bool>());
    EXPECT_FALSE(doc["export_rejection_reason"].get<std::string>().empty());
    EXPECT_FALSE(doc["reason"].get<std::string>().empty());
    EXPECT_EQ(doc["classification"], "operational_error");
    EXPECT_EQ(doc["structured_rows"], 0);
}

TEST_F(RecoveryCliReportTest, TDECandidateEmitsNonSuccessContractWithoutArtifacts) {
    const fs::path input = root_ / "tde_contract.ibd";
    write_tde_candidate_fixture(input);
    const fs::path output = root_ / "tde_contract_out";
    const fs::path result_path = root_ / "tde_contract_result.json";
    const fs::path log = root_ / "tde_contract.log";
    const auto arguments = "--ibd=" + shell_quote(input.string()) +
                           " --format=csv --schema='id:BIGINT:pk'" +
                           " --output=" + shell_quote(output.string()) +
                           " --result-json=" + shell_quote(result_path.string());

    EXPECT_EQ(process_exit_code(run_cli(arguments, log)), 1) << read_file(log);
    ASSERT_TRUE(fs::is_regular_file(result_path)) << read_file(log);

    const auto doc = nlohmann::json::parse(read_file(result_path));
    for (const auto& key : kRequiredContractKeys) {
        EXPECT_TRUE(doc.contains(key)) << "missing contract key: " << key;
    }
    EXPECT_EQ(doc["format_version"], 2);
    EXPECT_FALSE(doc["success"].get<bool>());
    EXPECT_FALSE(doc["export_success"].get<bool>());
    EXPECT_FALSE(doc["importable"].get<bool>());
    EXPECT_EQ(doc["reason"], "tde_candidate_UNVALIDATED");
    EXPECT_FALSE(fs::exists(output / "recovered.csv"));
    EXPECT_FALSE(fs::exists(output / "recovered.sql"));
}

TEST_F(RecoveryCliReportTest, DefaultRecoveryEmitsContractWithNoFabricatedSuccess) {
    const fs::path output = root_ / "default_contract";
    const fs::path result_path = output / "result.json";
    const fs::path log = root_ / "default_contract.log";
    const auto arguments = "--ibd=" + shell_quote("/app/mysql_recovery_pro/tests/data/mysql8.0.ibd") +
                           " --format=csv" +
                           " --output=" + shell_quote(output.string()) +
                           " --result-json=" + shell_quote(result_path.string());

    EXPECT_EQ(process_exit_code(run_cli(arguments, log)), 0) << read_file(log);
    ASSERT_TRUE(fs::is_regular_file(result_path)) << read_file(log);

    const auto doc = nlohmann::json::parse(read_file(result_path));
    for (const auto& key : kRequiredContractKeys) {
        EXPECT_TRUE(doc.contains(key)) << "missing contract key: " << key;
    }
    EXPECT_EQ(doc["format_version"], 2);
    EXPECT_FALSE(doc["importable"].get<bool>());
    // The fixture has no authoritative schema, so export must be denied and
    // the contract must not claim success without validation.
    if (!doc["export_success"].get<bool>()) {
        EXPECT_FALSE(doc["export_rejection_reason"].get<std::string>().empty());
        EXPECT_FALSE(fs::exists(output / "recovered.csv"));
    }
    // success may only be true when export actually succeeded
    if (!doc["export_success"].get<bool>()) {
        EXPECT_FALSE(doc["success"].get<bool>());
    }
}

TEST_F(RecoveryCliReportTest, ScanOnlyModeDoesNotEmitResultContract) {
    // --scan writes only the text report; it must not fabricate a result
    // contract that could be misread as a validated recovery outcome.
    const fs::path output = root_ / "scan_only";
    const fs::path result_path = output / "result.json";
    const fs::path log = root_ / "scan_only.log";
    const auto arguments = scan_arguments(output) +
                           " --result-json=" + shell_quote(result_path.string());

    EXPECT_EQ(process_exit_code(run_cli(arguments, log)), 0) << read_file(log);
    EXPECT_TRUE(fs::is_regular_file(output / "recovery_report.txt"));
    EXPECT_FALSE(fs::exists(result_path));
}

} // namespace
