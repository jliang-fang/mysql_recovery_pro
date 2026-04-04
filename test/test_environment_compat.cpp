/**
 * @file test_environment_compat.cpp
 * @brief 环境兼容性测试
 *
 * 测试跨平台的文件系统、内存、线程管理功能
 */

#include "dbrecovery/common/environment_compat.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>
#include <cassert>
#include <cstdio>

using namespace dbrecovery;
using namespace dbrecovery::env;

// ==================== 测试工具函数 ====================

/**
 * @brief 创建临时测试文件
 */
std::string create_temp_file(const std::string& content) {
    char temp_path[] = "/tmp/test_file_XXXXXX";
    #ifdef ENV_PLATFORM_WINDOWS
        int fd = _mktemp_s(temp_path, sizeof(temp_path));
        if (fd == -1) return "";
        FILE* f = fopen(temp_path, "w");
    #else
        int fd = mkstemp(temp_path);
        if (fd == -1) return "";
    #endif

    #ifdef ENV_PLATFORM_WINDOWS
        if (f) {
            fwrite(content.c_str(), 1, content.size(), f);
            fclose(f);
        }
    #else
        write(fd, content.c_str(), content.size());
        close(fd);
    #endif

    return std::string(temp_path);
}

/**
 * @brief 删除临时文件
 */
void remove_temp_file(const std::string& path) {
    #ifdef ENV_PLATFORM_WINDOWS
        DeleteFileA(path.c_str());
    #else
        std::remove(path.c_str());
    #endif
}

// ==================== 文件系统测试 ====================

bool test_file_system_exists() {
    std::cout << "Testing file existence check..." << std::endl;

    FileSystem& fs = FileSystem::instance();

    // Test existing file
    std::string content = "test content";
    std::string path = create_temp_file(content);
    if (path.empty()) {
        std::cerr << "Failed: Could not create temp file" << std::endl;
        return false;
    }

    auto result = fs.file_exists(path);
    if (result.code != ErrorCode::SUCCESS) {
        std::cerr << "Failed: file_exists for existing file: " << result.message << std::endl;
        remove_temp_file(path);
        return false;
    }
    std::cout << "  Existing file check: PASS" << std::endl;

    // Test non-existing file
    result = fs.file_exists("/non/existing/path/file.txt");
    if (result.code == ErrorCode::SUCCESS) {
        std::cerr << "Failed: file_exists should fail for non-existing file" << std::endl;
        remove_temp_file(path);
        return false;
    }
    std::cout << "  Non-existing file check: PASS" << std::endl;

    remove_temp_file(path);
    return true;
}

bool test_file_system_size() {
    std::cout << "Testing file size check..." << std::endl;

    FileSystem& fs = FileSystem::instance();

    std::string content = "Hello, World! This is a test file.";
    std::string path = create_temp_file(content);
    if (path.empty()) {
        std::cerr << "Failed: Could not create temp file" << std::endl;
        return false;
    }

    uint64_t size = 0;
    auto result = fs.file_size(path, size);
    if (result.code != ErrorCode::SUCCESS) {
        std::cerr << "Failed: file_size: " << result.message << std::endl;
        remove_temp_file(path);
        return false;
    }

    if (size != content.size()) {
        std::cerr << "Failed: Expected size " << content.size()
                  << ", got " << size << std::endl;
        remove_temp_file(path);
        return false;
    }
    std::cout << "  File size check: PASS (" << size << " bytes)" << std::endl;

    remove_temp_file(path);
    return true;
}

