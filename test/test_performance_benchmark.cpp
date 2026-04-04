#include <gtest/gtest.h>
#include "dbrecovery/innodb/performance_benchmark.hpp"
#include <thread>
#include <iostream>

namespace dbrecovery {
namespace innodb {

// ==================== PerformanceBenchmark Tests ====================

TEST(PerformanceBenchmarkTest, PageParserBenchmark) {
    PerformanceBenchmark benchmark;
    auto result = benchmark.benchmark_page_parser(1000);
    
    EXPECT_GT(result.iterations, 0);
    EXPECT_GT(result.throughput, 0);
    EXPECT_LT(result.avg_time_ms, 100);  // Should be fast
}

TEST(PerformanceBenchmarkTest, UndoLogParserBenchmark) {
    PerformanceBenchmark benchmark;
    auto result = benchmark.benchmark_undo_log_parser(100);
    
    EXPECT_GT(result.iterations, 0);
    EXPECT_GT(result.throughput, 0);
}

TEST(PerformanceBenchmarkTest, SDIParserBenchmark) {
    PerformanceBenchmark benchmark;
    auto result = benchmark.benchmark_sdi_parser(100);
    
    EXPECT_GT(result.iterations, 0);
    EXPECT_GT(result.throughput, 0);
}

TEST(PerformanceBenchmarkTest, BlobParserBenchmark) {
    PerformanceBenchmark benchmark;
    auto result = benchmark.benchmark_blob_parser(100);
    
    EXPECT_GT(result.iterations, 0);
    EXPECT_GT(result.throughput, 0);
}

TEST(PerformanceBenchmarkTest, ChecksumBenchmark) {
    PerformanceBenchmark benchmark;
    auto result = benchmark.benchmark_checksum(10000);
    
    EXPECT_GT(result.iterations, 0);
    EXPECT_GT(result.throughput, 0);
}

TEST(PerformanceBenchmarkTest, CRC32CBenchmark) {
    PerformanceBenchmark benchmark;
    auto result = benchmark.benchmark_crc32c(100000);
    
    EXPECT_GT(result.iterations, 0);
    EXPECT_GT(result.throughput, 0);
}

TEST(PerformanceBenchmarkTest, RowFormatBenchmark) {
    PerformanceBenchmark benchmark;
    auto result = benchmark.benchmark_row_format(1000);
    
    EXPECT_GT(result.iterations, 0);
    EXPECT_GT(result.throughput, 0);
}

TEST(PerformanceBenchmarkTest, RunAllBenchmarks) {
    PerformanceBenchmark benchmark;
    auto results = benchmark.run_all_benchmarks();
    
    EXPECT_GT(results.size(), 0);
    
    // Verify all results are valid
    for (const auto& result : results) {
        EXPECT_GT(result.iterations, 0);
        EXPECT_GT(result.throughput, 0);
        EXPECT_LT(result.avg_time_ms, 10000);  // Each test should complete in reasonable time
    }
}

TEST(PerformanceBenchmarkTest, PrintReport) {
    PerformanceBenchmark benchmark;
    auto results = benchmark.run_all_benchmarks();
    
    // Capture output
    testing::internal::CaptureStdout();
    benchmark.print_report(results);
    std::string output = testing::internal::GetCapturedStdout();
    
    // Verify output contains expected content
    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find("MySQL Recovery Pro"), std::string::npos);
    EXPECT_NE(output.find("PageParser"), std::string::npos);
}

// ==================== Multi-threaded Performance Tests ====================

TEST(PerformanceBenchmarkTest, MultiThreadedPageParser) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    
    const int num_threads = 4;
    const int iterations_per_thread = 1000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&]() {
            for (int j = 0; j < iterations_per_thread; j++) {
                PageHeaderV2 header;
                header.parse(page.data(), page.size());
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Should complete in reasonable time with parallel execution
    EXPECT_LT(duration.count(), 5000);
}

TEST(PerformanceBenchmarkTest, MultiThreadedChecksum) {
    std::vector<uint8_t> page(16384, 0x42);
    
    const int num_threads = 4;
    const int iterations_per_thread = 10000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&]() {
            for (int j = 0; j < iterations_per_thread; j++) {
                ChecksumVerifier::crc32c(page.data(), page.size());
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Should complete in reasonable time with parallel execution
    EXPECT_LT(duration.count(), 10000);
}

// ==================== Large Scale Performance Tests ====================

TEST(PerformanceBenchmarkTest, LargeScalePageParsing) {
    const int num_pages = 1000;
    std::vector<std::vector<uint8_t>> pages(num_pages, std::vector<uint8_t>(16384, 0));
    
    for (auto& page : pages) {
        write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (const auto& page : pages) {
        PageHeaderV2 header;
        header.parse(page.data(), page.size());
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    double throughput = (num_pages * 1000.0) / duration.count();
    
    // Should parse 1000 pages in reasonable time
    EXPECT_GT(throughput, 100);  // At least 100 pages/sec
}

TEST(PerformanceBenchmarkTest, LargeScaleUndoLogParsing) {
    const int num_pages = 100;
    std::vector<std::vector<uint8_t>> pages(num_pages, std::vector<uint8_t>(16384, 0));
    
    for (auto& page : pages) {
        write_le<uint16_t>(page.data(), 16, FIL_PAGE_UNDO_LOG);
    }
    
    UndoLogParser parser;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (const auto& page : pages) {
        parser.parse_page(page.data(), page.size());
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    double throughput = (num_pages * 1000.0) / duration.count();
    
    // Should parse 100 undo log pages in reasonable time
    EXPECT_GT(throughput, 10);  // At least 10 pages/sec
}

// ==================== Memory Efficiency Tests ====================

TEST(PerformanceBenchmarkTest, MemoryEfficiency) {
    const int num_iterations = 10000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < num_iterations; i++) {
        // Create and destroy objects
        PageHeaderV2 header;
        std::vector<uint8_t> data(16384, 0);
        header.parse(data.data(), data.size());
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Should complete in reasonable time with efficient memory management
    EXPECT_LT(duration.count(), 5000);
}

// ==================== Edge Case Performance Tests ====================

TEST(PerformanceBenchmarkTest, SmallPageParsing) {
    std::vector<uint8_t> page(100, 0);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < 10000; i++) {
        PageHeaderV2 header;
        header.parse(page.data(), page.size());
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Should be very fast for small pages
    EXPECT_LT(duration.count(), 1000);
}

TEST(PerformanceBenchmarkTest, InvalidPageHandling) {
    std::vector<uint8_t> page(10, 0);  // Too small
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < 10000; i++) {
        PageHeaderV2 header;
        header.parse(page.data(), page.size());
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Should handle invalid pages efficiently
    EXPECT_LT(duration.count(), 1000);
}

// ==================== Throughput Tests ====================

TEST(PerformanceBenchmarkTest, HighThroughputCRC32C) {
    std::vector<uint8_t> data(16384, 0x42);
    
    const int num_threads = std::thread::hardware_concurrency();
    const int iterations_per_thread = 100000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&]() {
            for (int j = 0; j < iterations_per_thread; j++) {
                ChecksumVerifier::crc32c(data.data(), data.size());
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    double total_iterations = num_threads * iterations_per_thread;
    double throughput = (total_iterations * 1000.0) / duration.count();
    
    std::cout << "CRC32C Throughput: " << throughput << " ops/sec" << std::endl;
    
    // Should achieve high throughput
    EXPECT_GT(throughput, 100000);  // At least 100K ops/sec
}

} // namespace innodb
} // namespace dbrecovery
