# MySQL Recovery Pro - 极端场景恢复功能文档

## 概述

本文档描述了 MySQL Recovery Pro 工具实现的多种极端场景恢复功能。

## 实现的功能

### 1. 加密表恢复 (Encrypted Table Recovery)

**头文件**: `include/dbrecovery/innodb/encrypted_table_recovery.hpp`

**功能特性**:
- 自动检测加密页
- 支持 TDE (Tablespace Encryption) 解密
- 支持主密钥和表密钥
- 支持从 Keyring 文件加载密钥
- 无密钥恢复模式（尝试直接读取加密数据）

**使用示例**:
```cpp
dbrecovery::innodb::EncryptedTableRecovery recovery;
recovery.set_file_path("/path/to/encrypted.ibd");
recovery.set_schema(schema);
recovery.set_primary_key(primary_key);
recovery.add_table_key(key_id, table_key, algorithm);
auto records = recovery.recover();
```

### 2. 分区表恢复 (Partition Table Recovery)

**头文件**: `include/dbrecovery/innodb/partition_table_recovery.hpp`

**功能特性**:
- 自动扫描分区表文件
- 支持 MySQL 分区表命名格式 (`table_name#P#partition_name.ibd`)
- 批量恢复所有分区
- 每个分区独立统计

**使用示例**:
```cpp
dbrecovery::innodb::PartitionTableRecovery recovery;
recovery.set_data_directory("/var/lib/mysql");
recovery.set_table_name("users");
recovery.set_schema(schema);
recovery.scan_partitions();
auto records = recovery.recover();
```

### 3. 压缩表恢复 (Compressed Table Recovery)

**头文件**: `include/dbrecovery/innodb/compressed_table_recovery.hpp`

**功能特性**:
- 自动检测压缩页
- 支持 zlib 解压缩
- 支持多种压缩页大小
- 从页头解析压缩信息

**使用示例**:
```cpp
dbrecovery::innodb::CompressedTableRecovery recovery;
recovery.set_file_path("/path/to/compressed.ibd");
recovery.set_schema(schema);
recovery.set_compressed_page_size(8192);
auto records = recovery.recover();
```

### 4. 碎片化数据恢复 (Fragmented Data Recovery)

**头文件**: `include/dbrecovery/innodb/fragmented_data_recovery.hpp`

**功能特性**:
- 扫描所有页并检测碎片化
- 重建页链（处理 orphaned 页）
- 支持跨文件碎片恢复
- 统计 orphaned 页数量

**使用示例**:
```cpp
dbrecovery::innodb::FragmentedDataRecovery recovery;
recovery.set_file_path("/path/to/fragmented.ibd");
recovery.set_schema(schema);
recovery.set_try_reconstruct_chains(true);
auto records = recovery.recover();
```

### 5. 多版本兼容恢复 (Multi-Version Recovery)

**头文件**: `include/dbrecovery/innodb/multi_version_recovery.hpp`

**功能特性**:
- 自动检测 MySQL 版本 (5.5/5.6/5.7/8.0/8.1/8.2/8.3/8.4/8.5/9.0)
- 根据版本选择合适的解析器
- 支持强制指定版本
- 从 LSN、页头、flags 等信息推断版本

**支持的版本**:
- MySQL 5.5/5.6/5.7 - 使用 PageParserV2
- MySQL 8.0/8.1/8.2/8.3 - 使用 PageParserV82
- MySQL 8.4/8.5/9.0 - 使用 PageParserV3

**使用示例**:
```cpp
dbrecovery::innodb::MultiVersionRecovery recovery;
recovery.set_file_path("/path/to/table.ibd");
recovery.set_schema(schema);
recovery.set_auto_detect_version(true);
// 或 recovery.set_force_version(MySQLVersion::MYSQL_80);
auto records = recovery.recover();
```

### 6. 内存映射快速恢复 (MMap Recovery)

**头文件**: `include/dbrecovery/innodb/mmap_recovery.hpp`

**功能特性**:
- 使用 mmap() 映射文件到内存
- 零拷贝数据访问
- 支持大文件快速扫描
- 使用 madvise() 优化预读

**使用示例**:
```cpp
dbrecovery::innodb::MMapRecovery recovery;
recovery.set_file_path("/path/to/large.ibd");
recovery.set_schema(schema);
auto records = recovery.recover();
```

### 7. 增量恢复 (Incremental Recovery)

**头文件**: `include/dbrecovery/innodb/incremental_recovery.hpp`

