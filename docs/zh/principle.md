# MySQL 8.x 数据恢复引擎 - 原理文档

**版本**: v1.0.0  
**作者**: jliang.fang  
**日期**: 2026-03-29

---

## 1. 概述

本文档详细说明 MySQL 8.x 数据恢复引擎的核心原理，包括 InnoDB 存储引擎、B+树、Redo/Undo 机制和 MySQL 8.x 内部格式。

---

## 2. InnoDB 存储引擎架构

### 2.1 表空间 (Tablespace)

InnoDB 使用表空间作为逻辑存储单位：

```
Tablespace
├── System Tablespace (ibdata1)
│   ├── Data Dictionary
│   ├── Undo Logs
│   ├── Insert Buffer
│   └── Doublewrite Buffer
├── File-Per-Table Tablespace (*.ibd)
│   ├── SDI (Space Descriptor Index)
│   ├── B+Tree Index
│   └── BLOB Data
└── General Tablespace
```

### 2.2 页 (Page)

InnoDB 使用页作为基本存储单位：

- **默认大小**: 16KB
- **支持大小**: 4KB, 8KB, 16KB, 32KB, 64KB
- **页类型**: INDEX, SDI, UNDO_LOG, BLOB, COMPRESSED

---

## 3. 页结构详解

### 3.1 FIL Header (38 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-3 | 4 bytes | FIL_PAGE_PREV | Previous page number (B+Tree leaf chain) |
| 4-7 | 4 bytes | FIL_PAGE_NEXT | Next page number (B+Tree leaf chain) |
| 8-15 | 8 bytes | FIL_PAGE_LSN | Log Sequence Number |
| 16-19 | 4 bytes | FIL_PAGE_SPACE_ID | Tablespace ID |
| 20-23 | 4 bytes | FIL_PAGE_OFFSET | Page offset |
| 24-27 | 4 bytes | FIL_PAGE_SPACE_CHECKSUM | Checksum (CRC32C/INNODB/OLD) |
| 28-31 | 4 bytes | FIL_PAGE_ARCH_LOG_NO | Archive log number |
| 32-37 | 6 bytes | FIL_PAGE_DATA_START | Data start marker |

### 3.2 Page Header (14+ bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-1 | 2 bytes | PAGE_N_DIR_SLOTS | Number of slots in page directory |
| 2-3 | 2 bytes | PAGE_HEAP_TOP | Heap top position |
| 4-5 | 2 bytes | PAGE_N_HEAP | Number of records in heap (15-bit) |
| 6-7 | 2 bytes | PAGE_FREE | Free space start position |
| 8-9 | 2 bytes | PAGE_GARBAGE_END | Garbage space end position |
| 10-11 | 2 bytes | PAGE_LAST_INSERT | Last insert position |
| 12-13 | 2 bytes | PAGE_DIRECTION | Insert direction |
| 14-15 | 2 bytes | PAGE_N_DIRECT_PAGES | Number of direct pages |

### 3.3 B+Tree Header (8 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-1 | 2 bytes | BTR_SEG_ID | Segment ID (8 bytes, but only 4 used) |
| 4-5 | 2 bytes | BTR_LEVEL | B+Tree level (0=leaf) |
| 6-7 | 2 bytes | BTR_INDEX_ID | Index ID (8 bytes) |

### 3.4 Record Area

#### 3.4.1 System Records

- **INFIMUM**: Minimum record, fixed offset 54
- **SUPREMUM**: Maximum record, dynamic offset

#### 3.4.2 Record Header (5 bytes for COMPACT)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 byte | info_bits | Info bits (bit 0-2: type, bit 3: delete flag, bit 4: min record, bit 5: external storage) |
| 1-2 | 2 bytes | next_record_offset | Next record offset (15-bit) |
| 3-4 | 2 bytes | heap_no | Heap number (15-bit) |

---

## 4. B+Tree 索引结构

### 4.1 B+Tree 特性

- **所有数据存储在叶子节点**
- **内部节点只存储索引键**
- **叶子节点通过双向链表连接**

### 4.2 B+Tree 页类型

```
Root Page (Level N)
├── Internal Page (Level N-1)
│   ├── Internal Page (Level N-2)
│   │   ├── Leaf Page (Level 0)
│   │   ├── Leaf Page (Level 0)
│   │   └── ...
│   └── Internal Page (Level N-2)
│       ├── Leaf Page (Level 0)
│       ├── Leaf Page (Level 0)
│       └── ...
└── Internal Page (Level N-1)
    ├── Internal Page (Level N-2)
    │   ├── Leaf Page (Level 0)
    │   ├── Leaf Page (Level 0)
    │   └── ...
    └── Internal Page (Level N-2)
        ├── Leaf Page (Level 0)
        ├── Leaf Page (Level 0)
        └── ...
```

### 4.3 页链结构

