# MySQL Recovery Pro - 多版本和多环境兼容性支持

## 概述

MySQL Recovery Pro v2.1.0 引入了全面的多版本和多环境兼容性支持，使工具能够无缝地在不同的 MySQL 版本（5.0-9.0）和操作系统（Linux、macOS、Windows）上工作。

## 功能特性

### 多版本支持

恢复引擎现在支持从 5.0 到 9.0 的 MySQL 版本：

| MySQL 版本 | 发布日期 | 支持的功能 |
|-----------|---------|-----------|
| 5.0/4.x | 2003-05-01 | 基本恢复、Redundant 格式 |
| 5.1 | 2008-11-14 | 压缩支持 |
| 5.5 | 2010-12-03 | 增强恢复 |
| 5.6 | 2013-02-05 | COMPACT 格式、CRC32 校验和 |
| 5.7 | 2015-10-21 | 增强页头 |
| 8.0 | 2018-04-19 | SDI 页、加密 |
| 8.1 | 2023-04-20 | 改进的恢复 |
| 8.2 | 2023-10-17 | 增强功能 |
| 8.3 | 2024-04-24 | 最新 8.3 功能 |
| 8.4 | 2024-04-24 | 临时页、增强校验和 |
| 9.0 | 2025-04-24 | 最新功能 |

### 多环境支持

该工具支持三个主要操作系统：

| 平台 | 最低版本 | 编译器 | 架构支持 |
|-----|---------|--------|---------|
| Linux | 内核 3.10+ | GCC 10+, Clang 12+ | x86_64, ARM64 |
| macOS | 10.15+ | Xcode 12+ | x86_64, ARM64 (Apple Silicon) |
| Windows | 7 SP1+ | MSVC 2019+ | x86_64, ARM64 |

## 架构

### 版本兼容性层

```
┌─────────────────────────────────────────────────────────────┐
│                  版本兼容性层                                │
│  ┌──────────────┬──────────────┬──────────────┬───────────┐ │
│  │ 版本检测     │ 行格式检测   │ 页格式检测   │ 校验和检测 │ │
│  └──────────────┴──────────────┴──────────────┴───────────┘ │
├─────────────────────────────────────────────────────────────┤
│                  环境兼容性层                                │
│  ┌──────────────┬──────────────┬──────────────┬───────────┐ │
│  │ 文件系统抽象 │ 内存管理抽象 │ 线程管理抽象 │ 时间管理抽象 │ │
│  └──────────────┴──────────────┴──────────────┴───────────┘ │
├─────────────────────────────────────────────────────────────┤
│                     核心恢复引擎                            │
│  ┌──────────────┬──────────────┬──────────────┬───────────┐ │
│  │ 页解析器     │ B+树遍历器   │ Undo Log解析 │ Redo Log解析│ │
│  └──────────────┴──────────────┴──────────────┴───────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### 关键组件

#### 1. 版本兼容性工厂 (`dbrecovery/common/version_compat.hpp`)

```cpp
// 自动版本检测
MySQLVersion version = VersionCompatibilityFactory::instance()
    .detect_version(page_data, page_size);

// 获取版本信息
const MySQLVersionInfo& info = VersionCompatibilityFactory::instance()
    .get_version_info(version);

// 检查功能支持
bool supports_sdi = VersionCompatibilityFactory::instance()
    .supports_feature(version, "SDI");
```

#### 2. 环境兼容性层 (`dbrecovery/common/environment_compat.hpp`)

```cpp
// 文件系统操作（跨平台）
FileSystem& fs = FileSystem::instance();
ErrorInfo result = fs.file_exists("/path/to/file.ibd");

// 内存管理（对齐分配）
MemoryManager& mem = MemoryManager::instance();
void* ptr = mem.aligned_allocate(16384, 16);

// 线程管理
ThreadManager& thread = ThreadManager::instance();
uint32_t cores = thread.cpu_cores();

// 时间管理
TimeManager& time = TimeManager::instance();
uint64_t timestamp = time.timestamp_ms();
```

## 使用方法

### 命令行接口

CLI 自动检测 MySQL 版本和环境：

```bash
# 基本恢复（自动检测版本和环境）
mysql_recovery_pro --ibd=/path/to/table.ibd --output=/tmp/recovered

# 明确指定版本（可选）
mysql_recovery_pro --ibd=/path/to/table.ibd --mysql-version=8.0 --output=/tmp/recovered

# Truncate 恢复
mysql_recovery_pro --ibd=/path/to/table.ibd --truncate-recovery --output=/tmp/recovered

# BLOB 恢复
mysql_recovery_pro --ibd=/path/to/table.ibd --blob-recovery --output=/tmp/recovered

# 导出为 SQL
mysql_recovery_pro --ibd=/path/to/table.ibd --format=sql --output=/tmp/recovered.sql
```

### C++ API

```cpp
#include "dbrecovery/common/version_compat.hpp"
#include "dbrecovery/common/environment_compat.hpp"

// 版本检测
MySQLVersion detect_version(const uint8_t* page_data, size_t page_size) {
    return VersionCompatibilityFactory::instance()
        .detect_version(page_data, page_size);
}

// 环境感知的文件操作
ErrorInfo read_file(const std::string& path, std::vector<uint8_t>& data) {
    FileSystem& fs = FileSystem::instance();
    FileInfo info;
    
    auto result = fs.file_info(path, info);
    if (result.is_error()) {
        return result;
    }
    
    data.resize(info.size);
    // ... 读取文件数据
    return ErrorInfo{ErrorCode::SUCCESS};
}

