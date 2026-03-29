# MySQL 8.x Data Recovery Engine - Principle Document (English)

**Version**: v1.0.0  
**Author**: jliang.fang  
**Date**: 2026-03-29

---

## 1. Overview

This document details the core principles of the MySQL 8.x Data Recovery Engine, including InnoDB storage engine, B+Tree, Redo/Undo mechanisms, and MySQL 8.x internal formats.

---

## 2. InnoDB Storage Engine Architecture

### 2.1 Tablespace

InnoDB uses tablespace as the logical storage unit:

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

### 2.2 Page

InnoDB uses page as the basic storage unit:

- **Default Size**: 16KB
- **Supported Sizes**: 4KB, 8KB, 16KB, 32KB, 64KB
- **Page Types**: INDEX, SDI, UNDO_LOG, BLOB, COMPRESSED

---

## 3. Page Structure

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

## 4. B+Tree Index Structure

### 4.1 B+Tree Properties

- **All data stored in leaf nodes**
- **Internal nodes store only index keys**
- **Leaf nodes connected via bidirectional链表**

### 4.2 B+Tree Page Types

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

### 4.3 Page Chain Structure

```
Leaf Page 0 <-> Leaf Page 1 <-> Leaf Page 2 <-> ...
     |           |            |
  Records     Records      Records
```

---

## 5. Row Formats

### 5.1 REDUNDANT Format (MySQL 5.0/5.1)

- **Record Header**: 6 bytes (info_bits + next_offset(2) + heap_no(2) + n_fields(1) + status(1))
- **Field Offset**: Calculated backwards from record end
- **NULL Bitmap**: Optional

### 5.2 COMPACT Format (MySQL 5.0+)

- **Record Header**: 5 bytes (info_bits + next_offset(2) + heap_no(2))
- **Field Offset**: Calculated backwards from record end
- **NULL Bitmap**: Optional

### 5.3 DYNAMIC Format (MySQL 5.7+)

- **Record Header**: 5 bytes (same as COMPACT)
- **Field Offset**: Dynamic calculation
- **External Storage**: BLOB pointer (20 bytes)

### 5.4 COMPRESSED Format (MySQL 5.7+)

- **Record Header**: 5 bytes (same as COMPACT)
- **Compression**: Using zlib
- **External Storage**: BLOB pointer (20 bytes)

---

## 6. Redo Log

### 6.1 Redo Log Structure

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

### 6.2 Redo Log Types

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

### 6.3 Redo Log Replay

```
1. Parse Redo Log records
2. Group by (space_id, page_no)
3. Sort by LSN
4. Apply updates to pages
5. Update page checksum
```

---

## 7. Undo Log

### 7.1 Undo Log Structure

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

### 7.2 Undo Log Rollback

```
1. Locate Undo Log via roll_ptr
2. Read old values from Undo Log
3. Apply updates to original position
4. Update roll_ptr to previous record
```

---

## 8. SDI (Space Descriptor Index)

### 8.1 SDI Structure

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

### 8.2 SDI Parsing

```
1. Locate SDI page (FIL_PAGE_TYPE = 9971)
2. Extract payload from page
3. Decompress using zlib (raw deflate)
4. Parse JSON to extract table structure
```

---

## 9. BLOB Recovery

### 9.1 BLOB Storage Structure

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

### 9.2 BLOB Page Chain Reconstruction

```
1. Read main record BLOB pointer
2. Build page chain from next_bpage
3. Validate chain integrity
4. Collect data from all pages
5. Reconstruct BLOB data
```

---

## 10. Checksum Algorithms

### 10.1 CRC32C (MySQL 8.0 default)

Using Castagnoli polynomial (0x82F63B78)

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

## 11. Recovery Principles

### 11.1 Truncate Recovery

```
1. Scan all pages in tablespace
2. Identify B+Tree index pages (FIL_PAGE_INDEX)
3. Reconstruct page chain via FIL_PAGE_PREV/FIL_PAGE_NEXT
4. Parse records between INFIMUM/SUPREMUM
5. Reconstruct BLOB page chains
6. Extract metadata from SDI page
```

### 11.2 rm -rf Recovery

```
1. Scan disk/partition for InnoDB page headers
2. Identify page types (INDEX, SDI, UNDO_LOG, BLOB)
3. Correlate pages via page number and LSN
4. Extract table structure from SDI
5. Reconstruct records from index pages
```

### 11.3 File Header Corruption Recovery

```
1. Read corrupted .ibd file
2. Identify corrupted page headers
3. Repair page type, checksum, etc.
4. Rebuild file header
5. Export repaired file
```

---

## 12. Performance Optimization

### 12.1 Memory Mapping

Use mmap to avoid additional memory copies:

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

### 12.2 CRC32C Lookup Table

Compile-time generated CRC32C table, zero runtime overhead:

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

### 12.3 Zero-Copy View

Use `std::span` to provide zero-copy page data view:

```cpp
std::span<const uint8_t> get_page_view(uint32_t page_no) {
    size_t offset = page_no * page_size_;
    return std::span<const uint8_t>(data_ + offset, page_size_);
}
```

---

## 13. Extension Points

### 13.1 Add New MySQL Version

```cpp
// Add version-specific constants in page_defs.hpp
constexpr uint16_t MYSQL_8_1_FEATURE_FLAG = 0x8000;
```

### 13.2 Add New Row Format

```cpp
enum class RowFormat {
    COMPACT,
    DYNAMIC,
    REDUNDANT,
    COMPRESSED,
    CUSTOM,
};
```

### 13.3 Add New Export Format

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

## 14. References

- MySQL 8.0 Source Code: `include/page0page.h`, `include/fil0fil.h`, `include/btr0btr.h`
- InnoDB Internal Structure: https://dev.mysql.com/doc/refman/8.0/en/innodb-file-space.html
- B+Tree Algorithm: https://dev.mysql.com/doc/refman/8.0/en/innodb-index-types.html

---

## 15. Author

**jliang.fang** - Chief Database Kernel Architect

---

<div align="center">
  <p>Building the world's leading MySQL data recovery tool</p>
</div>
