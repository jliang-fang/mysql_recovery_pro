#include "dbrecovery/innodb/performance_benchmark.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/page_parser_v2.hpp"
#include "dbrecovery/innodb/undo_log_parser.hpp"
#include "dbrecovery/innodb/redo_log_replayer.hpp"
#include "dbrecovery/recovery/blob_reconstructor.hpp"
#include <iostream>
#include <random>
#include <algorithm>
#include <thread>
#include <mutex>
#include <optional>

namespace dbrecovery {
namespace innodb {

// ==================== PerformanceBenchmark ====================

PerformanceBenchmark::PerformanceBenchmark() = default;

PerformanceBenchmark::~PerformanceBenchmark() = default;

BenchmarkResult PerformanceBenchmark::run(const std::string& name, int iterations,
                                           const std::function<void()>& func) {
    BenchmarkResult result;
    result.name = name;
    result.iterations = iterations;

    std::vector<double> times;
    times.reserve(iterations);

    for (int i = 0; i < iterations; ++i) {
        Timer timer;
        timer.start();
        func();
        timer.stop();

        double elapsed = timer.elapsed_ms();
        times.push_back(elapsed);
    }

    // Calculate statistics
    std::sort(times.begin(), times.end());

    result.min_time_ms = times.front();
    result.max_time_ms = times.back();

    double sum = 0;
    for (double t : times) {
        sum += t;
    }
    result.avg_time_ms = sum / times.size();
    result.total_time_ms = sum;

    // Calculate throughput
    if (result.avg_time_ms > 0) {
        result.throughput_ops_per_sec = 1000.0 / result.avg_time_ms;
    }

    results_.push_back(result);
    return result;
}

BenchmarkResult PerformanceBenchmark::benchmark_page_parse(uint32_t page_size, int iterations) {
    auto page = create_test_page(page_size, dbrecovery::innodb::FIL_PAGE_INDEX);

    return run("Page Parse (" + std::to_string(page_size) + " bytes)", iterations, [page]() mutable {
        PageParserV2 parser(page.data(), page.size());
        parser.parse();
    });
}

BenchmarkResult PerformanceBenchmark::benchmark_btree_parse(uint32_t page_size, int iterations) {
    auto page = create_test_btree_page(page_size);

    return run("B+Tree Parse (" + std::to_string(page_size) + " bytes)", iterations, [page]() mutable {
        BPlusTreeParser parser(page);
        parser.parse();
    });
}

BenchmarkResult PerformanceBenchmark::benchmark_record_parse(uint32_t page_size, int iterations) {
    auto page = create_test_btree_page(page_size);

    catalog::TableSchema schema;
    schema.table_name = "test_table";
    schema.row_format = 2;  // DYNAMIC
    schema.page_size = page_size;

    catalog::ColumnInfo col;
    col.name = "id";
    col.mysql_type_id = dbrecovery::innodb::mysql_types::LONGLONG;
    col.is_nullable = false;
    col.column_length = 8;
    schema.columns[1] = col;

    return run("Record Parse (" + std::to_string(page_size) + " bytes)", iterations, [page, schema]() mutable {
        BPlusTreeParser parser(page);
        parser.parse();
        auto records = parser.parse_records(schema);
    });
}

BenchmarkResult PerformanceBenchmark::benchmark_blob_reconstruct(uint32_t blob_size, int iterations) {
    std::vector<uint8_t> blob_data(blob_size);
    std::fill(blob_data.begin(), blob_data.end(), 0x42);

    return run("Blob Reconstruct (" + std::to_string(blob_size) + " bytes)", iterations, [blob_data]() mutable {
        dbrecovery::recovery::BlobReconstructor reconstructor(1);
        reconstructor.AddChunk(0, blob_data.data(), static_cast<uint32_t>(blob_data.size()));
        auto result = reconstructor.GetBlob();
    });
}

BenchmarkResult PerformanceBenchmark::benchmark_undo_log_parse(uint32_t page_size, int iterations) {
    auto page = create_test_undo_page(page_size);

    return run("Undo Log Parse (" + std::to_string(page_size) + " bytes)", iterations, [page, page_size]() mutable {
        dbrecovery::innodb::UndoLogParser parser;
        parser.set_page_size(page_size);
        parser.parse_page(page.data(), page.size(), 0);
    });
}

BenchmarkResult PerformanceBenchmark::benchmark_redo_log_replay(uint32_t page_size, int iterations) {
    auto page = create_test_page(page_size, dbrecovery::innodb::FIL_PAGE_INDEX);
    auto redo_records = create_test_redo_records(page_size, 100);

    dbrecovery::innodb::RedoLogParser redo_parser;
    auto records = redo_parser.parse_file("/dev/null");  // Will return empty, but sets up parser

    return run("Redo Log Replay (" + std::to_string(page_size) + " bytes)", iterations, [page, redo_parser, page_size]() mutable {
        dbrecovery::innodb::RedoLogReplayer replayer;
        replayer.set_page_size(page_size);
        replayer.add_page(0, std::move(page));
        // Note: This is a simplified benchmark
    });
}

const std::vector<BenchmarkResult>& PerformanceBenchmark::get_results() const {
    return results_;
}

void PerformanceBenchmark::print_report() const {
    std::cout << "\n=== Performance Benchmark Report ===\n\n";

    for (const auto& result : results_) {
        std::cout << result.name << ":\n";
        std::cout << "  Iterations: " << result.iterations << "\n";
        std::cout << "  Total time: " << result.total_time_ms << " ms\n";
        std::cout << "  Avg time: " << result.avg_time_ms << " ms/op\n";
        std::cout << "  Min time: " << result.min_time_ms << " ms\n";
        std::cout << "  Max time: " << result.max_time_ms << " ms\n";
        std::cout << "  Throughput: " << result.throughput_ops_per_sec << " ops/sec\n";
        std::cout << "\n";
    }
}

void PerformanceBenchmark::clear() {
    results_.clear();
}

std::vector<uint8_t> PerformanceBenchmark::create_test_page(uint32_t page_size, uint16_t page_type) {
    std::vector<uint8_t> page(page_size, 0);

    // FIL Header
    dbrecovery::innodb::write_le<uint32_t>(page.data(), 0, 0xFFFFFFFF);  // prev_page
    dbrecovery::innodb::write_le<uint32_t>(page.data(), 4, 0xFFFFFFFF);  // next_page
    dbrecovery::innodb::write_le<uint64_t>(page.data(), 8, 0);           // LSN
    dbrecovery::innodb::write_le<uint32_t>(page.data(), 16, 0);          // space_id
    dbrecovery::innodb::write_le<uint32_t>(page.data(), 20, 0);          // page_no
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 50, page_type);  // page_type

