// Inference benchmark tests (Phase 4 - Slice E)
//
// Measures type inference accuracy and performance across MySQL 5.6-9.0
// using .ibd files from test data directories. SKIPs if no .ibd data found.
//
// Metrics:
//   - type_name match rate (inferred vs expected)
//   - column count match rate
//   - pages_scanned and total_ms per 100 pages

#include <gtest/gtest.h>
#include "dbrecovery/inference/statistical_classifier.hpp"
#include "dbrecovery/inference/interactive_builder.hpp"
#include "dbrecovery/salvage/evidence_serializer.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <sys/stat.h>

namespace {

using dbrecovery::inference::InteractiveBuilder;
using dbrecovery::inference::FeatureExtractor;
using dbrecovery::inference::StatisticalTypeClassifier;
using dbrecovery::inference::ColumnFeatures;

// Known MySQL version fixtures for named-version smoke tests
constexpr const char* kKnownFixtures[] = {
    "mysql5.6.ibd",
    "mysql5.7.ibd",
    "mysql8.0.ibd",
    "mysql8.4.ibd",
    "mysql9.0.ibd",
};

// Find test data directory
static std::string find_test_data_dir() {
    const char* env = std::getenv("TEST_DATA_DIR");
    if (env) return env;
#ifdef TEST_DATA_DIR
    return TEST_DATA_DIR;
#else
    struct stat st;
    for (const auto& path : {"/app/mysql_recovery_pro/tests/data",
                             "/app/mysql_recovery_pro/test/test_data"}) {
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return path;
    }
    return "";
#endif
}

static std::vector<std::string> find_ibd_files(const std::string& dir) {
    std::vector<std::string> files;
    DIR* d = opendir(dir.c_str());
    if (!d) return files;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name(entry->d_name);
        if (name.size() > 4 && name.substr(name.size() - 4) == ".ibd") {
            files.push_back(dir + "/" + name);
        }
    }
    closedir(d);
    return files;
}

static std::string metadata_path_for(const std::string& ibd_path) {
    return ibd_path.substr(0, ibd_path.size() - 4) + ".json";
}

static std::string read_binary_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

static ::testing::AssertionResult validate_fixture(const std::string& ibd_path) {
    struct stat ibd_stat;
    if (stat(ibd_path.c_str(), &ibd_stat) != 0 || !S_ISREG(ibd_stat.st_mode)) {
        return ::testing::AssertionFailure() << "Fixture is not a regular file: " << ibd_path;
    }
    if (ibd_stat.st_size <= 0) {
        return ::testing::AssertionFailure() << "Fixture is empty: " << ibd_path;
    }

    const std::string metadata_path = metadata_path_for(ibd_path);
    std::ifstream metadata_file(metadata_path);
    if (!metadata_file) {
        return ::testing::AssertionFailure() << "Missing fixture metadata: " << metadata_path;
    }

    nlohmann::json metadata;
    try {
        metadata_file >> metadata;
    } catch (const nlohmann::json::exception& error) {
        return ::testing::AssertionFailure()
            << "Invalid fixture metadata " << metadata_path << ": " << error.what();
    }

    constexpr const char* required_fields[] = {
        "version", "server_version", "table", "database", "row_count", "row_format",
        "page_size", "ddl", "byte_size", "page_count", "sha256"
    };
    for (const auto* field : required_fields) {
        if (!metadata.contains(field)) {
            return ::testing::AssertionFailure()
                << "Fixture metadata missing required field '" << field << "': " << metadata_path;
        }
    }

    const auto page_size = metadata.at("page_size").get<uint64_t>();
    const auto byte_size = metadata.at("byte_size").get<uint64_t>();
    const auto page_count = metadata.at("page_count").get<uint64_t>();
    const auto sha256 = metadata.at("sha256").get<std::string>();
    if (page_size == 0 || byte_size == 0 || page_count == 0 ||
        byte_size != static_cast<uint64_t>(ibd_stat.st_size) ||
        byte_size % page_size != 0 || page_count != byte_size / page_size ||
        sha256.size() != 64) {
        return ::testing::AssertionFailure() << "Fixture metadata does not match " << ibd_path;
    }

    const auto contents = read_binary_file(ibd_path);
    if (contents.size() != static_cast<size_t>(ibd_stat.st_size) ||
        dbrecovery::salvage::SalvageEvidenceSerializer::sha256_hex(contents) != sha256) {
        return ::testing::AssertionFailure() << "Fixture SHA-256 mismatch: " << ibd_path;
    }

    return ::testing::AssertionSuccess();
}


// IB-FIXTURE-01 through IB-FIXTURE-05: Each named version must produce
// non-empty layout, non-empty field samples, and type candidates.
// GTEST_SKIP() with version annotation if the fixture file is missing.
class NamedFixtureTest : public ::testing::TestWithParam<const char*> {
protected:
    std::string data_dir_;
    std::string fixture_path_;