bool test_file_system_info() {
    std::cout << "Testing file info retrieval..." << std::endl;

    FileSystem& fs = FileSystem::instance();

    std::string content = "test";
    std::string path = create_temp_file(content);
    if (path.empty()) {
        std::cerr << "Failed: Could not create temp file" << std::endl;
        return false;
    }

    FileInfo info;
    auto result = fs.file_info(path, info);
    if (result.code != ErrorCode::SUCCESS) {
        std::cerr << "Failed: file_info: " << result.message << std::endl;
        remove_temp_file(path);
        return false;
    }

    if (info.size != content.size()) {
        std::cerr << "Failed: Wrong file size in info" << std::endl;
        remove_temp_file(path);
        return false;
    }

    if (info.filename.empty()) {
        std::cerr << "Failed: Empty filename in info" << std::endl;
        remove_temp_file(path);
        return false;
    }

    std::cout << "  File info retrieval: PASS" << std::endl;
    std::cout << "    Filename: " << info.filename << std::endl;
    std::cout << "    Size: " << info.size << " bytes" << std::endl;

    remove_temp_file(path);
    return true;
}

bool test_file_system_copy_move() {
    std::cout << "Testing file copy and move..." << std::endl;

    FileSystem& fs = FileSystem::instance();

    std::string content = "copy and move test";
    std::string src_path = create_temp_file(content);
    if (src_path.empty()) {
        std::cerr << "Failed: Could not create temp file" << std::endl;
        return false;
    }

    std::string dst_path = src_path + ".copy";

    // Test copy
    auto result = fs.file_copy(src_path, dst_path);
    if (result.code != ErrorCode::SUCCESS) {
        std::cerr << "Failed: file_copy: " << result.message << std::endl;
        remove_temp_file(src_path);
        return false;
    }
    std::cout << "  File copy: PASS" << std::endl;

    // Verify copy
    uint64_t size = 0;
    result = fs.file_size(dst_path, size);
    if (result.code != ErrorCode::SUCCESS || size != content.size()) {
        std::cerr << "Failed: Copy verification" << std::endl;
        remove_temp_file(src_path);
        remove_temp_file(dst_path);
        return false;
    }

    // Test move
    std::string move_path = src_path + ".move";
    result = fs.file_move(dst_path, move_path);
    if (result.code != ErrorCode::SUCCESS) {
        std::cerr << "Failed: file_move: " << result.message << std::endl;
        remove_temp_file(src_path);
        remove_temp_file(dst_path);
        return false;
    }
    std::cout << "  File move: PASS" << std::endl;

    // Verify move
    result = fs.file_exists(dst_path);
    if (result.code == ErrorCode::SUCCESS) {
        std::cerr << "Failed: Source file should not exist after move" << std::endl;
        remove_temp_file(src_path);
        remove_temp_file(move_path);
        return false;
    }

    result = fs.file_exists(move_path);
    if (result.code != ErrorCode::SUCCESS) {
        std::cerr << "Failed: Destination file should exist after move" << std::endl;
        remove_temp_file(src_path);
        remove_temp_file(move_path);
        return false;
    }

    remove_temp_file(src_path);
    remove_temp_file(move_path);
    return true;
}

bool test_file_system_delete() {
    std::cout << "Testing file deletion..." << std::endl;

    FileSystem& fs = FileSystem::instance();

    std::string content = "delete test";
    std::string path = create_temp_file(content);
    if (path.empty()) {
        std::cerr << "Failed: Could not create temp file" << std::endl;
        return false;
    }

    auto result = fs.file_exists(path);
    if (result.code != ErrorCode::SUCCESS) {
        std::cerr << "Failed: File should exist before deletion" << std::endl;
        return false;
    }

    result = fs.file_delete(path);
    if (result.code != ErrorCode::SUCCESS) {
        std::cerr << "Failed: file_delete: " << result.message << std::endl;
        return false;
    }
    std::cout << "  File deletion: PASS" << std::endl;

    result = fs.file_exists(path);
    if (result.code == ErrorCode::SUCCESS) {
        std::cerr << "Failed: File should not exist after deletion" << std::endl;
        return false;
    }

    return true;
}

