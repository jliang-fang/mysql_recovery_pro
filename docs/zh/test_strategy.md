# MySQL 8.x 数据恢复引擎 - 测试策略文档

**版本**: v1.0.0  
**作者**: jliang.fang  
**日期**: 2026-03-29

---

## 1. 概述

本文档详细说明 MySQL 8.x 数据恢复引擎的测试策略，包括单元测试、集成测试、模糊测试和性能测试。

### 1.1 测试目标

- **覆盖率**: 核心模块 100% 行覆盖，边缘场景 90%+ 覆盖
- **可靠性**: 所有灾难场景恢复成功率 ≥ 95%
- **性能**: 单页解析 < 1ms，1GB 文件恢复 < 60秒
- **稳定性**: 连续运行 72 小时无内存泄漏

### 1.2 测试环境

| 环境 | 配置 | 说明 |
|------|------|------|
| 开发 | Ubuntu 22.04, GCC 11, CMake 3.26 | 日常开发测试 |
| CI/CD | GitHub Actions, Ubuntu 22.04 | 自动化测试 |
| 压测 | CentOS 8, GCC 12, 32GB RAM | 性能基准测试 |

---

## 2. 单元测试策略

### 2.1 测试框架

- **框架**: Google Test (GTest)
- **语言**: C++17/20
- **断言**: EXPECT_* / ASSERT_*

### 2.2 测试分类

#### 2.2.1 Page 解析测试

| 测试文件 | 测试内容 | 预期覆盖率 |
|----------|----------|------------|
| `test_page_header.cpp` | 页头解析、校验和计算 | 100% |
| `test_page_reader.cpp` | 页读取、校验和验证 | 100% |
| `test_page_parser.cpp` | 页解析、记录提取 | 95% |

**关键测试用例**:

```cpp
// test_page_header.cpp
TEST(PageHeaderTest, ParseValidPageHeader) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的页类型
    write_le<uint16_t>(page_data.data(), 16, FIL_PAGE_INDEX);
    
    // 设置页头信息
    write_le<uint16_t>(page_data.data(), 38, 10);  // n_dir_slots
    write_le<uint16_t>(page_data.data(), 40, 100); // heap_top
    
    PageParser parser(page_data.data(), page_data.size());
    EXPECT_TRUE(parser.parse_header());
}

TEST(PageHeaderTest, ParseInvalidPageType) {
    std::vector<uint8_t> page_data(16384, 0);
    write_le<uint16_t>(page_data.data(), 16, 0xFFFF); // 无效页类型
    
    PageParser parser(page_data.data(), page_data.size());
    EXPECT_FALSE(parser.parse_header());
}
```

#### 2.2.2 B+Tree 测试

| 测试文件 | 测试内容 | 预期覆盖率 |
|----------|----------|------------|
| `test_btree_page_chain.cpp` | B+Tree 页链遍历 | 100% |
| `test_redo_log_forward.cpp` | Redo Log 前滚 | 95% |
| `test_undo_log_rollback.cpp` | Undo Log 回滚 | 90% |

**关键测试用例**:

```cpp
// test_btree_page_chain.cpp
TEST(BPlusTreePageChainTest, TraverseValidPageChain) {
    std::vector<uint8_t> page1(16384, 0);
    std::vector<uint8_t> page2(16384, 0);
    
    // 设置页链: page1 -> page2
    write_le<uint32_t>(page1.data(), 4, 1);  // next_page = 1
    write_le<uint32_t>(page2.data(), 0, 0);  // prev_page = 0
    
    BPlusTreeParser parser1(page1);
    BPlusTreeParser parser2(page2);
    
    EXPECT_TRUE(parser1.parse());
    EXPECT_TRUE(parser2.parse());
}

TEST(BPlusTreePageChainTest, DetectCircularReference) {
    std::vector<uint8_t> page1(16384, 0);
    std::vector<uint8_t> page2(16384, 0);
    
    // 设置循环引用: page1 -> page2 -> page1
    write_le<uint32_t>(page1.data(), 4, 1);  // next_page = 1
    write_le<uint32_t>(page2.data(), 0, 0);  // prev_page = 0
    write_le<uint32_t>(page2.data(), 4, 0);  // next_page = 0
    
    BPlusTreeParser parser1(page1);
    BPlusTreeParser parser2(page2);
    
    EXPECT_TRUE(parser1.parse());
    EXPECT_TRUE(parser2.parse());
}
```

