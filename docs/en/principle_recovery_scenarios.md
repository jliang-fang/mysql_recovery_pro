# MySQL 8.x Data Recovery Engine - Disaster Recovery Principles

## 1. Overview

This document details the disaster recovery scenarios supported by the MySQL 8.x Data Recovery Engine and their underlying principles.

## 2. Truncate Recovery Principle

### 2.1 Scenario Description

When executing `TRUNCATE TABLE`, InnoDB performs:
1. Releases all data pages (marks as unallocated)
2. Resets the B+Tree (creates new empty tree)
3. Updates metadata in SDI
4. Releases segment space

### 2.2 Recovery Principle

Despite `TRUNCATE` marking pages as unallocated, **actual data remains on disk** (not overwritten). Recovery principle:

```
Before Truncate:
+----------+     +----------+     +----------+
|  Page 0  | --> |  Page 1  | --> |  Page 2  |
| (Root)   |     | (Leaf)   |     | (Leaf)   |
| Records  |     | Records  |     | Records  |
+----------+     +----------+     +----------+

After Truncate:
+----------+     +----------+     +----------+
|  Page 0  |     |  Page 1  |     |  Page 2  |
| (New)    |     | (Unused) |     | (Unused) |
| Empty    |     | Free     |     | Free     |
+----------+     +----------+     +----------+

Data still exists physically (not overwritten):
+----------+     +----------+     +----------+
|  Page 0  |     |  Page 1  |     |  Page 2  |
| (New)    |     | [OLD]    |     | [OLD]    |
| Empty    |     | Records  |     | Records  |
+----------+     +----------+     +----------+
```

### 2.3 Recovery Steps

1. **Scan all pages**: Traverse entire tablespace, read all page data
2. **Page type identification**: Identify B+Tree index pages (FIL_PAGE_INDEX=9969)
3. **Page chain reconstruction**: Build page chain via FIL_PAGE_PREV/FIL_PAGE_NEXT
4. **Record parsing**: Parse user records between INFIMUM/SUPREMUM
5. **BLOB chain reconstruction**: For BLOB columns, rebuild BLOB page chain
6. **Metadata extraction**: Extract table structure from SDI page

### 2.4 Key Code Path

```cpp
// recovery/engine.cpp - recover_truncate()
for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
    auto page_data = file_reader_->read_page(page_no);
    
    // Identify index pages
    uint16_t page_type = get_page_type(page_data);
    if (page_type != FIL_PAGE_INDEX) continue;
    
    // Parse records
    BPlusTreeParser parser(page_data);
    auto records = parser.parse_records(schema);
    
    // Collect all records
    recovered_records.insert(recovered_records.end(), 
                             records.begin(), records.end());
}
```

## 3. rm -rf Recovery Principle

### 3.1 Scenario Description

After executing `rm -rf *.ibd`:
1. File system entries are deleted
2. Disk blocks are marked as available
3. **Data may still exist on disk** (not overwritten)

### 3.2 Recovery Principle

**Prerequisite**: Data blocks not overwritten by new data

Recovery strategies:
1. **Filesystem layer recovery**: Use extundelete, debugfs to restore files
2. **Disk layer recovery**: Directly scan disk/partition, identify InnoDB page structure
3. **Pattern matching**: Identify InnoDB page header signatures (FIL_PAGE_TYPE identifier)

### 3.3 Recovery Steps

1. **Disk scanning**: Scan entire disk or partition
2. **Page header identification**: Find InnoDB page header signatures:
   - FIL_PAGE_TYPE = 9969 (INDEX)
   - FIL_PAGE_TYPE = 9971 (SDI)
   - FIL_PAGE_TYPE = 9970 (UNDO_LOG)
3. **Page chain association**: Associate pages via page number and LSN
4. **SDI parsing**: Extract table structure metadata
5. **Data reconstruction**: Rebuild records and BLOB chains

### 3.4 Key Signatures

InnoDB page header signatures (offset 16-37):
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

## 4. File Header Corruption Recovery Principle

### 4.1 Scenario Description

File header corruption may be caused by:
1. Disk bad sectors
2. File system errors
3. Force power off
4. Software bugs

### 4.2 Recovery Principle

InnoDB page headers contain redundant information for recovery:

```
FIL Header (38 bytes):
+--------+----------------+----------------------------------+
| Offset | Field          | Description                      |
+--------+----------------+----------------------------------+
| 0      | space_id       | Tablespace ID                    |
| 4      | checksum1      | Checksum field 1                 |
| 8      | LSN low        | Log Sequence Number low 4 bytes  |
| 12     | LSN high       | Log Sequence Number high 4 bytes |
| 16     | page_type      | Page type (critical!)            |
| 18     | page_no        | Page number                      |
| 20     | prev_page      | Previous page (B+Tree)           |
| 22     | next_page      | Next page (B+Tree)               |
| 24     | btree_index    | B+Tree index ID                  |
| 26     | reserved       | Reserved fields                  |
+--------+----------------+----------------------------------+
```

### 4.3 Recovery Strategy

1. **Page type repair**:
   - If page_type is incorrect, try to infer correct type
   - Validate via LSN continuity

