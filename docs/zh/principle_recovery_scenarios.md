# MySQL 8.x 数据恢复引擎 - 灾难场景恢复原理

## 1. 概述

本文档详细说明 MySQL 8.x 数据恢复引擎支持的各类灾难场景及其恢复原理。

## 2. Truncate 恢复原理

### 2.1 场景描述

当执行 `TRUNCATE TABLE` 命令时，InnoDB 执行以下操作：
1. 释放所有数据页（标记为未分配）
2. 重置 B+树（创建新的空树）
3. 更新 SDI 中的元数据
4. 释放段空间

### 2.2 恢复原理

尽管 `TRUNCATE` 标记页为未分配，但**实际数据并未被覆盖**，仍保留在磁盘上。恢复原理如下：

```
原始状态:
+----------+     +----------+     +----------+
|  Page 0  | --> |  Page 1  | --> |  Page 2  |
| (Root)   |     | (Leaf)   |     | (Leaf)   |
| Records  |     | Records  |     | Records  |
+----------+     +----------+     +----------+

Truncate 后:
+----------+     +----------+     +----------+
|  Page 0  |     |  Page 1  |     |  Page 2  |
| (New)    |     | (Unused) |     | (Unused) |
| Empty    |     | Free     |     | Free     |
+----------+     +----------+     +----------+

数据仍存在于物理页中（未被覆盖）:
+----------+     +----------+     +----------+
|  Page 0  |     |  Page 1  |     |  Page 2  |
| (New)    |     | [OLD]    |     | [OLD]    |
| Empty    |     | Records  |     | Records  |
+----------+     +----------+     +----------+
```

### 2.3 恢复步骤

1. **扫描所有页**：遍历整个表空间，读取所有页数据
2. **页类型识别**：识别 B+树索引页（FIL_PAGE_INDEX=9969）
3. **页链重建**：通过 FIL_PAGE_PREV/FIL_PAGE_NEXT 构建页链
4. **记录解析**：解析 INFIMUM/SUPREMUM 之间的用户记录
5. **BLOB 链重建**：对于 BLOB 字段，重建 BLOB 页链
6. **元数据提取**：从 SDI 页提取表结构

### 2.4 关键代码路径

```cpp
// recovery/engine.cpp - recover_truncate()
for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
    auto page_data = file_reader_->read_page(page_no);
    
    // 识别索引页
    uint16_t page_type = get_page_type(page_data);
    if (page_type != FIL_PAGE_INDEX) continue;
    
    // 解析记录
    BPlusTreeParser parser(page_data);
    auto records = parser.parse_records(schema);
    
    // 收集所有记录
    recovered_records.insert(recovered_records.end(), 
                             records.begin(), records.end());
}
```

## 3. rm -rf 恢复原理

### 3.1 场景描述

执行 `rm -rf *.ibd` 后：
1. 文件系统条目被删除
2. 磁盘块被标记为可用
3. **数据可能仍存在于磁盘上**（未被覆盖）

### 3.2 恢复原理

**前提条件**：数据块未被新数据覆盖

恢复策略：
1. **文件系统层恢复**：使用 extundelete、debugfs 等工具恢复文件
2. **磁盘层恢复**：直接扫描磁盘/分区，识别 InnoDB 页结构
3. **模式匹配**：识别 InnoDB 页头特征（FIL_PAGE_TYPE 标识）

### 3.3 恢复步骤

1. **磁盘扫描**：扫描整个磁盘或分区
2. **页头识别**：查找 InnoDB 页头特征：
   - FIL_PAGE_TYPE = 9969 (INDEX)
   - FIL_PAGE_TYPE = 9971 (SDI)
   - FIL_PAGE_TYPE = 9970 (UNDO_LOG)
3. **页链关联**：通过页号和 LSN 关联页
4. **SDI 解析**：提取表结构元数据
5. **数据重建**：重建记录和 BLOB 链

### 3.4 关键特征

InnoDB 页头特征（偏移 16-37）：
```
Offset 16:  FIL_PAGE_TYPE (2 bytes)
  - 9969 (0x26F1): B+Tree Index
  - 9970 (0x26F2): Undo Log
  - 9971 (0x26F3): SDI
  - 9972 (0x26F4): BLOB
  
Offset 38:  PAGE_HEADER (14 bytes)
  - PAGE_N_DIR_SLOTS
  - PAGE_HEAP_TOP
  - PAGE_N_HEAP
  - PAGE_FREE
  - ...
```

