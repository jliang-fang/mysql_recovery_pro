#ifndef DBRECOVERY_RECOVERY_PARALLEL_EXPORTER_HPP
#define DBRECOVERY_RECOVERY_PARALLEL_EXPORTER_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <functional>
#include <fstream>
#include <iostream>
#include <chrono>
#include <condition_variable>
#include <future>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <algorithm>

#include "dbrecovery/export/data_exporter.hpp"

namespace dbrecovery {
namespace recovery {

/**
 * @brief ParallelExporter — multi-threaded data export
 *
 * Divides records into chunks and exports them in parallel using
 * multiple worker threads. Supports CSV, SQL, and JSON output.
 *
 * Usage:
 *   ParallelExporter exporter;
 *   exporter.set_num_workers(4);
 *   exporter.set_output_format("csv");
 *   exporter.export_parallel(records, "/tmp/out/");
 */
class ParallelExporter {
public:
    struct Config {
        Config() = default;
        uint32_t num_workers = 4;
        std::string output_format = "csv";
        std::string output_dir = "/tmp/recovered";
        std::string table_name = "recovered_table";
        std::string base_filename = "recovered";
        size_t chunk_size = 50000;        // records per chunk
        bool include_headers = true;
        bool verbose = false;
    };

    struct ExportStats {
        uint64_t total_records = 0;
        uint64_t chunks_exported = 0;
        uint64_t bytes_written = 0;
        double elapsed_seconds = 0.0;
        std::vector<std::string> output_files;
    };

    ParallelExporter() = default;
    explicit ParallelExporter(const Config& config) : config_(config) {}

    void set_config(const Config& config) { config_ = config; }

    void set_num_workers(uint32_t n) { config_.num_workers = std::max(1u, n); }

    const ExportStats& get_stats() const { return stats_; }

