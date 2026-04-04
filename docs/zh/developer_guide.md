# MySQL 8.x 数据恢复引擎 - 开发者指南

## 概述

本文档为开发者提供构建、开发和贡献 MySQL 8.x 数据恢复引擎的完整指南。

## 环境要求

### 基本要求

- **操作系统**: Linux (推荐 Ubuntu 20.04+ / CentOS 8+)
- **编译器**: GCC 11+ 或 Clang 13+
- **CMake**: 3.20+
- **C++ 标准**: C++20

### 依赖项

| 依赖 | 版本 | 说明 |
|------|------|------|
| nlohmann/json | 3.9+ | JSON 处理 |
| zlib | 1.2.11+ | SDI 解压缩 |
| Google Test | 1.10+ | 单元测试 |
| Qt6 | 6.2+ | GUI 支持（可选） |

### 安装依赖

```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    libnlohmann-json-dev \
    zlib1g-dev \
    libgtest-dev \
    qt6-base-dev

# CentOS/RHEL
sudo yum install -y \
    gcc-c++ \
    cmake \
    nlohmann_json-devel \
    zlib-devel \
    gtest-devel \
    qt6-qtbase-devel
```

## 构建项目

### 基本构建

```bash
# 克隆项目
cd /app/mysql_recovery_proy

# 创建构建目录
mkdir -p build && cd build

# 配置 CMake
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译
make -j$(nproc)

# 安装（可选）
sudo make install
```

### 启用测试

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON
make -j$(nproc)
ctest --verbose
```

### 启用静态分析

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_CLANG_TIDY=ON
make -j$(nproc)
```

### 启用动态分析

```bash
# AddressSanitizer
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
make -j$(nproc)

# UndefinedBehaviorSanitizer
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_UBSAN=ON
make -j$(nproc)

# LeakSanitizer
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_LSAN=ON
make -j$(nproc)
```

## 项目结构

```
mysql_recovery_proy/
├── include/dbrecovery/     # 头文件
│   ├── catalog/           # 元数据目录
│   ├── crypt/             # 加密扫描
│   ├── disk/              # 磁盘扫描
│   ├── engine/            # 引擎功能
│   ├── export/            # 数据导出
│   ├── innodb/            # InnoDB 核心
│   ├── recovery/          # 恢复功能
│   ├── repair/            # 修复功能
│   └── utils/             # 工具函数
├── src/                   # 源代码
│   ├── catalog/
│   ├── checker/
│   ├── disk/
│   ├── engine/
│   ├── export/
│   ├── gui/               # GUI 实现
│   ├── innodb/
│   ├── recovery/
│   ├── repair/
│   └── utils/
├── tests/                 # 测试代码
│   ├── benchmark/         # 性能测试
│   ├── innodb/            # InnoDB 单元测试
│   └── unit/              # 单元测试
├── docs/                  # 文档
│   ├── zh/                # 中文文档
│   └── en/                # 英文文档
├── tools/                 # 工具脚本
│   ├── generate_test_data.py  # 测试数据生成
│   └── ibd_generator.py       # IBD 文件生成
├── CMakeLists.txt         # CMake 配置
└── README.md              # 项目说明
```

## 编码规范

### 命名约定

- **类名**: PascalCase (如 `RecoveryEngine`)
- **函数名**: PascalCase (如 `recover_truncate`)
- **变量名**: snake_case (如 `recovered_records`)
- **常量**: UPPER_SNAKE_CASE (如 `FIL_PAGE_INDEX`)
- **模板参数**: PascalCase (如 `T`)

### 代码风格

- 使用 4 空格缩进
- 行宽不超过 120 字符
- 使用 C++20 特性
- 遵循 Google C++ Style Guide

### 注释规范

```cpp
/**
 * @brief 类/函数描述
 *
 * 详细描述（如果需要）
 *
 * @param param1 参数描述
 * @param param2 参数描述
 * @return 返回值描述
 */
```

## 添加新功能

### 步骤 1: 创建测试

```cpp
// tests/innodb/new_feature_test.cpp
#include <gtest/gtest.h>
#include "dbrecovery/innodb/new_feature.hpp"

TEST(NewFeatureTest, BasicFunctionality) {
    // 测试代码
    EXPECT_TRUE(feature_function());
}
```

### 步骤 2: 实现功能

```cpp
// include/dbrecovery/innodb/new_feature.hpp
#pragma once

namespace dbrecovery {
namespace innodb {

bool new_feature_function();

} // namespace innodb
} // namespace dbrecovery
```

```cpp
// src/innodb/new_feature.cpp
#include "dbrecovery/innodb/new_feature.hpp"

namespace dbrecovery {
namespace innodb {

bool new_feature_function() {
    // 实现代码
    return true;
}

} // namespace innodb
} // namespace dbrecovery
```

### 步骤 3: 更新 CMakeLists.txt

```cmake
# src/innodb/CMakeLists.txt
set(SOURCES
    ...
    new_feature.cpp
)
```

### 步骤 4: 运行测试

```bash
cd build
make new_feature_test
./tests/new_feature_test
```

## 调试技巧

### 使用 GDB

```bash
gdb ./build/mysql_recovery_pro
(gdb) break main
(gdb) run --ibd=/path/to/file.ibd
(gdb) backtrace
```

### 使用 Valgrind

```bash
valgrind --leak-check=full ./build/mysql_recovery_pro --ibd=/path/to/file.ibd
```

### 使用 perf

```bash
perf record -g ./build/mysql_recovery_pro --ibd=/path/to/file.ibd
perf report
```

## 常见问题

### Q: 编译时找不到 nlohmann/json

A: 安装开发包：
```bash
sudo apt-get install libnlohmann-json-dev
```

### Q: GUI 编译失败

A: 确保 Qt6 开发包已安装：
```bash
sudo apt-get install qt6-base-dev
```

### Q: 测试运行失败

A: 确保测试数据目录存在：
```bash
mkdir -p tests/data
python3 tools/generate_test_data.py
```

## 贡献指南

### 提交流程

1. Fork 项目
2. 创建特性分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 提交 Pull Request

### PR 模板

```
## 描述
[简要描述 PR 的内容]

## 类型
- [ ] 新功能
- [ ] 修复
- [ ] 优化
- [ ] 文档

## 影响范围
- [ ] 核心功能
- [ ] 测试
- [ ] 文档
- [ ] 构建系统

## 测试
- [ ] 单元测试通过
- [ ] 集成测试通过
- [ ] 性能测试通过
```

## 许可证

MIT License

## 联系方式

- **作者**: jliang.fang
- **邮箱**: jliang.fang@example.com