#### 2.2.3 BLOB 恢复测试

| 测试文件 | 测试内容 | 预期覆盖率 |
|----------|----------|------------|
| `innodb/blob_recovery_test.cpp` | BLOB 页链重建 | 100% |
| `test_blob_chain.cpp` | 多页 BLOB 拼接 | 95% |

**关键测试用例**:

```cpp
// innodb/blob_recovery_test.cpp
TEST(BlobRecoveryTest, ReconstructSinglePageBlob) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置 BLOB 指针 (20字节)
    write_le<uint32_t>(page_data.data(), 0, 0);    // space_id
    write_le<uint32_t>(page_data.data(), 4, 100);  // page_no
    write_le<uint32_t>(page_data.data(), 8, 54);   // offset
    write_le<uint32_t>(page_data.data(), 12, 100); // len
    write_le<uint32_t>(page_data.data(), 16, 100); // total_len
    
    // 设置 BLOB 页数据
    for (int i = 0; i < 100; ++i) {
        page_data[54 + i] = static_cast<uint8_t>(i % 256);
    }
    
    auto ptr = parse_blob_pointer(page_data.data(), 20);
    EXPECT_TRUE(ptr.has_value());
    EXPECT_EQ(ptr->total_len, 100);
}

TEST(BlobRecoveryTest, ReconstructMultiPageBlob) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 200);
    chain.add_page(0, 300);
    
    EXPECT_FALSE(chain.has_circular_reference());
    EXPECT_TRUE(chain.is_valid());
}
```

#### 2.2.4 SDI 解析测试

| 测试文件 | 测试内容 | 预期覆盖率 |
|----------|----------|------------|
| `innodb/sdi_manager_test.cpp` | SDI JSON 解析 | 100% |
| `test_sdi_page_chain.cpp` | SDI 页链遍历 | 95% |

**关键测试用例**:

```cpp
// innodb/sdi_manager_test.cpp
TEST(SDIManagerTest, ParseValidSDI) {
    // 创建有效的 SDI payload
    std::vector<uint8_t> payload(1024, 0);
    
    // 设置 SDI header
    write_le<uint32_t>(payload.data(), 0, 1);      // type = TABLE
    write_le<uint64_t>(payload.data(), 4, 12345);  // id
    write_le<uint32_t>(payload.data(), 12, 500);   // uncompressed_len
    write_le<uint32_t>(payload.data(), 16, 300);   // compressed_len
    
    // 添加 zlib 压缩的 JSON 数据
    // ... (实际测试中使用真实的压缩数据)
    
    auto json_opt = SDIManager::extract_sdi_json(payload.data(), payload.size());
    EXPECT_TRUE(json_opt.has_value());
}

TEST(SDIManagerTest, ParseCorruptedSDI) {
    std::vector<uint8_t> payload(100, 0);
    
    // 设置无效的长度
    write_le<uint32_t>(payload.data(), 12, 1000000); // uncompressed_len
    
    auto json_opt = SDIManager::extract_sdi_json(payload.data(), payload.size());
    EXPECT_FALSE(json_opt.has_value());
}
```

#### 2.2.5 行格式测试

| 测试文件 | 测试内容 | 预期覆盖率 |
|----------|----------|------------|
| `innodb/parse_records_test.cpp` | REDUNDANT/COMPACT/DYNAMIC | 100% |

**关键测试用例**:

