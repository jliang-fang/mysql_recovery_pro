# MySQL 8.x 数据恢复引擎 - 用户指南

**版本**: v1.0.0  
**作者**: jliang.fang  
**日期**: 2026-03-29

---

## 1. 概述

MySQL 8.x 数据恢复引擎是一个高性能的离线 MySQL 数据恢复工具，支持 MySQL 5.6/5.7/8.0/8.1/8.2/8.3/8.4/9.0 的深度数据恢复。

### 1.1 核心功能

- **InnoDB 页解析**: 完整支持 COMPACT/REDUNDANT/DYNAMIC/COMPRESSED 行格式
- **B+树遍历**: 支持页链验证的递归索引遍历
- **BLOB 恢复**: 多页 BLOB 重建与校验和验证
- **Redo Log 回放**: 支持所有 InnoDB 行格式的 Redo 日志解析与回放
- **Undo Log 解析**: 事务回滚支持
- **SDI 元数据解析**: Space Descriptor Index 解析与表结构重建
- **跨平台**: Linux/Windows 统一 API
- **CLI/GUI**: 命令行工具和图形界面

### 1.2 支持的灾难场景

| 场景 | 恢复能力 | 备注 |
|------|----------|------|
| Truncate | ✅ 完整恢复 | 基于页链和 Redo Log |
| rm -rf | ✅ 部分恢复 | 需要未覆盖的数据页 |
| 文件头损坏 | ✅ 修复 | 重建文件头 |
| 表空间损坏 | ✅ 部分恢复 | 基于可用页 |
| Redo Log 应用 | ✅ 完整恢复 | 前滚恢复 |
| BLOB 损坏 | ✅ 部分恢复 | 基于页链 |

---

## 2. 快速开始

### 2.1 编译构建

```bash
# 克隆项目
cd /app/mysql8_recovery

# 创建构建目录
mkdir -p build && cd build

# 配置 CMake
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译
make -j$(nproc)

# 运行测试
ctest --verbose
```

### 2.2 命令行工具

```bash
# 恢复表数据
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered

# 指定表 ID
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --table-id=12345

# Truncate 恢复
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --truncate-recovery

# rm -rf 恢复
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --rmrf-recovery

# 查看帮助
./mysql8_recover --help
```

### 2.3 GUI 工具

```bash
# 启动图形界面
cd build/gui
./mysql8_recovery_gui
```

---

## 3. 命令行参数

### 3.1 必需参数

| 参数 | 说明 | 示例 |
|------|------|------|
| `--ibd=<path>` | .ibd 文件路径 | `--ibd=/var/lib/mysql/test/table.ibd` |

### 3.2 可选参数

| 参数 | 说明 | 默认值 | 示例 |
|------|------|--------|------|
| `--output=<dir>` | 输出目录 | `/tmp/recovered` | `--output=/data/recovered` |
| `--table-id=<id>` | 表 ID 用于 SDI 查找 | 0 | `--table-id=12345` |
| `--page-size=<size>` | 页大小: 4096/8192/16384/32768/65536 | 16384 | `--page-size=8192` |
| `--check-headers` | 仅检查页头 | false | `--check-headers` |
| `--truncate-recovery` | Truncate 恢复模式 | false | `--truncate-recovery` |
| `--rmrf-recovery` | rm -rf 恢复模式 | false | `--rmrf-recovery` |
| `--blob-recovery` | 启用 BLOB 恢复 | false | `--blob-recovery` |
| `--format=<csv\|sql\|ibd>` | 输出格式 | csv | `--format=sql` |

### 3.3 示例

