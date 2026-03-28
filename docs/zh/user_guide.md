# MySQL 8.0 数据恢复引擎 - 用户指南

## 1. 快速开始

### 1.1 编译构建

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

### 1.2 命令行工具

```bash
# 基本用法
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered

# 指定表 ID
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --table-id=12345

# 查看帮助
./mysql8_recover --help
```

### 1.3 GUI 工具

```bash
# 启动图形界面
cd build/gui
./mysql8_recovery_gui
```

## 2. 功能特性

### 2.1 页健康状态检查

```bash
# 检查页校验和
./mysql8_recover --ibd=/path/to/table.ibd --check-headers
```

### 2.2 Truncate 恢复

```bash
# 从 Truncate 的表中恢复数据
./mysql8_recover --ibd=/path/to/truncated.ibd --output=/tmp/recovered --truncate-recovery
```

### 2.3 BLOB 恢复

```bash
# 恢复包含 BLOB 字段的表
./mysql8_recover --ibd=/path/to/table_with_blob.ibd --output=/tmp/recovered --blob-recovery
```

### 2.4 Redo Log 回放

```bash
# 应用 Redo Log 进行前滚恢复
./mysql8_recover --ibd=/path/to/table.ibd --redo-log=/path/to/ib_logfile0 --output=/tmp/recovered
```

### 2.5 导出为多种格式

```bash
# 导出为 CSV
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --format=csv

# 导出为 SQL
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --format=sql

# 导出为 ibd (修复后的表空间)
./mysql8_recover --ibd=/path/to/corrupted.ibd --output=/tmp/repaired.ibd --format=ibd
```

## 3. 支持的 MySQL 版本

| MySQL 版本 | 行格式 | 校验和算法 | SDI | 备注 |
|------------|--------|------------|-----|------|
| 5.6        | REDUNDANT, COMPACT | INNODB, OLD | 无 | 基本支持 |
| 5.7        | REDUNDANT, COMPACT | INNODB, CRC32C | 无 | 基本支持 |
| 8.0        | REDUNDANT, COMPACT, DYNAMIC, COMPRESSED | CRC32C | 支持 | 完整支持 |
| 8.1+       | 所有 8.0 格式 | CRC32C | 支持 | 完整支持 |
| 9.0+       | 所有 8.0 格式 | CRC32C | 支持 | 预留支持 |

## 4. 支持的灾难场景

| 场景 | 恢复能力 | 备注 |
|------|----------|------|
| Truncate | ✅ 完整恢复 | 基于页链和 Redo Log |
| rm -rf | ✅ 部分恢复 | 需要未覆盖的数据页 |
| 文件头损坏 | ✅ 修复 | 重建文件头 |
| 表空间损坏 | ✅ 部分恢复 | 基于可用页 |
| Redo Log 应用 | ✅ 完整恢复 | 前滚恢复 |
| BLOB 损坏 | ✅ 部分恢复 | 基于页链 |

## 5. 输出格式

### 5.1 CSV 格式

```csv
id,name,email,created_at
1,John Doe,john@example.com,2024-01-01 10:00:00
2,Jane Smith,jane@example.com,2024-01-02 11:30:00
```

### 5.2 SQL 格式

```sql
-- Recovered from table: users
-- Engine: InnoDB
-- Row Format: DYNAMIC

CREATE TABLE `users` (
  `id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `name` varchar(255) NOT NULL,
  `email` varchar(255) DEFAULT NULL,
  `created_at` datetime DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

INSERT INTO `users` (`id`, `name`, `email`, `created_at`) VALUES
(1, 'John Doe', 'john@example.com', '2024-01-01 10:00:00'),
(2, 'Jane Smith', 'jane@example.com', '2024-01-02 11:30:00');
```

### 5.3 IBD 格式

修复后的表空间文件，可直接用于 MySQL 恢复。

## 6. 常见问题

### 6.1 无法读取文件

**问题**：`[ERROR] Cannot open file: /path/to/file.ibd`

**解决方案**：
1. 检查文件权限：`ls -l /path/to/file.ibd`
2. 确认文件存在：`file /path/to/file.ibd`
3. 检查文件是否被其他进程占用

### 6.2 校验和验证失败

**问题**：`[WARN] Page checksum validation failed`

**解决方案**：
1. 使用 `--no-checksum` 选项跳过校验和验证
2. 检查文件是否损坏
3. 尝试从备份恢复

### 6.3 SDI 解析失败

**问题**：`[WARN] Failed to parse SDI JSON`

**解决方案**：
1. 检查文件是否为有效的 InnoDB 表空间
2. 尝试从 ibdata 文件中提取 SDI
3. 使用 `--force` 选项强制恢复

### 6.4 BLOB 恢复失败

**问题**：`[WARN] Failed to reconstruct BLOB data`

**解决方案**：
1. 检查 BLOB 页链是否完整
2. 确认所有 BLOB 页都可访问
3. 尝试使用 `--partial-blob` 选项进行部分恢复

## 7. 高级选项

### 7.1 调试模式

```bash
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --debug
```

### 7.2 详细日志

```bash
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --verbose
```

### 7.3 自定义页大小

```bash
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --page-size=8192
```

支持的页大小：4096, 8192, 16384, 32768, 65536

### 7.4 指定行格式

```bash
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --row-format=compact
```

支持的行格式：compact, dynamic, redundant, compressed

## 8. 性能优化

### 8.1 多线程处理

```bash
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --threads=4
```

### 8.2 内存映射

默认启用内存映射以提高性能。

### 8.3 缓存优化

使用页缓存减少磁盘 I/O。

## 9. 贡献指南

### 9.1 代码风格

- 遵循 Google C++ Style Guide
- 使用 clang-format 进行格式化
- 添加单元测试

### 9.2 提交规范

```
type(scope): description

body

footer
```

类型：feat, fix, docs, style, refactor, test, chore

### 9.3 测试

```bash
# 运行所有测试
ctest --verbose

# 运行特定测试
./build/tests/test_page_header

# 生成测试数据
python3 tools/ibd_generator.py test_table
```

## 10. 许可证

MIT License
