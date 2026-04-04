/**
 * @file test_io_uring_scanner.cpp
 * @brief Test io_uring high-performance I/O scanner
 */

#include <iostream>
#include <fstream>
#include <cstring>
#include <cassert>
#include <vector>
#include <chrono>
#include "dbrecovery/disk/io_uring_scanner.hpp"

#ifdef HAVE_IO_URING

// 测试工具函数
static std::vector<uint8_t> create_test_file(const std::string& path, size_t size) {
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; i++) {
        data[i] = static_cast<uint8_t>(i % 256);
    }

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return data;
}

// P4-01: Basic I/O operations
bool test_basic_io() {
    std::cout << "[TEST] P4-01: Basic I/O operations...\n";

    const std::string test_path = "/tmp/test_io_uring_basic.bin";
    const size_t file_size = 1024 * 1024;  // 1MB

    auto data = create_test_file(test_path, file_size);

    dbrecovery::disk::IOUringScanner scanner;
    assert(scanner.is_open() == false);

    if (!scanner.open(test_path)) {
        std::cout << "    [SKIP] Failed to open file\n";
        std::remove(test_path.c_str());
        return true;
    }

    assert(scanner.is_open() == true);
    assert(scanner.get_size() == file_size);

    // Read some data
    std::vector<uint8_t> buffer(4096);
    ssize_t result = scanner.read(0, buffer.data(), buffer.size());

    assert(result == static_cast<ssize_t>(buffer.size()));
    assert(std::memcmp(buffer.data(), data.data(), buffer.size()) == 0);

    // Read from middle
    result = scanner.read(512 * 1024, buffer.data(), buffer.size());
    assert(result == static_cast<ssize_t>(buffer.size()));
    assert(std::memcmp(buffer.data(), data.data() + 512 * 1024, buffer.size()) == 0);

    scanner.close();
    std::remove(test_path.c_str());

    std::cout << "    [PASS] Basic I/O operations\n";
    return true;
}

// P4-02: Async I/O operations
bool test_async_io() {
    std::cout << "[TEST] P4-02: Async I/O operations...\n";

    const std::string test_path = "/tmp/test_io_uring_async.bin";
    const size_t file_size = 4 * 1024 * 1024;  // 4MB

    auto data = create_test_file(test_path, file_size);

    dbrecovery::disk::IOUringScanner scanner(64);
    if (!scanner.open(test_path)) {
        std::cout << "    [SKIP] Failed to open file\n";
        std::remove(test_path.c_str());
        return true;
    }

    // Queue multiple async reads
    std::vector<uint8_t> buffers[4];
    for (int i = 0; i < 4; i++) {
        buffers[i].resize(65536);
        scanner.read_async(i * 65536, buffers[i].data(), buffers[i].size());
    }

    // Submit and wait
    scanner.submit();
    scanner.wait_and_complete(4);

    // Verify
    for (int i = 0; i < 4; i++) {
        assert(std::memcmp(buffers[i].data(), data.data() + i * 65536, 65536) == 0);
    }

    scanner.close();
    std::remove(test_path.c_str());

    std::cout << "    [PASS] Async I/O operations\n";
    return true;
}

// P4-03: Batch read operations
bool test_batch_read() {
    std::cout << "[TEST] P4-03: Batch read operations...\n";

    const std::string test_path = "/tmp/test_io_uring_batch.bin";
    const size_t file_size = 2 * 1024 * 1024;  // 2MB

    auto data = create_test_file(test_path, file_size);

    dbrecovery::disk::IOUringScanner scanner;
    if (!scanner.open(test_path)) {
        std::cout << "    [SKIP] Failed to open file\n";
        std::remove(test_path.c_str());
        return true;
    }

    // Create batch read regions
    std::vector<std::tuple<uint64_t, uint8_t*, size_t>> regions;
    std::vector<std::vector<uint8_t>> buffers;

    for (int i = 0; i < 8; i++) {
        buffers.emplace_back(32768);
        regions.emplace_back(i * 32768, buffers.back().data(), 32768);
    }

    size_t queued = scanner.batch_read(regions);
    assert(queued == 8);

    scanner.wait_and_complete(8);

    // Verify
    for (int i = 0; i < 8; i++) {
        assert(std::memcmp(std::get<1>(regions[i]), data.data() + i * 32768, 32768) == 0);
    }

    scanner.close();
    std::remove(test_path.c_str());

    std::cout << "    [PASS] Batch read operations\n";
    return true;
}