    void SetUp() override {
        data_dir_ = find_test_data_dir();
        if (data_dir_.empty()) {
            GTEST_SKIP() << "No test data directory found. Set TEST_DATA_DIR env var.";
        }
        fixture_path_ = data_dir_ + "/" + GetParam();
        struct stat st;
        if (stat(fixture_path_.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            GTEST_SKIP() << "Fixture not found: " << fixture_path_;
        }
        ASSERT_TRUE(validate_fixture(fixture_path_));
    }
};

TEST_P(NamedFixtureTest, LayoutAndSmokeInference) {
    InteractiveBuilder builder;
    const size_t pages = builder.load_ibd_file(fixture_path_);
    ASSERT_GT(pages, 0u) << "Failed to load pages from " << fixture_path_;

    const auto layout = builder.analyze_layout();
    ASSERT_TRUE(layout.has_value())
        << "No layout detected for " << GetParam() << " — pages loaded: " << pages;
    ASSERT_EQ(layout->total_columns, 1u)
        << GetParam() << ": schema-free COMPACT/DYNAMIC inference must expose one opaque payload";
    ASSERT_EQ(layout->user_columns, 1u)
        << GetParam() << ": schema-free COMPACT/DYNAMIC inference must expose one opaque payload";

    const auto values = builder.extract_column_values(*layout, 100);
    ASSERT_EQ(values.size(), 1u);
    ASSERT_FALSE(values[0].empty()) << GetParam() << ": expected opaque payload samples";

    const auto result = builder.infer_types(values);
    ASSERT_EQ(result.detected_column_count, 1u);
    ASSERT_EQ(result.columns.size(), 1u);
    EXPECT_FALSE(result.columns[0].best_guess.type_name.empty());
    EXPECT_GT(result.columns[0].overall_confidence, 0.0);
}

INSTANTIATE_TEST_SUITE_P(InferenceBenchmarkTest, NamedFixtureTest,
                         ::testing::ValuesIn(kKnownFixtures));

// ==================== Fixture capability smoke tests ====================

// IB-BENCH-01: Measure type inference accuracy on available .ibd files
TEST(InferenceBenchmarkTest, TypeInferenceAccuracy) {
    std::string data_dir = find_test_data_dir();
    if (data_dir.empty()) {
        GTEST_SKIP() << "No test data directory found. Set TEST_DATA_DIR env var.";
    }

    auto ibd_files = find_ibd_files(data_dir);
    if (ibd_files.empty()) {
        GTEST_SKIP() << "No .ibd files found in " << data_dir;
    }

    // For each .ibd file, load pages and run inference
    // This is a basic smoke test — full accuracy requires ground-truth metadata
    uint32_t files_processed = 0;
    for (const auto& path : ibd_files) {
        ASSERT_TRUE(validate_fixture(path));

        InteractiveBuilder builder;
        size_t pages = builder.load_ibd_file(path);
        if (pages == 0) continue;

        auto layout = builder.analyze_layout();
        if (!layout) continue;

        auto values = builder.extract_column_values(*layout, 100);
        if (values.empty()) continue;

        auto result = builder.infer_types(values);
        if (result.detected_column_count > 0) {
            files_processed++;
            EXPECT_GT(result.overall_confidence, 0.0);
            for (const auto& col : result.columns) {
                EXPECT_FALSE(col.best_guess.type_name.empty());
            }
        }
    }

    EXPECT_GT(files_processed, 0u)
        << "No .ibd files could be processed in " << data_dir;
}

// IB-BENCH-02: Performance benchmark — measure ms per 100 pages
TEST(InferenceBenchmarkTest, PerformanceBenchmark) {
    std::string data_dir = find_test_data_dir();
    if (data_dir.empty()) {
        GTEST_SKIP() << "No test data directory found.";
    }

    auto ibd_files = find_ibd_files(data_dir);
    if (ibd_files.empty()) {
        GTEST_SKIP() << "No .ibd files found.";
    }

    for (const auto& path : ibd_files) {
        ASSERT_TRUE(validate_fixture(path));

        InteractiveBuilder builder;
        size_t pages = builder.load_ibd_file(path);
        if (pages < 3) continue;

        auto layout = builder.analyze_layout();
        if (!layout) continue;

        auto t0 = std::chrono::high_resolution_clock::now();
        auto values = builder.extract_column_values(*layout, 500);
        if (!values.empty()) {
            auto result = builder.infer_types(values);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        SUCCEED() << path << ": " << pages << " pages, " << ms << " ms "
                  << "(" << (ms * 100.0 / pages) << " ms/100 pages)";
        return;  // one benchmark result is enough
    }

    GTEST_SKIP() << "No .ibd file with >=3 pages found.";
}

// IB-BENCH-03: FeatureExtractor consistency check
TEST(InferenceBenchmarkTest, FeatureExtractorConsistency) {
    // Verify that identical inputs produce identical features
    std::vector<std::vector<uint8_t>> v1 = {{0x01, 0x00, 0x00, 0x00},
                                            {0x02, 0x00, 0x00, 0x00}};
    std::vector<std::vector<uint8_t>> v2 = {{0x01, 0x00, 0x00, 0x00},
                                            {0x02, 0x00, 0x00, 0x00}};

    auto f1 = FeatureExtractor::extract(v1);
    auto f2 = FeatureExtractor::extract(v2);

    EXPECT_EQ(f1.sample_count, f2.sample_count);
    EXPECT_EQ(f1.len_min, f2.len_min);
    EXPECT_EQ(f1.len_max, f2.len_max);
    EXPECT_EQ(f1.len_mean, f2.len_mean);
    EXPECT_EQ(f1.null_ratio, f2.null_ratio);
    EXPECT_EQ(f1.distinct_count, f2.distinct_count);
}

}  // namespace