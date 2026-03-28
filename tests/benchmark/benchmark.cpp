// Performance benchmarks for MySQL 8.0 Recovery Engine
// Usage: g++ -std=c++20 -O2 -Iinclude benchmark.cpp -o benchmark

#include <benchmark/benchmark.h>
#include "dbrecovery/innodb/page_parser.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/btree.hpp"

#include <vector>
#include <cstdint>
#include <cstring>

// ==================== Page Parser Benchmarks ====================

// Benchmark: Parse a valid 16KB page
static void BM_PageParser_ParseValidPage(benchmark::State& state) {
    // Create a valid 16KB page
    std::vector<uint8_t> page_data(16384, 0);
    
    // Set page type to FIL_PAGE_INDEX (9969)
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 50, 9969);
    
    // Set page header
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 38, 16);  // n_dir_slots
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 40, 100); // heap_top
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 42, 2);   // n_heap
    
    for (auto _ : state) {
        dbrecovery::innodb::PageParser parser(page_data.data(), page_data.size());
        parser.parse_header();
    }
}
BENCHMARK(BM_PageParser_ParseValidPage);

// Benchmark: Parse records from a page
static void BM_PageParser_ParseRecords(benchmark::State& state) {
    std::vector<uint8_t> page_data(16384, 0);
    
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 50, 9969);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 38, 16);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 40, 100);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 42, 2);
    
    for (auto _ : state) {
        dbrecovery::innodb::PageParser parser(page_data.data(), page_data.size());
        parser.parse_header();
        auto records = parser.parse_records();
    }
}
BENCHMARK(BM_PageParser_ParseRecords);

// ==================== Page Reader Benchmarks ====================

// Benchmark: CRC32C checksum calculation
static void BM_PageReader_CRC32C(benchmark::State& state) {
    std::vector<uint8_t> page_data(16384, 0);
    
    for (auto _ : state) {
        auto checksum = dbrecovery::innodb::PageReader::compute_crc32c(page_data);
        benchmark::DoNotOptimize(checksum);
    }
}
BENCHMARK(BM_PageReader_CRC32C);

// Benchmark: INNODB checksum calculation
static void BM_PageReader_INNODB(benchmark::State& state) {
    std::vector<uint8_t> page_data(16384, 0);
    
    for (auto _ : state) {
        auto checksum = dbrecovery::innodb::PageReader::compute_innodb_checksum(page_data);
        benchmark::DoNotOptimize(checksum);
    }
}
BENCHMARK(BM_PageReader_INNODB);

// Benchmark: OLD checksum calculation
static void BM_PageReader_OLD(benchmark::State& state) {
    std::vector<uint8_t> page_data(16384, 0);
    
    for (auto _ : state) {
        auto checksum = dbrecovery::innodb::PageReader::compute_old_checksum(page_data);
        benchmark::DoNotOptimize(checksum);
    }
}
BENCHMARK(BM_PageReader_OLD);

// Benchmark: Validate checksum
static void BM_PageReader_ValidateChecksum(benchmark::State& state) {
    std::vector<uint8_t> page_data(16384, 0);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 50, 9969);
    
    auto checksum = dbrecovery::innodb::PageReader::compute_crc32c(page_data);
    for (int i = 0; i < 4; ++i) {
        page_data[24 + i] = (checksum >> (i * 8)) & 0xFF;
    }
    
    for (auto _ : state) {
        auto valid = dbrecovery::innodb::PageReader::validate_checksum(page_data);
        benchmark::DoNotOptimize(valid);
    }
}
BENCHMARK(BM_PageReader_ValidateChecksum);

// ==================== B+Tree Parser Benchmarks ====================

// Benchmark: Parse B+Tree page
static void BM_BPlusTreeParser_Parse(benchmark::State& state) {
    std::vector<uint8_t> page_data(16384, 0);
    
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 50, 9969);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 38, 16);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 40, 100);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 42, 2);
    
    for (auto _ : state) {
        dbrecovery::innodb::BPlusTreeParser parser(page_data);
        parser.parse();
    }
}
BENCHMARK(BM_BPlusTreeParser_Parse);

// Benchmark: Parse B+Tree records
static void BM_BPlusTreeParser_ParseRecords(benchmark::State& state) {
    std::vector<uint8_t> page_data(16384, 0);
    
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 50, 9969);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 38, 16);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 40, 100);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 42, 2);
    
    dbrecovery::catalog::TableSchema schema;
    schema.table_name = "test";
    schema.row_format = 1;
    schema.page_size = 16384;
    
    for (auto _ : state) {
        dbrecovery::innodb::BPlusTreeParser parser(page_data);
        parser.parse();
        auto records = parser.parse_records(schema);
    }
}
BENCHMARK(BM_BPlusTreeParser_ParseRecords);