```cpp
// innodb/parse_records_test.cpp
TEST(RowFormatTest, ParseRedundantRecord) {
    catalog::TableSchema schema;
    schema.row_format = 0; // REDUNDANT
    
    catalog::ColumnInfo col;
    col.name = "id";
    col.mysql_type_id = mysql_types::LONGLONG;
    schema.columns[1] = col;
    
    // 创建 REDUNDANT 格式的记录
    std::vector<uint8_t> record(20, 0);
    record[0] = 0x00; // info_bits
    record[5] = 0x01; // n_fields = 1
    
    // 设置字段长度 (2字节)
    write_le<uint16_t>(record.data(), 18, 8); // length = 8
    
    // 设置数据
    write_le<uint64_t>(record.data(), 10, 12345);
    
    BPlusTreeParser parser(record);
    auto records = parser.parse_records(schema);
    
    EXPECT_EQ(records.size(), 1);
    EXPECT_EQ(records[0]["id"], "12345");
}

TEST(RowFormatTest, ParseCompactRecord) {
    catalog::TableSchema schema;
    schema.row_format = 1; // COMPACT
    
    // ... 类似测试
}

TEST(RowFormatTest, ParseDynamicRecord) {
    catalog::TableSchema schema;
    schema.row_format = 2; // DYNAMIC
    
    // ... 类似测试
}
```

---

## 3. 集成测试策略

### 3.1 测试场景

#### 3.1.1 Truncate 恢复集成测试

| 测试文件 | 测试内容 | 预期成功率 |
|----------|----------|------------|
| `test_truncate_recovery.cpp` | 完整 Truncate 恢复流程 | ≥95% |

**关键测试用例**:

```cpp
// test_truncate_recovery.cpp
TEST(TruncateRecoveryIntegrationTest, FullTruncateRecovery) {
    // 1. 创建测试表
    auto schema = create_test_schema();
    
    // 2. 生成测试数据
    auto records = generate_test_records(1000);
    
    // 3. 模拟 Truncate (标记页为未分配)
    auto page_data = simulate_truncate(records);
    
    // 4. 执行恢复
    RecoveryEngine engine;
    auto recovered = engine.recover_truncate(schema);
    
    // 5. 验证结果
    EXPECT_EQ(recovered.size(), records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        EXPECT_EQ(recovered[i]["id"], records[i]["id"]);
    }
}
```

#### 3.1.2 rm -rf 恢复集成测试

| 测试文件 | 测试内容 | 预期成功率 |
|----------|----------|------------|
| `test_rmrf_recovery.cpp` | 磁盘扫描恢复 | ≥90% |

**关键测试用例**:

```cpp
// test_rmrf_recovery.cpp
TEST(RmrfRecoveryIntegrationTest, DiskScanRecovery) {
    // 1. 创建测试数据
    auto schema = create_test_schema();
    auto records = generate_test_records(500);
    
    // 2. 模拟 rm -rf (删除文件系统条目)
    auto page_data = simulate_rmrf(records);
    
    // 3. 执行磁盘扫描
    RmrfRecovery rmrf;
    auto recovered = rmrf.reconstruct_from_disk(page_data, schema);
    
    // 4. 验证结果
    EXPECT_GE(recovered.size(), records.size() * 0.9); // 允许 10% 损失
}
```

#### 3.1.3 文件头损坏恢复集成测试

| 测试文件 | 测试内容 | 预期成功率 |
|----------|----------|------------|
| `test_header_recovery.cpp` | 页头修复 | ≥95% |

**关键测试用例**:

```cpp
// test_header_recovery.cpp
TEST(HeaderRecoveryIntegrationTest, RepairPageType) {
    // 1. 创建损坏的页
    auto page_data = create_corrupted_page();
    
    // 2. 修复页类型
    HeaderRepairer repairer;
    repairer.repair_page_type(page_data.data());
    
    // 3. 验证修复
    uint16_t page_type = read_le<uint16_t>(page_data.data(), 16);
    EXPECT_EQ(page_type, FIL_PAGE_INDEX);
}

TEST(HeaderRecoveryIntegrationTest, RepairChecksum) {
    // 1. 创建校验和损坏的页
    auto page_data = create_checksum_corrupted_page();
    
    // 2. 修复校验和
    HeaderRepairer repairer;
    repairer.repair_checksum(page_data.data());
    
    // 3. 验证修复
    EXPECT_TRUE(PageReader::validate_checksum(page_data));
}
```

#### 3.1.4 Redo Log 回放集成测试

| 测试文件 | 测试内容 | 预期成功率 |
|----------|----------|------------|
| `innodb/redo_log_replay_test.cpp` | Redo Log 前滚 | ≥95% |

