# Redo Log 原理与实现

## 1. 概述

Redo Log（重做日志）是 InnoDB 实现崩溃恢复（Crash Recovery）的核心机制。它记录了所有对数据页的修改，确保在系统崩溃后能够恢复未持久化的数据。

## 2. Redo Log 结构

### 2.1 Redo Log 文件

Redo Log 存储在 `ib_logfile0` 和 `ib_logfile1` 中，采用循环写入方式。

```
┌─────────────────────────────────────────────────────┐
│  Log Header (512 bytes)                             │
├─────────────────────────────────────────────────────┤
│  Log Records...                                     │
└─────────────────────────────────────────────────────┘
```

### 2.2 Redo Log Record 结构

```
┌─────────────────────────────────────────────────────┐
│  MLOG_SINGLE_REC_FLAG (1 bit)                       │
│  MLOG_MULTI_REC_FLAG (1 bit)                        │
│  MLOG_CHECKPOINT_FLAG (1 bit)                       │
│  MLOG_DIRTY_FLAG (1 bit)                            │
│  MLOG_MULTI_FLAG (1 bit)                            │
│  MLOG_COMPRESSED_FLAG (1 bit)                       │
│  MLOG_BIG_REC_FLAG (1 bit)                          │
│  MLOG_LARGE_REC_FLAG (1 bit)                        │
│  MLOG_TYPE (5 bits)                                 │
├─────────────────────────────────────────────────────┤
│  Space ID (4 bytes)                                 │
├─────────────────────────────────────────────────────┤
│  Page No (4 bytes)                                  │
├─────────────────────────────────────────────────────┤
│  Data...                                            │
└─────────────────────────────────────────────────────┘
```

### 2.3 Redo Log 类型

| 类型值 | 类型名称 | 说明 |
|--------|----------|------|
| 1 | MLOG_1BYTE | 单字节写入 |
| 2 | MLOG_2BYTES | 双字节写入 |
| 3 | MLOG_4BYTES | 四字节写入 |
| 4 | MLOG_8BYTES | 八字节写入 |
| 10 | MLOG_REC_INSERT | 插入记录 |
| 11 | MLOG_REC_UPDATE | 更新记录 |
| 12 | MLOG_REC_DELETE | 删除记录 |
| 15 | MLOG_PAGE_CREATE | 创建页 |
| 16 | MLOG_PAGE_REINIT | 页重新初始化 |
| 30 | MLOG_COMP_REC_INSERT | COMPACT格式插入 |
| 31 | MLOG_COMP_REC_UPDATE | COMPACT格式更新 |
| 32 | MLOG_COMP_REC_DELETE | COMPACT格式删除 |
| 33 | MLOG_COMP_PAGE_CREATE | COMPACT格式创建页 |
| 34 | MLOG_COMP_PAGE_REINIT | COMPACT格式页重新初始化 |
| 40 | MLOG_BLOB_WRITE | BLOB写入 |
| 50 | MLOG_MULTI_REC_END | 多记录结束 |
| 51 | MLOG_CHECKPOINT | 检查点 |
| 52 | MLOG_INDEX_LOAD | 索引加载 |

## 3. Redo Log 解析

### 3.1 Redo Log Record 结构

```cpp
struct RedoLogRecord {
    uint16_t r_type;          // 日志类型 (MLOG_*)
    uint32_t r_space_id;      // 表空间ID
    uint32_t r_page_no;       // 页号
    std::vector<uint8_t> r_data; // 原始日志数据
    
    // 可选事务信息
    uint64_t r_trx_id = 0;
    uint64_t r_roll_ptr = 0;
};
```

### 3.2 解析实现

```cpp
std::optional<RedoLogRecord> RedoLogParser::parse_record(const uint8_t* data, size_t len) {
    if (!data || len < 5) {
        return std::nullopt;
    }
    
    RedoLogRecord record;
    size_t pos = 0;
    
    // 解析日志类型 (1 byte)
    record.r_type = read_le<uint8_t>(data, pos);
    pos += sizeof(uint8_t);
    
    // 解析空间ID和页号 (各4 bytes)
    if (pos + 8 <= len) {
        record.r_space_id = read_le<uint32_t>(data, pos);
        pos += sizeof(uint32_t);
        record.r_page_no = read_le<uint32_t>(data, pos);
        pos += sizeof(uint32_t);
    }
    
    // 解析剩余数据作为日志内容
    if (pos < len) {
        record.r_data.assign(data + pos, data + len);
    }
    
    return record;
}
```