bool test_file_system_directory() {
    std::cout << "Testing directory operations..." << std::endl;

    FileSystem& fs = FileSystem::instance();

    std::string dir_path = "/tmp/test_dir_" + std::to_string(std::time(nullptr));

    // Test create directory
    auto result = fs.directory_create(dir_path, false);
    if (result.code != ErrorCode::SUCCESS) {
        std::cerr << "Failed: directory_create: " << result.message << std::endl;
        return false;
    }
    std::cout << "  Directory creation: PASS" << std::endl;

    // Test list directory
    std::vector<std::string> files;
    result = fs.directory_list(dir_path, files);
    if (result.code != ErrorCode::SUCCESS) {
        std::cerr << "Failed: directory_list: " << result.message << std::endl;
        fs.directory_remove(dir_path, false);
        return false;
    }
    std::cout << "  Directory listing: PASS (" << files.size() << " entries)" << std::endl;

    // Test recursive directory creation
    std::string nested_path = dir_path + "/sub1/sub2/sub3";
    result = fs.directory_create(nested_path, true);
    if (result.code != ErrorCode::SUCCESS) {
        std::cerr << "Failed: recursive directory_create: " << result.message << std::endl;
        fs.directory_remove(dir_path, true);
        return false;
    }
    std::cout << "  Recursive directory creation: PASS" << std::endl;

    // Test remove directory
    result = fs.directory_remove(dir_path, true);
    if (result.code != ErrorCode::SUCCESS) {
        std::cerr << "Failed: directory_remove: " << result.message << std::endl;
        return false;
    }
    std::cout << "  Directory removal: PASS" << std::endl;

    return true;
}

// ==================== 内存管理测试 ====================

bool test_memory_allocation() {
    std::cout << "Testing memory allocation..." << std::endl;

    MemoryManager& mm = MemoryManager::instance();

    // Test basic allocation
    void* ptr = mm.allocate(1024);
    if (!ptr) {
        std::cerr << "Failed: allocate returned null" << std::endl;
        return false;
    }
    std::cout << "  Basic allocation: PASS" << std::endl;

    // Test deallocation
    mm.deallocate(ptr);
    std::cout << "  Deallocation: PASS" << std::endl;

    // Test aligned allocation
    void* aligned_ptr = mm.aligned_allocate(1024, 64);
    if (!aligned_ptr) {
        std::cerr << "Failed: aligned_allocate returned null" << std::endl;
        return false;
    }

    // Verify alignment
    uintptr_t addr = reinterpret_cast<uintptr_t>(aligned_ptr);
    if (addr % 64 != 0) {
        std::cerr << "Failed: Not 64-byte aligned: " << addr << std::endl;
        mm.aligned_deallocate(aligned_ptr);
        return false;
    }
    std::cout << "  Aligned allocation (64-byte): PASS (addr = " << addr << ")" << std::endl;

    mm.aligned_deallocate(aligned_ptr);

    return true;
}

bool test_memory_operations() {
    std::cout << "Testing memory operations..." << std::endl;

    MemoryManager& mm = MemoryManager::instance();

    // Test copy
    void* src = mm.allocate(100);
    void* dst = mm.allocate(100);

    // Fill source with pattern
    uint8_t* src_bytes = static_cast<uint8_t*>(src);
    for (int i = 0; i < 100; i++) {
        src_bytes[i] = static_cast<uint8_t>(i);
    }

    // Copy
    mm.copy_memory(dst, src, 100);

    // Verify
    uint8_t* dst_bytes = static_cast<uint8_t*>(dst);
    bool match = true;
    for (int i = 0; i < 100; i++) {
        if (dst_bytes[i] != static_cast<uint8_t>(i)) {
            match = false;
            break;
        }
    }

    if (!match) {
        std::cerr << "Failed: Memory copy verification" << std::endl;
        mm.deallocate(src);
        mm.deallocate(dst);
        return false;
    }
    std::cout << "  Memory copy: PASS" << std::endl;

    mm.deallocate(src);
    mm.deallocate(dst);

    return true;
}

// ==================== 线程管理测试 ====================