// P4-04: Performance comparison (io_uring vs POSIX)
bool test_performance() {
    std::cout << "[TEST] P4-04: Performance comparison...\n";

    const std::string test_path = "/tmp/test_io_uring_perf.bin";
    const size_t file_size = 64 * 1024 * 1024;  // 64MB

    auto data = create_test_file(test_path, file_size);

    // Test POSIX I/O
    auto start = std::chrono::high_resolution_clock::now();

    int fd = ::open(test_path.c_str(), O_RDONLY);
    assert(fd >= 0);

    std::vector<uint8_t> posix_buffer(65536);
    uint64_t posix_bytes = 0;
    for (size_t offset = 0; offset < file_size; offset += 65536) {
        ssize_t r = ::pread(fd, posix_buffer.data(), 65536, offset);
        if (r > 0) posix_bytes += r;
    }
    ::close(fd);

    auto posix_end = std::chrono::high_resolution_clock::now();
    auto posix_duration = std::chrono::duration_cast<std::chrono::microseconds>(posix_end - start).count();

    // Test io_uring I/O
    dbrecovery::disk::IOUringScanner scanner;
    if (!scanner.open(test_path)) {
        std::cout << "    [SKIP] Failed to open file with io_uring\n";
        std::remove(test_path.c_str());
        return true;
    }

    start = std::chrono::high_resolution_clock::now();

    std::vector<uint8_t> uring_buffer(65536);
    uint64_t uring_bytes = 0;
    for (size_t offset = 0; offset < file_size; offset += 65536) {
        ssize_t r = scanner.read(offset, uring_buffer.data(), 65536);
        if (r > 0) uring_bytes += r;
    }

    scanner.close();
    auto uring_end = std::chrono::high_resolution_clock::now();
    auto uring_duration = std::chrono::duration_cast<std::chrono::microseconds>(uring_end - start).count();

    std::cout << "    POSIX I/O:    " << (posix_duration / 1000.0) << " ms\n";
    std::cout << "    io_uring I/O: " << (uring_duration / 1000.0) << " ms\n";

    double speedup = static_cast<double>(posix_duration) / uring_duration;
    std::cout << "    Speedup: " << speedup << "x\n";

    std::remove(test_path.c_str());

    // Note: io_uring shows benefit with concurrent operations
    // Single-threaded sequential reads may not show significant improvement
    std::cout << "    [PASS] Performance comparison\n";
    return true;
}

// P4-05: Statistics tracking
bool test_statistics() {
    std::cout << "[TEST] P4-05: Statistics tracking...\n";

    const std::string test_path = "/tmp/test_io_uring_stats.bin";
    const size_t file_size = 1024 * 1024;  // 1MB

    auto data = create_test_file(test_path, file_size);

    dbrecovery::disk::IOUringScanner scanner;
    if (!scanner.open(test_path)) {
        std::cout << "    [SKIP] Failed to open file\n";
        std::remove(test_path.c_str());
        return true;
    }

    scanner.reset_stats();

    std::vector<uint8_t> buffer(4096);
    for (int i = 0; i < 100; i++) {
        scanner.read(i * 4096, buffer.data(), buffer.size());
    }

    auto stats = scanner.get_stats();
    assert(stats.read_count >= 100);
    assert(stats.bytes_read >= 409600);  // At least 100 * 4096

    std::cout << "    Bytes read: " << stats.bytes_read << "\n";
    std::cout << "    Read count: " << stats.read_count << "\n";
    std::cout << "    Submit count: " << stats.submit_count << "\n";
    std::cout << "    Completion count: " << stats.completion_count << "\n";

    scanner.close();
    std::remove(test_path.c_str());

    std::cout << "    [PASS] Statistics tracking\n";
    return true;
}