### 3.3 Redo Log 类型解析

```cpp
void RedoLogParser::parse_record_type(const RedoLogRecord& record) {
    switch (record.r_type) {
        case MLOG_1BYTE:
            // 单字节写入
            parse_mlog_1byte(record);
            break;
            
        case MLOG_2BYTES:
            // 双字节写入
            parse_mlog_2bytes(record);
            break;
            
        case MLOG_4BYTES:
            // 四字节写入
            parse_mlog_4bytes(record);
            break;
            
        case MLOG_8BYTES:
            // 八字节写入
            parse_mlog_8bytes(record);
            break;
            
        case MLOG_COMP_REC_INSERT:
            // COMPACT格式插入
            parse_comp_rec_insert(record);
            break;
            
        case MLOG_COMP_REC_UPDATE:
            // COMPACT格式更新
            parse_comp_rec_update(record);
            break;
            
        case MLOG_COMP_REC_DELETE:
            // COMPACT格式删除
            parse_comp_rec_delete(record);
            break;
            
        case MLOG_PAGE_CREATE:
            // 页创建
            parse_page_create(record);
            break;
            
        case MLOG_PAGE_REINIT:
            // 页重新初始化
            parse_page_reinit(record);
            break;
            
        case MLOG_BLOB_WRITE:
            // BLOB写入
            parse_blob_write(record);
            break;
            
        default:
            // 未知类型
            std::cerr << "Unknown log type: " << record.r_type << std::endl;
    }
}
```

## 4. Redo Log 回放

### 4.1 回放流程

```cpp
bool RedoLogParser::apply_redo_log(const std::vector<RedoLogRecord>& records) {
    // 按LSN顺序排序
    std::sort(records.begin(), records.end(),
              [](const auto& a, const auto& b) {
                  return a.r_lsn < b.r_lsn;
              });
    
    // 应用每个记录
    for (const auto& record : records) {
        // 1. 读取目标页
        auto page_data = read_page(record.r_space_id, record.r_page_no);
        if (!page_data) {
            std::cerr << "Failed to read page " << record.r_space_id << ":" 
                      << record.r_page_no << std::endl;
            continue;
        }
        
        // 2. 应用日志
        if (!apply_redo_record(page_data.value(), record)) {
            std::cerr << "Failed to apply redo record" << std::endl;
            continue;
        }
        
        // 3. 更新校验和
        update_page_checksum(page_data.value());
        
        // 4. 写回页
        write_page(record.r_space_id, record.r_page_no, page_data.value());
    }
    
    return true;
}
```

### 4.2 Redo 记录应用

```cpp
bool RedoLogParser::apply_redo_record(std::vector<uint8_t>& page_data, 
                                       const RedoLogRecord& record) {
    switch (record.r_type) {
        case MLOG_1BYTE:
            return apply_mlog_1byte(page_data, record);
            
        case MLOG_2BYTES:
            return apply_mlog_2bytes(page_data, record);
            
        case MLOG_4BYTES:
            return apply_mlog_4bytes(page_data, record);
            
        case MLOG_8BYTES:
            return apply_mlog_8bytes(page_data, record);
            
        case MLOG_COMP_REC_INSERT:
            return apply_comp_rec_insert(page_data, record);
            
        case MLOG_COMP_REC_UPDATE:
            return apply_comp_rec_update(page_data, record);
            
        case MLOG_COMP_REC_DELETE:
            return apply_comp_rec_delete(page_data, record);
            
        default:
            return false;
    }
}
```

### 4.3 字段更新