bool test_thread_management() {
    std::cout << "Testing thread management..." << std::endl;

    ThreadManager& tm = ThreadManager::instance();

    // Test CPU core count
    uint32_t cores = tm.cpu_cores();
    if (cores == 0) {
        std::cerr << "Failed: cpu_cores returned 0" << std::endl;
        return false;
    }
    std::cout << "  CPU core count: PASS (" << cores << " cores)" << std::endl;

    // Test thread creation
    std::atomic<bool> thread_ran(false);
    std::atomic<int> thread_result(0);

    auto thread_func = [&thread_ran, &thread_result]() {
        thread_ran = true;
        thread_result = 42;
    };

    auto handle = tm.create_thread(thread_func);
    if (!handle) {
        std::cerr << "Failed: create_thread returned null" << std::endl;
        return false;
    }
    std::cout << "  Thread creation: PASS" << std::endl;

    // Wait for thread
    tm.join_thread(handle);

    if (!thread_ran.load()) {
        std::cerr << "Failed: Thread did not run" << std::endl;
        return false;
    }
    std::cout << "  Thread execution: PASS" << std::endl;

    if (thread_result.load() != 42) {
        std::cerr << "Failed: Wrong thread result" << std::endl;
        return false;
    }
    std::cout << "  Thread result: PASS (42)" << std::endl;

    return true;
}

bool test_thread_pool() {
    std::cout << "Testing thread pool..." << std::endl;

    ThreadManager& tm = ThreadManager::instance();

    // Test thread pool creation
    auto pool = tm.create_thread_pool(4);
    if (!pool) {
        std::cerr << "Failed: create_thread_pool returned null" << std::endl;
        return false;
    }
    std::cout << "  Thread pool creation: PASS (4 threads)" << std::endl;

    // Test task execution
    std::atomic<int> task_count(0);
    int num_tasks = 100;

    for (int i = 0; i < num_tasks; i++) {
        pool->enqueue([&task_count]() {
            task_count++;
        });
    }

    // Wait for all tasks
    pool->wait_all();

    if (task_count.load() != num_tasks) {
        std::cerr << "Failed: Expected " << num_tasks << " tasks, got "
                  << task_count.load() << std::endl;
        return false;
    }
    std::cout << "  Thread pool task execution: PASS (" << num_tasks << " tasks)" << std::endl;

    return true;
}

// ==================== 时间管理测试 ====================

bool test_time_management() {
    std::cout << "Testing time management..." << std::endl;

    TimeManager& tm = TimeManager::instance();

    // Test timestamp_ms
    uint64_t ts1 = tm.timestamp_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint64_t ts2 = tm.timestamp_ms();

    if (ts2 <= ts1) {
        std::cerr << "Failed: timestamp_ms not increasing" << std::endl;
        return false;
    }
    std::cout << "  timestamp_ms: PASS (" << (ts2 - ts1) << " ms elapsed)" << std::endl;

    // Test timestamp_us
    uint64_t tus1 = tm.timestamp_us();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    uint64_t tus2 = tm.timestamp_us();

    if (tus2 <= tus1) {
        std::cerr << "Failed: timestamp_us not increasing" << std::endl;
        return false;
    }
    std::cout << "  timestamp_us: PASS" << std::endl;

    // Test timestamp_ns
    uint64_t tns1 = tm.timestamp_ns();
    uint64_t tns2 = tm.timestamp_ns();

    if (tns2 <= tns1) {
        std::cerr << "Failed: timestamp_ns not increasing" << std::endl;
        return false;
    }
    std::cout << "  timestamp_ns: PASS" << std::endl;

    // Test high_res_counter
    uint64_t hrc1 = tm.high_res_counter();
    uint64_t hrc2 = tm.high_res_counter();

    if (hrc2 <= hrc1) {
        std::cerr << "Failed: high_res_counter not increasing" << std::endl;
        return false;
    }
    std::cout << "  high_res_counter: PASS" << std::endl;

    // Test high_res_frequency
    uint64_t freq = tm.high_res_frequency();
    if (freq == 0) {
        std::cerr << "Failed: high_res_frequency returned 0" << std::endl;
        return false;
    }
    std::cout << "  high_res_frequency: PASS (" << freq << " Hz)" << std::endl;

    return true;
}

// ==================== 系统信息测试 ====================