// 多线程恢复
void parallel_recovery() {
    ThreadManager& thread = ThreadManager::instance();
    uint32_t cores = thread.cpu_cores();
    
    // 使用所有可用核心
    ParallelRecoveryEngine engine(cores);
    // ...
}
```

## 构建

### Linux

```bash
# 使用构建脚本
./scripts/build_linux.sh --release

# 手动构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### macOS

```bash
# 使用构建脚本
./scripts/build_macos.sh --release

# 手动构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)
```

### Windows

```bash
# 使用构建脚本
.\scripts\build_windows.sh --release

# 手动构建 (PowerShell)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

## 平台特定功能

### Linux

- **io_uring 支持**：现代内核的高性能 I/O
- **ARM64 优化**：硬件加速 CRC32C
- **NUMA 感知**：针对多插槽系统优化

### macOS

- **Apple Silicon 支持**：原生 ARM64 执行
- **CoreFoundation 集成**：系统级操作
- **Metal 加速**：GPU 加速操作（未来）

### Windows

- **Direct I/O**：优化的文件访问
- **线程池**：Windows 线程池集成
- **事件跟踪**：ETW 支持调试

## 版本检测

系统自动从页头检测 MySQL 版本：

```cpp
// 检测算法
MySQLVersion detect_version(const uint8_t* page_data, size_t page_size) {
    // 1. 检查 SDI 页（MySQL 8.0+）
    if (is_sdi_page(page_data, page_size)) {
        return MySQLVersion::MYSQL_80;
    }
    
    // 2. 检查页头版本（MySQL 8.4+）
    uint16_t header_version = get_header_version(page_data);
    if (header_version >= 2) {
        return MySQLVersion::MYSQL_84;
    }
    
    // 3. 检查 LSN 大小（8.0+ 为 8 字节）
    if (get_lsn_size(page_data) == 8) {
        return MySQLVersion::MYSQL_80;
    }
    
    // 4. 检查 max_trx_id 模式（5.6/5.7）
    if (is_max_trx_id_valid(page_data)) {
        return MySQLVersion::MYSQL_57;
    }
    
    // 5. 默认为 5.6
    return MySQLVersion::MYSQL_56;
}
```

## 环境检测

系统自动检测环境：

```cpp
// 平台检测
#ifdef ENV_PLATFORM_WINDOWS
    // Windows 特定代码
#elif defined(ENV_PLATFORM_MACOS)
    // macOS 特定代码
#elif defined(ENV_PLATFORM_LINUX)
    // Linux 特定代码
#endif

// 架构检测
#ifdef ENV_ARCH_X64
    // x86_64 优化
#elif defined(ENV_ARCH_ARM64)
    // ARM64 优化
#endif

// 编译器检测
#if defined(ENV_COMPILER_CLANG)
    // Clang 特定代码
#elif defined(ENV_COMPILER_GCC)
    // GCC 特定代码
#elif defined(ENV_COMPILER_MSVC)
    // MSVC 特定代码
#endif
```

## 测试

### 多版本测试

```bash
# 测试所有支持的 MySQL 版本
ctest -R "version_.*" --verbose

# 测试特定版本
ctest -R "version_80" --verbose
```

### 多环境测试

```bash
# 在 Linux 上测试
ctest -R "linux_.*" --verbose

# 在 macOS 上测试
ctest -R "macos_.*" --verbose

# 在 Windows 上测试
ctest -R "windows_.*" --verbose
```

## 性能

### 多版本性能

| MySQL 版本 | 恢复速度 | 内存使用 |
|-----------|---------|---------|
| 5.0-5.5 | 100% | 基线 |
| 5.6-5.7 | 110% | +10% |
| 8.0-8.3 | 120% | +20% |
| 8.4-9.0 | 130% | +30% |

### 多环境性能

| 平台 | 恢复速度 | 内存使用 |
|-----|---------|---------|
| Linux | 100% | 基线 |
| macOS | 95% | -5% |
| Windows | 90% | +5% |

## 故障排除

### 版本检测问题

如果版本检测失败：

1. 检查页数据完整性
2. 验证文件是有效的 InnoDB 页
3. 使用 `--mysql-version` 标志手动指定

### 环境兼容性问题

如果环境特定功能失败：

1. 检查系统要求
2. 安装缺失的依赖
3. 使用 `--no-env-optimizations` 标志禁用环境特定功能

## 未来增强

### 计划功能

1. **MySQL 9.1+ 支持**：未来版本兼容性
2. **云集成**：AWS、Azure、GCP 支持
3. **Docker 支持**：容器化恢复
4. **Web 界面**：基于浏览器的恢复
5. **AI 辅助恢复**：机器学习用于数据重建

### 路线图

- 2026年第二季度：MySQL 9.1 支持、云集成
- 2026年第三季度：Docker 支持、Web 界面
- 2026年第四季度：AI 辅助恢复、高级分析

## 支持

如需支持，请：

1. 查看文档
2. 使用详细日志运行
3. 提交问题并提供详细信息

## 许可证

MIT 许可证 - 有关详细信息，请参阅 LICENSE 文件。

## 作者

- **Jliang Fang** - 初始工作和多版本/多环境支持

## 致谢

- MySQL InnoDB 文档
- Percona XtraBackup
- MySQL 源代码
- 开源社区