```cpp
bool BPlusTreeParser::apply_field_update(
    uint8_t* record_data, 
    size_t record_size,
    const std::vector<std::pair<uint16_t, std::vector<uint8_t>>>& updates) {
    
    if (!record_data || updates.empty()) return false;
    
    for (const auto& [field_no, new_value] : updates) {
        std::optional<uint16_t> opt_offset;
        
        // 根据行格式计算字段偏移
        if (btr_format_ == row_formats::REDUNDANT) {
            opt_offset = calculate_redundant_field_offset(
                record_data, record_size, field_no);
        } else if (is_compact_or_dynamic_format()) {
            opt_offset = calculate_dynamic_field_offset(
                record_data, record_size, field_no, true);
        }
        
        if (!opt_offset.has_value()) {
            continue;
        }
        
        size_t target_pos = opt_offset.value();
        
        // 边界检查
        if (target_pos >= record_size) {
            continue;
        }
        
        size_t end_pos = target_pos + new_value.size();
        if (end_pos > record_size) {
            // 截断更新
            size_t copy_size = record_size - target_pos;
            std::memcpy(record_data + target_pos, new_value.data(), copy_size);
        } else {
            std::memcpy(record_data + target_pos, new_value.data(), new_value.size());
        }
    }
    
    return true;
}
```

## 5. Redo Log 检查点

### 5.1 检查点结构

```
┌─────────────────────────────────────────────────────┐
│  MLOG_CHECKPOINT (type = 51)                        │
├─────────────────────────────────────────────────────┤
│  Checkpoint LSN (8 bytes)                           │
├─────────────────────────────────────────────────────┤
│  Log Group Number (4 bytes)                         │
├─────────────────────────────────────────────────────┤
│  Checkpoint No (4 bytes)                            │
└─────────────────────────────────────────────────────┘
```

### 5.2 检查点解析

```cpp
struct Checkpoint {
    uint64_t checkpoint_lsn;      // 检查点LSN
    uint32_t log_group_number;    // 日志组号
    uint32_t checkpoint_no;       // 检查点编号
};

std::optional<Checkpoint> RedoLogParser::parse_checkpoint(const RedoLogRecord& record) {
    if (record.r_type != MLOG_CHECKPOINT) {
        return std::nullopt;
    }
    
    if (record.r_data.size() < 16) {
        return std::nullopt;
    }
    
    Checkpoint checkpoint;
    size_t pos = 0;
    
    checkpoint.checkpoint_lsn = read_le<uint64_t>(record.r_data.data(), pos);
    pos += 8;
    
    checkpoint.log_group_number = read_le<uint32_t>(record.r_data.data(), pos);
    pos += 4;
    
    checkpoint.checkpoint_no = read_le<uint32_t>(record.r_data.data(), pos);
    
    return checkpoint;
}
```

## 6. 实际应用

### 6.1 崩溃恢复

```cpp
bool RecoveryEngine::crash_recovery(const std::string& ibd_path,
                                     const std::string& redo_log_path) {
    // 1. 读取Redo Log
    auto records = parse_redo_log(redo_log_path);
    if (records.empty()) {
        return false;
    }
    
    // 2. 找到最后检查点
    uint64_t last_checkpoint_lsn = find_last_checkpoint(records);
    
    // 3. 过滤检查点之后的记录
    auto filtered_records = filter_records_after_checkpoint(
        records, last_checkpoint_lsn);
    
    // 4. 应用Redo Log
    return apply_redo_log(filtered_records);
}
```

### 6.2 前滚恢复

```cpp
bool RecoveryEngine::forward_recovery(const std::string& ibd_path,
                                       const std::string& redo_log_path) {
    // 1. 读取Redo Log
    auto records = parse_redo_log(redo_log_path);
    
    // 2. 按LSN排序
    std::sort(records.begin(), records.end(),
              [](const auto& a, const auto& b) {
                  return a.r_lsn < b.r_lsn;
              });
    
    // 3. 应用所有记录
    for (const auto& record : records) {
        apply_redo_record(ibd_path, record);
    }
    
    return true;
}
```

### 6.3 事务回滚

```cpp
bool RecoveryEngine::rollback_transaction(uint64_t trx_id) {
    // 1. 读取Undo Log
    auto undo_records = parse_undo_log(trx_id);
    
    // 2. 反向应用Undo Log
    for (auto it = undo_records.rbegin(); it != undo_records.rend(); ++it) {
        apply_undo_record(*it);
    }
    
    return true;
}
```

## 7. 参考资料

- MySQL 8.0 源码：`include/mtr0mtr.h`, `include/log0log.h`
- Redo Log 机制：https://dev.mysql.com/doc/refman/8.0/en/innodb-redo-log.html
