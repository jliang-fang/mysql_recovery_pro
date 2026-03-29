# InnoDB Redo Log 原理详解

## 1. 概述

Redo Log（重做日志）是 InnoDB 存储引擎实现持久性和崩溃恢复的核心机制。它记录了所有对数据页的修改操作。

## 2. Redo Log 基本概念

### 2.1 作用

- **持久性**：确保事务提交后的数据不会丢失
- **崩溃恢复**：在崩溃后重放日志，恢复未写入磁盘的数据
- **预写日志**：先写日志，再写数据（WAL）

### 2.2 特性

- **顺序写入**：Redo Log 是顺序写入的，性能高
- **循环使用**：Redo Log 文件循环使用
- **组提交**：多个事务可以组提交，减少 I/O

## 3. Redo Log 结构

### 3.1 Redo Log File

Redo Log 由一组日志文件组成（ib_logfile0, ib_logfile1, ...）：

```
+------------------+------------------+------------------+
|   ib_logfile0    |   ib_logfile1    |   ib_logfile2    |
+------------------+------------------+------------------+
```

每个日志文件包含：
- **Log Header**：日志文件头
- **Log Records**：日志记录
- **Log Checkpoint**：检查点信息

### 3.2 Redo Log Record

每条 Redo Log Record 包含：

| 字段 | 大小 | 描述 |
|------|------|------|
| r_type | 1 byte | 日志类型（MLOG_*） |
| r_space_id | 4 bytes | 表空间 ID |
| r_page_no | 4 bytes | 页号 |
| r_data | 变长 | 日志数据 |

### 3.3 Redo Log 类型

| 类型值 | 名称 | 描述 |
|--------|------|------|
| 10 | MLOG_REC_INSERT | 插入记录 |
| 11 | MLOG_REC_UPDATE | 更新记录 |
| 12 | MLOG_REC_DELETE | 删除记录 |
| 30 | MLOG_COMP_REC_INSERT | COMPACT 插入 |
| 31 | MLOG_COMP_REC_UPDATE | COMPACT 更新 |
| 32 | MLOG_COMP_REC_DELETE | COMPACT 删除 |
| 15 | MLOG_PAGE_CREATE | 创建页 |
| 16 | MLOG_PAGE_REINIT | 页重新初始化 |

## 4. Redo Log 格式

### 4.1 Redo Log Record Header

```
+--------+------------+-----------+----------+
| Type   | Space ID   | Page No   | Data     |
| (1B)   | (4B)       | (4B)      | (Var)    |
+--------+------------+-----------+----------+
```

### 4.2 Redo Log Record Data

不同类型的日志记录有不同的数据格式：

#### MLOG_REC_INSERT（插入记录）

```
+----------------+----------------+-----------+-----------+
| Field Offsets  | Field Values   | Header    | Next Rec  |
| (Var)          | (Var)          | (5B)      | (2B)      |
+----------------+----------------+-----------+-----------+
```

#### MLOG_REC_UPDATE（更新记录）

```
+----------------+----------------+-----------+-----------+
| Field Offsets  | Old Values     | New Values| Header    |
| (Var)          | (Var)          | (Var)     | (5B)      |
+----------------+----------------+-----------+-----------+
```

#### MLOG_PAGE_CREATE（创建页）

```
+----------------+
| Page Data      |
| (16KB)         |
+----------------+
```

## 5. Redo Log 回放

### 5.1 回放步骤

```cpp
1. 读取 Redo Log 文件
2. 解析每条 Redo Log Record
3. 读取对应的页
4. 应用日志记录到页
5. 更新页的 LSN
6. 重新计算校验和
```

### 5.2 回放示例

```cpp
// 解析 Redo Log Record
RedoLogRecord record = parse_redo_record(log_data);

// 读取页
auto page = read_page(record.r_space_id, record.r_page_no);

// 应用日志
switch (record.r_type) {
    case MLOG_REC_INSERT:
        apply_insert(page, record.r_data);
        break;
    case MLOG_REC_UPDATE:
        apply_update(page, record.r_data);
        break;
    case MLOG_REC_DELETE:
        apply_delete(page, record.r_data);
        break;
}

// 更新 LSN
page.lsn = record.lsn;

// 重新计算校验和
page.checksum = compute_checksum(page);
```

## 6. Redo Log 恢复要点

### 6.1 崩溃恢复

崩溃恢复的步骤：

1. **检查检查点**：找到最近的检查点
2. **读取 Redo Log**：从检查点开始读取 Redo Log
3. **回放日志**：将 Redo Log 应用到数据页
4. **验证完整性**：验证数据页的完整性

### 6.2 Truncate 恢复

Truncate 后，Redo Log 可能包含被删除的记录：

```cpp
// 1. 读取 Redo Log
auto logs = parse_redo_log("ib_logfile0");

// 2. 过滤出相关的记录
for (auto& log : logs) {
    if (log.r_space_id == target_space_id) {
        // 应用日志
        apply_redo_log(log);
    }
}
```

### 6.3 rm -rf 恢复

rm -rf 后，Redo Log 可能仍然存在：

```cpp
// 1. 读取 Redo Log
auto logs = parse_redo_log("ib_logfile0");

// 2. 重建页链
for (auto& log : logs) {
    if (log.r_type == MLOG_PAGE_CREATE) {
        // 创建新页
        create_page(log.r_space_id, log.r_page_no);
    }
}

// 3. 回放日志
for (auto& log : logs) {
    apply_redo_log(log);
}
```

## 7. Redo Log 损坏场景

| 场景 | 影响 | 恢复策略 |
|------|------|---------|
| 日志文件损坏 | 无法回放 | 使用备份的 Redo Log |
| 日志记录损坏 | 部分数据丢失 | 跳过损坏的记录 |
| LSN 不匹配 | 顺序错误 | 重新排序日志记录 |

## 8. Redo Log 与 Undo Log 的区别

| 特性 | Redo Log | Undo Log |
|------|----------|----------|
| 作用 | 恢复未写入的数据 | 回滚事务 |
| 写入时机 | 事务提交前 | 事务开始时 |
| 存储位置 | ib_logfile* | ibdata1 |
| 循环使用 | 是 | 否 |
| 恢复类型 | 崩溃恢复 | 事务回滚 |

## 9. 实际案例

### 9.1 事务提交

```cpp
// 1. 写入 Redo Log
write_redo_log(MLOG_REC_INSERT, space_id, page_no, record_data);

// 2. 刷盘
fsync(ib_logfile);

// 3. 写入数据页
write_page(space_id, page_no, page_data);

// 4. 刷盘
fsync(ibd_file);
```

### 9.2 崩溃恢复

```cpp
// 1. 读取检查点
checkpoint_lsn = read_checkpoint();

// 2. 读取 Redo Log
auto logs = read_redo_log_from(checkpoint_lsn);

// 3. 回放日志
for (auto& log : logs) {
    apply_redo_log(log);
}

// 4. 验证完整性
verify_data_integrity();
```

## 10. 参考资料

- MySQL 8.0 源码：`include/mtr0mtr.h`
- MySQL 8.0 源码：`include/log0log.h`
- InnoDB Redo Log 实现文档