```
Leaf Page 0 <-> Leaf Page 1 <-> Leaf Page 2 <-> ...
     |           |            |
  Records     Records      Records
```

---

## 5. 行格式

### 5.1 REDUNDANT 格式 (MySQL 5.0/5.1)

- **记录头**: 6 bytes (info_bits + next_offset(2) + heap_no(2) + n_fields(1) + status(1))
- **字段偏移**: 从记录末尾反向计算
- **NULL 位图**: 可选

### 5.2 COMPACT 格式 (MySQL 5.0+)

- **记录头**: 5 bytes (info_bits + next_offset(2) + heap_no(2))
- **字段偏移**: 从记录末尾反向计算
- **NULL 位图**: 可选

### 5.3 DYNAMIC 格式 (MySQL 5.7+)

- **记录头**: 5 bytes (同 COMPACT)
- **字段偏移**: 动态计算
- **外部存储**: BLOB 指针 (20 bytes)

### 5.4 COMPRESSED 格式 (MySQL 5.7+)

- **记录头**: 5 bytes (同 COMPACT)
- **压缩**: 使用 zlib 压缩
- **外部存储**: BLOB 指针 (20 bytes)

---

## 6. Redo Log

### 6.1 Redo Log 结构

```
Redo Log Record:
+--------+-------------+----------------------------------+
| Offset | Field       | Description                      |
+--------+-------------+----------------------------------+
| 0      | type        | Log type (1 byte)                |
| 1      | space_id    | Tablespace ID (4 bytes)          |
| 5      | page_no     | Page number (4 bytes)            |
| 9      | data_len    | Data length (2 bytes, optional)  |
| 11     | data        | Log data (variable)              |
| ...    | trx_id      | Transaction ID (8 bytes, opt)    |
| ...    | roll_ptr    | Rollback pointer (8 bytes, opt)  |
+--------+-------------+----------------------------------+
```

### 6.2 Redo Log 类型

| Type | Name | Description |
|------|------|-------------|
| 10 | MLOG_REC_INSERT | Insert record |
| 11 | MLOG_REC_UPDATE | Update record |
| 12 | MLOG_REC_DELETE | Delete record |
| 30 | MLOG_COMP_REC_INSERT | Insert record (COMPACT) |
| 31 | MLOG_COMP_REC_UPDATE | Update record (COMPACT) |
| 32 | MLOG_COMP_REC_DELETE | Delete record (COMPACT) |
| 15 | MLOG_PAGE_CREATE | Create page |
| 16 | MLOG_PAGE_REINIT | Reinitialize page |

### 6.3 Redo Log 回放

```
1. Parse Redo Log records
2. Group by (space_id, page_no)
3. Sort by LSN
4. Apply updates to pages
5. Update page checksum
```

---

## 7. Undo Log

### 7.1 Undo Log 结构

```
Undo Log Record:
+--------+-------------+----------------------------------+
| Offset | Field       | Description                      |
+--------+-------------+----------------------------------+
| 0      | type        | Record type (1 byte)             |
| 1      | next_rec    | Next record offset (2 bytes)     |
| 3      | trx_id      | Transaction ID (6/8 bytes)       |
| ...    | roll_ptr    | Rollback pointer                 |
| ...    | undo_data   | Undo data                        |
+--------+-------------+----------------------------------+
```

### 7.2 Undo Log 回滚

```
1. Locate Undo Log via roll_ptr
2. Read old values from Undo Log
3. Apply updates to original position
4. Update roll_ptr to previous record
```

---

## 8. SDI (Space Descriptor Index)

### 8.1 SDI 结构

```
SDI Payload Header (20 bytes):
+--------+-------------+----------------------------------+
| Offset | Field       | Description                      |
+--------+-------------+----------------------------------+
| 0      | type        | Type (1=TABLE, 2=INDEX, ...)     |
| 4      | id          | Object ID (8 bytes)              |
| 12     | uncompressed_len | Uncompressed length (4 bytes) |
| 16     | compressed_len   | Compressed length (4 bytes)   |
+--------+-------------+----------------------------------+
```

### 8.2 SDI 解析

```
1. Locate SDI page (FIL_PAGE_TYPE = 9971)
2. Extract payload from page
3. Decompress using zlib (raw deflate)
4. Parse JSON to extract table structure
```

---

## 9. BLOB 恢复

### 9.1 BLOB 存储结构

