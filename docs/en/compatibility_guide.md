# MySQL Recovery Pro - 兼容性指南

## 目录

1. [概述](#概述)
2. [MySQL版本兼容性](#mysql版本兼容性)
3. [跨平台兼容性](#跨平台兼容性)
4. [CPU架构兼容性](#cpu架构兼容性)
5. [构建系统](#构建系统)
6. [测试](#测试)
7. [API参考](#api参考)

## 概述

MySQL Recovery Pro 支持多种MySQL版本、操作系统和CPU架构。本指南详细介绍如何使用这些兼容性功能。

## MySQL版本兼容性

### 支持的版本

| 版本 | 代码名称 | 发布日期 | 特性 |
|------|----------|----------|------|
| 5.0 | Neon | 2005-10-19 | 基础InnoDB |
| 5.1 | Chimera | 2008-11-14 | 压缩页 |
| 5.5 | Sapphire | 2010-12-03 | 新校验和 |
| 5.6 | Drizzle | 2013-02-05 | COMPACT/DYNAMIC |
| 5.7 | Invisible | 2015-10-21 | Online DDL |
| 8.0 | Silent | 2018-04-19 | SDI, LSN扩展 |
| 8.1 | LTS | 2023-10-12 | |
| 8.2 | Custom | 2024-01-18 | |
| 8.3 | Custom | 2024-04-24 | |
| 8.4 | LTS | 2024-04-24 | 加密, 临时页 |
| 9.0 | Future | 2025-01-01 | 增强校验和 |

### 版本检测

#### 快速检测

```cpp
#include "dbrecovery/common/mysql_version_detector.hpp"

// 从页数据检测版本
MySQLVersion version = quick_detect_version(page_data, page_size);
std::cout << "Version: " << get_mysql_version_string(version) << std::endl;
```

#### 详细检测

```cpp
#include "dbrecovery/common/mysql_version_detector_enhanced.hpp"

VersionDetectionResult result = detailed_detect_version_enhanced(page_data, page_size);
std::cout << "Version: " << get_mysql_version_string(result.version) << std::endl;
std::cout << "Confidence: " << result.confidence << "%" << std::endl;
std::cout << "Features: ";
for (const auto& feature : result.matching_features) {
    std::cout << feature << " ";
}
```

#### 多页检测

```cpp
std::vector<const uint8_t*> pages = {page1, page2, page3};
MySQLVersion version = detect_version_from_multiple_pages_enhanced(pages, page_size);
```

### 版本信息

```cpp
#include "dbrecovery/common/mysql_version_detector_enhanced.hpp"

auto& factory = EnhancedVersionDetectionFactory::instance();
const auto& info = factory.get_version_info(MySQLVersion::MYSQL_80);

std::cout << "Version: " << info.name << std::endl;
std::cout << "Codename: " << info.codename << std::endl;
std::cout << "Release: " << info.release_date << std::endl;
std::cout << "Default page size: " << info.default_page_size << std::endl;
std::cout << "Supports SDI: " << (info.has_sdi ? "YES" : "NO") << std::endl;
std::cout << "Supports encryption: " << (info.supports_encryption ? "YES" : "NO") << std::endl;
```

### 版本系列

```cpp
auto& factory = EnhancedVersionDetectionFactory::instance();
std::string series = get_version_series_enhanced(MySQLVersion::MYSQL_80);
// 返回 "MySQL 8.x"
```

### 版本兼容性

```cpp
auto& factory = EnhancedVersionDetectionFactory::instance();
auto& matrix = factory.get_compatibility_matrix();

// 检查两个版本是否兼容
bool compatible = matrix.are_compatible(MySQLVersion::MYSQL_80, MySQLVersion::MYSQL_81);

// 检查是否可以升级
bool can_upgrade = matrix.can_upgrade_to(MySQLVersion::MYSQL_80, MySQLVersion::MYSQL_84);

// 获取升级路径
auto path = matrix.get_upgrade_path(MySQLVersion::MYSQL_80, MySQLVersion::MYSQL_84);
// 返回: [8.0, 8.1, 8.2, 8.3, 8.4]
```

## 跨平台兼容性

### 支持的平台

| 平台 | 架构 | 状态 |
|------|------|------|
| Linux | x86_64, ARM64 | 完全支持 |
| macOS | x86_64, ARM64 | 完全支持 |
| Windows | x86_64, ARM64 | 完全支持 |
| FreeBSD | x86_64 | 完全支持 |
| OpenBSD | x86_64 | 完全支持 |

### 平台检测

```cpp
#include "dbrecovery/common/platform_detection.hpp"

auto& detector = PlatformDetector::instance();

std::cout << "Platform: " << detector.get_platform_name() << std::endl;
std::cout << "Architecture: " << detector.get_arch_name() << std::endl;
std::cout << "Compiler: " << detector.get_compiler_name() << std::endl;
std::cout << "C++ Standard: " << detector.get_cpp_standard() << std::endl;

// 平台检测函数
if (detector.is_linux()) {
    // Linux特定代码
} else if (detector.is_macos()) {
    // macOS特定代码
} else if (detector.is_windows()) {
    // Windows特定代码
}

// 架构检测函数
if (detector.is_x86_64()) {
    // x86_64特定代码
} else if (detector.is_arm64()) {
    // ARM64特定代码
}
```

### 文件系统操作

```cpp
#include "dbrecovery/common/environment_compat.hpp"

auto& fs = FileSystem::instance();

// 检查文件是否存在
auto result = fs.file_exists("/path/to/file.ibd");
if (result.is_success()) {
    std::cout << "File exists" << std::endl;
}

// 获取文件大小
uint64_t size = 0;
fs.file_size("/path/to/file.ibd", size);
std::cout << "Size: " << size << " bytes" << std::endl;

// 复制文件
fs.file_copy("/path/to/source.ibd", "/path/to/dest.ibd");

// 移动文件
fs.file_move("/path/to/source.ibd", "/path/to/dest.ibd");

// 删除文件
fs.file_delete("/path/to/file.ibd");

// 创建目录
fs.directory_create("/path/to/dir", true);  // recursive

// 列出目录
std::vector<std::string> files;
fs.directory_list("/path/to/dir", files);
```

### 内存管理

```cpp
#include "dbrecovery/common/environment_compat.hpp"

auto& mm = MemoryManager::instance();

// 分配内存
void* ptr = mm.allocate(1024);

// 对齐分配
void* aligned_ptr = mm.aligned_allocate(1024, 64);

// 复制内存
mm.copy_memory(dest, src, size);

// 清零内存
mm.zero_memory(ptr, size);

// 统计信息
std::cout << "Total allocated: " << mm.total_allocated() << std::endl;
std::cout << "Peak allocated: " << mm.peak_allocated() << std::endl;
std::cout << "Allocation count: " << mm.allocation_count() << std::endl;
```

### 线程管理

```cpp
#include "dbrecovery/common/environment_compat.hpp"

auto& tm = ThreadManager::instance();

// 获取CPU核心数
uint32_t cores = tm.cpu_cores();

// 创建线程
auto handle = tm.create_thread([]() {
    // 线程函数
});
tm.join_thread(handle);

// 创建线程池
auto pool = tm.create_thread_pool(8);
pool->enqueue([]() {
    // 任务函数
});
pool->wait_all();

// 睡眠
tm.sleep_ms(100);
tm.sleep_us(1000);

// 设置线程优先级
tm.set_thread_priority(0);  // -2 to 2
```

### 时间管理

```cpp
#include "dbrecovery/common/environment_compat.hpp"

auto& tm = TimeManager::instance();

// 获取时间戳
uint64_t ms = tm.timestamp_ms();
uint64_t us = tm.timestamp_us();
uint64_t ns = tm.timestamp_ns();

// 高精度计时
uint64_t counter = tm.high_res_counter();
uint64_t frequency = tm.high_res_frequency();

// 格式化时间
auto now = tm.now();
std::string formatted = tm.format_time(now);
```

### 系统信息

```cpp
#include "dbrecovery/common/environment_compat.hpp"

auto& sysinfo = SystemInfoGetter::instance();

std::cout << "OS: " << sysinfo.os_name() << std::endl;
std::cout << "OS Version: " << sysinfo.os_version() << std::endl;
std::cout << "Hostname: " << sysinfo.hostname() << std::endl;
std::cout << "CPU Cores: " << sysinfo.cpu_cores() << std::endl;
std::cout << "Total Memory: " << sysinfo.total_memory() / 1024 / 1024 << " MB" << std::endl;
std::cout << "Available Memory: " << sysinfo.available_memory() / 1024 / 1024 << " MB" << std::endl;
```

## CPU架构兼容性

### 支持的架构

| 架构 | SIMD指令集 | 特性 |
|------|------------|------|
| x86_64 | SSE2, SSE4.2, AVX2, AVX-512 | CRC32, AES-NI |
| ARM64 | NEON | CRC32, AES, PMULL, SHA |

### CPU特性检测

```cpp
#include "dbrecovery/common/cpu_optimizations.hpp"

auto& caps = CPUCapabilities::instance();

// x86_64特性
std::cout << "SSE4.2: " << (caps.has_sse42() ? "YES" : "NO") << std::endl;
std::cout << "AVX2: " << (caps.has_avx2() ? "YES" : "NO") << std::endl;
std::cout << "AVX-512: " << (caps.has_avx512() ? "YES" : "NO") << std::endl;
std::cout << "PCLMULQDQ: " << (caps.has_pclmulqdq() ? "YES" : "NO") << std::endl;
std::cout << "AES-NI: " << (caps.has_aesni() ? "YES" : "NO") << std::endl;
std::cout << "CRC32: " << (caps.has_crc32() ? "YES" : "NO") << std::endl;

// ARM64特性
std::cout << "NEON: " << (caps.has_neon() ? "YES" : "NO") << std::endl;
std::cout << "CRC32: " << (caps.has_crc32_arm() ? "YES" : "NO") << std::endl;
std::cout << "AES: " << (caps.has_aes() ? "YES" : "NO") << std::endl;
std::cout << "PMULL: " << (caps.has_pmull() ? "YES" : "NO") << std::endl;
std::cout << "SHA: " << (caps.has_sha() ? "YES" : "NO") << std::endl;
```

### CRC32计算

```cpp
#include "dbrecovery/common/cpu_optimizations.hpp"

// 自动选择最优实现
uint32_t crc = calculate_crc32(data, size);

// 使用硬件CRC32（如果可用）
uint32_t hardware_crc = CRC32Optimizer::calculate(data, size, initial_crc);
```

### 内存操作

```cpp
#include "dbrecovery/common/cpu_optimizations.hpp"

// 快速内存复制
fast_memcpy(dest, src, size);

// 对齐内存分配
auto ptr = aligned_allocate<uint8_t>(1024);
// 自动选择64字节对齐（AVX-512）
// 或32字节对齐（AVX2）
// 或16字节对齐（SSE2/NEON）
```

### SIMD操作

```cpp
#include "dbrecovery/common/simd_operations.hpp"

// 检测SIMD架构
std::cout << "SIMD Architecture: " << get_arch_name() << std::endl;
std::cout << "Has SIMD: " << (has_simd() ? "YES" : "NO") << std::endl;

// 快速复制
fast_copy_16(dest, src);
fast_copy_32(dest, src);
fast_copy_64(dest, src);

// 快速清零
fast_zero_16(dest);
fast_zero_32(dest);
fast_zero_64(dest);

// 任意大小复制
fast_memcpy(dest, src, size);
fast_memset_zero(dest, size);
```

## 构建系统

### 基本构建

```bash
# 克隆仓库
git clone <repository-url>
cd mysql_recovery_pro

# 创建构建目录
mkdir build && cd build

# 配置
cmake .. -DCMAKE_BUILD_TYPE=Release

# 构建
make -j$(nproc)
```

### 跨平台构建

#### Linux x86_64

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64
```

#### Linux ARM64

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
```

#### macOS ARM64 (Apple Silicon)

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Darwin \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64
```

#### Windows x86_64

```bash
cmake .. -G "Visual Studio 17 2022" -A x64
```

### 静态链接

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DSTATIC_LINK=ON
```

### 启用特定优化

```bash
# 启用AVX2
cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-mavx2"

# 启用NEON
cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-mfpu=neon"
```

### 使用工具链文件

```bash
# ARM64工具链
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-arm64.cmake

# x86_64工具链
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-x86_64.cmake
```

## 测试

### 运行所有测试

```bash
cd build
make test
```

### 运行特定测试

```bash
# MySQL版本兼容性测试
./test_multi_version_compat

# 环境兼容性测试
./test_multi_environment_compat

# CPU优化测试
./test_cpu_optimizations

# 平台检测测试
./test_platform_detection

# SIMD操作测试
./test_simd_operations
```

### 编写测试

```cpp
#include "dbrecovery/common/mysql_version_detector.hpp"
#include "dbrecovery/common/environment_compat.hpp"
#include "dbrecovery/common/cpu_optimizations.hpp"
#include <gtest/gtest.h>

TEST(MySQLVersionTest, DetectVersion) {
    auto version = quick_detect_version(page_data, page_size);
    EXPECT_EQ(version, MySQLVersion::MYSQL_80);
}

TEST(EnvironmentTest, FileSystem) {
    auto& fs = FileSystem::instance();
    auto result = fs.file_exists("/path/to/file");
    EXPECT_TRUE(result.is_success());
}

TEST(CPUOptimizationTest, CRC32) {
    uint32_t crc = calculate_crc32(data, size);
    EXPECT_NE(crc, 0);
}
```

## API参考

### MySQL版本检测

| 函数 | 描述 |
|------|------|
| `quick_detect_version()` | 快速版本检测 |
| `detailed_detect_version()` | 详细版本检测 |
| `detect_version_from_multiple_pages()` | 多页版本检测 |
| `get_version_info()` | 获取版本信息 |
| `get_version_series()` | 获取版本系列 |
| `get_version_codename()` | 获取代码名称 |
| `get_version_release_date()` | 获取发布日期 |

### 平台检测

| 函数 | 描述 |
|------|------|
| `get_platform_info()` | 获取平台信息 |
| `get_platform_name()` | 获取平台名称 |
| `get_arch_name()` | 获取架构名称 |
| `get_compiler_name()` | 获取编译器名称 |
| `is_windows()` | 检查是否为Windows |
| `is_macos()` | 检查是否为macOS |
| `is_linux()` | 检查是否为Linux |
| `is_x86_64()` | 检查是否为x86_64 |
| `is_arm64()` | 检查是否为ARM64 |

### 环境兼容性

| 类 | 描述 |
|----|------|
| `FileSystem` | 文件系统操作 |
| `MemoryManager` | 内存管理 |
| `ThreadManager` | 线程管理 |
| `TimeManager` | 时间管理 |
| `SystemInfoGetter` | 系统信息 |

### CPU优化

| 类 | 描述 |
|----|------|
| `CPUCapabilities` | CPU特性检测 |
| `CRC32Optimizer` | CRC32优化 |
| `MemoryOptimizer` | 内存操作优化 |
| `AlignedAllocator` | 对齐内存分配 |
| `VectorizedOperations` | SIMD操作 |

## 最佳实践

### 1. 版本检测

始终使用详细版本检测来获取最大置信度：

```cpp
auto result = detailed_detect_version_enhanced(page_data, page_size);
if (result.confidence > 80) {
    // 高置信度，可以使用
}
```

### 2. 平台检测

使用平台检测来编写跨平台代码：

```cpp
#ifdef DETECT_PLATFORM_WINDOWS
    // Windows特定代码
#else
    // Unix特定代码
#endif
```

### 3. CPU优化

使用统一接口自动选择最优实现：

```cpp
// 自动选择最优CRC32实现
uint32_t crc = calculate_crc32(data, size);

// 自动选择最优内存复制
fast_memcpy(dest, src, size);
```

### 4. 内存对齐

使用对齐分配以获得最佳性能：

```cpp
auto ptr = aligned_allocate<uint8_t>(1024);
// 自动选择64字节对齐（AVX-512）
```

## 故障排除

### 问题：版本检测不准确

**解决方案**: 使用多页检测提高准确性：

```cpp
std::vector<const uint8_t*> pages = {page1, page2, page3, page4};
MySQLVersion version = detect_version_from_multiple_pages_enhanced(pages, page_size);
```

### 问题：跨平台编译失败

**解决方案**: 使用工具链文件：

```bash
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-arm64.cmake
```

### 问题：SIMD指令不可用

**解决方案**: 代码会自动回退到参考实现，性能可能降低。

## 相关文档

- [架构指南](docs/en/architecture.md)
- [用户指南](docs/en/user_guide.md)
- [开发者指南](docs/en/developer_guide.md)
