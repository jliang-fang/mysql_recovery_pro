# MySQL Recovery Pro - 兼容性特性

## 概述

MySQL Recovery Pro v2.0.0 完全支持多版本MySQL、多操作系统和多CPU架构。本项目实现了完整的兼容性层，确保在各种环境下都能正常工作。

## 功能特性

### 1. MySQL版本兼容性

支持从MySQL 5.0到9.0的所有版本，包括：

- **MySQL 5.0/5.1/5.5**: 早期版本支持
- **MySQL 5.6/5.7**: COMPACT/DYNAMIC行格式
- **MySQL 8.0/8.1/8.2/8.3**: SDI页、LSN扩展
- **MySQL 8.4**: 加密、临时页支持
- **MySQL 9.0**: 增强校验和

### 2. 跨平台兼容性

完全支持以下操作系统：

- **Linux**: x86_64, ARM64
- **macOS**: x86_64, ARM64 (Apple Silicon)
- **Windows**: x86_64, ARM64
- **FreeBSD**: x86_64
- **OpenBSD**: x86_64

### 3. CPU架构优化

针对不同CPU架构进行优化：

- **x86_64**: SSE2, SSE4.2, AVX2, AVX-512
- **ARM64**: NEON, CRC32, AES, PMULL, SHA

## 目录结构

```
include/dbrecovery/common/
├── version_compat.hpp              # MySQL版本兼容性
├── endian_compat.hpp               # 字节序兼容性
├── environment_compat.hpp          # 环境兼容性
├── platform_detection.hpp          # 平台检测
├── cpu_optimizations.hpp           # CPU优化统一接口
├── simd_operations.hpp             # SIMD操作统一接口
└── mysql_version_detector.hpp      # 版本检测器

src/common/
├── mysql_version_detector.cpp      # 版本检测实现
└── version_compat.cpp              # 版本兼容性实现

src/platform/
├── linux/                          # Linux平台实现
│   ├── file_system_linux.cpp
│   ├── memory_linux.cpp
│   └── thread_linux.cpp
├── macos/                          # macOS平台实现
│   ├── file_system_macos.cpp
│   ├── memory_macos.cpp
│   └── thread_macos.cpp
├── windows/                        # Windows平台实现
│   ├── file_system_windows.cpp
│   ├── memory_windows.cpp
│   └── thread_windows.cpp
├── x86_64/                         # x86_64架构优化
│   ├── cpu_optimizations_x86_64.hpp
│   └── simd_operations_x86_64.hpp
└── arm64/                          # ARM64架构优化
    ├── cpu_optimizations_arm64.hpp
    ├── cpu_optimizations_arm64.cpp
    └── simd_operations_arm64.hpp

cmake/
└── cross_compilation.cmake         # 跨平台编译配置

docs/en/
└── compatibility_guide.md          # 兼容性指南

tests/
├── test_multi_version_compat.cpp   # 多版本兼容性测试
├── test_multi_environment_compat.cpp # 多环境兼容性测试
├── test_cpu_optimizations.cpp      # CPU优化测试
├── test_environment_compat.cpp     # 环境兼容性测试
├── test_platform_detection.cpp     # 平台检测测试
└── test_simd_operations.cpp        # SIMD操作测试
```

## 使用示例

### 版本检测

```cpp
#include "dbrecovery/common/mysql_version_detector.hpp"

// 快速检测
MySQLVersion version = quick_detect_version(page_data, page_size);

// 详细检测
VersionDetectionResult result = detailed_detect_version_enhanced(page_data, page_size);
std::cout << "Version: " << get_mysql_version_string(result.version) << std::endl;
std::cout << "Confidence: " << result.confidence << "%" << std::endl;
```

### 平台检测

```cpp
#include "dbrecovery/common/platform_detection.hpp"

auto& detector = PlatformDetector::instance();

if (detector.is_linux()) {
    std::cout << "Running on Linux" << std::endl;
} else if (detector.is_macos()) {
    std::cout << "Running on macOS" << std::endl;
} else if (detector.is_windows()) {
    std::cout << "Running on Windows" << std::endl;
}
```