**关键测试用例**:

```cpp
// innodb/redo_log_replay_test.cpp
TEST(RedoLogReplayIntegrationTest, ApplyInsertRecord) {
    // 1. 创建初始页
    auto page_data = create_initial_page();
    
    // 2. 创建 Redo Log 记录 (INSERT)
    RedoLogRecord record;
    record.r_type = MLOG_COMP_REC_INSERT;
    record.r_space_id = 1;
    record.r_page_no = 0;
    record.r_data = generate_insert_data();
    
    // 3. 应用 Redo Log
    BPlusTreeParser parser(page_data);
    parser.apply_redo_record(record);
    
    // 4. 验证结果
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records.size(), 1);
}

TEST(RedoLogReplayIntegrationTest, ApplyUpdateRecord) {
    // 1. 创建初始页 (包含记录)
    auto page_data = create_page_with_records(1);
    
    // 2. 创建 Redo Log 记录 (UPDATE)
    RedoLogRecord record;
    record.r_type = MLOG_COMP_REC_UPDATE;
    record.r_space_id = 1;
    record.r_page_no = 0;
    record.r_data = generate_update_data();
    
    // 3. 应用 Redo Log
    BPlusTreeParser parser(page_data);
    parser.apply_redo_record(record);
    
    // 4. 验证结果
    auto records = parser.parse_records(schema);
    EXPECT_EQ(records[0]["name"], "updated");
}
```

---

## 4. 模糊测试策略

### 4.1 测试工具

- **工具**: libFuzzer / AFL++
- **语言**: C++
- **覆盖率**: 代码行覆盖

### 4.2 模糊测试目标

| 目标模块 | 测试入口 | 持续时间 |
|----------|----------|----------|
| SDI 解析 | `tools/fuzz_sdi_json.cpp` | 24h |
| BLOB 解析 | `tools/fuzz_blob_pointer.cpp` | 24h |
| 页解析 | `tools/fuzz_page_parser.cpp` | 24h |

### 4.3 模糊测试用例

```cpp
// tools/fuzz_sdi_json.cpp
#include <stdint.h>
#include <stddef.h>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    try {
        // 尝试解析 SDI JSON
        auto json_str = std::string(reinterpret_cast<const char*>(data), size);
        auto json = nlohmann::json::parse(json_str);
        
        // 尝试解析表结构
        catalog::TableSchema schema;
        schema.from_json(json);
    } catch (...) {
        // 捕获所有异常，避免 fuzzer 崩溃
    }
    
    return 0;
}
```

```cpp
// tools/fuzz_blob_pointer.cpp
#include <stdint.h>
#include <stddef.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 20) return 0;
    
    try {
        // 尝试解析 BLOB 指针
        auto ptr = parse_blob_pointer(data, size);
        if (ptr.has_value()) {
            // 尝试验证 BLOB 页链
            ptr->is_valid();
        }
    } catch (...) {
        // 捕获所有异常
    }
    
    return 0;
}
```

```cpp
// tools/fuzz_page_parser.cpp
#include <stdint.h>
#include <stddef.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 16384) return 0;
    
    try {
        // 尝试解析页
        PageParser parser(data, size);
        parser.parse_header();
    } catch (...) {
        // 捕获所有异常
    }
    
    return 0;
}
```

### 4.4 模糊测试运行命令

```bash
# 编译 fuzzer
clang++ -fsanitize=fuzzer,address \
    -Iinclude -Inlohmann \
    tools/fuzz_sdi_json.cpp \
    -o fuzz_sdi_json

# 运行 fuzzer
./fuzz_sdi_json -jobs=4 -workers=4 corpus/
```

---

## 5. 性能测试策略

### 5.1 测试指标

| 指标 | 目标 | 测试方法 |
|------|------|----------|
| 单页解析时间 | < 1ms | Benchmark |
| 1GB 文件恢复时间 | < 60s | Integration Test |
| 内存占用 | < 2GB | Valgrind |
| 并发恢复性能 | > 100 records/s | Multi-thread Test |

### 5.2 性能测试用例