    /**
     * @brief Export records in parallel
     *
     * Splits records into chunks, dispatches each chunk to a thread pool worker,
     * and writes chunk-N.csv (or .sql/.json) files.
     *
     * @param records All recovered records
     * @return true if all chunks exported successfully
     */
    bool export_parallel(const std::vector<std::map<std::string, std::string>>& records) {
        auto start = std::chrono::high_resolution_clock::now();
        stats_ = ExportStats{};
        stats_.total_records = records.size();

        if (records.empty()) return true;

        size_t num_chunks = (records.size() + config_.chunk_size - 1) / config_.chunk_size;
        if (num_chunks > 1000) num_chunks = 1000; // cap
        size_t actual_chunk_size = (records.size() + num_chunks - 1) / num_chunks;
        uint32_t workers = std::min(config_.num_workers, static_cast<uint32_t>(num_chunks));

        if (config_.verbose) {
            std::cout << "[ParallelExport] " << records.size() << " records, "
                      << num_chunks << " chunks, " << workers << " workers\n";
        }

        // Thread pool
        std::atomic<size_t> next_chunk{0};
        std::atomic<bool> has_error{false};
        std::mutex file_mutex;
        std::vector<std::string> output_files;

        auto worker_fn = [&]() {
            while (true) {
                size_t chunk_idx = next_chunk.fetch_add(1);
                if (chunk_idx >= num_chunks || has_error) break;

                size_t start_idx = chunk_idx * actual_chunk_size;
                size_t end_idx = std::min(start_idx + actual_chunk_size, records.size());

                // Build chunk
                std::vector<std::map<std::string, std::string>> chunk(
                    records.begin() + start_idx, records.begin() + end_idx);

                // Generate filename
                std::ostringstream fname;
                fname << config_.output_dir << "/" << config_.base_filename
                      << "_chunk_" << std::setfill('0') << std::setw(4) << chunk_idx;
                if (config_.output_format == "sql") fname << ".sql";
                else if (config_.output_format == "json") fname << ".json";
                else fname << ".csv";

                // Export chunk
                bool ok = false;
                if (config_.output_format == "sql") {
                    ok = dbrecovery::exporter::Exporter::export_to_sql(
                        chunk, config_.table_name, fname.str());
                } else if (config_.output_format == "json") {
                    ok = dbrecovery::exporter::Exporter::export_to_json(
                        chunk, fname.str(), false);
                } else {
                    // CSV — extract headers from first record
                    std::vector<std::string> headers;
                    if (!chunk.empty()) {
                        for (const auto& [key, val] : chunk[0]) {
                            headers.push_back(key);
                        }
                    }
                    ok = dbrecovery::exporter::Exporter::export_to_csv(
                        chunk, headers, fname.str());
                }

                if (ok) {
                    std::lock_guard<std::mutex> lock(file_mutex);
                    output_files.push_back(fname.str());
                } else {
                    has_error = true;
                }
            }
        };

        // Launch workers
        std::vector<std::thread> threads;
        for (uint32_t w = 0; w < workers; w++) {
            threads.emplace_back(worker_fn);
        }

        for (auto& t : threads) {
            t.join();
        }

        if (has_error) return false;

        stats_.chunks_exported = output_files.size();
        stats_.output_files = output_files;

        // Calculate total bytes
        for (const auto& f : output_files) {
            std::error_code ec;
            stats_.bytes_written += std::filesystem::file_size(f, ec);
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats_.elapsed_seconds = std::chrono::duration<double>(end - start).count();

        if (config_.verbose) {
            std::cout << "[ParallelExport] Done: " << stats_.chunks_exported
                      << " files, " << stats_.bytes_written << " bytes, "
                      << std::fixed << std::setprecision(2)
                      << stats_.elapsed_seconds << "s\n";
        }

        return true;
    }

    /**
     * @brief Export with a column ordering specifier (for SQL format)
     */
    bool export_parallel_with_columns(
        const std::vector<std::map<std::string, std::string>>& records,
        const std::vector<std::string>& column_order) {

        // Same as export_parallel but passes column_order to SQL exporter
        auto start = std::chrono::high_resolution_clock::now();
        stats_ = ExportStats{};
        stats_.total_records = records.size();

        if (records.empty()) return true;

        size_t num_chunks = (records.size() + config_.chunk_size - 1) / config_.chunk_size;
        if (num_chunks > 1000) num_chunks = 1000;
        size_t actual_chunk_size = (records.size() + num_chunks - 1) / num_chunks;
        uint32_t workers = std::min(config_.num_workers, static_cast<uint32_t>(num_chunks));

        std::atomic<size_t> next_chunk{0};
        std::atomic<bool> has_error{false};
        std::mutex file_mutex;
        std::vector<std::string> output_files;

        auto worker_fn = [&]() {
            while (true) {
                size_t chunk_idx = next_chunk.fetch_add(1);
                if (chunk_idx >= num_chunks || has_error) break;

                size_t start_idx = chunk_idx * actual_chunk_size;
                size_t end_idx = std::min(start_idx + actual_chunk_size, records.size());

                std::vector<std::map<std::string, std::string>> chunk(
                    records.begin() + start_idx, records.begin() + end_idx);

                std::ostringstream fname;
                fname << config_.output_dir << "/" << config_.base_filename
                      << "_chunk_" << std::setfill('0') << std::setw(4) << chunk_idx;
                if (config_.output_format == "sql") fname << ".sql";
                else if (config_.output_format == "json") fname << ".json";
                else fname << ".csv";

                bool ok = false;
                if (config_.output_format == "sql") {
                    ok = dbrecovery::exporter::Exporter::export_to_sql(
                        chunk, config_.table_name, column_order, fname.str());
                } else if (config_.output_format == "json") {
                    ok = dbrecovery::exporter::Exporter::export_to_json(
                        chunk, fname.str(), false);
                } else {
                    ok = dbrecovery::exporter::Exporter::export_to_csv(
                        chunk, column_order, fname.str());
                }

                if (ok) {
                    std::lock_guard<std::mutex> lock(file_mutex);
                    output_files.push_back(fname.str());
                } else {
                    has_error = true;
                }
            }
        };

        std::vector<std::thread> threads;
        for (uint32_t w = 0; w < workers; w++) {
            threads.emplace_back(worker_fn);
        }
        for (auto& t : threads) t.join();

        if (has_error) return false;

        stats_.chunks_exported = output_files.size();
        stats_.output_files = output_files;
        for (const auto& f : output_files) {
            std::error_code ec;
            stats_.bytes_written += std::filesystem::file_size(f, ec);
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats_.elapsed_seconds = std::chrono::duration<double>(end - start).count();

        return true;
    }

private:
    Config config_;
    ExportStats stats_;
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_PARALLEL_EXPORTER_HPP