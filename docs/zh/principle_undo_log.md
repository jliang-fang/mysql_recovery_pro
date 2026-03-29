# InnoDB Undo Log 原理详解

## 1. 概述

Undo Log（回滚日志）是 InnoDB 存储引擎实现事务回滚和 MVCC（多版本并发控制）的核心机制。它记录了数据修改前的旧值。

## 2. Undo Log 基本概念

### 2.1 作用

- **事务回滚**：在事务回滚时，使用 Undo Log 恢复旧值
- **MVCC**：提供一致性读，不同事务可以看到不同版本的数据
- **崩溃恢复**：在崩溃恢复时，回滚未提交的事务

### 2.2 特性

- **先写 Undo Log**：修改数据前，先写 Undo Log
- **段式存储**：Undo Log 存储在 Undo Segment 中
- **循环使用**：Undo Log 页可以循环使用

## 3. Undo Log 结构

### 3.1 Undo Log Record

每条 Undo Log Record 包含：

| 字段 | 大小 | 描述 |
|------|------|------|
| u_trx_id | 8 bytes | 事务 ID |
| u_roll_ptr | 8 bytes | 回滚指针 |
| u_type | 2 bytes | 记录类型 |
| u_data | 变长 | 记录数据 |

### 3.2 Undo Log 类型

| 类型值 | 名称 | 描述 |
|--------|------|------|
| 1 | TRX_UNDO_INSERT | 插入记录 |
| 2 | TRX_UNDO_UPDATE | 更新记录 |
| 3 | TRX_UNDO_DEL_MARK | 删除标记 |

### 3.3 回滚指针（Roll Pointer）

回滚指针指向 Undo Log Record：

```
+------------------+------------------+
| TRX_ID (6B)      | ROLL_PTR (9B)    |
+------------------+------------------+
         |
         v
+------------------+
| Undo Log Record  |
+------------------+
```

## 4. Undo Log 格式

### 4.1 Undo Log Record Header

```
+--------+------------+-----------+----------+
| Type   | TRX_ID     | Roll Ptr  | Data     |
| (2B)   | (8B)       | (8B)      | (Var)    |
+--------+------------+-----------+----------+
```

### 4.2 Undo Log Record Data

不同类型的日志记录有不同的数据格式：

#### TRX_UNDO_INSERT（插入记录）

```
+----------------+
| Record Data    |
| (Var)          |
+----------------+
```

#### TRX_UNDO_UPDATE（更新记录）

```
+----------------+----------------+
| Old Record     | New Record     |
| (Var)          | (Var)          |
+----------------+----------------+
```

#### TRX_UNDO_DEL_MARK（删除标记）

```
+----------------+
| Record Data    |
| (Var)          |
+----------------+
```

## 5. Undo Log 回滚

### 5.1 回滚步骤

```cpp
1. 读取 Undo Log Record
2. 解析事务 ID 和回滚指针
3. 读取当前记录
4. 根据 Undo Log 类型执行回滚操作
5. 更新记录的回滚指针
```

### 5.2 回放示例

```cpp
// 解析 Undo Log Record
UndoLogRecord record = parse_undo_record(undo_data);

// 读取当前记录
auto current = read_record(record.trx_id, record.roll_ptr);

// 执行回滚
switch (record.type) {
    case TRX_UNDO_INSERT:
        // 删除新插入的记录
        delete_record(current);
        break;
    case TRX_UNDO_UPDATE:
        // 恢复旧值
        update_record(current, record.old_data);
        break;
    case TRX_UNDO_DEL_MARK:
        // 取消删除标记
        unmark_delete(current);
        break;
}
```

## 6. Undo Log 恢复要点

### 6.1 事务回滚

事务回滚的步骤：

1. **找到事务的 Undo Log**：通过事务 ID
2. **从最新到最旧**：按回滚指针链表回滚
3. **逐条回滚**：应用每条 Undo Log Record
4. **更新系统页**：更新事务系统页

### 6.2 MVCC 恢复

MVCC 恢复需要读取历史版本：

```cpp
// 1. 读取当前记录
auto current = read_record(record_id);

// 2. 通过回滚指针读取历史版本
while (current.roll_ptr != 0) {
    auto history = read_history_record(current.roll_ptr);
    
    // 3. 检查事务 ID
    if (history.trx_id < snapshot_trx_id) {
        return history;
    }
    
    current = history;
}
```

### 6.3 崩溃恢复

崩溃恢复时，回滚未提交的事务：

```cpp
// 1. 读取事务系统页
trx_sys = read_trx_sys_page();

// 2. 找到未提交的事务
for (auto& trx : trx_sys.active_transactions) {
    if (!trx.committed) {
        // 3. 回滚事务
        rollback_transaction(trx);
    }
}
```

## 7. Undo Log 损坏场景

| 场景 | 影响 | 恢复策略 |
|------|------|---------|
| Undo Log 页损坏 | 无法回滚 | 使用备份的 Undo Log |
| 回滚指针损坏 | 链表断裂 | 重建回滚指针链 |
| 事务 ID 错误 | 事务识别错误 | 验证事务 ID |

## 8. Undo Log 与 Redo Log 的区别

| 特性 | Redo Log | Undo Log |
|------|----------|----------|
| 作用 | 恢复未写入的数据 | 回滚事务 |
| 写入时机 | 事务提交前 | 事务开始时 |
| 存储位置 | ib_logfile* | ibdata1 |
| 循环使用 | 是 | 否 |
| 恢复类型 | 崩溃恢复 | 事务回滚 |

## 9. 实际案例

### 9.1 事务回滚

```cpp
// 1. 读取事务的 Undo Log
auto undo_logs = read_transaction_undo_logs(trx_id);

// 2. 从最新到最旧回滚
for (auto it = undo_logs.rbegin(); it != undo_logs.rend(); ++it) {
    apply_undo_log(*it);
}

// 3. 更新事务状态
update_transaction_status(trx_id, ROLLED_BACK);
```

### 9.2 MVCC 读取

```cpp
// 1. 创建快照
auto snapshot = create_snapshot();

// 2. 读取记录
auto record = read_record(record_id);

// 3. 检查可见性
if (is_visible(record, snapshot)) {
    return record;
}

// 4. 读取历史版本
while (record.roll_ptr != 0) {
    auto history = read_history_record(record.roll_ptr);
    if (is_visible(history, snapshot)) {
        return history;
    }
    record = history;
}
```

## 10. 参考资料

- MySQL 8.0 源码：`include/trx0trx.h`
- MySQL 8.0 源码：`include/trx0undo.h`
- InnoDB Undo Log 实现文档
