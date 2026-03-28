# Performance Benchmarking Guide

## Overview

This document describes how to run performance benchmarks for the MySQL 8.0 Recovery Engine.

## Prerequisites

- C++20 compatible compiler (GCC 11+, Clang 13+)
- Google Benchmark library

## Building with Benchmark Support

```bash
# Clone Google Benchmark
cd /tmp
git clone https://github.com/google/benchmark.git
cd benchmark
cmake -E create_directory "build"
cmake -E chdir "build" cmake -DBENCHMARK_ENABLE_GTEST_TESTS=OFF -DCMAKE_BUILD_TYPE=Release ../
cmake -E chdir "build" make -j$(nproc)
sudo cmake -E chdir "build" make install

# Build the project with benchmark support
cd /app/mysql8_recovery
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_BENCHMARK=ON
make -j$(nproc)
```

## Running Benchmarks

```bash
# Run all benchmarks
./tests/benchmark/benchmark

# Run specific benchmark
./tests/benchmark/benchmark --benchmark_filter=BM_PageParser_ParseValidPage

# Run with JSON output
./tests/benchmark/benchmark --benchmark_format=json > benchmark_results.json

# Run with detailed output
./tests/benchmark/benchmark --benchmark_report_aggregates_only=true
```

## Benchmark Results

The benchmark results are stored in the `build/coverage/benchmark/` directory:

```
build/
└── coverage/
    └── benchmark/
        ├── benchmark_results.json
        ├── benchmark_results.html
        └── benchmark_summary.txt
```

## Benchmark Categories

### 1. Page Parser Benchmarks

| Benchmark | Description |
|-----------|-------------|
| `BM_PageParser_ParseValidPage` | Parse a valid 16KB page |
| `BM_PageParser_ParseRecords` | Parse records from a page |

### 2. Page Reader Benchmarks

| Benchmark | Description |
|-----------|-------------|
| `BM_PageReader_CRC32C` | CRC32C checksum calculation |
| `BM_PageReader_INNODB` | INNODB checksum calculation |
| `BM_PageReader_OLD` | OLD checksum calculation |
| `BM_PageReader_ValidateChecksum` | Validate page checksum |

### 3. B+Tree Parser Benchmarks

| Benchmark | Description |
|-----------|-------------|
| `BM_BPlusTreeParser_Parse` | Parse B+Tree page |
| `BM_BPlusTreeParser_ParseRecords` | Parse B+Tree records |

### 4. Mach Read Benchmarks

| Benchmark | Description |
|-----------|-------------|
| `BM_MachRead_ReadLE16` | Read little-endian uint16_t |
| `BM_MachRead_ReadLE32` | Read little-endian uint32_t |
| `BM_MachRead_ReadLE64` | Read little-endian uint64_t |

### 5. BLOB Pointer Benchmarks

| Benchmark | Description |
|-----------|-------------|
| `BM_BlobPointer_Parse` | Parse BLOB pointer |
| `BM_BlobPointer_Validate` | Validate BLOB pointer |

### 6. SDI Parser Benchmarks

| Benchmark | Description |
|-----------|-------------|
| `BM_SDI_Decompress` | SDI decompression |

## Performance Targets

| Operation | Target Time | Notes |
|-----------|-------------|-------|
| Parse 16KB page | < 100 μs | Page header parsing |
| CRC32C checksum | < 50 μs | 16KB page |
| B+Tree parse | < 200 μs | Full page parsing |
| BLOB pointer parse | < 10 μs | 20-byte pointer |
| SDI decompress | < 1 ms | Typical SDI size |

## Troubleshooting

### Missing Google Benchmark

```bash
# Install Google Benchmark
sudo apt-get install libbenchmark-dev
```

### Linker Errors

```bash
# Ensure Google Benchmark is installed
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_BENCHMARK=ON
make -j$(nproc)
```

## Contributing Benchmarks

To add new benchmarks:

1. Add benchmark function to `tests/benchmark/benchmark.cpp`
2. Use `BENCHMARK()` macro to register
3. Run benchmarks and verify results
4. Update this documentation

## References

- Google Benchmark: https://github.com/google/benchmark
- Performance Best Practices: https://github.com/google/benchmark/blob/main/docs/user_guide.md
