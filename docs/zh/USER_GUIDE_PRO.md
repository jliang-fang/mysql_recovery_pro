# MySQL Recovery Pro - 专业版使用指南

## 目录
1. [产品概述](#产品概述)
2. [系统要求](#系统要求)
3. [快速开始](#快速开始)
4. [核心功能](#核心功能)
5. [高级用法](#高级用法)
6. [常见问题](#常见问题)
7. [最佳实践](#最佳实践)

---

## 产品概述

MySQL Recovery Pro 是一款专业的 MySQL InnoDB 数据恢复工具，支持从损坏的 `.ibd` 文件中恢复数据。本工具兼容 MySQL 5.6 至 9.0 所有版本，支持所有 InnoDB 字段类型的数据恢复。

### 核心特性

| 特性 | 说明 |
|------|------|
| **版本兼容** | MySQL 5.6, 5.7, 8.0, 8.4, 9.0 |
| **行格式支持** | REDUNDANT, COMPACT, DYNAMIC, COMPRESSED |
| **字段类型** | 支持所有 InnoDB 类型（INT, VARCHAR, TEXT, BLOB, DATE, DATETIME, DECIMAL, ENUM, JSON 等） |
| **恢复场景** | 正常恢复、DROP TABLE恢复、TRUNCATE恢复、rm -rf恢复、物理损坏恢复 |
| **输出格式** | CSV、JSON、SQL INSERT |

---

## 系统要求

### 编译环境
- GCC 11+ 或 Clang 14+
- CMake 3.20+
- zlib 开发库
- C++20 支持

### 运行环境
- Linux (CentOS 7+, Ubuntu 20.04+, Debian 11+)
- 内存：建议 4GB 以上
- 存储：需要足够空间存放恢复数据

---

## 快速开始

### 1. 编译安装

```bash
# 克隆项目
git clone https://github.com/your-repo/mysql_recovery_pro.git
cd mysql_recovery_pro

# 编译
mkdir build && cd build
cmake ..
make -j$(nproc)

# 可执行文件位置
./mysql_recovery_pro
```

### 2. 基本使用

```bash
# 从 .ibd 文件恢复数据
./mysql_recovery_pro --ibd=/path/to/table.ibd --output=/path/to/output

# 输出文件
ls /path/to/output/
# recovered.csv  - 恢复的CSV数据
# schema.json    - 表结构信息
# stats.txt      - 恢复统计
```

### 3. 使用手动 Schema 恢复

当 SDI 不可用（如 DROP TABLE 后恢复）：

```bash
# 手动指定表结构
./mysql_recovery_pro --ibd=/path/to/dropped_table.ibd \
    --output=/path/to/output \
    --schema="id:INT,name:VARCHAR(100),created_at:DATETIME,value:DECIMAL(10,2)"
```

---

## 核心功能

### 1. SDI 自动识别

MySQL 8.0+ 使用 SDI (Serialized Dictionary Information) 存储表结构。本工具自动解析 SDI：

```
[INFO] Successfully parsed schema from compressed SDI: table_name (N columns)
```

支持的 SDI 格式：
- 未压缩 SDI (0x45BE)
- 压缩 SDI (0x45BD) - 自动 zlib 解压

### 2. 多版本兼容

| MySQL 版本 | 行格式 | SDI 支持 | 特殊处理 |
|------------|--------|----------|----------|
| 5.6 | COMPACT/DYNAMIC | ❌ | 需要 FRM 或手动 Schema |
| 5.7 | COMPACT/DYNAMIC | ❌ | 需要 FRM 或手动 Schema |
| 8.0 | DYNAMIC/COMPRESSED | ✅ | 自动 SDI 解析 |
| 8.4 | DYNAMIC/COMPRESSED | ✅ | 页大小检测优化 |
| 9.0 | DYNAMIC/COMPRESSED | ✅ | 最新格式支持 |

### 3. 字段类型支持

#### 数值类型
| 类型 | InnoDB ID | 说明 |
|------|-----------|------|
| TINYINT | 2 | 1字节整数 |
| SMALLINT | 3 | 2字节整数 |
| INT | 4 | 4字节整数 |
| BIGINT | 9 | 8字节整数 |
| FLOAT | 5 | 4字节浮点 |
| DOUBLE | 6 | 8字节双精度 |
| DECIMAL | 21 | 精确数值 |

#### 时间类型
| 类型 | InnoDB ID | 存储 | 说明 |
|------|-----------|------|------|
| DATE | 15 | 3字节 | 日期 (YYYY-MM-DD) |
| DATETIME2 | 19 | 5-8字节 | 日期时间，支持微秒 |
| TIMESTAMP2 | 18 | 4-7字节 | 时间戳，UTC存储 |
| TIME2 | 20 | 3-6字节 | 时间，支持微秒 |
| YEAR | 14 | 1字节 | 年份 |

#### 字符/二进制类型
| 类型 | InnoDB ID | 说明 |
|------|-----------|------|
| VARCHAR | 16 | 可变长度字符串 |
| CHAR | 29 | 固定长度（多字节字符集为可变） |
| TEXT | 27 | 长文本 |
| BLOB | 27 | 二进制数据 |
| JSON | 31 | JSON 文档 |

#### 特殊类型
| 类型 | InnoDB ID | 说明 |
|------|-----------|------|
| ENUM | 22 | 枚举值 |
| SET | 23 | 集合值 |
| BIT | 17 | 位字段 |
| GEOMETRY | 30 | 空间数据 |

### 4. 恢复场景

#### 场景 A：正常恢复
```bash
# 表文件完整，SDI 可读
./mysql_recovery_pro --ibd=/data/mysql/db/table.ibd --output=/recovery/
```

#### 场景 B：DROP TABLE 恢复
```bash
# 表已删除，但 .ibd 文件未被覆盖
./mysql_recovery_pro --ibd=/backup/table.ibd \
    --output=/recovery/ \
    --schema="id:INT,name:VARCHAR(100),status:ENUM('active','inactive')"
```

#### 场景 C：TRUNCATE 恢复
```bash
# 表被 TRUNCATE，数据页可能残留
./mysql_recovery_pro --ibd=/data/table.ibd \
    --output=/recovery/ \
    --mode=truncate
```

#### 场景 D：rm -rf 恢复
```bash
# 整个数据目录被删除
./mysql_recovery_pro --device=/dev/sda1 \
    --output=/recovery/ \
    --carve-mode=raw
```

---

## 高级用法

### 1. 指定页大小

```bash
# 强制指定页大小（默认自动检测）
./mysql_recovery_pro --ibd=/path/to/table.ibd \
    --output=/path/to/output \
    --page-size=16384
```

支持的页大小：4KB, 8KB, 16KB, 32KB, 64KB

### 2. 恢复限制

```bash
# 限制恢复记录数
./mysql_recovery_pro --ibd=/path/to/table.ibd \
    --output=/path/to/output \
    --max-records=10000
```

### 3. 输出格式选择

```bash
# CSV 格式（默认）
./mysql_recovery_pro --ibd=/path/to/table.ibd \
    --output=/path/to/output \
    --format=csv

# JSON 格式
./mysql_recovery_pro --ibd=/path/to/table.ibd \
    --output=/path/to/output \
    --format=json

# SQL INSERT 格式
./mysql_recovery_pro --ibd=/path/to/table.ibd \
    --output=/path/to/output \
    --format=sql
```

### 4. 调试模式

```bash
# 启用详细日志
./mysql_recovery_pro --ibd=/path/to/table.ibd \
    --output=/path/to/output \
    --verbose
```

---

## 常见问题

### Q1: 恢复后数据不完整？

**原因**：可能是页损坏或 BLOB 链断裂。

**解决**：
```bash
# 使用完整恢复模式
./mysql_recovery_pro --ibd=/path/to/table.ibd \
    --output=/path/to/output \
    --mode=full
```

### Q2: 时间字段显示错误值？

**原因**：DATETIME2/TIMESTAMP2 解码精度问题。

**解决**：确保 Schema 中指定了正确的精度：
```bash
--schema="created_at:DATETIME(6)"
```

### Q3: ENUM/SET 字段显示为 `<ENUM_XXX>`？

**原因**：缺少 ENUM 值定义。

**解决**：在 Schema 中指定 ENUM 值：
```bash
--schema="status:ENUM('active','inactive','pending')"
```

### Q4: MySQL 5.6/5.7 无法识别 Schema？

**原因**：这些版本没有 SDI，需要 FRM 文件或手动 Schema。

**解决**：
```bash
# 使用 FRM 文件
./mysql_recovery_pro --ibd=/path/to/table.ibd \
    --frm=/path/to/table.frm \
    --output=/path/to/output

# 或手动指定 Schema
--schema="col1:type1,col2:type2,..."
```

---

## 最佳实践

### 1. 恢复前准备

- ✅ 备份原始 .ibd 文件
- ✅ 确认 MySQL 版本和页大小
- ✅ 准备表结构信息（FRM 或手动 Schema）

### 2. 恢复流程

```
1. 检查文件 → 2. 自动检测 → 3. 解析 Schema → 4. 恢复数据 → 5. 验证输出
```

### 3. 数据验证

恢复后建议：
- 检查记录数是否匹配预期
- 验证关键字段的值范围
- 对比原始数据（如有备份）

### 4. 性能优化

- 大表恢复：使用 `--max-records` 分批恢复
- 多表恢复：并行运行多个恢复进程
- 内存限制：适当调整系统内存分配

---

## 技术支持

- GitHub Issues: https://github.com/your-repo/mysql_recovery_pro/issues
- 文档更新: docs/ 目录下的详细文档

---

## 版本历史

| 版本 | 日期 | 主要更新 |
|------|------|----------|
| 1.0 | 2026-06 | 初始版本，支持 MySQL 5.6-9.0 |
| - | - | DYNAMIC 列存储顺序修复 |
| - | - | 所有字段类型支持 |
| - | - | 99% 测试通过率 |