### CPU优化

```cpp
#include "dbrecovery/common/cpu_optimizations.hpp"

// 自动选择最优CRC32实现
uint32_t crc = calculate_crc32(data, size);

// 自动选择最优内存复制
fast_memcpy(dest, src, size);
```

### SIMD操作

```cpp
#include "dbrecovery/common/simd_operations.hpp"

// 快速复制
fast_copy_16(dest, src);
fast_copy_32(dest, src);
fast_copy_64(dest, src);

// 快速清零
fast_zero_16(dest);
fast_zero_32(dest);
fast_zero_64(dest);
```

## 构建

### 基本构建

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 跨平台构建

#### Linux ARM64

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
```

#### macOS ARM64

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
# 自动检测ARM64架构
```

#### Windows x86_64

```bash
cmake .. -G "Visual Studio 17 2022" -A x64
```

### 静态链接

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DSTATIC_LINK=ON
```

## 测试

```bash
# 运行所有测试
make test

# 运行特定测试
./test_multi_version_compat
./test_multi_environment_compat
./test_cpu_optimizations
./test_platform_detection
./test_simd_operations
```

## API文档

### MySQL版本检测

| 函数 | 描述 |
|------|------|
| `quick_detect_version()` | 快速版本检测 |
| `detailed_detect_version()` | 详细版本检测 |
| `detect_version_from_multiple_pages()` | 多页版本检测 |
| `get_version_info()` | 获取版本信息 |
| `get_version_series()` | 获取版本系列 |

### 平台检测

| 函数 | 描述 |
|------|------|
| `get_platform_info()` | 获取平台信息 |
| `get_platform_name()` | 获取平台名称 |
| `get_arch_name()` | 获取架构名称 |
| `is_windows()`, `is_macos()`, `is_linux()` | 平台检测 |
| `is_x86_64()`, `is_arm64()` | 架构检测 |

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

## 构建系统

### CMake选项

| 选项 | 默认值 | 描述 |
|------|--------|------|
| `CMAKE_BUILD_TYPE` | Release | 构建类型 |
| `STATIC_LINK` | OFF | 静态链接 |
| `ENABLE_ASAN` | OFF | AddressSanitizer |
| `ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer |
| `ENABLE_LSAN` | OFF | LeakSanitizer |

### 工具链文件

- `cmake/cross_compilation.cmake` - 跨平台编译配置
- `cmake/toolchain-arm64.cmake` - ARM64工具链
- `cmake/toolchain-x86_64.cmake` - x86_64工具链

## CI/CD

项目使用GitHub Actions进行持续集成：

- Linux x86_64, ARM64
- macOS x86_64, ARM64
- Windows x86_64, ARM64
- 代码质量检查
- 代码覆盖率

## 文档

- [兼容性指南](docs/en/compatibility_guide.md) - 详细的兼容性使用指南
- [架构指南](docs/en/architecture.md) - 系统架构文档
- [用户指南](docs/en/user_guide.md) - 用户使用指南

## 贡献

欢迎贡献！请遵循以下步骤：

1. Fork仓库
2. 创建特性分支
3. 提交更改
4. 发起Pull Request

## 许可证

MIT License - 详见LICENSE文件

## 作者

- **Jliang Fang** - 初始工作

## 致谢

- MySQL InnoDB文档
- Percona XtraBackup
- MySQL源代码

## 版本历史

### v2.0.0 - 增强版
- ✅ MySQL 5.0-9.0完整支持
- ✅ Linux/macOS/Windows跨平台支持
- ✅ ARM/x86 CPU架构优化
- ✅ SIMD向量化操作
- ✅ 完整的CI/CD配置

### v1.0.0 - 初始版本
- 基础恢复功能
- 页解析支持
- BLOB恢复
