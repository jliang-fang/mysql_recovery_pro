# MySQL 8.0 Data Recovery Engine - Static and Dynamic Analysis Guide

## 1. Static Analysis Tool Configuration

### 1.1 clang-tidy Configuration

The project has removed the problematic `.clang-tidy` configuration. Use the following configuration:

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

### 1.2 cppcheck Configuration

```bash
# Run cppcheck
cppcheck --enable=all --inconclusive --std=c++20 \
    --include=include/ \
    --suppress=missingIncludeSystem \
    src/ include/
```

### 1.3 Common Static Analysis Warnings and Fixes

| Warning Type | Severity | Fix Recommendation |
|-------------|----------|-------------------|
| `bugprone-easily-swappable-parameters` | P1 | Use named parameters or add parameter name comments |
| `clang-analyzer-core.NullDereference` | P0 | Add null pointer checks |
| `performance-inefficient-vector-operation` | P2 | Pre-allocate vector space |
| `readability-identifier-naming` | P3 | Follow project naming conventions |

## 2. Dynamic Analysis Tool Configuration

### 2.1 AddressSanitizer (ASAN)

```bash
# Configure build
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_ASAN=ON \
      -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer" \
      ..

# Run tests
ctest --output-on-failure

# Run specific test
./build/tests/test_page_header
```

### 2.2 UndefinedBehaviorSanitizer (UBSAN)

```bash
# Configure build
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_UBSAN=ON \
      -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer" \
      ..

# Run tests
ctest --output-on-failure
```

### 2.3 LeakSanitizer (LSAN)

```bash
# Configure build
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_LSAN=ON \
      -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer" \
      ..

# Run tests
LSAN_OPTIONS=verbosity=1:log_threads=1 ctest --output-on-failure
```

### 2.4 Combined Sanitizer Usage

```bash
# ASAN + UBSAN combination
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_ASAN=ON \
      -DENABLE_UBSAN=ON \
      -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer" \
      ..

# Run tests
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --output-on-failure
```

## 3. Performance Analysis Tools

### 3.1 gprof

```bash
# Compile with -pg option
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-pg" \
      -DCMAKE_EXE_LINKER_FLAGS="-pg" \
      ..

# Run program
./build/mysql_recovery_pro --ibd=test.ibd

# Generate profile report
gprof ./build/mysql_recovery_pro gmon.out > profile.txt
```

### 3.2 perf

```bash
# Record performance data
perf record -g ./build/mysql_recovery_pro --ibd=test.ibd

# View report
perf report

# Generate flame graph
perf script | stackcollapse-perf.pl | flamegraph.pl > profile.svg
```

### 3.3 valgrind

```bash
# Memory error detection
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         ./build/mysql_recovery_pro --ibd=test.ibd

# Data race detection (requires compilation with -fsanitize=thread)
valgrind --tool=helgrind ./build/mysql_recovery_pro --ibd=test.ibd
```

## 4. CI/CD Integration

### 4.1 GitHub Actions Configuration Example

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

## 5. Common Issue Fixes

### 5.1 ASAN Report Example

```
==12345==ERROR: AddressSanitizer: heap-use-after-free on address 0x602000000010
    at main.cpp:42 in main()
    ...
```

**Fix Method**:
1. Check pointer lifecycle
2. Use smart pointers (unique_ptr/shared_ptr)
3. Ensure objects are initialized before use

### 5.2 UBSAN Report Example

```
==12345==ERROR: UndefinedBehaviorSanitizer: signed-integer-overflow
    src/innodb/mach_read.hpp:45:5 in read_le()
```

**Fix Method**:
1. Use unsigned integers for bitwise operations
2. Add boundary checks
3. Use `std::optional` for operations that may fail

### 5.3 Memory Leak Example

```
==12345==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 1024 byte(s) in 1 object(s)
    at malloc (asan_malloc.cpp:145)
    ...
```

**Fix Method**:
1. Use RAII pattern for resource management
2. Ensure all paths properly release resources
3. Use `std::unique_ptr` and `std::shared_ptr`

## 6. Performance Optimization Recommendations

### 6.1 Memory Allocation Optimization

```cpp
// Before optimization
std::vector<uint8_t> data;
for (int i = 0; i < 1000; ++i) {
    data.push_back(value);
}

// After optimization
std::vector<uint8_t> data;
data.reserve(1000); // Pre-allocate space
for (int i = 0; i < 1000; ++i) {
    data.push_back(value);
}
```

### 6.2 String Processing Optimization

```cpp
// Before optimization
std::string result;
for (const auto& s : strings) {
    result += s;
}

// After optimization
std::string result;
result.reserve(total_size);
for (const auto& s : strings) {
    result += s;
}
```

### 6.3 Loop Optimization

```cpp
// Before optimization
for (size_t i = 0; i < vec.size(); ++i) {
    // ...
}

// After optimization
for (size_t i = 0, n = vec.size(); i < n; ++i) {
    // ...
}
```

## 7. Test Coverage Requirements

| Module | Unit Test Coverage | Integration Test Coverage | Fuzz Test Coverage |
|--------|-------------------|--------------------------|-------------------|
| PageParser | ≥95% | ≥90% | ≥80% |
| BPlusTreeParser | ≥95% | ≥90% | ≥80% |
| SDIManager | ≥90% | ≥85% | ≥90% |
| RedoLogParser | ≥90% | ≥85% | ≥80% |
| BlobPointer | ≥95% | ≥90% | ≥85% |

## 8. Static Analysis Checklist

- [ ] All pointers are checked for null before use
- [ ] All array accesses are bounds-checked
- [ ] All division operations check for zero divisor
- [ ] All integer operations check for overflow
- [ ] All file operations check return values
- [ ] All memory allocations check return values
- [ ] All thread operations use synchronization primitives
- [ ] All resources are managed with RAII
- [ ] All exceptions are caught at interface boundaries
- [ ] All global variables use thread-safe initialization

## 9. Dynamic Analysis Checklist

- [ ] All tests pass in ASAN mode
- [ ] All tests pass in UBSAN mode
- [ ] All tests have no memory leaks in LSAN mode
- [ ] All tests pass Valgrind without errors
- [ ] Performance tests analyze hotspots with perf
- [ ] Stress tests have no data races in multi-threaded mode
