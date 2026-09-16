#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

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

} // namespace
