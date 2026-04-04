# 坏块扫描和修复命令说明

## 概述

MySQL Recovery Pro v2.1.0 提供了完整的坏块扫描和修复功能，帮助您检测和修复 InnoDB 表空间中的各种损坏。

## 坏块扫描命令

### 基本用法
```bash
mysql_recovery_pro --ibd=/path/to/table.ibd --check-corruption --output=/tmp/corruption
```

### 功能说明
- 扫描指定的 .ibd 文件
- 检测各种类型的坏块：
  - **页头损坏** (PAGE_HEADER_CORRUPTED)
  - **无效页类型** (PAGE_TYPE_INVALID)
  - **校验和不匹配** (PAGE_CHECKSUM_MISMATCH)
  - **页链断裂** (PAGE_CHAIN_BROKEN)
  - **记录头损坏** (RECORD_HEADER_CORRUPTED)
  - **记录链断裂** (RECORD_CHAIN_BROKEN)
  - **BLOB 链断裂** (BLOB_CHAIN_BROKEN)
  - **SDI 损坏** (SDI_CORRUPTED)

### 输出
1. **控制台输出**：
   ```
   === Corruption Check Results ===
   Total pages: 100
   Valid pages: 95
   Corrupted pages: 5
   Critical corruptions: 1
   Warning corruptions: 4
   ```

2. **JSON 报告** (`corruption_report.json`)：
   ```json
   {
     "file": "/path/to/table.ibd",
     "page_size": 16384,
     "total_pages": 100,
     "valid_pages": 95,
     "corrupted_pages": 5,
     "critical_corruptions": 1,
     "warning_corruptions": 4,
     "corruptions": [
       {
         "page_no": 10,
         "type": 2,
         "description": "Checksum mismatch",
         "severity": "warning"
       }
     ]
   }
   ```

## 坏块修复命令

### 基本用法
```bash
mysql_recovery_pro --ibd=/path/to/table.ibd --repair --output=/tmp/repair
```

### 功能说明
- 扫描文件中的所有页
- 检测并修复校验和错误
- 生成修复后的文件

### 输出
1. **控制台输出**：
   ```
   === Repair Results ===
   Total pages: 100
   Repaired pages: 5
   ```

2. **修复后的文件** (`repaired.ibd`)

## 组合使用示例

### 1. 先扫描后修复
```bash
# 第一步：扫描坏块
mysql_recovery_pro --ibd=/var/lib/mysql/test/users.ibd --check-corruption --output=/tmp/corruption

# 第二步：查看报告，了解损坏情况
cat /tmp/corruption/corruption_report.json

# 第三步：修复坏块
mysql_recovery_pro --ibd=/var/lib/mysql/test/users.ibd --repair --output=/tmp/repair
```

### 2. 扫描整个数据目录
```bash
# 扫描所有表的坏块
mysql_recovery_pro --data-dir=/var/lib/mysql --scan --output=/tmp/scanned

# 对每个表进行坏块检查
mysql_recovery_pro --ibd=/var/lib/mysql/test/users.ibd --check-corruption --output=/tmp/users_corruption
mysql_recovery_pro --ibd=/var/lib/mysql/test/orders.ibd --check-corruption --output=/tmp/orders_corruption
```

### 3. 批量修复
```bash
# 扫描数据目录
mysql_recovery_pro --data-dir=/var/lib/mysql --scan --output=/tmp/scanned

# 批量修复所有表
mysql_recovery_pro --data-dir=/var/lib/mysql --repair --bulk --output=/tmp/repair
```

## 修复模式说明

### 页类型损坏
- **检测**：页类型值不在有效范围内
- **修复**：需要手动干预，根据上下文推断正确类型

### 校验和不匹配
- **检测**：计算的校验和与存储的校验和不一致
- **修复**：自动重新计算并更新校验和

### 页链断裂
- **检测**：next_page 或 prev_page 指针无效
- **修复**：需要手动重建页链

### 记录损坏
- **检测**：记录头或记录链损坏
- **修复**：跳过损坏的记录，恢复其他有效记录

## 命令行选项

| 选项 | 描述 |
|------|------|
| `--ibd=<path>` | 要检查或修复的 .ibd 文件路径 |
| `--check-corruption` | 检查坏块模式 |
| `--repair` | 修复坏块模式 |
| `--output=<dir>` | 输出目录 |
| `--page-size=<size>` | 页大小 (4096/8192/16384/32768/65536) |

## 完整命令列表

```bash
# 基本恢复
mysql_recovery_pro --ibd=/path/to/table.ibd --output=/tmp/recovered

# Truncate 恢复
mysql_recovery_pro --ibd=/path/to/table.ibd --truncate-recovery --output=/tmp/recovered

# rm -rf 恢复
mysql_recovery_pro --ibd=/path/to/table.ibd --rmrf-recovery --output=/tmp/recovered

# 页头检查
mysql_recovery_pro --ibd=/path/to/table.ibd --check-headers --output=/tmp/headers

# 坏块扫描
mysql_recovery_pro --ibd=/path/to/table.ibd --check-corruption --output=/tmp/corruption

# 坏块修复
mysql_recovery_pro --ibd=/path/to/table.ibd --repair --output=/tmp/repair

# BLOB 恢复
mysql_recovery_pro --ibd=/path/to/table.ibd --blob-recovery --output=/tmp/recovered

# 导出为 SQL
mysql_recovery_pro --ibd=/path/to/table.ibd --format=sql --output=/tmp/recovered

# 导出为 IBD (修复后)
mysql_recovery_pro --ibd=/path/to/table.ibd --format=ibd --output=/tmp/repaired

# 扫描数据目录
mysql_recovery_pro --data-dir=/var/lib/mysql --scan --output=/tmp/scanned

# 搜索表名
mysql_recovery_pro --data-dir=/var/lib/mysql --table-name="users*" --search

# 批量恢复
mysql_recovery_pro --data-dir=/var/lib/mysql --rmrf-recovery --bulk --output=/tmp/recovered
```

## 注意事项

1. **备份原始文件**：在进行修复操作前，务必备份原始的 .ibd 文件
2. **校验和修复**：修复校验和后，MySQL 可能会再次检测到问题，需要重启或重新加载表
3. **页链修复**：页链断裂通常需要更复杂的修复过程，可能需要手动重建
4. **数据丢失**：严重损坏的页可能无法完全恢复，建议结合多个恢复模式尝试

## 故障排除

### 问题：修复后 MySQL 仍然报错
**解决方案**：
1. 检查 MySQL 错误日志
2. 尝试 `ANALYZE TABLE` 重新分析表
3. 必要时重建表索引

### 问题：无法修复某些坏块
**解决方案**：
1. 使用 `--rmrf-recovery` 模式尝试从碎片中恢复
2. 使用 `--truncate-recovery` 模式尝试恢复
3. 结合多个恢复模式，取最佳结果

### 问题：修复后的文件无法使用
**解决方案**：
1. 确保修复后的文件权限正确
2. 检查 MySQL 版本兼容性
3. 尝试在测试环境中先验证
