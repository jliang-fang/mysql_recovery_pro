# MySQL 8.0 数据恢复引擎 - 静态与动态分析指南

## 1. 静态分析工具配置

### 1.1 clang-tidy 配置

项目已移除有问题的 `.clang-tidy` 配置。建议使用以下配置：

```yaml
# .clang-tidy
Checks: >
    -*
    ,clang-analyzer-*
    ,performance-*
    ,readability-*
    ,bugprone-*
    ,cert-*
    ,misc-*
CheckOptions:
    bugprone-easily-swappable-parameters:
        ParameterNames: "buffer,size"
    readability-identifier-naming:
        CaseStyle: camelBack
    modernize-loop-convert:
        UseAuto: true
```

### 1.2 cppcheck 配置

```bash
# 运行 cppcheck
cppcheck --enable=all --inconclusive --std=c++20 \
    --include=include/ \
    --suppress=missingIncludeSystem \
    src/ include/
```

### 1.3 常见静态分析警告及修复

| 警告类型 | 严重性 | 修复建议 |
|---------|--------|---------|
| `bugprone-easily-swappable-parameters` | P1 | 使用命名参数或添加参数名注释 |
| `clang-analyzer-core.NullDereference` | P0 | 添加空指针检查 |
| `performance-inefficient-vector-operation` | P2 | 预分配 vector 空间 |
| `readability-identifier-naming` | P3 | 遵循项目命名规范 |

## 2. 动态分析工具配置

### 2.1 AddressSanitizer (ASAN)

```bash
# 配置构建
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_ASAN=ON \
      -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer" \
      ..

# 运行测试
ctest --output-on-failure

# 运行特定测试
./build/tests/test_page_header
```

### 2.2 UndefinedBehaviorSanitizer (UBSAN)

```bash
# 配置构建
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_UBSAN=ON \
      -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer" \
      ..

# 运行测试
ctest --output-on-failure
```

### 2.3 LeakSanitizer (LSAN)

```bash
# 配置构建
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_LSAN=ON \
      -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer" \
      ..

# 运行测试
LSAN_OPTIONS=verbosity=1:log_threads=1 ctest --output-on-failure
```

### 2.4 组合使用多个 Sanitizer

```bash
# ASAN + UBSAN 组合
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_ASAN=ON \
      -DENABLE_UBSAN=ON \
      -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer" \
      ..

# 运行测试
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --output-on-failure
```

## 3. 性能分析工具

### 3.1 gprof

```bash
# 编译时添加 -pg 选项
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-pg" \
      -DCMAKE_EXE_LINKER_FLAGS="-pg" \
      ..

# 运行程序
./build/mysql_recovery_pro --ibd=test.ibd

# 生成分析报告
gprof ./build/mysql_recovery_pro gmon.out > profile.txt
```

### 3.2 perf

```bash
# 记录性能数据
perf record -g ./build/mysql_recovery_pro --ibd=test.ibd

# 查看报告
perf report

# 生成火焰图
perf script | stackcollapse-perf.pl | flamegraph.pl > profile.svg
```

### 3.3 valgrind

```bash
# 内存错误检测
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         ./build/mysql_recovery_pro --ibd=test.ibd

# 数据竞争检测（需要编译时添加 -fsanitize=thread）
valgrind --tool=helgrind ./build/mysql_recovery_pro --ibd=test.ibd
```

## 4. 集成到 CI/CD

### 4.1 GitHub Actions 配置示例