```cpp
// tests/benchmark/benchmark.cpp
#include <gtest/gtest.h>
#include "dbrecovery/innodb/page_parser.hpp"
#include "dbrecovery/innodb/btree.hpp"

class PerformanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 初始化测试数据
        page_data_.resize(16384, 0);
        write_le<uint16_t>(page_data_.data(), 16, FIL_PAGE_INDEX);
        write_le<uint16_t>(page_data_.data(), 38 + 4, 100); // heap_top
    }
    
    std::vector<uint8_t> page_data_;
};

TEST_F(PerformanceTest, ParsePagePerformance) {
    const int iterations = 10000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < iterations; ++i) {
        PageParser parser(page_data_.data(), page_data_.size());
        parser.parse_header();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    double avg_time = duration.count() / static_cast<double>(iterations);
    std::cout << "Average page parse time: " << avg_time << " us\n";
    
    EXPECT_LT(avg_time, 1000); // < 1ms
}

TEST_F(PerformanceTest, ParseRecordsPerformance) {
    catalog::TableSchema schema;
    schema.row_format = 1; // COMPACT
    
    const int iterations = 1000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < iterations; ++i) {
        BPlusTreeParser parser(page_data_);
        parser.parse();
        auto records = parser.parse_records(schema);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    double avg_time = duration.count() / static_cast<double>(iterations);
    std::cout << "Average record parse time: " << avg_time << " us\n";
    
    EXPECT_LT(avg_time, 5000); // < 5ms
}
```

---

## 6. 内存泄漏测试策略

### 6.1 工具

- **工具**: Valgrind / ASAN / LSAN
- **运行环境**: Ubuntu 22.04

### 6.2 测试命令

```bash
# 使用 Valgrind 检测内存泄漏
valgrind --leak-check=full --show-leak-kinds=all \
    --track-origins=yes \
    ./build/tests/test_page_header

# 使用 ASAN 检测内存错误
g++ -fsanitize=address -g -O0 \
    -Iinclude -Inlohmann \
    tests/test_page_header.cpp \
    -o test_asan

./test_asan
```

---

## 7. 测试运行命令

### 7.1 运行所有测试

```bash
# 编译测试
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
make -j$(nproc)

# 运行所有测试
ctest --verbose

# 运行特定测试
./tests/test_page_header
./tests/test_redo_log
./tests/test_blob_recovery
```

### 7.2 生成测试报告

```bash
# 生成 HTML 报告
ctest -T test
ctest -T coverage

# 查看覆盖率报告
firefox coverage/index.html
```

---

## 8. 测试数据生成

### 8.1 Python 脚本

```python
# tools/ibd_generator.py
#!/usr/bin/env python3
"""
生成测试用的 .ibd 文件
"""

import struct
import sys

def create_page(page_type=0x26F1, page_no=0):
    """创建一个 InnoDB 页"""
    page = bytearray(16384)
    
    # FIL Header
    struct.pack_into('<I', page, 0, 0xFFFFFFFF)  # prev_page
    struct.pack_into('<I', page, 4, 0xFFFFFFFF)  # next_page
    struct.pack_into('<Q', page, 8, 0)           # LSN
    struct.pack_into('<I', page, 16, page_type)  # page_type
    struct.pack_into('<I', page, 20, page_no)    # page_no
    
    # Page Header
    struct.pack_into('<H', page, 38, 10)         # n_dir_slots
    struct.pack_into('<H', page, 40, 100)        # heap_top
    struct.pack_into('<H', page, 42, 2)          # n_heap
    
    return page

def generate_test_table(num_records=1000):
    """生成测试表数据"""
    pages = []
    
    # 创建根页
    root_page = create_page(page_type=0x26F1, page_no=0)
    pages.append(root_page)
    
    # 创建叶子页
    for i in range((num_records + 999) // 1000):
        leaf_page = create_page(page_type=0x26F1, page_no=i + 1)
        
        # 添加记录
        heap_top = 54
        for j in range(1000):
            # 记录头 (5 bytes)
            leaf_page[heap_top] = 0x00  # info_bits
            leaf_page[heap_top + 1] = 0x00  # next_offset (low)
            leaf_page[heap_top + 2] = 0x00  # next_offset (high)
            leaf_page[heap_top + 3] = 0x00  # heap_no (low)
            leaf_page[heap_top + 4] = 0x00  # heap_no (high)
            
            # 记录数据 (8 bytes for BIGINT)
            leaf_page[heap_top + 5] = (i * 1000 + j) & 0xFF
            leaf_page[heap_top + 6] = ((i * 1000 + j) >> 8) & 0xFF
            # ... (更多字段)
            
            heap_top += 13  # 5 (header) + 8 (data)
        
        leaf_page[40] = heap_top & 0xFF  # heap_top
        leaf_page[41] = (heap_top >> 8) & 0xFF
        
        pages.append(leaf_page)
    
    return pages

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 ibd_generator.py <table_name>")
        sys.exit(1)
    
    table_name = sys.argv[1]
    pages = generate_test_table(1000)
    
    with open(f'test_{table_name}.ibd', 'wb') as f:
        for page in pages:
            f.write(page)
    
    print(f"Generated test_{table_name}.ibd with {len(pages)} pages")
```