```bash
# 基本恢复
./mysql8_recover --ibd=/var/lib/mysql/test/users.ibd --output=/tmp/users_recovered

# Truncate 恢复
./mysql8_recover --ibd=/var/lib/mysql/test/orders.ibd --output=/tmp/orders_recovered --truncate-recovery --format=sql

# rm -rf 恢复
./mysql8_recover --ibd=/var/lib/mysql/test/products.ibd --output=/tmp/products_recovered --rmrf-recovery --format=csv

# 指定页大小（用于非标准页大小）
./mysql8_recover --ibd=/var/lib/mysql/test/large.ibd --output=/tmp/large_recovered --page-size=32768

# 仅检查页头
./mysql8_recover --ibd=/var/lib/mysql/test/check.ibd --check-headers
```

---

## 4. 图形界面

### 4.1 界面布局

```
┌─────────────────────────────────────────────────────────────┐
│  MySQL 8.x Recovery Engine v1.0.0                          │
├─────────────────────────────────────────────────────────────┤
│  [File] [Recovery] [Export] [View] [Help]                  │
├─────────────────────────────────────────────────────────────┤
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Recovery Options                                     │  │
│  │  ┌─────────────────────────────────────────────────┐  │  │
│  │  │ Row Format: [Auto Detect ▼]                    │  │  │
│  │  │ Page Size: [16384]                              │  │  │
│  │  │ [✓] Validate Checksums                          │  │  │
│  │  │ [✓] Enable BLOB Recovery                        │  │  │
│  │  │ Output Path: [/tmp/recovered] [Browse]         │  │  │
│  │  └─────────────────────────────────────────────────┘  │  │
│  │  [Start Recovery]                                     │  │
│  └───────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Pages | Records | Log                                │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 功能说明

#### 4.2.1 Recovery 选项卡

- **Row Format**: 选择行格式（Auto Detect 自动检测）
- **Page Size**: 设置页大小（默认 16384）
- **Validate Checksums**: 启用校验和验证
- **Enable BLOB Recovery**: 启用 BLOB 恢复
- **Output Path**: 设置输出目录

#### 4.2.2 Pages 选项卡

- 显示所有页的列表
- 显示页类型、记录数、状态
- 支持页级预览

#### 4.2.3 Records 选项卡

- 显示解析后的记录
- 支持记录级预览
- 支持导出

#### 4.2.4 Log 选项卡

- 显示操作日志
- 支持日志搜索
- 支持日志导出

---

## 5. 数据导出

### 5.1 CSV 导出

```bash
# 命令行
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --format=csv

# GUI
1. 打开 GUI
2. 选择 Recovery 选项卡
3. 点击 "Start Recovery"
4. 在 Log 选项卡查看导出结果
```

### 5.2 SQL 导出

```bash
# 命令行
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --format=sql

# GUI
1. 打开 GUI
2. 选择 Recovery 选项卡
3. 点击 "Start Recovery"
4. 在 Log 选项卡查看导出结果
```

### 5.3 IBD 导出

```bash
# 命令行
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/repaired --format=ibd

# GUI
1. 打开 GUI
2. 选择 Recovery 选项卡
3. 点击 "Start Recovery"
4. 在 Log 选项卡查看导出结果
```

---

## 6. 灾难场景恢复

### 6.1 Truncate 恢复

#### 6.1.1 场景描述

当执行 `TRUNCATE TABLE` 命令后，数据页被标记为未分配，但实际数据仍保留在磁盘上。

#### 6.1.2 恢复步骤

1. **立即停止数据库写入**
2. **备份当前磁盘状态**
3. **使用恢复工具扫描 .ibd 文件**
4. **解析页链并提取记录**
5. **导出为 CSV/SQL/IBD**

#### 6.1.3 命令行示例

```bash
./mysql8_recover --ibd=/var/lib/mysql/test/users.ibd \
    --output=/tmp/users_recovered \
    --truncate-recovery \
    --format=sql