```yaml
# .github/workflows/analysis.yml
name: Static and Dynamic Analysis

on: [pull_request]

jobs:
  clang-tidy:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Run clang-tidy
        run: |
          sudo apt-get install clang-tidy
          clang-tidy -p build src/**/*.cpp --checks='clang-analyzer-*,bugprone-*'

  asan:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Configure with ASAN
        run: |
          cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON ..
      - name: Build
        run: make -j$(nproc)
      - name: Run tests
        run: ctest --output-on-failure

  coverage:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Configure with coverage
        run: |
          cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON ..
      - name: Build
        run: make -j$(nproc)
      - name: Run tests
        run: ctest --output-on-failure
      - name: Generate coverage report
        run: |
          lcov --capture --directory . --output-file coverage.info
          genhtml coverage.info --output-directory coverage_report
      - name: Upload coverage
        uses: actions/upload-artifact@v3
        with:
          name: coverage-report
          path: coverage_report
```

## 5. 常见问题修复

### 5.1 ASAN 报告示例

```
==12345==ERROR: AddressSanitizer: heap-use-after-free on address 0x602000000010
    at main.cpp:42 in main()
    ...
```

**修复方法**：
1. 检查指针生命周期
2. 使用智能指针（unique_ptr/shared_ptr）
3. 确保对象在使用前已初始化

### 5.2 UBSAN 报告示例

```
==12345==ERROR: UndefinedBehaviorSanitizer: signed-integer-overflow
    src/innodb/mach_read.hpp:45:5 in read_le()
```

**修复方法**：
1. 使用无符号整数进行位运算
2. 添加边界检查
3. 使用 `std::optional` 处理可能失败的操作

### 5.3 内存泄漏示例

```
==12345==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 1024 byte(s) in 1 object(s)
    at malloc (asan_malloc.cpp:145)
    ...
```

**修复方法**：
1. 使用 RAII 模式管理资源
2. 确保所有路径都正确释放资源
3. 使用 `std::unique_ptr` 和 `std::shared_ptr`

## 6. 性能优化建议

### 6.1 内存分配优化

```cpp
// 优化前
std::vector<uint8_t> data;
for (int i = 0; i < 1000; ++i) {
    data.push_back(value);
}

// 优化后
std::vector<uint8_t> data;
data.reserve(1000); // 预分配空间
for (int i = 0; i < 1000; ++i) {
    data.push_back(value);
}
```

### 6.2 字符串处理优化

```cpp
// 优化前
std::string result;
for (const auto& s : strings) {
    result += s;
}

// 优化后
std::string result;
result.reserve(total_size);
for (const auto& s : strings) {
    result += s;
}
```

### 6.3 循环优化

```cpp
// 优化前
for (size_t i = 0; i < vec.size(); ++i) {
    // ...
}

// 优化后
for (size_t i = 0, n = vec.size(); i < n; ++i) {
    // ...
}
```

## 7. 测试覆盖率要求

| 模块 | 单元测试覆盖率 | 集成测试覆盖率 | 模糊测试覆盖 |
|------|---------------|---------------|-------------|
| PageParser | ≥95% | ≥90% | ≥80% |
| BPlusTreeParser | ≥95% | ≥90% | ≥80% |
| SDIManager | ≥90% | ≥85% | ≥90% |
| RedoLogParser | ≥90% | ≥85% | ≥80% |
| BlobPointer | ≥95% | ≥90% | ≥85% |

## 8. 静态分析检查项清单

- [ ] 所有指针在使用前进行空指针检查
- [ ] 所有数组访问进行边界检查
- [ ] 所有除法操作检查除数是否为零
- [ ] 所有整数运算检查溢出
- [ ] 所有文件操作检查返回值
- [ ] 所有内存分配检查返回值
- [ ] 所有线程操作使用同步原语
- [ ] 所有资源使用 RAII 管理
- [ ] 所有异常在接口边界被捕获
- [ ] 所有全局变量使用线程安全初始化

## 9. 动态分析检查项清单

- [ ] 所有测试在 ASAN 模式下通过
- [ ] 所有测试在 UBSAN 模式下通过
- [ ] 所有测试在 LSAN 模式下无内存泄漏
- [ ] 所有测试在 Valgrind 下无错误
- [ ] 性能测试在 perf 下分析热点
- [ ] 压力测试在多线程下无数据竞争
