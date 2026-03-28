# Undo Log 原理与实现

## 1. 概述

Undo Log（撤销日志）是 InnoDB 实现事务回滚（Rollback）和多版本并发控制（MVCC）的核心机制。它记录了数据修改前的旧版本，用于：
- 事务回滚：撤销未提交的事务
- MVCC：提供一致性读视图
- 崩溃恢复：配合Redo Log确保数据一致性

## 2. Undo Log 结构

### 2.1 Undo Log 页

Undo Log 存储在专门的 Undo Log 页中，页类型为 `FIL_PAGE_UNDO_LOG (9970)`。

```
┌─────────────────────────────────────────────────────┐
│  FIL Header (38 bytes)                              │
├─────────────────────────────────────────────────────┤
│  Undo Log Header (34 bytes)                         │
│  - TRX_ID (6 bytes)                                 │
│  - TRX_NO (8 bytes)                                 │
│  - ROLL_PTR (7 bytes)                               │
│  - INFO_BITS (1 byte)                               │
│  - STATE (1 byte)                                   │
│  - START_OFFSET (2 bytes)                           │
│  - PREV_UNDO (2 bytes)                              │
│  - NEXT_UNDO (2 bytes)                              │
│  - LEN (2 bytes)                                    │
│  - RECS (variable)                                  │
├─────────────────────────────────────────────────────┤
│  Undo Log Records...                                │
└─────────────────────────────────────────────────────┘
```

### 2.2 Undo Log Record 结构

```
┌─────────────────────────────────────────────────────┐
│  TRX_ID (6 bytes)                                   │
├─────────────────────────────────────────────────────┤
│  TRX_NO (8 bytes)                                   │
├─────────────────────────────────────────────────────┤
│  ROLL_PTR (7 bytes)                                 │
│  - BIT 1: DELETE (1 bit)                            │
│  - BIT 2-7: PAGE_NO (6 bytes)                       │
├─────────────────────────────────────────────────────┤
│  TYPE (1 byte)                                      │
│  - TRX_UNDO_INSERT (1)                              │
│  - TRX_UNDO_UPDATE (2)                              │
├─────────────────────────────────────────────────────┤
│  DATA...                                            │
└─────────────────────────────────────────────────────┘
```

### 2.3 Undo Log 类型

| 类型值 | 类型名称 | 说明 |
|--------|----------|------|
| 1 | TRX_UNDO_INSERT | 插入操作 |
| 2 | TRX_UNDO_UPDATE | 更新操作 |

## 3. Undo Log 解析

### 3.1 Undo Log Record 结构

```cpp
struct UndoLogRecord {
    uint64_t u_trx_id;        // 事务ID
    uint64_t u_trx_no;        // 事务编号
    uint64_t u_roll_ptr;      // 回滚指针
    uint16_t u_type;          // 类型
    std::vector<uint8_t> u_data; // 记录数据
    
    std::string to_string() const {
        return "UndoLogRecord{trx_id=" + std::to_string(u_trx_id) +
               ", trx_no=" + std::to_string(u_trx_no) +
               ", roll_ptr=" + std::to_string(u_roll_ptr) +
               ", type=" + std::to_string(u_type) +
               ", data_len=" + std::to_string(u_data.size()) + "}";
    }
};
```

### 3.2 解析实现

```cpp
std::optional<UndoLogRecord> UndoLogParser::parse_record(const uint8_t* data, size_t len) {
    if (!data || len < 20) {
        return std::nullopt;
    }
    
    UndoLogRecord record;
    size_t pos = 0;
    
    // 解析事务ID (6 bytes)
    record.u_trx_id = read_le<uint64_t>(data, pos);
    pos += 8;
    
    // 解析事务编号 (8 bytes)
    record.u_trx_no = read_le<uint64_t>(data, pos);
    pos += 8;
    
    // 解析回滚指针 (7 bytes)
    record.u_roll_ptr = read_le<uint64_t>(data, pos);
    pos += 8;
    
    // 解析类型 (1 byte)
    record.u_type = read_le<uint16_t>(data, pos);
    pos += 2;
    
    // 解析剩余数据作为记录数据
    if (pos < len) {
        record.u_data.assign(data + pos, data + len);
    }
    
    return record;
}
```

### 3.3 Undo Log 文件解析

```cpp
std::vector<UndoLogRecord> UndoLogParser::parse_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Failed to open undo log file: " << path << std::endl;
        return {};
    }
    
    // 读取文件大小
    file.seekg(0, std::ios::end);
    size_t file_size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    
    if (file_size == 0) {
        std::cerr << "[WARN] Undo log file is empty: " << path << std::endl;
        return {};
    }
    
    // 读取整个文件
    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    file.close();
    
    // 解析所有记录
    std::vector<UndoLogRecord> records;
    size_t pos = 0;
    
    while (pos + 20 <= buffer.size()) {
        auto record_opt = parse_record(buffer.data() + pos, buffer.size() - pos);
        if (!record_opt.has_value()) {
            break;
        }
        
        const auto& record = record_opt.value();
        records.push_back(record);
        
        // 移动到下一个记录
        pos += 20 + record.u_data.size();
    }
    
    return records;
}
```

## 4. Undo Log 回滚

### 4.1 回滚流程

```cpp
bool UndoLogParser::rollback_transaction(uint64_t trx_id) {
    // 1. 读取事务的Undo Log
    auto records = parse_transaction_undo_log(trx_id);
    if (records.empty()) {
        return false;
    }
    
    // 2. 反向应用Undo Log（从最新到最旧）
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        if (!apply_undo_record(*it)) {
            std::cerr << "Failed to apply undo record for transaction " << trx_id << std::endl;
            return false;
        }
    }
    
    return true;
}
```

