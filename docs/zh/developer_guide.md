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
├── scripts/               # 脚本文件
├── CMakeLists.txt         # CMake 配置
└── README.md              # 项目说明
```

## 数据导出

mysql_recovery_pro 支持 **8 种导出格式**，每种格式都有专门的导出器实现：

### 1. ExportFormat 枚举

```cpp
enum class ExportFormat {
    SQL,        // SQL INSERT 语句
    CSV,        // CSV 格式
    JSON,       // JSON 格式
    XML,        // XML 格式
    TEXT,       // 纯文本格式
};
```

### 2. 导出器分类

#### 基础导出器 (DataExporter)
- **位置**: `include/dbrecovery/innodb/data_exporter.hpp`
- **实现**: `src/innodb/data_exporter.cpp`
- **支持格式**: SQL, CSV, JSON, XML, TEXT
- **功能**: 行级别数据导出

#### 页导出器 (PageExporter)
- **位置**: `include/dbrecovery/innodb/page_exporter.hpp`
- **实现**: `src/innodb/page_exporter.cpp`
- **支持格式**: JSON, CSV, SQL, Binary
- **功能**: 页级别数据导出

#### 格式导出器 (export_formats.hpp)
- **位置**: `include/dbrecovery/innodb/export_formats.hpp`
- **支持格式**: 
  - **JSONExporter**: JSON 格式
  - **ParquetExporter**: Parquet 格式 (简化实现)
  - **XMLExporter**: XML 格式
  - **YAMLExporter**: YAML 格式
  - **AvroExporter**: Avro 格式 (简化实现)

### 3. 导出流程

```
用户请求
    ↓
RecoveryEngine.recover_truncate()
    ↓
获得 records (vector<map<string, string>>)
    ↓
根据 format 参数选择导出器
    ↓
调用对应导出器
    ↓
写入输出文件
```

### 4. 导出器使用示例

#### DataExporter 使用

```cpp
#include "dbrecovery/innodb/data_exporter.hpp"

// 创建导出器
dbrecovery::innodb::DataExporter exporter;

// 设置配置
dbrecovery::innodb::ExportConfig config;
config.format = dbrecovery::innodb::ExportFormat::JSON;
config.table_name = "users";
config.include_header = true;
config.include_transaction = false;
exporter.set_config(config);

// 导出行数据
std::string output = exporter.export_row(row);
```

#### PageExporter 使用

```cpp
#include "dbrecovery/innodb/page_exporter.hpp"

// 创建导出器
dbrecovery::innodb::PageExportOptions options;
options.output_format = "json";
options.include_header = true;
options.include_records = true;
dbrecovery::innodb::PageExporter exporter(options);

// 导出页数据
auto result = exporter.export_page(page_data.data(), page_data.size());
std::cout << result.output << std::endl;
```

### 5. CLI 参数处理

命令行参数通过 `CliParser` 解析，支持的格式选项：

```cpp
std::string format = parser_.get_option("format", "csv");
```

支持的格式值：
- `csv` - CSV 格式
- `sql` - SQL 格式
- `json` - JSON 格式
- `xml` - XML 格式
- `yaml` - YAML 格式
- `avro` - Avro 格式
- `text` - TEXT 格式
- `ibd` - IBD 修复模式

### 6. 导出格式对比

| 格式 | 优点 | 缺点 | 适用场景 |
|------|------|------|----------|
| CSV | 简单、通用、Excel 支持 | 无类型信息 | 数据迁移、Excel 分析 |
| SQL | 可直接导入 MySQL | 文件较大 | MySQL 数据恢复 |
| JSON | Web 友好、层次结构 | 文件较大 | API 交换、Web 应用 |
| XML | 标准化、Schema 支持 | 文件最大、解析慢 | 系统集成、配置 |
| YAML | 人类可读、配置友好 | 需要 YAML 库 | 配置文件、脚本 |
| Avro | 二进制、高效 | 需要 Avro 库、二进制 | Hadoop、大数据 |
| TEXT | 人类可读 | 无结构 | 日志、调试 |
| IBD | 文件级恢复 | 仅用于修复 | 表空间修复 |

---

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