## 4. 文件头损坏恢复原理

### 4.1 场景描述

文件头损坏可能由以下原因导致：
1. 磁盘坏道
2. 文件系统错误
3. 强制断电
4. 软件 Bug

### 4.2 恢复原理

InnoDB 页头包含冗余信息，可用于恢复：

```
FIL Header (38 bytes):
+--------+----------------+----------------------------------+
| Offset | Field          | Description                      |
+--------+----------------+----------------------------------+
| 0      | space_id       | 表空间ID                         |
| 4      | checksum1      | 校验和字段1                      |
| 8      | LSN low        | 日志序列号低4字节                |
| 12     | LSN high       | 日志序列号高4字节                |
| 16     | page_type      | 页类型（关键！）                 |
| 18     | page_no        | 页号                             |
| 20     | prev_page      | 前一页（B+Tree）                 |
| 22     | next_page      | 后一页（B+Tree）                 |
| 24     | btree_index    | B+Tree索引ID                     |
| 26     | reserved       | 保留字段                         |
+--------+----------------+----------------------------------+
```

### 4.3 恢复策略

1. **页类型修复**：
   - 如果 page_type 错误，尝试推断正确类型
   - 检查 LSN 连续性验证

2. **页号修复**：
   - 检查 prev_page/next_page 链
   - 验证页号连续性

3. **校验和重建**：
   - 清零校验和字段
   - 重新计算 CRC32C/INNODB 校验和

### 4.4 关键代码

```cpp
// repair/header_repairer.cpp
bool HeaderRepairer::repair_page_type(uint8_t* page_data) {
    uint16_t* page_type_ptr = reinterpret_cast<uint16_t*>(
        page_data + FIL_PAGE_TYPE_ABS);
    
    // 检查页类型是否有效
    if (!is_valid_page_type(*page_type_ptr)) {
        // 尝试推断页类型
        uint16_t inferred_type = infer_page_type(page_data);
        if (inferred_type != 0) {
            *page_type_ptr = inferred_type;
            return true;
        }
        return false;
    }
    return true;
}
```

## 5. BLOB 恢复原理

### 5.1 BLOB 存储结构

InnoDB BLOB 存储采用链表结构：

```
主记录（在主页中）:
+------------------+
| 20-byte pointer  |
| - space_id       |
| - page_no        |
| - offset         |
| - len            |
| - total_len      |
+------------------+
         |
         v
+------------------+
|  BLOB Page 1     |
| +--------------+ |
| | next_bpage   | --> next page
| | prev_bpage   | <-- prev page
| | data_len     |
| | data[0..n]   |
| +--------------+ |
+------------------+
         |
         v
+------------------+
|  BLOB Page 2     |
| ...              |
+------------------+
```

### 5.2 BLOB 页头结构

```
BLOB Page Header (at offset 38):
+--------+-------------+----------------------------------+
| Offset | Field       | Description                      |
+--------+-------------+----------------------------------+
| 38     | next_bpage  | 下一个 BLOB 页页号               |
| 42     | prev_bpage  | 前一个 BLOB 页页号               |
| 46     | data_len    | 本页数据长度                     |
| 50     | reserved    | 保留字段                         |
+--------+-------------+----------------------------------+
```

### 5.3 恢复步骤

1. **读取主记录**：从主表中读取 BLOB 指针
2. **构建页链**：从 next_bpage 开始遍历
3. **验证链完整性**：检查循环引用和断裂
4. **收集数据**：按顺序收集所有页的数据
5. **重组 BLOB**：拼接所有数据块

### 5.4 关键代码

```cpp
// recovery/blob_reconstructor.cpp
bool BlobReconstructor::reconstruct_chain(uint32_t space_id, 
                                          uint32_t first_page) {
    uint32_t current_page = first_page;
    std::set<uint32_t> visited;
    
    while (current_page != 0xFFFFFFFF) {
        // 循环检测
        if (visited.count(current_page)) {
            return false; // 循环引用
        }
        visited.insert(current_page);
        
        // 读取 BLOB 页
        auto page_data = read_blob_page(space_id, current_page);
        if (page_data.empty()) break;
        
        // 提取数据
        uint32_t data_len = get_data_len(page_data);
        auto data = get_data(page_data, 54, data_len);
        chunks_.push_back(data);
        
        // 移动到下一页
        current_page = get_next_page(page_data);
    }
    
    return true;
}
```