```
Main Record (in main page):
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

### 9.2 BLOB 页链重建

```
1. Read main record BLOB pointer
2. Build page chain from next_bpage
3. Validate chain integrity
4. Collect data from all pages
5. Reconstruct BLOB data
```

---

## 10. 校验和算法

### 10.1 CRC32C (MySQL 8.0 default)

使用 Castagnoli 多项式 (0x82F63B78)

```cpp
constexpr std::array<uint32_t, 256> generate_crc32c_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x82F63B78;
            } else {
                crc >>= 1;
            }
        }
        table[i] = crc;
    }
    return table;
}
```

### 10.2 INNODB (MySQL 5.6/5.7)

```cpp
uint32_t compute_innodb_checksum(const std::vector<uint8_t>& page_data) {
    const uint32_t* data = reinterpret_cast<const uint32_t*>(page_data.data());
    size_t len = page_data.size() / 4;

    uint32_t checksum = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        if (i == 6 || i == 7) continue; // Skip checksum fields
        checksum ^= read_le<uint32_t>(page_data.data(), i * 4);
        checksum += (checksum >> 16);
        checksum &= 0xFFFFFFFF;
    }
    return (checksum >> 16) ^ (checksum & 0xFFFF);
}
```

### 10.3 OLD (MySQL 5.5)

```cpp
uint32_t compute_old_checksum(const std::vector<uint8_t>& page_data) {
    const uint16_t* data = reinterpret_cast<const uint16_t*>(page_data.data());
    size_t len = page_data.size() / 2;

    uint64_t checksum = 0;
    for (size_t i = 0; i < len; ++i) {
        if (i == 12 || i == 13) continue; // Skip checksum fields
        checksum += read_le<uint16_t>(page_data.data(), i * 2);
        checksum = (checksum & 0xFFFFFFFF) + (checksum >> 32);
    }
    return static_cast<uint32_t>((checksum & 0xFFFFFFFF) + (checksum >> 32));
}
```

---

## 11. 恢复原理

### 11.1 Truncate 恢复

```
1. Scan all pages in tablespace
2. Identify B+Tree index pages (FIL_PAGE_INDEX)
3. Reconstruct page chain via FIL_PAGE_PREV/FIL_PAGE_NEXT
4. Parse records between INFIMUM/SUPREMUM
5. Reconstruct BLOB page chains
6. Extract metadata from SDI page
```

### 11.2 rm -rf 恢复

```
1. Scan disk/partition for InnoDB page headers
2. Identify page types (INDEX, SDI, UNDO_LOG, BLOB)
3. Correlate pages via page number and LSN
4. Extract table structure from SDI
5. Reconstruct records from index pages
```

### 11.3 文件头损坏恢复

```
1. Read corrupted .ibd file
2. Identify corrupted page headers
3. Repair page type, checksum, etc.
4. Rebuild file header
5. Export repaired file
```

---

## 12. 性能优化

### 12.1 内存映射

使用 mmap 避免额外的内存拷贝：

```cpp
class FileMapper {
public:
    explicit FileMapper(const std::string& path) {
        fd_ = open(path.c_str(), O_RDONLY);
        size_ = lseek(fd_, 0, SEEK_END);
        data_ = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    }
    
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    
private:
    int fd_;
    size_t size_;
    const uint8_t* data_;
};
```

### 12.2 CRC32C 查表法

编译时生成 CRC32C 表，零运行时开销：

```cpp
constexpr auto crc32c_table = generate_crc32c_table();

uint32_t compute_crc32c(const std::vector<uint8_t>& page_data) {
    uint32_t crc = ~0U;
    for (size_t i = 0; i < page_data.size(); ++i) {
        if (i >= 24 && i < 28) continue; // Skip checksum fields
        crc = crc32c_table[(crc ^ page_data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}
```

### 12.3 零拷贝视图

使用 `std::span` 提供零拷贝的页数据视图：

```cpp
std::span<const uint8_t> get_page_view(uint32_t page_no) {
    size_t offset = page_no * page_size_;
    return std::span<const uint8_t>(data_ + offset, page_size_);
}
```

---

## 13. 扩展点设计

### 13.1 添加新的 MySQL 版本

```cpp
// 在 page_defs.hpp 中添加版本特定常量
constexpr uint16_t MYSQL_8_1_FEATURE_FLAG = 0x8000;
```

### 13.2 添加新的行格式

```cpp
enum class RowFormat {
    COMPACT,
    DYNAMIC,
    REDUNDANT,
    COMPRESSED,
    CUSTOM,
};
```

### 13.3 添加新的导出格式

```cpp
class ExportModule {
public:
    virtual bool export_to_format(const std::vector<...>& records,
                                  const std::string& path) = 0;
};

class CSVExporter : public ExportModule { ... };
class SQLOutputter : public ExportModule { ... };
class IBDExporter : public ExportModule { ... };
```

---

## 14. 参考资料

- MySQL 8.0 源码: `include/page0page.h`, `include/fil0fil.h`, `include/btr0btr.h`
- InnoDB 内部结构: https://dev.mysql.com/doc/refman/8.0/en/innodb-file-space.html
- B+树算法: https://dev.mysql.com/doc/refman/8.0/en/innodb-index-types.html

---

## 15. 作者

**jliang.fang** - 首席数据库内核架构师

---

<div align="center">
  <p>打造世界领先的 MySQL 数据恢复工具</p>
</div>