bool test_system_info() {
    std::cout << "Testing system information..." << std::endl;

    SystemInfoGetter& sig = SystemInfoGetter::instance();

    // Test OS name
    std::string os_name = sig.os_name();
    if (os_name.empty()) {
        std::cerr << "Failed: os_name returned empty" << std::endl;
        return false;
    }
    std::cout << "  OS name: PASS (" << os_name << ")" << std::endl;

    // Test OS version
    std::string os_version = sig.os_version();
    if (os_version.empty()) {
        std::cerr << "Failed: os_version returned empty" << std::endl;
        return false;
    }
    std::cout << "  OS version: PASS (" << os_version << ")" << std::endl;

    // Test total memory
    uint64_t total_mem = sig.total_memory();
    if (total_mem == 0) {
        std::cerr << "Failed: total_memory returned 0" << std::endl;
        return false;
    }
    std::cout << "  Total memory: PASS (" << (total_mem / 1024 / 1024) << " MB)" << std::endl;

    // Test available memory
    uint64_t avail_mem = sig.available_memory();
    if (avail_mem == 0) {
        std::cerr << "Failed: available_memory returned 0" << std::endl;
        return false;
    }
    std::cout << "  Available memory: PASS (" << (avail_mem / 1024 / 1024) << " MB)" << std::endl;

    return true;
}

// ==================== 错误处理测试 ====================

bool test_error_handling() {
    std::cout << "Testing error handling..." << std::endl;

    ErrorHandler& eh = ErrorHandler::instance();

    // Test error creation
    ErrorInfo err(ErrorCode::IO_ERROR, "Test error", "Additional info");
    if (err.code != ErrorCode::IO_ERROR) {
        std::cerr << "Failed: Error code not set correctly" << std::endl;
        return false;
    }
    std::cout << "  Error creation: PASS" << std::endl;

    // Test success
    ErrorInfo success(ErrorCode::SUCCESS);
    if (success.code != ErrorCode::SUCCESS) {
        std::cerr << "Failed: Success error not correct" << std::endl;
        return false;
    }
    std::cout << "  Success error: PASS" << std::endl;

    // Test error message
    std::string msg = eh.format_error(err);
    if (msg.empty()) {
        std::cerr << "Failed: format_error returned empty" << std::endl;
        return false;
    }
    std::cout << "  Error formatting: PASS" << std::endl;

    return true;
}

// ==================== 主测试函数 ====================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Environment Compatibility Tests" << std::endl;
    std::cout << "Platform: " << ENV_PLATFORM_NAME << std::endl;
    std::cout << "Architecture: " << ENV_ARCH_NAME << std::endl;
    std::cout << "Compiler: " << ENV_COMPILER_NAME << " " << ENV_COMPILER_VERSION << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    bool all_passed = true;

    // File System Tests
    std::cout << "--- File System Tests ---" << std::endl;
    all_passed &= test_file_system_exists();
    all_passed &= test_file_system_size();
    all_passed &= test_file_system_info();
    all_passed &= test_file_system_copy_move();
    all_passed &= test_file_system_delete();
    all_passed &= test_file_system_directory();
    std::cout << std::endl;

    // Memory Management Tests
    std::cout << "--- Memory Management Tests ---" << std::endl;
    all_passed &= test_memory_allocation();
    all_passed &= test_memory_operations();
    std::cout << std::endl;

    // Thread Management Tests
    std::cout << "--- Thread Management Tests ---" << std::endl;
    all_passed &= test_thread_management();
    all_passed &= test_thread_pool();
    std::cout << std::endl;

    // Time Management Tests
    std::cout << "--- Time Management Tests ---" << std::endl;
    all_passed &= test_time_management();
    std::cout << std::endl;

    // System Info Tests
    std::cout << "--- System Info Tests ---" << std::endl;
    all_passed &= test_system_info();
    std::cout << std::endl;

    // Error Handling Tests
    std::cout << "--- Error Handling Tests ---" << std::endl;
    all_passed &= test_error_handling();
    std::cout << std::endl;

    // Summary
    std::cout << "========================================" << std::endl;
    if (all_passed) {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cout << "Some tests FAILED!" << std::endl;
        return 1;
    }
}