## 6. Redo Log 回放原理

### 6.1 Redo Log 结构

```
Redo Log Record:
+--------+-------------+----------------------------------+
| Offset | Field       | Description                      |
+--------+-------------+----------------------------------+
| 0      | type        | 日志类型 (1 byte)                |
| 1      | space_id    | 表空间ID (4 bytes)               |
| 5      | page_no     | 页号 (4 bytes)                   |
| 9      | data_len    | 数据长度 (2 bytes, optional)     |
| 11     | data        | 日志数据 (variable)              |
| ...    | trx_id      | 事务ID (8 bytes, optional)       |
| ...    | roll_ptr    | 回滚指针 (8 bytes, optional)     |
+--------+-------------+----------------------------------+
```

### 6.2 回放步骤

1. **解析 Redo Log**：读取所有 Redo Log 记录
2. **按页分组**：按 space_id + page_no 分组
3. **排序应用**：按 LSN 顺序应用
4. **更新页数据**：应用字段更新

### 6.3 支持的日志类型

- MLOG_COMP_REC_INSERT: 插入记录
- MLOG_COMP_REC_UPDATE: 更新记录
- MLOG_COMP_REC_DELETE: 删除记录
- MLOG_PAGE_CREATE: 创建页
- MLOG_PAGE_REINIT: 重初始化页

## 7. Undo Log 回滚原理

### 7.1 Undo Log 结构

```
Undo Log Record:
+--------+-------------+----------------------------------+
| Offset | Field       | Description                      |
+--------+-------------+----------------------------------+
| 0      | type        | 记录类型 (1 byte)                |
| 1      | next_rec    | 下一条记录偏移 (2 bytes)         |
| 3      | trx_id      | 事务ID (6/8 bytes)               |
| ...    | roll_ptr    | 回滚指针                         |
| ...    | undo_data   | 撤销数据                         |
+--------+-------------+----------------------------------+
```

### 7.2 回滚步骤

1. **定位 Undo Log**：通过 roll_ptr 定位
2. **读取旧值**：从 Undo Log 中读取旧值
3. **应用更新**：将旧值写回原位置
4. **更新指针**：更新 roll_ptr 指向前一条记录

## 8. SDI 元数据恢复

### 8.1 SDI 结构

```
SDI Payload Header (20 bytes):
+--------+-------------+----------------------------------+
| Offset | Field       | Description                      |
+--------+-------------+----------------------------------+
| 0      | type        | 类型 (1=TABLE, 2=INDEX, ...)     |
| 4      | id          | 对象ID (8 bytes)                 |
| 12     | uncompressed_len | 解压后长度 (4 bytes)        |
| 16     | compressed_len   | 压缩后长度 (4 bytes)        |
+--------+-------------+----------------------------------+
```

### 8.2 恢复步骤

1. **定位 SDI 页**：FIL_PAGE_TYPE = 9971
2. **提取 payload**：从页中提取 SDI 数据
3. **解压缩**：使用 zlib 解压（raw deflate）
4. **解析 JSON**：提取表结构

## 9. 综合恢复流程

```
+------------------+
| 1. 扫描文件      |
+------------------+
         |
         v
+------------------+
| 2. 识别页类型    |
+------------------+
         |
         v
+------------------+
| 3. 提取 SDI      |
+------------------+
         |
         v
+------------------+
| 4. 解析表结构    |
+------------------+
         |
         v
+------------------+
| 5. 遍历 B+Tree   |
+------------------+
         |
         v
+------------------+
| 6. 解析记录      |
+------------------+
         |
         v
+------------------+
| 7. 重建 BLOB     |
+------------------+
         |
         v
+------------------+
| 8. 导出数据      |
+------------------+
```

## 10. 恢复成功率影响因素

1. **数据覆盖程度**：未覆盖数据恢复成功率高
2. **页链完整性**：页链断裂影响恢复
3. **BLOB 页链**：BLOB 页链完整性
4. **SDI 可用性**：SDI 页是否损坏
5. **Redo Log 可用性**：Redo Log 是否可用

## 11. 最佳实践

1. **立即停止写入**：发现误操作后立即停止数据库写入
2. **备份当前状态**：先备份当前磁盘状态
3. **使用专业工具**：使用经过验证的恢复工具
4. **测试恢复**：在测试环境验证恢复结果
5. **验证数据**：恢复后验证数据完整性