### 4.2 Undo 记录应用

```cpp
bool UndoLogParser::apply_undo_record(const UndoLogRecord& record) {
    switch (record.u_type) {
        case TRX_UNDO_INSERT:
            // 插入操作的回滚：删除记录
            return rollback_insert(record);
            
        case TRX_UNDO_UPDATE:
            // 更新操作的回滚：恢复旧值
            return rollback_update(record);
            
        default:
            std::cerr << "Unknown undo type: " << record.u_type << std::endl;
            return false;
    }
}
```

### 4.3 插入回滚

```cpp
bool UndoLogParser::rollback_insert(const UndoLogRecord& record) {
    // 1. 读取目标页
    auto page_data = read_page(record.u_roll_ptr);
    if (!page_data) {
        return false;
    }
    
    // 2. 解析页
    BPlusTreeParser parser(page_data.value());
    parser.parse();
    
    // 3. 删除记录
    // 根据Undo Log中的信息定位并删除记录
    if (!delete_record(parser, record)) {
        return false;
    }
    
    // 4. 更新页
    write_page(record.u_roll_ptr, page_data.value());
    
    return true;
}
```

### 4.4 更新回滚

```cpp
bool UndoLogParser::rollback_update(const UndoLogRecord& record) {
    // 1. 读取目标页
    auto page_data = read_page(record.u_roll_ptr);
    if (!page_data) {
        return false;
    }
    
    // 2. 解析页
    BPlusTreeParser parser(page_data.value());
    parser.parse();
    
    // 3. 找到记录
    auto record_opt = find_record(parser, record.u_trx_id);
    if (!record_opt) {
        return false;
    }
    
    // 4. 恢复旧值
    auto& target_record = record_opt.value();
    if (!apply_field_update(target_record.data, target_record.size, 
                             record.u_data)) {
        return false;
    }
    
    // 5. 更新页
    write_page(record.u_roll_ptr, page_data.value());
    
    return true;
}
```

## 5. MVCC 支持

### 5.1 一致性读

```cpp
std::optional<Record> RecoveryEngine::consistent_read(
    uint64_t table_id,
    uint64_t trx_id,
    uint64_t view_lsn) {
    
    // 1. 读取最新记录
    auto record = read_latest_record(table_id);
    if (!record) {
        return std::nullopt;
    }
    
    // 2. 检查可见性
    if (!is_visible(record.value(), trx_id, view_lsn)) {
        // 3. 查找旧版本
        return read_old_version(record.value(), trx_id, view_lsn);
    }
    
    return record;
}
```

### 5.2 旧版本查找

```cpp
std::optional<Record> RecoveryEngine::read_old_version(
    const Record& current_record,
    uint64_t trx_id,
    uint64_t view_lsn) {
    
    // 1. 读取Roll Pointer
    uint64_t roll_ptr = current_record.roll_ptr;
    
    // 2. 遍历Undo Log链
    while (roll_ptr != 0) {
        // 3. 读取Undo Log记录
        auto undo_record = read_undo_record(roll_ptr);
        if (!undo_record) {
            break;
        }
        
        // 4. 检查可见性
        if (undo_record->u_trx_no < view_lsn) {
            // 5. 恢复旧版本
            return restore_record(undo_record.value());
        }
        
        // 6. 移动到下一个Undo Log
        roll_ptr = undo_record->u_roll_ptr;
    }
    
    return std::nullopt;
}
```

## 6. 实际应用

### 6.1 事务回滚

```cpp
bool RecoveryEngine::rollback(uint64_t trx_id) {
    // 1. 读取事务的Undo Log
    auto undo_records = parse_transaction_undo_log(trx_id);
    if (undo_records.empty()) {
        std::cerr << "No undo log found for transaction " << trx_id << std::endl;
        return false;
    }
    
    std::cout << "Rolling back transaction " << trx_id 
              << " with " << undo_records.size() << " undo records" << std::endl;
    
    // 2. 反向应用Undo Log
    for (auto it = undo_records.rbegin(); it != undo_records.rend(); ++it) {
        std::cout << "Applying undo record: " << it->to_string() << std::endl;
        
        if (!apply_undo_record(*it)) {
            std::cerr << "Failed to apply undo record" << std::endl;
            return false;
        }
    }
    
    std::cout << "Transaction " << trx_id << " rolled back successfully" << std::endl;
    return true;
}
```

### 6.2 MVCC 查询

```cpp
std::vector<std::map<std::string, std::string>> 
RecoveryEngine::mvcc_query(uint64_t table_id, uint64_t trx_id) {
    std::vector<std::map<std::string, std::string>> results;
    
    // 1. 读取表的所有记录
    auto records = read_table_records(table_id);
    
    // 2. 过滤可见记录
    for (const auto& record : records) {
        if (is_visible(record, trx_id)) {
            results.push_back(record.values);
        }
    }
    
    return results;
}
```

### 6.3 崩溃恢复

```cpp
bool RecoveryEngine::crash_recovery() {
    // 1. 读取Redo Log
    auto redo_records = parse_redo_log();
    
    // 2. 应用Redo Log
    apply_redo_log(redo_records);
    
    // 3. 读取Undo Log
    auto undo_records = parse_undo_log();
    
    // 4. 回滚未提交事务
    for (const auto& record : undo_records) {
        if (!is_committed(record.u_trx_id)) {
            rollback(record.u_trx_id);
        }
    }
    
    return true;
}
```

## 7. 参考资料

- MySQL 8.0 源码：`include/trx0rec.h`, `include/trx0undo.h`
- Undo Log 机制：https://dev.mysql.com/doc/refman/8.0/en/innodb-undo-logs.html
- MVCC 机制：https://dev.mysql.com/doc/refman/8.0/en/innodb-multi-versioning.html