    // Page Header
    uint8_t* page_header = page.data() + 38;
    dbrecovery::innodb::write_le<uint16_t>(page_header, 0, 2);   // n_dir_slots
    dbrecovery::innodb::write_le<uint16_t>(page_header, 2, 54 + 13 + 8);  // heap_top
    dbrecovery::innodb::write_le<uint16_t>(page_header, 4, 2 | 0x8000);   // n_heap

    // INFIMUM record
    page[54] = dbrecovery::innodb::REC_STATUS_INFIMUM;
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 55, 54 + 13);

    // SUPREMUM record
    page[54 + 13] = dbrecovery::innodb::REC_STATUS_SUPREMUM;

    return page;
}

std::vector<uint8_t> PerformanceBenchmark::create_test_btree_page(uint32_t page_size) {
    auto page = create_test_page(page_size, dbrecovery::innodb::FIL_PAGE_INDEX);

    // Add some user records
    uint16_t offset = 54 + 13 + 8;  // After INFIMUM and SUPREMUM

    for (int i = 0; i < 10; ++i) {
        if (offset + 100 < page_size) {
            page[offset] = 0;  // info_bits
            dbrecovery::innodb::write_le<uint16_t>(page.data(), offset + 1, offset + 100);  // next_offset
            offset += 100;
        }
    }

    return page;
}

std::vector<uint8_t> PerformanceBenchmark::create_test_undo_page(uint32_t page_size) {
    auto page = create_test_page(page_size, dbrecovery::innodb::FIL_PAGE_UNDO_LOG);

    // Add undo log header
    uint8_t* page_header = page.data() + 38;
    dbrecovery::innodb::write_le<uint64_t>(page_header, 0, 12345);  // trx_id
    dbrecovery::innodb::write_le<uint64_t>(page_header, 8, 67890);  // roll_ptr

    return page;
}