// ==================== Mach Read Benchmarks ====================

// Benchmark: Read little-endian uint16_t
static void BM_MachRead_ReadLE16(benchmark::State& state) {
    std::vector<uint8_t> data(1024, 0);
    for (size_t i = 0; i < data.size(); i += 2) {
        dbrecovery::innodb::write_le<uint16_t>(data.data(), i, static_cast<uint16_t>(i));
    }
    
    for (auto _ : state) {
        for (size_t i = 0; i < data.size(); i += 2) {
            auto val = dbrecovery::innodb::read_le<uint16_t>(data.data(), i);
            benchmark::DoNotOptimize(val);
        }
    }
}
BENCHMARK(BM_MachRead_ReadLE16);

// Benchmark: Read little-endian uint32_t
static void BM_MachRead_ReadLE32(benchmark::State& state) {
    std::vector<uint8_t> data(1024, 0);
    for (size_t i = 0; i < data.size(); i += 4) {
        dbrecovery::innodb::write_le<uint32_t>(data.data(), i, static_cast<uint32_t>(i));
    }
    
    for (auto _ : state) {
        for (size_t i = 0; i < data.size(); i += 4) {
            auto val = dbrecovery::innodb::read_le<uint32_t>(data.data(), i);
            benchmark::DoNotOptimize(val);
        }
    }
}
BENCHMARK(BM_MachRead_ReadLE32);

// Benchmark: Read little-endian uint64_t
static void BM_MachRead_ReadLE64(benchmark::State& state) {
    std::vector<uint8_t> data(1024, 0);
    for (size_t i = 0; i < data.size(); i += 8) {
        dbrecovery::innodb::write_le<uint64_t>(data.data(), i, static_cast<uint64_t>(i));
    }
    
    for (auto _ : state) {
        for (size_t i = 0; i < data.size(); i += 8) {
            auto val = dbrecovery::innodb::read_le<uint64_t>(data.data(), i);
            benchmark::DoNotOptimize(val);
        }
    }
}
BENCHMARK(BM_MachRead_ReadLE64);

// ==================== BLOB Pointer Benchmarks ====================

// Benchmark: Parse BLOB pointer
static void BM_BlobPointer_Parse(benchmark::State& state) {
    std::vector<uint8_t> data(20, 0);
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 0, 0);   // space_id
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 4, 100); // page_no
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 8, 0);   // offset
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 12, 50); // len
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 16, 50); // total_len
    
    for (auto _ : state) {
        auto ptr = dbrecovery::innodb::parse_blob_pointer(data);
        benchmark::DoNotOptimize(ptr);
    }
}
BENCHMARK(BM_BlobPointer_Parse);

// Benchmark: Validate BLOB pointer
static void BM_BlobPointer_Validate(benchmark::State& state) {
    std::vector<uint8_t> data(20, 0);
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 0, 0);
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 4, 100);
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 8, 0);
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 12, 50);
    dbrecovery::innodb::write_le<uint32_t>(data.data(), 16, 50);
    
    auto ptr = dbrecovery::innodb::parse_blob_pointer(data);
    
    for (auto _ : state) {
        auto valid = ptr->is_complete();
        benchmark::DoNotOptimize(valid);
    }
}
BENCHMARK(BM_BlobPointer_Validate);

// ==================== SDI Parser Benchmarks ====================

// Benchmark: SDI decompression
static void BM_SDI_Decompress(benchmark::State& state) {
    // Create a compressed SDI payload
    std::vector<uint8_t> payload(100, 0);
    dbrecovery::innodb::write_le<uint32_t>(payload.data(), 0, 1);   // type
    dbrecovery::innodb::write_le<uint64_t>(payload.data(), 4, 12345); // id
    dbrecovery::innodb::write_le<uint32_t>(payload.data(), 12, 100); // uncompressed_len
    dbrecovery::innodb::write_le<uint32_t>(payload.data(), 16, 50);  // compressed_len
    
    // Add some compressed data (placeholder)
    for (size_t i = 20; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i % 256);
    }
    
    for (auto _ : state) {
        try {
            auto schema = dbrecovery::innodb::SDIManager::parse_sdi_record(payload.data(), payload.size());
            benchmark::DoNotOptimize(schema);
        } catch (...) {
            // Expected for placeholder data
        }
    }
}
BENCHMARK(BM_SDI_Decompress);

// ==================== Run benchmarks ====================

BENCHMARK_MAIN();