**功能特性**:
- 从 Redo Log 恢复
- 支持 LSN 范围恢复
- 支持页号范围恢复
- 统计恢复的页和记录

**使用示例**:
```cpp
dbrecovery::innodb::IncrementalRecovery recovery;
recovery.set_file_path("/path/to/table.ibd");
recovery.set_schema(schema);
recovery.set_recovery_range(start_lsn, end_lsn);
auto records = recovery.recover();
```

### 8. 并行恢复 (Parallel Recovery)

**头文件**: `include/dbrecovery/innodb/parallel_recovery.hpp`

**功能特性**:
- 多线程页扫描
- 多线程记录解析
- 多线程 B+树遍历
- 自动线程池管理

**使用示例**:
```cpp
dbrecovery::innodb::ParallelRecoveryEngine recovery;
recovery.set_file_path("/path/to/table.ibd");
recovery.set_schema(schema);
recovery.set_max_threads(8);
auto records = recovery.recover();
```

## 统计信息

所有恢复器都提供详细的统计信息：

```cpp
auto stats = recovery.get_stats();
std::cout << "Total pages: " << stats.total_pages << std::endl;
std::cout << "Recovered records: " << stats.recovered_records << std::endl;
std::cout << "Time: " << stats.total_time_ms << " ms" << std::endl;
```

## 使用场景

### 场景 1: rm -rf 灾难恢复

```bash
# 扫描整个数据目录
./mysql_recovery_pro --data-dir=/var/lib/mysql --scan --output=/tmp/scanned

# 批量恢复所有表
./mysql_recovery_pro --data-dir=/var/lib/mysql --bulk --output=/tmp/recovered --format=csv
```

### 场景 2: 加密表恢复

```cpp
// 在代码中使用
EncryptedTableRecovery recovery;
recovery.set_file_path("/path/to/encrypted.ibd");
recovery.set_primary_key(primary_key);
recovery.add_table_key(key_id, table_key, TDEAlgorithm::AES_256_CBC);
auto records = recovery.recover();
```

### 场景 3: 分区表恢复

```cpp
PartitionTableRecovery recovery;
recovery.set_data_directory("/var/lib/mysql");
recovery.set_table_name("orders");
recovery.scan_partitions();
auto records = recovery.recover();
```

### 场景 4: 大文件快速恢复

```cpp
MMapRecovery recovery;
recovery.set_file_path("/path/to/large.ibd");
recovery.set_schema(schema);
auto records = recovery.recover();
```

### 场景 5: 多版本兼容

```cpp
MultiVersionRecovery recovery;
recovery.set_file_path("/path/to/table.ibd");
recovery.set_auto_detect_version(true);
auto records = recovery.recover();
```

## 输出格式

### 批量恢复输出结构

```
output_dir/
├── global_recovery_summary.txt    # 全局汇总报告
├── recovery_summary.json          # JSON 格式汇总
└── database_table_timestamp/
    ├── table_info.json            # 表信息（包含源路径、页统计等）
    ├── table_name_recovered.csv   # 恢复的 CSV 数据
    ├── table_name_recovered.sql   # 恢复的 SQL 数据
    └── recovery_summary.txt       # 表恢复摘要
```

### 统计信息字段

| 字段 | 类型 | 说明 |
|------|------|------|
| total_pages | size_t | 总页数 |
| scanned_pages | size_t | 扫描的页数 |
| parsed_pages | size_t | 解析的页数 |
| recovered_records | size_t | 恢复的记录数 |
| failed_pages | size_t | 失败的页数 |
| total_time_ms | double | 总耗时（毫秒） |
| detected_version | MySQLVersion | 检测到的 MySQL 版本 |

## 性能优化

1. **内存映射**: 使用 mmap() 避免文件 I/O 开销
2. **多线程**: 并行扫描和解析
3. **智能缓存**: 页缓存和结果缓存
4. **增量恢复**: 只恢复修改的页

## 错误处理

所有恢复器都提供错误处理：

```cpp
if (!recovery.recover()) {
    std::cerr << "Recovery failed" << std::endl;
    auto stats = recovery.get_stats();
    std::cerr << "Failed pages: " << stats.failed_pages << std::endl;
}
```

## 后续改进

1. **BLOB 页链重建**: 完善 BLOB 页链的重建逻辑
2. **Undo Log 恢复**: 支持从 undo log 中恢复数据
3. **Redo Log 前滚**: 实现 redo log 前滚恢复
4. **GUI 界面**: 完善 GUI 界面支持
5. **Web API**: 提供 RESTful API 接口