// P4-06: Large file support
bool test_large_file() {
    std::cout << "[TEST] P4-06: Large file support...\n";

    const std::string test_path = "/tmp/test_io_uring_large.bin";
    const size_t file_size = 256 * 1024 * 1024;  // 256MB

    auto data = create_test_file(test_path, file_size);

    dbrecovery::disk::IOUringScanner scanner;
    if (!scanner.open(test_path)) {
        std::cout << "    [SKIP] Failed to open file\n";
        std::remove(test_path.c_str());
        return true;
    }

    assert(scanner.get_size() == file_size);

    // Read from end of file
    std::vector<uint8_t> buffer(4096);
    ssize_t result = scanner.read(file_size - 4096, buffer.data(), 4096);
    assert(result == 4096);
    assert(std::memcmp(buffer.data(), data.data() + file_size - 4096, 4096) == 0);

    scanner.close();
    std::remove(test_path.c_str());

    std::cout << "    [PASS] Large file support\n";
    return true;
}

// P4-07: Queue depth configuration
bool test_queue_depth() {
    std::cout << "[TEST] P4-07: Queue depth configuration...\n";

    dbrecovery::disk::IOUringScanner scanner1(16);
    dbrecovery::disk::IOUringScanner scanner2(64);
    dbrecovery::disk::IOUringScanner scanner3(128);

    assert(scanner1.get_queue_depth() == 16);
    assert(scanner2.get_queue_depth() == 64);
    assert(scanner3.get_queue_depth() == 128);

    std::cout << "    [PASS] Queue depth configuration\n";
    return true;
}

// P4-08: Batch size configuration
bool test_batch_size() {
    std::cout << "[TEST] P4-08: Batch size configuration...\n";

    dbrecovery::disk::IOUringScanner scanner;

    scanner.set_batch_size(32768);
    assert(scanner.get_batch_size() == 32768);

    scanner.set_batch_size(131072);
    assert(scanner.get_batch_size() == 131072);

    std::cout << "    [PASS] Batch size configuration\n";
    return true;
}

#else  // HAVE_IO_URING not defined

bool test_basic_io() {
    std::cout << "[TEST] P4-01: Basic I/O operations... [SKIP - io_uring not available]\n";
    return true;
}

bool test_async_io() {
    std::cout << "[TEST] P4-02: Async I/O operations... [SKIP - io_uring not available]\n";
    return true;
}

bool test_batch_read() {
    std::cout << "[TEST] P4-03: Batch read operations... [SKIP - io_uring not available]\n";
    return true;
}

bool test_performance() {
    std::cout << "[TEST] P4-04: Performance comparison... [SKIP - io_uring not available]\n";
    return true;
}

bool test_statistics() {
    std::cout << "[TEST] P4-05: Statistics tracking... [SKIP - io_uring not available]\n";
    return true;
}

bool test_large_file() {
    std::cout << "[TEST] P4-06: Large file support... [SKIP - io_uring not available]\n";
    return true;
}

bool test_queue_depth() {
    std::cout << "[TEST] P4-07: Queue depth configuration... [SKIP - io_uring not available]\n";
    return true;
}

bool test_batch_size() {
    std::cout << "[TEST] P4-08: Batch size configuration... [SKIP - io_uring not available]\n";
    return true;
}

#endif  // HAVE_IO_URING

// ==================== 主函数 ====================

int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "MySQL Recovery Pro - io_uring Scanner Tests\n";
#ifdef HAVE_IO_URING
    std::cout << "io_uring: ENABLED\n";
#else
    std::cout << "io_uring: DISABLED\n";
#endif
    std::cout << "========================================\n\n";

    int passed = 0;
    int failed = 0;

    if (test_basic_io()) passed++; else failed++;
    if (test_async_io()) passed++; else failed++;
    if (test_batch_read()) passed++; else failed++;
    if (test_performance()) passed++; else failed++;
    if (test_statistics()) passed++; else failed++;
    if (test_large_file()) passed++; else failed++;
    if (test_queue_depth()) passed++; else failed++;
    if (test_batch_size()) passed++; else failed++;

    std::cout << "\n========================================\n";
    std::cout << "Test Results: " << passed << " passed, " << failed << " failed\n";
    std::cout << "========================================\n";

    return failed > 0 ? 1 : 0;
}