std::vector<uint8_t> PerformanceBenchmark::create_test_redo_records(uint32_t page_size, int count) {
    std::vector<uint8_t> data(count * 20, 0);

    for (int i = 0; i < count; ++i) {
        size_t pos = i * 20;
        data[pos] = dbrecovery::innodb::MLOG_2BYTES;  // type
        dbrecovery::innodb::write_le<uint32_t>(data.data(), pos + 1, 0);  // space_id
        dbrecovery::innodb::write_le<uint32_t>(data.data(), pos + 5, 0);  // page_no
    }

    return data;
}

// ==================== MemoryPool ====================

MemoryPool::MemoryPool(size_t block_size, size_t initial_capacity)
    : block_size_(block_size) {
    for (size_t i = 0; i < initial_capacity; ++i) {
        allocate_block();
    }
}

MemoryPool::~MemoryPool() {
    for (auto ptr : all_blocks_) {
        delete[] ptr;
    }
}

uint8_t* MemoryPool::allocate() {
    if (free_list_.empty()) {
        allocate_block();
    }

    uint8_t* ptr = free_list_.back();
    free_list_.pop_back();
    stats_.total_allocated += block_size_;
    stats_.active_blocks++;
    stats_.peak_usage = std::max(stats_.peak_usage, stats_.active_blocks);

    return ptr;
}

void MemoryPool::free(uint8_t* ptr) {
    if (ptr) {
        free_list_.push_back(ptr);
        stats_.total_freed += block_size_;
        stats_.active_blocks--;
    }
}

MemoryPool::Stats MemoryPool::get_stats() const {
    return stats_;
}

void MemoryPool::clear() {
    for (auto ptr : all_blocks_) {
        delete[] ptr;
    }
    all_blocks_.clear();
    free_list_.clear();
    stats_ = {};
}

void MemoryPool::allocate_block() {
    uint8_t* block = new uint8_t[block_size_];
    all_blocks_.push_back(block);
    free_list_.push_back(block);
}

// ==================== Timer ====================

Timer::Timer() : running_(false) {
    reset();
}

Timer::~Timer() = default;

void Timer::start() {
    start_time_ = std::chrono::high_resolution_clock::now();
    running_ = true;
}

void Timer::stop() {
    end_time_ = std::chrono::high_resolution_clock::now();
    running_ = false;
}

void Timer::reset() {
    start_time_ = {};
    end_time_ = {};
    running_ = false;
}

double Timer::elapsed_ms() const {
    auto duration = std::chrono::high_resolution_clock::now() - start_time_;
    return std::chrono::duration<double, std::milli>(duration).count();
}

double Timer::elapsed_us() const {
    auto duration = std::chrono::high_resolution_clock::now() - start_time_;
    return std::chrono::duration<double, std::micro>(duration).count();
}

bool Timer::is_running() const {
    return running_;
}

// ==================== BenchmarkRunner ====================

BenchmarkRunner::BenchmarkRunner() = default;

BenchmarkRunner::~BenchmarkRunner() = default;

void BenchmarkRunner::run_all() {
    // Run page parsing benchmarks
    results_.push_back(benchmark_.benchmark_page_parse(4096, 1000));
    results_.push_back(benchmark_.benchmark_page_parse(8192, 1000));
    results_.push_back(benchmark_.benchmark_page_parse(16384, 1000));
    results_.push_back(benchmark_.benchmark_page_parse(32768, 1000));

    // Run B+Tree parsing benchmarks
    results_.push_back(benchmark_.benchmark_btree_parse(16384, 1000));

    // Run record parsing benchmarks
    results_.push_back(benchmark_.benchmark_record_parse(16384, 1000));

    // Run BLOB reconstruction benchmarks
    results_.push_back(benchmark_.benchmark_blob_reconstruct(1024, 1000));
    results_.push_back(benchmark_.benchmark_blob_reconstruct(65536, 100));
    results_.push_back(benchmark_.benchmark_blob_reconstruct(1048576, 10));

    // Run undo log parsing benchmarks
    results_.push_back(benchmark_.benchmark_undo_log_parse(16384, 1000));

    // Run redo log replay benchmarks
    results_.push_back(benchmark_.benchmark_redo_log_replay(16384, 1000));
}

const std::vector<BenchmarkResult>& BenchmarkRunner::get_results() const {
    return results_;
}

void BenchmarkRunner::print_report() const {
    benchmark_.print_report();
}

} // namespace innodb
} // namespace dbrecovery
