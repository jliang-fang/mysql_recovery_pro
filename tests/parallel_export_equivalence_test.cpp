/**
 * @file parallel_export_equivalence_test.cpp
 * @brief Phase 4 regression: parallel vs serial export produce equivalent output
 *        (row count, column order, NULL/escape handling).
 */

#include <gtest/gtest.h>
#include <vector>
#include <map>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <set>

#include "dbrecovery/recovery/parallel_exporter.hpp"
#include "dbrecovery/export/data_exporter.hpp"

using namespace dbrecovery::recovery;
using namespace dbrecovery::exporter;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

/// Count non-empty data rows in a CSV (skip header).
size_t count_csv_data_rows(const std::string& content) {
    std::istringstream ss(content);
    std::string line;
    size_t count = 0;
    bool first = true;
    while (std::getline(ss, line)) {
        if (first) { first = false; continue; }  // skip header
        if (!line.empty()) count++;
    }
    return count;
}

/// Extract the first header line from a CSV.
std::string csv_header(const std::string& content) {
    std::istringstream ss(content);
    std::string line;
    std::getline(ss, line);
    return line;
}

}  // namespace

// ==================== Parallel vs Serial CSV equivalence ====================

TEST(ParallelExportEquivalenceTest, RowCountMatches) {
    // Build a deterministic record set with various value types.
    std::vector<std::map<std::string, std::string>> records;
    for (int i = 0; i < 200; i++) {
        records.push_back({
            {"id", std::to_string(i)},
            {"name", "row_" + std::to_string(i)},
            {"value", std::to_string(i * 3.14)},
        });
    }

    auto out_dir = std::filesystem::temp_directory_path() / "dbrec_parallel_equiv";
    std::filesystem::create_directories(out_dir);

    // Serial export (single file)
    std::string serial_path = (out_dir / "serial.csv").string();
    std::vector<std::string> headers = {"id", "name", "value"};
    ASSERT_TRUE(Exporter::export_to_csv(records, headers, serial_path));

    // Parallel export (chunked)
    ParallelExporter::Config cfg;
    cfg.output_dir = out_dir.string();
    cfg.output_format = "csv";
    cfg.table_name = "equiv_test";
    cfg.base_filename = "parallel";
    cfg.chunk_size = 50;  // force multiple chunks
    cfg.num_workers = 4;
    ParallelExporter exporter(cfg);
    ASSERT_TRUE(exporter.export_parallel(records));

    // Read serial file
    std::string serial_content = read_file(serial_path);
    size_t serial_rows = count_csv_data_rows(serial_content);

    // Read all parallel chunk files and sum rows
    size_t parallel_total_rows = 0;
    std::string parallel_headers;
    for (const auto& f : std::filesystem::directory_iterator(out_dir)) {
        if (f.path().filename().string().find("parallel_chunk_") == 0) {
            std::string chunk_content = read_file(f.path().string());
            if (parallel_headers.empty()) {
                parallel_headers = csv_header(chunk_content);
            }
            parallel_total_rows += count_csv_data_rows(chunk_content);
        }
    }

    EXPECT_EQ(serial_rows, 200u);
    EXPECT_EQ(parallel_total_rows, 200u);
    EXPECT_EQ(serial_rows, parallel_total_rows);

    // Header order must match
    EXPECT_EQ(csv_header(serial_content), parallel_headers);

    std::filesystem::remove_all(out_dir);
}

TEST(ParallelExportEquivalenceTest, NullAndEscapeHandling) {
    // Records with empty values and values containing commas/quotes.
    std::vector<std::map<std::string, std::string>> records = {
        {{"id", "1"}, {"data", ""}},
        {{"id", "2"}, {"data", "has,comma"}},
        {{"id", "3"}, {"data", "has\"quote"}},
        {{"id", "4"}, {"data", "line\break"}},
    };

    auto out_dir = std::filesystem::temp_directory_path() / "dbrec_parallel_null";
    std::filesystem::create_directories(out_dir);

    std::string serial_path = (out_dir / "serial.csv").string();
    std::vector<std::string> headers = {"id", "data"};
    ASSERT_TRUE(Exporter::export_to_csv(records, headers, serial_path));

    ParallelExporter::Config cfg;
    cfg.output_dir = out_dir.string();
    cfg.output_format = "csv";
    cfg.table_name = "null_test";
    cfg.base_filename = "parallel";
    cfg.chunk_size = 2;  // force 2 chunks
    cfg.num_workers = 2;
    ParallelExporter exporter(cfg);
    ASSERT_TRUE(exporter.export_parallel(records));

    // Both must produce exactly 4 data rows
    std::string serial_content = read_file(serial_path);
    EXPECT_EQ(count_csv_data_rows(serial_content), 4u);

    size_t parallel_total = 0;
    for (const auto& f : std::filesystem::directory_iterator(out_dir)) {
        if (f.path().filename().string().find("parallel_chunk_") == 0) {
            parallel_total += count_csv_data_rows(read_file(f.path().string()));
        }
    }
    EXPECT_EQ(parallel_total, 4u);

    std::filesystem::remove_all(out_dir);
}

// ==================== Main ====================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
