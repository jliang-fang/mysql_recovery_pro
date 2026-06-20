# MySQL Recovery Pro - Technical Principles and Implementation

## Table of Contents
1. [Architecture Overview](#architecture-overview)
2. [InnoDB Storage Structure](#innodb-storage-structure)
3. [Core Algorithms](#core-algorithms)
4. [Field Type Decoding](#field-type-decoding)
5. [Recovery Workflow](#recovery-workflow)
6. [Key Technical Details](#key-technical-details)

---

## Architecture Overview

### System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    mysql_recovery_pro                        │
├─────────────────────────────────────────────────────────────┤
│  CLI Layer (main_cli.cpp)                                   │
│  - Parameter parsing, output format selection, progress      │
├─────────────────────────────────────────────────────────────┤
│  Recovery Layer                                             │
│  ├── IndexPageParser     - B+Tree leaf node parsing         │
│  ├── BPlusTreeParser     - B+Tree structure traversal       │
│  ├── SDIExtractor        - MySQL 8.0+ SDI parsing           │
│  ├── FRMParser           - MySQL 5.6/5.7 FRM parsing        │
│  └── RawDiskCarver       - Raw disk scanning recovery       │
├─────────────────────────────────────────────────────────────┤
│  Decoding Layer                                             │
│  ├── FieldDecoder        - Field value decoding             │
│  ├── BlobChainRebuilder  - BLOB chain reconstruction        │
│  └── PageReader          - Page reading and validation      │
├─────────────────────────────────────────────────────────────┤
│  Utility Layer                                              │
│  ├── mach_read            - InnoDB byte reading             │
│  ├── page_defs            - Page structure constants        │
│  └── schema_defs          - Table schema definitions        │
└─────────────────────────────────────────────────────────────┘
```

### Core Component Responsibilities

| Component | Responsibility | Key Methods |
|-----------|----------------|-------------|
| IndexPageParser | Parse B+Tree leaf nodes, extract records | parse_page(), extract_records() |
| BPlusTreeParser | Traverse B+Tree structure, locate data pages | traverse(), find_leaf_page() |
| SDIExtractor | Parse SDI to get table schema | extract_schema(), decompress_sdi() |
| FieldDecoder | Decode various field types | decode_int(), decode_datetime(), decode_decimal() |
| BlobChainRebuilder | Reconstruct externally stored BLOB data | rebuild_chain(), fetch_blob_page() |
| RawDiskCarver | Scan raw disk for valid pages | scan_device(), carve_pages() |

---

## InnoDB Storage Structure

### 1. Page Structure

InnoDB uses pages as the basic I/O unit, default 16KB:

```
┌────────────────────────────────────────────────────────────┐
│ FIL Header (38 bytes)                                      │
│ ├── Space ID (4 bytes) - Tablespace ID                     │
│ ├── Page Number (4 bytes) - Page number                    │
│ ├── Previous Page (4 bytes) - Previous page pointer        │
│ ├── Next Page (4 bytes) - Next page pointer                │
│ ├── LSN (8 bytes) - Log Sequence Number                    │
│ ├── Page Type (2 bytes) - Page type                        │
│ └── Flush LSN (8 bytes) - Flush LSN                        │
├────────────────────────────────────────────────────────────┤
│ Page Body (varies by page type)                            │
│ ├── INDEX page: B+Tree node                                │
│ ├── SDI page: Table metadata                               │
│ ├── BLOB page: External storage data                       │
│ └── ...                                                    │
├────────────────────────────────────────────────────────────┤
│ FIL Trailer (8 bytes)                                      │
│ └── Old-style Checksum (4 bytes) + LSN Low 32 (4 bytes)    │
└────────────────────────────────────────────────────────────┘
```

### 2. B+Tree Page Structure (INDEX Page)

```
┌────────────────────────────────────────────────────────────┐
│ FIL Header (38 bytes)                                      │
├────────────────────────────────────────────────────────────┤
│ BTR Header (36 bytes)                                      │
│ ├── n_levels (4 bytes) - Tree levels                       │
│ ├── index_page_offset (2 bytes) - Index page offset        │
│ ├── max_size (2 bytes) - Maximum records                   │
│ ├── n_owned (2 bytes) - Owned records count                │
│ ├── flags (2 bytes) - Row format flags                     │
│ ├── heap_top (2 bytes) - Heap top position                 │
│ ├── n_heap (2 bytes) - Heap record count                   │
│ └── ...                                                    │
├────────────────────────────────────────────────────────────┤
│ Record Directory (variable length)                         │
│ └── Record offset array (grows backward)                   │
├────────────────────────────────────────────────────────────┤
│ System Records                                             │
│ ├── INFIMUM (minimum record)                               │
│ └── SUPREMUM (maximum record)                              │
├────────────────────────────────────────────────────────────┤
│ User Records (grows upward)                                │
│ └── Actual data records                                    │
└────────────────────────────────────────────────────────────┘
```

### 3. Row Format

InnoDB supports four row formats:

| Format | MySQL Version | Features |
|--------|---------------|----------|
| REDUNDANT | 5.0- | Legacy format, redundant storage |
| COMPACT | 5.0+ | Compact storage, NULL optimization |
| DYNAMIC | 5.7+ | Dynamic storage, BLOB external storage optimization |
| COMPRESSED | 5.7+ | Compressed storage, page-level compression |

**DYNAMIC Format Record Structure**:

```
┌─────────────────────────────────────────────────────────────┐
│ Record Header (variable, 5-6 bytes)                         │
│ ├── next_offset (2 bytes) - Next record offset              │
│ ├── flags (1 byte) - Delete flags etc.                      │
│ └── ...                                                    │
├─────────────────────────────────────────────────────────────┤
│ NULL Bitmap (variable)                                      │
│ └── 1 bit per nullable column                               │
├─────────────────────────────────────────────────────────────┤
│ Variable Length Field Offsets (variable)                    │
│ └── Length info for each VARCHAR/TEXT column                │
├─────────────────────────────────────────────────────────────┤
│ System Columns (fixed)                                      │
│ ├── TRX_ID (6 bytes) - Transaction ID                       │
│ └── ROLL_PTR (7 bytes) - Rollback pointer                   │
├─────────────────────────────────────────────────────────────┤
│ Variable Length Fields (variable field data)                │
│ └── VARCHAR, TEXT, BLOB, JSON actual data                   │
├─────────────────────────────────────────────────────────────┤
│ Fixed Length Fields (fixed field data)                      │
│ └── INT, BIGINT, DATE, DATETIME, DECIMAL actual data        │
└─────────────────────────────────────────────────────────────┘
```

**Key Discovery**: InnoDB DYNAMIC format stores column data in this order:
1. **Variable fields FIRST**: VARCHAR, TEXT, BLOB, JSON, etc.
2. **Fixed fields AFTER**: INT, BIGINT, DATE, DATETIME, DECIMAL, ENUM, etc.

---

## Core Algorithms

### 1. Page Detection Algorithm

```cpp
bool detect_valid_page(const uint8_t* data, size_t size) {
    // Step 1: Check page type (big-endian)
    uint16_t page_type = read_be<uint16_t>(data, 24);
    if (page_type != FIL_PAGE_INDEX && 
        page_type != FIL_PAGE_SDI &&
        page_type != FIL_PAGE_BLOB) {
        return false;
    }
    
    // Step 2: Check LSN validity
    uint64_t lsn = read_be<uint64_t>(data, 16);
    if (lsn == 0 || lsn > MAX_REASONABLE_LSN) {
        return false;
    }
    
    // Step 3: Check page number continuity
    uint32_t page_no = read_be<uint32_t>(data, 4);
    if (page_no > MAX_REASONABLE_PAGE_NO) {
        return false;
    }
    
    return true;
}
```

### 2. SDI Parsing Algorithm

MySQL 8.0+ uses SDI (Serialized Dictionary Information) to store table structure:

```cpp
TableSchema parse_sdi(const uint8_t* sdi_data, size_t size) {
    // Step 1: Check SDI type
    uint16_t sdi_type = read_be<uint16_t>(sdi_data, 0);
    
    if (sdi_type == 0x45BD) {
        // Compressed SDI - requires zlib decompression
        return parse_compressed_sdi(sdi_data, size);
    } else if (sdi_type == 0x45BE) {
        // Uncompressed SDI
        return parse_uncompressed_sdi(sdi_data, size);
    }
    
    throw InvalidSDIError();
}

TableSchema parse_compressed_sdi(const uint8_t* data, size_t size) {
    // Skip SDI header (6 bytes)
    const uint8_t* compressed = data + 6;
    size_t compressed_size = size - 6;
    
    // zlib decompression
    std::vector<uint8_t> decompressed = zlib_decompress(compressed, compressed_size);
    
    // Parse JSON format SDI
    return parse_sdi_json(decompressed);
}
```

### 3. Record Parsing Algorithm

```cpp
std::vector<Record> parse_records(const uint8_t* page_data, 
                                   const TableSchema& schema) {
    std::vector<Record> records;
    
    // Step 1: Locate record start position
    uint16_t infimum_offset = PAGE_INFIMUM_REC_OFFSET_ABS;
    uint16_t current_offset = read_next_offset(page_data, infimum_offset);
    
    // Step 2: Traverse record chain
    while (current_offset != SUPREMUM_OFFSET) {
        Record rec = parse_single_record(page_data, current_offset, schema);
        records.push_back(rec);
        current_offset = read_next_offset(page_data, current_offset);
    }
    
    return records;
}

Record parse_single_record(const uint8_t* page, uint16_t offset,
                           const TableSchema& schema) {
    Record rec;
    const uint8_t* rec_ptr = page + offset;
    
    // Step 1: Parse record header
    uint16_t next_offset = read_le<uint16_t>(rec_ptr, 0);
    rec_ptr += RECORD_HEADER_SIZE;
    
    // Step 2: Parse NULL bitmap
    std::vector<bool> null_bitmap = parse_null_bitmap(rec_ptr, schema);
    rec_ptr += null_bitmap_size(schema);
    
    // Step 3: Parse variable length field offset table
    auto var_lengths = parse_var_length_table(rec_ptr, schema);
    rec_ptr += var_lengths.bytes_consumed;
    
    // Step 4: Skip system columns (TRX_ID + ROLL_PTR)
    rec_ptr += 6 + 7;  // 13 bytes
    
    // Step 5: Parse variable fields (first)
    for (auto& col : schema.variable_columns) {
        if (null_bitmap[col.index]) {
            rec.values[col.name] = nullptr;
        } else {
            rec.values[col.name] = decode_var_field(rec_ptr, col, var_lengths[col.index]);
            rec_ptr += var_lengths[col.index];
        }
    }
    
    // Step 6: Parse fixed fields (after)
    for (auto& col : schema.fixed_columns) {
        if (null_bitmap[col.index]) {
            rec.values[col.name] = nullptr;
        } else {
            rec.values[col.name] = decode_fixed_field(rec_ptr, col);
            rec_ptr += col.storage_size;
        }
    }
    
    return rec;
}
```

---

## Field Type Decoding

### 1. Numeric Types

```cpp
// INT (4 bytes, big-endian)
int32_t decode_int(const uint8_t* data) {
    return static_cast<int32_t>(read_be<uint32_t>(data, 0));
}

// BIGINT (8 bytes, big-endian)
int64_t decode_bigint(const uint8_t* data) {
    return static_cast<int64_t>(read_be<uint64_t>(data, 0));
}

// DECIMAL (precision-dependent storage)
std::string decode_decimal(const uint8_t* data, int precision, int scale) {
    // Calculate storage bytes
    int intg = precision - scale;
    int intg_full = intg / 9;
    int frac_full = scale / 9;
    int intg_rest = intg % 9;
    int frac_rest = scale % 9;
    
    size_t bytes = intg_full * 4 + frac_full * 4;
    bytes += (intg_rest > 0) ? decimal_size_table[intg_rest] : 0;
    bytes += (frac_rest > 0) ? decimal_size_table[frac_rest] : 0;
    
    // Parse BCD encoding
    return parse_bcd_decimal(data, bytes, precision, scale);
}
```

### 2. Temporal Types

```cpp
// DATE (3 bytes)
std::string decode_date(const uint8_t* data) {
    uint32_t packed = (data[0] << 16) | (data[1] << 8) | data[2];
    int year = (packed >> 9) & 0x7FFF;   // 15 bits
    int month = (packed >> 5) & 0xF;     // 4 bits
    int day = packed & 0x1F;             // 5 bits
    
    return format("%04d-%02d-%02d", year, month, day);
}

// DATETIME2 (5-8 bytes, depending on precision)
std::string decode_datetime2(const uint8_t* data, int precision) {
    // Parse integer part (5 bytes)
    uint64_t int_part = read_be<uint64_t>(data, 0) >> 24;
    
    int64_t time = int_part - 0x8000000000LL;
    int day = time / 10000000;
    time %= 10000000;
    int hour = time / 100000;
    time %= 100000;
    int minute = time / 1000;
    int second = time % 1000;
    
    // Parse fractional part (0-3 bytes)
    uint32_t frac = decode_fractional(data + 5, precision);
    
    // Calculate actual date
    int year = day / 13 + 1000;
    int month = day % 13;
    // ... calculate day
    
    return format("%04d-%02d-%02d %02d:%02d:%02d.%0*d",
                  year, month, day, hour, minute, second, precision, frac);
}
```

### 3. Character Types

```cpp
// VARCHAR (length-prefixed)
std::string decode_varchar(const uint8_t* data, size_t length,
                           const std::string& charset) {
    // Character set conversion
    if (charset == "utf8mb4") {
        return convert_utf8mb4(data, length);
    } else if (charset == "latin1") {
        return convert_latin1(data, length);
    }
    return std::string(reinterpret_cast<const char*>(data), length);
}

// BLOB (may be externally stored)
std::vector<uint8_t> decode_blob(const uint8_t* data, size_t length,
                                 BlobChainRebuilder& rebuilder) {
    if (length <= BLOB_INLINE_THRESHOLD) {
        // Inline storage
        return std::vector<uint8_t>(data, data + length);
    } else {
        // External storage - need to rebuild BLOB chain
        uint32_t blob_page = read_be<uint32_t>(data, 0);
        return rebuilder.rebuild_chain(blob_page);
    }
}
```

### 4. Special Types

```cpp
// ENUM (1-2 bytes, depending on member count)
std::string decode_enum(const uint8_t* data, const std::vector<std::string>& members) {
    uint16_t index = (members.size() <= 256) ? data[0] : read_le<uint16_t>(data, 0);
    if (index == 0 || index > members.size()) {
        return format("<ENUM_%d>", index);
    }
    return members[index - 1];
}

// JSON (JSON binary format)
std::string decode_json(const uint8_t* data, size_t length) {
    // MySQL JSON binary format parsing
    // Type flag (1 byte) + data
    uint8_t type = data[0];
    
    switch (type) {
        case JSON_TYPE_LITERAL:
            return decode_json_literal(data[1]);
        case JSON_TYPE_INT32:
            return std::to_string(read_le<int32_t>(data, 1));
        case JSON_TYPE_STRING:
            return decode_json_string(data + 1, length - 1);
        case JSON_TYPE_ARRAY:
            return decode_json_array(data, length);
        case JSON_TYPE_OBJECT:
            return decode_json_object(data, length);
        default:
            throw InvalidJSONTypeError(type);
    }
}
```

---

## Recovery Workflow

### Normal Recovery Workflow

```
┌─────────────────────────────────────────────────────────────┐
│ 1. Open .ibd file                                           │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. Detect page size (4KB/8KB/16KB/32KB/64KB)                │
│    - Read page 0 FIL Header                                 │
│    - Check page type and LSN                                │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. Extract table schema                                     │
│    - MySQL 8.0+: Parse SDI page                             │
│    - MySQL 5.6/5.7: Parse FRM file or use manual Schema     │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 4. Locate root node                                         │
│    - Scan INDEX pages                                       │
│    - Find page with highest n_levels                        │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 5. Traverse B+Tree                                          │
│    - Start from root node                                   │
│    - Recursively visit all child nodes                      │
│    - Collect leaf node data                                 │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 6. Parse records                                            │
│    - Parse record header and NULL bitmap                    │
│    - Decode field values                                    │
│    - Handle BLOB external storage                           │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 7. Output results                                           │
│    - CSV / JSON / SQL INSERT                                │
└─────────────────────────────────────────────────────────────┘
```

### Disaster Recovery Workflow

```
┌─────────────────────────────────────────────────────────────┐
│ Raw Disk Scanning (RawDiskCarver)                           │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 1. Scan device with 512-byte stride                         │
│    - Check each position for valid page header              │
│    - Relaxed LSN validation                                 │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. Collect valid pages                                      │
│    - INDEX pages: Data pages                                │
│    - SDI pages: Table structure                             │
│    - BLOB pages: External storage data                      │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. Rebuild table structure                                  │
│    - Try to recover from SDI pages                          │
│    - Fall back to manual Schema on failure                  │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 4. Recover data records                                     │
│    - Parse all INDEX pages                                  │
│    - Rebuild BLOB chains                                    │
│    - Deduplicate and sort                                   │
└─────────────────────────────────────────────────────────────┘
```

---

## Key Technical Details

### 1. Byte Order Handling

InnoDB uses mixed byte order:
- **FIL Header fields**: Big-endian
- **BTR Header fields**: Little-endian
- **Field data**: Depends on type

```cpp
// Big-endian read (FIL Header)
uint32_t page_type = read_be<uint16_t>(page, 24);
uint64_t lsn = read_be<uint64_t>(page, 16);

// Little-endian read (BTR Header)
uint16_t flags = read_le<uint16_t>(page, FIL_HEADER_SIZE + 10);
uint16_t heap_top = read_le<uint16_t>(page, FIL_HEADER_SIZE + 2);
```

### 2. NULL Value Handling

NULL bitmap has 1 bit per nullable column:
- Bit 1 means NULL
- Bit 0 means non-NULL

```cpp
bool is_null(const uint8_t* bitmap, int col_index) {
    int byte_index = col_index / 8;
    int bit_index = col_index % 8;
    return (bitmap[byte_index] >> bit_index) & 1;
}
```

### 3. Variable Length Field Length Calculation

```cpp
size_t get_var_field_length(const uint8_t* data, bool is_long) {
    if (is_long) {
        // Length stored in 2 bytes
        return read_le<uint16_t>(data, 0);
    } else {
        // Length stored in 1 byte
        return data[0];
    }
}
```

### 4. BLOB Chain Storage

BLOBs over 768 bytes are stored in external pages:

```
┌─────────────────────────────────────────────────────────────┐
│ Inline Part (768 bytes)                                     │
│ ├── BLOB data (first 768 bytes)                             │
│ └── External Pointer (20 bytes)                             │
│     ├── Space ID (4 bytes)                                  │
│     ├── Page Number (4 bytes)                               │
│     ├── Offset (4 bytes)                                    │
│     └── Length (8 bytes)                                    │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ External BLOB Page                                          │
│ ├── BLOB Header                                             │
│ │   ├── Part Length (4 bytes)                               │
│ │   ├── Next Page (4 bytes)                                 │
│ │   └── ...                                                 │
│ └── BLOB Data (remaining page space)                        │
└─────────────────────────────────────────────────────────────┘
```

### 5. Page Size Detection

```cpp
uint32_t detect_page_size(const uint8_t* data, size_t file_size) {
    std::vector<uint32_t> candidates = {4096, 8192, 16384, 32768, 65536};
    
    for (auto page_size : candidates) {
        // Check if file size is multiple of page size
        if (file_size % page_size != 0) continue;
        
        // Check if page 0 is valid SDI or INDEX page
        const uint8_t* page0 = data;
        uint16_t type = read_be<uint16_t>(page0, 24);
        
        if (type == FIL_PAGE_SDI || type == FIL_PAGE_INDEX) {
            // Validate LSN
            uint64_t lsn = read_be<uint64_t>(page0, 16);
            if (lsn > 0 && lsn < MAX_REASONABLE_LSN) {
                return page_size;
            }
        }
    }
    
    return 16384;  // Default 16KB
}
```

---

## Performance Optimization

### 1. Memory Mapping

For large files, use mmap to reduce memory copies:

```cpp
void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
// Operate on mapped pointer directly
munmap(mapped, file_size);
```

### 2. Parallel Processing

Multiple table recovery can run in parallel:

```cpp
std::vector<std::future<void>> futures;
for (auto& table : tables) {
    futures.push_back(std::async(std::launch::async, [&]() {
        recover_table(table);
    }));
}
for (auto& f : futures) f.wait();
```

### 3. Cache Optimization

- Page cache: Avoid re-reading same page
- Schema cache: Avoid re-parsing SDI
- BLOB cache: Avoid rebuilding BLOB chains

---

## Error Handling

### 1. Checksum Verification

```cpp
bool verify_checksum(const uint8_t* page, uint32_t page_size) {
    uint32_t stored = read_le<uint32_t>(page, 0);
    uint32_t computed = crc32_z(std::nothrow, page + 4, page_size - 4);
    return stored == computed;
}
```

### 2. Fault Tolerance

- Corrupted pages: Skip and log warning
- Broken BLOB chains: Return partial data
- Missing Schema: Use manual Schema

---

## Version Compatibility

| Feature | MySQL 5.6 | MySQL 5.7 | MySQL 8.0 | MySQL 8.4 | MySQL 9.0 |
|---------|-----------|-----------|-----------|-----------|-----------|
| SDI | ❌ | ❌ | ✅ | ✅ | ✅ |
| FRM | ✅ | ✅ | ❌ | ❌ | ❌ |
| DYNAMIC | ✅ | ✅ | ✅ | ✅ | ✅ |
| COMPRESSED | ✅ | ✅ | ✅ | ✅ | ✅ |
| Instant ADD COLUMN | ❌ | ❌ | ✅ | ✅ | ✅ |
