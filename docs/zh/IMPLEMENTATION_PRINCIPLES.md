# MySQL Recovery Pro - 技术原理与实现说明

## 目录
1. [架构概览](#架构概览)
2. [InnoDB 存储结构](#innodb-存储结构)
3. [核心算法](#核心算法)
4. [字段类型解码](#字段类型解码)
5. [恢复流程](#恢复流程)
6. [关键技术细节](#关键技术细节)

---

## 架构概览

### 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                    mysql_recovery_pro                        │
├─────────────────────────────────────────────────────────────┤
│  CLI Layer (main_cli.cpp)                                   │
│  - 参数解析、输出格式选择、进度显示                          │
├─────────────────────────────────────────────────────────────┤
│  Recovery Layer                                             │
│  ├── IndexPageParser     - B+Tree 叶节点解析                 │
│  ├── BPlusTreeParser     - B+Tree 结构遍历                   │
│  ├── SDIExtractor        - MySQL 8.0+ SDI 解析               │
│  ├── FRMParser           - MySQL 5.6/5.7 FRM 解析            │
│  └── RawDiskCarver       - 原始磁盘扫描恢复                  │
├─────────────────────────────────────────────────────────────┤
│  Decoding Layer                                             │
│  ├── FieldDecoder        - 字段值解码                        │
│  ├── BlobChainRebuilder  - BLOB 链式存储重组                 │
│  └── PageReader          - 页面读取与校验                    │
├─────────────────────────────────────────────────────────────┤
│  Utility Layer                                              │
│  ├── mach_read            - InnoDB 字节读取                  │
│  ├── page_defs            - 页面结构常量                      │
│  └── schema_defs          - 表结构定义                        │
└─────────────────────────────────────────────────────────────┘
```

### 核心组件职责

| 组件 | 职责 | 关键方法 |
|------|------|----------|
| IndexPageParser | 解析 B+Tree 叶节点，提取行记录 | parse_page(), extract_records() |
| BPlusTreeParser | 遍历 B+Tree 结构，定位数据页 | traverse(), find_leaf_page() |
| SDIExtractor | 解析 SDI 获取表结构 | extract_schema(), decompress_sdi() |
| FieldDecoder | 解码各类型字段值 | decode_int(), decode_datetime(), decode_decimal() |
| BlobChainRebuilder | 重组外部存储的 BLOB 数据 | rebuild_chain(), fetch_blob_page() |
| RawDiskCarver | 扫描原始磁盘寻找有效页面 | scan_device(), carve_pages() |

---

## InnoDB 存储结构

### 1. 页面结构 (Page Structure)

InnoDB 使用页面作为基本 I/O 单位，默认 16KB：

```
┌────────────────────────────────────────────────────────────┐
│ FIL Header (38 bytes)                                      │
│ ├── Space ID (4 bytes) - 表空间 ID                         │
│ ├── Page Number (4 bytes) - 页号                           │
│ ├── Previous Page (4 bytes) - 前一页指针                   │
│ ├── Next Page (4 bytes) - 后一页指针                       │
│ ├── LSN (8 bytes) - 日志序列号                             │
│ ├── Page Type (2 bytes) - 页类型                           │
│ └── Flush LSN (8 bytes) - 刷新 LSN                         │
├────────────────────────────────────────────────────────────┤
│ Page Body (根据页类型不同)                                  │
│ ├── INDEX 页: B+Tree 节点                                  │
│ ├── SDI 页: 表结构元数据                                   │
│ ├── BLOB 页: 外部存储数据                                  │
│ └── ...                                                    │
├────────────────────────────────────────────────────────────┤
│ FIL Trailer (8 bytes)                                      │
│ └── Old-style Checksum (4 bytes) + LSN Low 32 (4 bytes)    │
└────────────────────────────────────────────────────────────┘
```

### 2. B+Tree 页结构 (INDEX Page)

```
┌────────────────────────────────────────────────────────────┐
│ FIL Header (38 bytes)                                      │
├────────────────────────────────────────────────────────────┤
│ BTR Header (36 bytes)                                      │
│ ├── n_levels (4 bytes) - 树层数                            │
│ ├── index_page_offset (2 bytes) - 索引页偏移               │
│ ├── max_size (2 bytes) - 最大记录数                        │
│ ├── n_owned (2 bytes) - 所属记录数                         │
│ ├── flags (2 bytes) - 行格式标志                           │
│ ├── heap_top (2 bytes) - 堆顶位置                          │
│ ├── n_heap (2 bytes) - 堆记录数                            │
│ └── ...                                                    │
├────────────────────────────────────────────────────────────┤
│ Record Directory (变长)                                    │
│ └── 记录偏移数组 (从后向前增长)                             │
├────────────────────────────────────────────────────────────┤
│ System Records                                             │
│ ├── INFIMUM (最小记录)                                     │
│ └── SUPREMUM (最大记录)                                    │
├────────────────────────────────────────────────────────────┤
│ User Records (从下向上增长)                                 │
│ └── 实际数据记录                                           │
└────────────────────────────────────────────────────────────┘
```

### 3. 行格式 (Row Format)

InnoDB 支持四种行格式：

| 格式 | MySQL 版本 | 特点 |
|------|------------|------|
| REDUNDANT | 5.0- | 旧格式，冗余存储 |
| COMPACT | 5.0+ | 紧凑存储，NULL 优化 |
| DYNAMIC | 5.7+ | 动态存储，BLOB 外部存储优化 |
| COMPRESSED | 5.7+ | 压缩存储，页级压缩 |

**DYNAMIC 格式记录结构**：

```
┌─────────────────────────────────────────────────────────────┐
│ Record Header (变长, 5-6 bytes)                             │
│ ├── next_offset (2 bytes) - 下一条记录偏移                  │
│ ├── flags (1 byte) - 删除标志等                             │
│ └── ...                                                    │
├─────────────────────────────────────────────────────────────┤
│ NULL Bitmap (变长)                                          │
│ └── 每个可空列 1 bit                                        │
├─────────────────────────────────────────────────────────────┤
│ Variable Length Field Offsets (变长)                        │
│ └── 每个 VARCHAR/TEXT 列的长度信息                          │
├─────────────────────────────────────────────────────────────┤
│ System Columns (固定)                                       │
│ ├── TRX_ID (6 bytes) - 事务 ID                             │
│ └── ROLL_PTR (7 bytes) - 回滚指针                          │
├─────────────────────────────────────────────────────────────┤
│ Variable Length Fields (变长字段数据)                       │
│ └── VARCHAR, TEXT, BLOB, JSON 等实际数据                    │
├─────────────────────────────────────────────────────────────┤
│ Fixed Length Fields (定长字段数据)                          │
│ └── INT, BIGINT, DATE, DATETIME, DECIMAL 等实际数据         │
└─────────────────────────────────────────────────────────────┘
```

**关键发现**：InnoDB DYNAMIC 格式按以下顺序存储列数据：
1. **变长字段在前**：VARCHAR, TEXT, BLOB, JSON 等
2. **定长字段在后**：INT, BIGINT, DATE, DATETIME, DECIMAL, ENUM 等

---

## 核心算法

### 1. 页面检测算法

```cpp
bool detect_valid_page(const uint8_t* data, size_t size) {
    // Step 1: 检查页类型 (大端序)
    uint16_t page_type = read_be<uint16_t>(data, 24);
    if (page_type != FIL_PAGE_INDEX && 
        page_type != FIL_PAGE_SDI &&
        page_type != FIL_PAGE_BLOB) {
        return false;
    }
    
    // Step 2: 检查 LSN 合理性
    uint64_t lsn = read_be<uint64_t>(data, 16);
    if (lsn == 0 || lsn > MAX_REASONABLE_LSN) {
        return false;
    }
    
    // Step 3: 检查页号连续性
    uint32_t page_no = read_be<uint32_t>(data, 4);
    if (page_no > MAX_REASONABLE_PAGE_NO) {
        return false;
    }
    
    return true;
}
```

### 2. SDI 解析算法

MySQL 8.0+ 使用 SDI (Serialized Dictionary Information) 存储表结构：

```cpp
TableSchema parse_sdi(const uint8_t* sdi_data, size_t size) {
    // Step 1: 检查 SDI 类型
    uint16_t sdi_type = read_be<uint16_t>(sdi_data, 0);
    
    if (sdi_type == 0x45BD) {
        // 压缩 SDI - 需要 zlib 解压
        return parse_compressed_sdi(sdi_data, size);
    } else if (sdi_type == 0x45BE) {
        // 未压缩 SDI
        return parse_uncompressed_sdi(sdi_data, size);
    }
    
    throw InvalidSDIError();
}

TableSchema parse_compressed_sdi(const uint8_t* data, size_t size) {
    // 跳过 SDI 头部 (6 bytes)
    const uint8_t* compressed = data + 6;
    size_t compressed_size = size - 6;
    
    // zlib 解压
    std::vector<uint8_t> decompressed = zlib_decompress(compressed, compressed_size);
    
    // 解析 JSON 格式的 SDI
    return parse_sdi_json(decompressed);
}
```

### 3. 行记录解析算法

```cpp
std::vector<Record> parse_records(const uint8_t* page_data, 
                                   const TableSchema& schema) {
    std::vector<Record> records;
    
    // Step 1: 定位记录起始位置
    uint16_t infimum_offset = PAGE_INFIMUM_REC_OFFSET_ABS;
    uint16_t current_offset = read_next_offset(page_data, infimum_offset);
    
    // Step 2: 遍历记录链
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
    
    // Step 1: 解析记录头
    uint16_t next_offset = read_le<uint16_t>(rec_ptr, 0);
    rec_ptr += RECORD_HEADER_SIZE;
    
    // Step 2: 解析 NULL bitmap
    std::vector<bool> null_bitmap = parse_null_bitmap(rec_ptr, schema);
    rec_ptr += null_bitmap_size(schema);
    
    // Step 3: 解析变长字段长度表
    auto var_lengths = parse_var_length_table(rec_ptr, schema);
    rec_ptr += var_lengths.bytes_consumed;
    
    // Step 4: 跳过系统列 (TRX_ID + ROLL_PTR)
    rec_ptr += 6 + 7;  // 13 bytes
    
    // Step 5: 解析变长字段 (在前)
    for (auto& col : schema.variable_columns) {
        if (null_bitmap[col.index]) {
            rec.values[col.name] = nullptr;
        } else {
            rec.values[col.name] = decode_var_field(rec_ptr, col, var_lengths[col.index]);
            rec_ptr += var_lengths[col.index];
        }
    }
    
    // Step 6: 解析定长字段 (在后)
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

## 字段类型解码

### 1. 数值类型

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
    // 计算存储字节数
    int intg = precision - scale;
    int intg_full = intg / 9;
    int frac_full = scale / 9;
    int intg_rest = intg % 9;
    int frac_rest = scale % 9;
    
    size_t bytes = intg_full * 4 + frac_full * 4;
    bytes += (intg_rest > 0) ? decimal_size_table[intg_rest] : 0;
    bytes += (frac_rest > 0) ? decimal_size_table[frac_rest] : 0;
    
    // 解析 BCD 编码
    return parse_bcd_decimal(data, bytes, precision, scale);
}
```

### 2. 时间类型

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
    // 解析整数部分 (5 bytes)
    uint64_t int_part = read_be<uint64_t>(data, 0) >> 24;
    
    int64_t time = int_part - 0x8000000000LL;
    int day = time / 10000000;
    time %= 10000000;
    int hour = time / 100000;
    time %= 100000;
    int minute = time / 1000;
    int second = time % 1000;
    
    // 解析小数部分 (0-3 bytes)
    uint32_t frac = decode_fractional(data + 5, precision);
    
    // 计算实际日期
    int year = day / 13 + 1000;
    int month = day % 13;
    // ... 计算日
    
    return format("%04d-%02d-%02d %02d:%02d:%02d.%0*d",
                  year, month, day, hour, minute, second, precision, frac);
}
```

### 3. 字符类型

```cpp
// VARCHAR (length-prefixed)
std::string decode_varchar(const uint8_t* data, size_t length,
                           const std::string& charset) {
    // 字符集转换
    if (charset == "utf8mb4") {
        return convert_utf8mb4(data, length);
    } else if (charset == "latin1") {
        return convert_latin1(data, length);
    }
    return std::string(reinterpret_cast<const char*>(data), length);
}

// BLOB (可能外部存储)
std::vector<uint8_t> decode_blob(const uint8_t* data, size_t length,
                                 BlobChainRebuilder& rebuilder) {
    if (length <= BLOB_INLINE_THRESHOLD) {
        // 内联存储
        return std::vector<uint8_t>(data, data + length);
    } else {
        // 外部存储 - 需要重组 BLOB 链
        uint32_t blob_page = read_be<uint32_t>(data, 0);
        return rebuilder.rebuild_chain(blob_page);
    }
}
```

### 4. 特殊类型

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
    // MySQL JSON 二进制格式解析
    // 类型标志 (1 byte) + 数据
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

## 恢复流程

### 正常恢复流程

```
┌─────────────────────────────────────────────────────────────┐
│ 1. 打开 .ibd 文件                                            │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. 检测页大小 (4KB/8KB/16KB/32KB/64KB)                       │
│    - 读取第 0 页 FIL Header                                  │
│    - 检查页类型和 LSN                                        │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. 提取表结构                                                │
│    - MySQL 8.0+: 解析 SDI 页                                 │
│    - MySQL 5.6/5.7: 解析 FRM 文件或使用手动 Schema           │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 4. 定位根节点                                                │
│    - 扫描 INDEX 页                                          │
│    - 找到 n_levels 最大的页                                  │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 5. 遍历 B+Tree                                               │
│    - 从根节点开始                                            │
│    - 递归访问所有子节点                                      │
│    - 收集叶节点数据                                          │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 6. 解析记录                                                  │
│    - 解析记录头和 NULL bitmap                                │
│    - 解码各字段值                                            │
│    - 处理 BLOB 外部存储                                      │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 7. 输出结果                                                  │
│    - CSV / JSON / SQL INSERT                                 │
└─────────────────────────────────────────────────────────────┘
```

### 灾难恢复流程

```
┌─────────────────────────────────────────────────────────────┐
│ 原始磁盘扫描 (RawDiskCarver)                                 │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 1. 以 512 字节步长扫描设备                                   │
│    - 检查每个位置是否为有效页开头                            │
│    - 放宽 LSN 验证条件                                      │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. 收集有效页面                                              │
│    - INDEX 页: 数据页                                       │
│    - SDI 页: 表结构                                         │
│    - BLOB 页: 外部存储数据                                  │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. 重建表结构                                                │
│    - 尝试从 SDI 页恢复                                      │
│    - 失败则使用手动 Schema                                   │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 4. 恢复数据记录                                              │
│    - 解析所有 INDEX 页                                      │
│    - 重组 BLOB 链                                           │
│    - 去重和排序                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## 关键技术细节

### 1. 字节序处理

InnoDB 使用混合字节序：
- **FIL Header 字段**：大端序 (Big-Endian)
- **BTR Header 字段**：小端序 (Little-Endian)
- **字段数据**：取决于类型

```cpp
// 大端序读取 (FIL Header)
uint32_t page_type = read_be<uint16_t>(page, 24);
uint64_t lsn = read_be<uint64_t>(page, 16);

// 小端序读取 (BTR Header)
uint16_t flags = read_le<uint16_t>(page, FIL_HEADER_SIZE + 10);
uint16_t heap_top = read_le<uint16_t>(page, FIL_HEADER_SIZE + 2);
```

### 2. NULL 值处理

NULL bitmap 每位对应一个可空列：
- 位为 1 表示 NULL
- 位为 0 表示非 NULL

```cpp
bool is_null(const uint8_t* bitmap, int col_index) {
    int byte_index = col_index / 8;
    int bit_index = col_index % 8;
    return (bitmap[byte_index] >> bit_index) & 1;
}
```

### 3. 变长字段长度计算

```cpp
size_t get_var_field_length(const uint8_t* data, bool is_long) {
    if (is_long) {
        // 长度存储在 2 bytes
        return read_le<uint16_t>(data, 0);
    } else {
        // 长度存储在 1 byte
        return data[0];
    }
}
```

### 4. BLOB 链式存储

超过 768 字节的 BLOB 存储在外部页面：

```
┌─────────────────────────────────────────────────────────────┐
│ Inline Part (768 bytes)                                      │
│ ├── BLOB data (前 768 字节)                                  │
│ └── External Pointer (20 bytes)                              │
│     ├── Space ID (4 bytes)                                   │
│     ├── Page Number (4 bytes)                                │
│     ├── Offset (4 bytes)                                     │
│     └── Length (8 bytes)                                     │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ External BLOB Page                                           │
│ ├── BLOB Header                                              │
│ │   ├── Part Length (4 bytes)                                │
│ │   ├── Next Page (4 bytes)                                  │
│ │   └── ...                                                  │
│ └── BLOB Data (剩余页空间)                                   │
└─────────────────────────────────────────────────────────────┘
```

### 5. 页大小检测

```cpp
uint32_t detect_page_size(const uint8_t* data, size_t file_size) {
    std::vector<uint32_t> candidates = {4096, 8192, 16384, 32768, 65536};
    
    for (auto page_size : candidates) {
        // 检查文件大小是否为页大小的整数倍
        if (file_size % page_size != 0) continue;
        
        // 检查第 0 页是否为有效 SDI 或 INDEX 页
        const uint8_t* page0 = data;
        uint16_t type = read_be<uint16_t>(page0, 24);
        
        if (type == FIL_PAGE_SDI || type == FIL_PAGE_INDEX) {
            // 验证 LSN
            uint64_t lsn = read_be<uint64_t>(page0, 16);
            if (lsn > 0 && lsn < MAX_REASONABLE_LSN) {
                return page_size;
            }
        }
    }
    
    return 16384;  // 默认 16KB
}
```

---

## 性能优化

### 1. 内存映射

对于大文件，使用 mmap 减少内存拷贝：

```cpp
void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
// 直接操作 mapped 指针
munmap(mapped, file_size);
```

### 2. 并行处理

多表恢复可并行执行：

```cpp
std::vector<std::future<void>> futures;
for (auto& table : tables) {
    futures.push_back(std::async(std::launch::async, [&]() {
        recover_table(table);
    }));
}
for (auto& f : futures) f.wait();
```

### 3. 缓存优化

- 页面缓存：避免重复读取同一页
- Schema 缓存：避免重复解析 SDI
- BLOB 缓存：避免重复重组 BLOB 链

---

## 错误处理

### 1. 校验和验证

```cpp
bool verify_checksum(const uint8_t* page, uint32_t page_size) {
    uint32_t stored = read_le<uint32_t>(page, 0);
    uint32_t computed = crc32_z(std::nothrow, page + 4, page_size - 4);
    return stored == computed;
}
```

### 2. 容错处理

- 页面损坏：跳过并记录警告
- BLOB 链断裂：返回部分数据
- Schema 缺失：使用手动 Schema

---

## 灾难恢复技术

### 1. 磁盘雕刻 (Disk Carving)

磁盘雕刻是一种无需备份的数据恢复技术，通过扫描原始磁盘寻找有效的数据页：

```cpp
class RawDiskCarver {
    // 多线程扫描设备
    void scan_device(const std::string& device_path);

    // 识别 InnoDB 页面签名
    bool is_index_page(const uint8_t* data) {
        return read_be<uint16_t>(data, 24) == 0x45BF;
    }

    // 放宽 LSN 验证（允许部分损坏页面）
    bool is_valid_page_relaxed(const uint8_t* data) {
        uint64_t lsn = read_be<uint64_t>(data, 16);
        return lsn > 0;  // 仅检查 LSN 非零
    }
};
```

### 2. Schema 提取策略

工具按优先级尝试多种 Schema 来源：

```
┌─────────────────────────────────────────────────────────────┐
│ 1. 外部 Schema 文件 (--schema-file)                         │
│    - JSON 格式，完全控制                                     │
├─────────────────────────────────────────────────────────────┤
│ 2. FRM 文件 (--frm)                                         │
│    - MySQL 5.6/5.7 专用                                     │
│    - 自动类型映射 (MySQL → InnoDB)                          │
├─────────────────────────────────────────────────────────────┤
│ 3. SDI 提取 (自动)                                          │
│    - MySQL 8.0+ 自动识别                                    │
│    - 支持压缩 SDI (zlib 解压)                               │
├─────────────────────────────────────────────────────────────┤
│ 4. 手动 Schema (--schema)                                   │
│    - 命令行直接指定                                          │
├─────────────────────────────────────────────────────────────┤
│ 5. 启发式推断 (fallback)                                    │
│    - 无 Schema 时尝试字段边界检测                            │
└─────────────────────────────────────────────────────────────┘
```

### 3. FRM 文件解析

MySQL 5.6/5.7 使用 FRM 文件存储表结构：

```cpp
struct FRMHeader {
    uint16_t frm_type;      // 0x01FE = TABLE
    uint16_t frm_version;   // 最低 9
    uint32_t frm_length;    // 文件长度
    uint16_t key_info_length;
    // ...
};

// MySQL 类型到 InnoDB 类型映射
InnoDBType map_mysql_to_innodb(uint8_t mysql_type) {
    switch (mysql_type) {
        case 0x03: return InnoDBType::INT;       // INT
        case 0x05: return InnoDBType::DOUBLE;    // DOUBLE
        case 0x0F: return InnoDBType::VARCHAR;   // VARCHAR
        case 0xF6: return InnoDBType::DOUBLE;    // DOUBLE (另一种编码)
        case 0xF7: return InnoDBType::ENUM;      // ENUM
        // ...
    }
}
```

### 4. 压缩 SDI 解析

MySQL 8.0+ SDI 页可能是压缩格式 (0x45BD)：

```cpp
std::optional<TableSchema> extract_compressed_sdi(const uint8_t* page) {
    // 查找 zlib 签名 (0x78 0x9C 或 0x78 0xDA)
    size_t zlib_offset = find_zlib_signature(page);

    // zlib 解压
    std::vector<uint8_t> decompressed;
    zlib_decompress(page + zlib_offset, decompressed);

    // 解析 JSON 格式的 SDI
    auto json = nlohmann::json::parse(decompressed);
    return parse_sdi_json(json);
}
```

---

## 版本兼容性

| 特性 | MySQL 5.6 | MySQL 5.7 | MySQL 8.0 | MySQL 8.4 | MySQL 9.0 |
|------|-----------|-----------|-----------|-----------|-----------|
| SDI | ❌ | ❌ | ✅ | ✅ | ✅ |
| FRM | ✅ | ✅ | ❌ | ❌ | ❌ |
| DYNAMIC | ✅ | ✅ | ✅ | ✅ | ✅ |
| COMPRESSED | ✅ | ✅ | ✅ | ✅ | ✅ |
| Instant ADD COLUMN | ❌ | ❌ | ✅ | ✅ | ✅ |
| Disk Carving + FRM | ✅ | ✅ | ✅ | ✅ | ✅ |