```

### 6.2 rm -rf 恢复

#### 6.2.1 场景描述

当执行 `rm -rf *.ibd` 后，文件系统条目被删除，但数据可能仍存在于磁盘上。

#### 6.2.2 恢复步骤

1. **立即停止磁盘写入**
2. **使用磁盘扫描工具恢复文件**
3. **或使用恢复工具直接扫描磁盘**
4. **识别 InnoDB 页结构**
5. **提取表结构和数据**

#### 6.2.3 命令行示例

```bash
./mysql8_recover --ibd=/var/lib/mysql/test/users.ibd \
    --output=/tmp/users_recovered \
    --rmrf-recovery \
    --format=csv
```

### 6.3 文件头损坏恢复

#### 6.3.1 场景描述

文件头损坏可能由磁盘坏道、文件系统错误、强制断电等导致。

#### 6.3.2 恢复步骤

1. **读取损坏的 .ibd 文件**
2. **识别损坏的页头**
3. **修复页类型、校验和等**
4. **重建文件头**
5. **导出修复后的文件**

#### 6.3.3 命令行示例

```bash
./mysql8_recover --ibd=/var/lib/mysql/test/users.ibd \
    --output=/tmp/users_repaired \
    --format=ibd
```

---

## 7. 高级功能

### 7.1 SDI 元数据解析

SDI (Space Descriptor Index) 存储表的元数据信息，包括：

- 表名
- 列定义
- 索引定义
- 分区信息

### 7.2 BLOB 恢复

BLOB 数据存储在独立的页链中，恢复工具支持：

- 多页 BLOB 重建
- 页链完整性验证
- 循环引用检测

### 7.3 Redo Log 回放

Redo Log 记录所有数据修改操作，恢复工具支持：

- Redo Log 解析
- 前滚恢复
- 事务回滚

---

## 8. 性能优化

### 8.1 内存映射

使用 mmap 进行文件映射，避免额外的内存拷贝。

### 8.2 CRC32C 查表法

使用编译时生成的 CRC32C 表，零运行时开销。

### 8.3 零拷贝视图

使用 `std::span` 提供零拷贝的页数据视图。

---

## 9. 故障排除

### 9.1 常见问题

#### 9.1.1 无法打开文件

**错误**: `Failed to open file: /path/to/file.ibd`

**解决方案**:
1. 检查文件路径是否正确
2. 检查文件权限
3. 检查文件是否被其他进程占用

#### 9.1.2 解析失败

**错误**: `Failed to parse page 0`

**解决方案**:
1. 检查页大小是否正确
2. 检查行格式是否正确
3. 检查文件是否损坏

#### 9.1.3 恢复率低

**问题**: 恢复的记录数远少于预期

**解决方案**:
1. 检查数据是否被覆盖
2. 检查页链是否完整
3. 尝试不同的行格式

### 9.2 日志分析

日志文件位于输出目录的 `recovery.log`，包含：

- 操作步骤
- 错误信息
- 性能统计

---

## 10. 最佳实践

### 10.1 灾难恢复

1. **立即停止写入**: 发现误操作后立即停止数据库写入
2. **备份当前状态**: 先备份当前磁盘状态
3. **使用专业工具**: 使用经过验证的恢复工具
4. **测试恢复**: 在测试环境验证恢复结果
5. **验证数据**: 恢复后验证数据完整性

### 10.2 性能优化

1. **使用 SSD**: SSD 提供更快的 I/O 性能
2. **增加内存**: 更多内存减少磁盘 I/O
3. **并行处理**: 使用多线程加速恢复

---

## 11. 技术支持

### 11.1 文档

- [架构设计](./architecture.md)
- [原理文档](./principle_*.md)
- [测试策略](./test_strategy.md)

### 11.2 社区

- GitHub Issues: https://github.com/your-org/mysql8_recovery/issues
- 邮件: support@mysql8-recovery.com

---

## 12. 许可证

MIT License

---

## 13. 作者

**jliang.fang** - 首席数据库内核架构师

---

<div align="center">
  <p>打造世界领先的 MySQL 数据恢复工具</p>
</div>