2. **Page number repair**:
   - Check prev_page/next_page chain
   - Verify page number continuity

3. **Checksum reconstruction**:
   - Clear checksum fields
   - Recalculate CRC32C/INNODB checksum

### 4.4 Key Code

```cpp
// repair/header_repairer.cpp
bool HeaderRepairer::repair_page_type(uint8_t* page_data) {
    uint16_t* page_type_ptr = reinterpret_cast<uint16_t*>(
        page_data + FIL_PAGE_TYPE_ABS);
    
    // Check if page type is valid
    if (!is_valid_page_type(*page_type_ptr)) {
        // Try to infer page type
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

## 5. BLOB Recovery Principle

### 5.1 BLOB Storage Structure

InnoDB BLOB storage uses linked list structure:

```
Main record (in main page):
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

### 5.2 BLOB Page Header Structure

```
BLOB Page Header (at offset 38):
+--------+-------------+----------------------------------+
| Offset | Field       | Description                      |
+--------+-------------+----------------------------------+
| 38     | next_bpage  | Next BLOB page page number       |
| 42     | prev_bpage  | Previous BLOB page page number   |
| 46     | data_len    | Data length in this page         |
| 50     | reserved    | Reserved fields                  |
+--------+-------------+----------------------------------+
```

### 5.3 Recovery Steps

1. **Read main record**: Read BLOB pointer from main table
2. **Build page chain**: Traverse from next_bpage
3. **Verify chain integrity**: Check for circular references and breaks
4. **Collect data**: Collect data from all pages in order
5. **Reconstruct BLOB**: Concatenate all data chunks

### 5.4 Key Code

```cpp
// recovery/blob_reconstructor.cpp
bool BlobReconstructor::reconstruct_chain(uint32_t space_id, 
                                          uint32_t first_page) {
    uint32_t current_page = first_page;
    std::set<uint32_t> visited;
    
    while (current_page != 0xFFFFFFFF) {
        // Circular reference detection
        if (visited.count(current_page)) {
            return false; // Circular reference
        }
        visited.insert(current_page);
        
        // Read BLOB page
        auto page_data = read_blob_page(space_id, current_page);
        if (page_data.empty()) break;
        
        // Extract data
        uint32_t data_len = get_data_len(page_data);
        auto data = get_data(page_data, 54, data_len);
        chunks_.push_back(data);
        
        // Move to next page
        current_page = get_next_page(page_data);
    }
    
    return true;
}
```

## 6. Redo Log Replay Principle

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
| ...    | trx_id      | Transaction ID (8 bytes, opt.)   |
| ...    | roll_ptr    | Rollback pointer (8 bytes, opt.) |
+--------+-------------+----------------------------------+
```

### 6.2 Replay Steps

1. **Parse Redo Log**: Read all Redo Log records
2. **Group by page**: Group by space_id + page_no
3. **Sort and apply**: Apply in LSN order
4. **Update page data**: Apply field updates

### 6.3 Supported Log Types

- MLOG_COMP_REC_INSERT: Insert record
- MLOG_COMP_REC_UPDATE: Update record
- MLOG_COMP_REC_DELETE: Delete record
- MLOG_PAGE_CREATE: Create page
- MLOG_PAGE_REINIT: Reinitialize page

## 7. Undo Log Rollback Principle

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

### 7.2 Rollback Steps

1. **Locate Undo Log**: Find via roll_ptr
2. **Read old value**: Read old value from Undo Log
3. **Apply update**: Write old value back to original location
4. **Update pointer**: Update roll_ptr to point to previous record

## 8. SDI Metadata Recovery

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

### 8.2 Recovery Steps

1. **Locate SDI page**: FIL_PAGE_TYPE = 9971
2. **Extract payload**: Extract SDI data from page
3. **Decompress**: Decompress using zlib (raw deflate)
4. **Parse JSON**: Extract table structure

## 9. Comprehensive Recovery Flow

```
+------------------+
| 1. Scan file     |
+------------------+
         |
         v
+------------------+
| 2. Identify page |
+------------------+
         |
         v
+------------------+
| 3. Extract SDI   |
+------------------+
         |
         v
+------------------+
| 4. Parse schema  |
+------------------+
         |
         v
+------------------+
| 5. Traverse B+Tree|
+------------------+
         |
         v
+------------------+
| 6. Parse records |
+------------------+
         |
         v
+------------------+
| 7. Reconstruct   |
|    BLOB          |
+------------------+
         |
         v
+------------------+
| 8. Export data   |
+------------------+
```

## 10. Recovery Success Factors

1. **Data overwrite degree**: Higher success rate for non-overwritten data
2. **Page chain integrity**: Chain breaks affect recovery
3. **BLOB page chain**: BLOB page chain integrity
4. **SDI availability**: Whether SDI page is corrupted
5. **Redo log availability**: Whether Redo Log is available

## 11. Best Practices

1. **Stop writes immediately**: Stop database writes upon detecting accidental operation
2. **Backup current state**: First backup current disk state
3. **Use professional tools**: Use verified recovery tools
4. **Test recovery**: Verify recovery results in test environment
5. **Validate data**: Verify data integrity after recovery