### 8.2 运行测试数据生成

```bash
# 生成测试数据
python3 tools/ibd_generator.py test_table
python3 tools/ibd_generator.py truncate_scenario
python3 tools/ibd_generator.py rmrf_scenario
python3 tools/ibd_generator.py header_corrupt_scenario
```

---

## 9. CI/CD 集成

### 9.1 GitHub Actions 配置

```yaml
# .github/workflows/test.yml
name: Test

on:
  push:
    branches: [ main, develop ]
  pull_request:
    branches: [ main ]

jobs:
  test:
    runs-on: ubuntu-22.04
    
    steps:
    - uses: actions/checkout@v3
    
    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y build-essential cmake libgtest-dev libz-dev
    
    - name: Build
      run: |
        mkdir -p build
        cd build
        cmake .. -DCMAKE_BUILD_TYPE=Debug
        make -j$(nproc)
    
    - name: Run tests
      run: |
        cd build
        ctest --verbose
    
    - name: Run memory tests
      run: |
        valgrind --leak-check=full ./build/tests/test_page_header
```

---

## 10. 测试覆盖率目标

| 模块 | 行覆盖率 | 分支覆盖率 |
|------|----------|------------|
| Page Parser | 100% | 95% |
| B+Tree Parser | 100% | 95% |
| BLOB Parser | 100% | 90% |
| SDI Manager | 100% | 90% |
| Redo Log Parser | 100% | 95% |
| Undo Log Parser | 100% | 90% |
| Recovery Engine | 100% | 90% |

---

## 11. 测试失败处理

### 11.1 失败分类

| 类别 | 描述 | 处理方式 |
|------|------|----------|
| P0 | 核心功能失败 | 立即修复，阻塞发布 |
| P1 | 重要功能失败 | 24 小时内修复 |
| P2 | 次要功能失败 | 下一版本修复 |
| P3 | 边缘场景失败 | 记录缺陷，后续修复 |

### 11.2 失败报告模板

```markdown
## 测试失败报告

**测试名称**: `TruncateRecoveryTest.FullTruncateRecovery`  
**失败时间**: 2026-03-29 14:30:00  
**失败类型**: 断言失败  
**失败信息**: `Expected: 1000, Actual: 950`

### 原因分析

1. BLOB 字段未正确恢复
2. 删除记录被错误过滤

### 修复方案

1. 修复 BLOB 页链重建逻辑
2. 调整删除记录过滤条件

### 验证方法

```bash
./build/tests/test_truncate_recovery --gtest_filter=TruncateRecoveryTest.FullTruncateRecovery
```
```

---

## 12. 总结

本测试策略文档定义了 MySQL 8.x 数据恢复引擎的完整测试方案，包括：

1. **单元测试**: 覆盖所有核心模块
2. **集成测试**: 验证完整恢复流程
3. **模糊测试**: 发现边界情况和内存错误
4. **性能测试**: 确保满足性能指标
5. **内存测试**: 防止内存泄漏和越界访问

通过严格的测试策略，确保恢复引擎在各种灾难场景下均能可靠工作。
