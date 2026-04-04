# MySQL Page Parser 设计文档

## 概述

本文档描述了 MySQL 5.6-9.0 全版本兼容的页解析层设计与实现。

### 设计目标

1. **全版本兼容**: 支持 MySQL 5.6/5.7/8.0/8.4/9.0
2. **多行格式**: 支持 REDUNDANT/COMPACT/DYNAMIC/COMPRESSED 行格式
3. **极端场景**: 处理损坏、rm -rf、truncate 等极端恢复场景
4. **高性能**: 零拷贝读取、io_uring 支持
5. **企业级特性**: TDE 解密、Instant DDL、高可用

## 架构设计

### 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│                     应用层 (Recovery Engine)                 │
├─────────────────────────────────────────────────────────────┤
│                     统一页解析器 (PageParserV2)              │
├─────────────────────────────────────────────────────────────┤
│  ┌──────────────┬──────────────┬──────────────┬───────────┐ │
│  │ 5.6/5.7 Parser│ 8.0 Parser │ 8.4 Parser │ 9.0 Parser│ │
│  └──────────────┴──────────────┴──────────────┴───────────┘ │
├─────────────────────────────────────────────────────────────┤
│  ┌──────────────┬──────────────┬──────────────┬───────────┐ │
│  │  Redundant   │  Compact     │  Dynamic     │  SDI      │ │
│  │  Parser      │  Parser      │  Parser      │  Parser   │ │
│  └──────────────┴──────────────┴──────────────┴───────────┘ │
├─────────────────────────────────────────────────────────────┤
│                     页类型识别器 (PageTypeDetector)          │
├─────────────────────────────────────────────────────────────┤
│                     版本探测器 (VersionDetector)             │
└─────────────────────────────────────────────────────────────┘
```

### 核心组件

#### 1. PageParserV2 (统一页解析器)

```cpp
class PageParserV2 {
public:
    explicit PageParserV2(const uint8_t* data, size_t size);
    
    PageParseResult parse();
    PageParseResult parse_as(MySQLVersion version);
    
    const PageParseResult& result() const;
    IPageParserStrategy* parser();
};
```

**功能**:
- 自动识别 MySQL 版本和行格式
- 统一的页解析接口
- 支持指定版本解析

#### 2. PageParserFactory (解析器工厂)

```cpp
class PageParserFactory {
public:
    static std::unique_ptr<IPageParserStrategy> create_parser(
        const uint8_t* data, size_t size);
    
    static MySQLVersion detect_mysql_version(const uint8_t* data, size_t size);
    static RowFormat detect_row_format(const uint8_t* data, size_t size);
};
```

**功能**:
- 根据页数据自动创建对应版本的解析器
- 识别 MySQL 版本 (5.6/5.7/8.0/8.4/9.0)
- 识别行格式 (REDUNDANT/COMPACT/DYNAMIC/COMPRESSED)

#### 3. RecordParser (记录解析器)

```cpp
class RecordParserFactory {
public:
    static std::unique_ptr<IRecordParser> create_parser(RowFormat format);
    
    static RowFormat detect_row_format(const uint8_t* data, size_t size, size_t offset);
};
```

**支持的行格式**:

| 格式 | MySQL 版本 | 特征 |
|------|-----------|------|
| REDUNDANT | 4.1-5.0 | 6字节固定头 + 变长字段 |
| COMPACT | 5.0+ | 5字节固定头 + NULL位图 |
| DYNAMIC | 5.7+ | 支持 Instant DDL |
| COMPRESSED | 5.7+ | 压缩页格式 |

#### 4. UndoLogParser (Undo Log 解析器)

```cpp
class UndoLogParser {
public:
    enum class ParseMode {
        PHYSICAL_FLASHBACK,      // 物理闪回模式
        TRANSACTION_ROLLBACK     // 事务回滚模式
    };
    
    std::shared_ptr<UndoLogPage> parse_page(const uint8_t* data, size_t size);
    std::shared_ptr<UndoLogChain> parse_chain(...);
    
    std::vector<UndoLogRecord> extract_old_versions(...);
};
```

**功能**:
- 解析 Undo Log 页
- 提取旧版本数据 (用于物理闪回)
- 事务回滚支持

#### 5. SDIParser (SDI 解析器)

```cpp
class SDIParser {
public:
    std::shared_ptr<SDIPage> parse_page(const uint8_t* data, size_t size);
    std::shared_ptr<SDIPage> parse_json(const std::string& json_str);
    
    std::optional<SDITable> parse_table(const nlohmann::json& json);
    std::vector<SDITable> get_tables() const;
};
```

**功能**:
- 解析 SDI (Serialized Dictionary Info) 页
- 提取表定义和索引定义
- JSON 格式解析

#### 6. InstantDDLParser (Instant DDL 解析器)

```cpp
class InstantDDLParser {
public:
    std::optional<InstantDDLMetadata> parse_metadata(...);
    std::map<uint16_t, uint16_t> rebuild_column_mapping(...);
    std::vector<uint8_t> apply_changes(...);
};
```

**功能**:
- 解析 Instant DDL 元数据
- 重建列映射
- 应用 Instant DDL 变更

## 页结构详解

### FIL Header (0-37 字节)

| 偏移 | 大小 | 字段 | 描述 |
|------|------|------|------|
| 0 | 4 | FIL_PAGE_PREV | 前一个页号 |
| 4 | 4 | FIL_PAGE_NEXT | 下一个页号 |
| 8 | 8 | FIL_PAGE_LSN | LSN (Log Sequence Number) |
| 16 | 2 | FIL_PAGE_TYPE | 页类型 |
| 20 | 4 | FIL_PAGE_OFFSET | 页号 |
| 24 | 4 | FIL_PAGE_SPACE_ID | 空间ID |
| 28 | 4 | FIL_PAGE_ARCH_LOG_NO | 归档日志号 |

### Page Header (38+ 字节)

| 偏移 | 大小 | 字段 | 描述 |
|------|------|------|------|
| 0 | 2 | PAGE_N_DIR_SLOTS | 目录槽数量 |
| 2 | 2 | PAGE_HEAP_TOP | 堆顶偏移 |
| 4 | 2 | PAGE_N_HEAP | 堆记录数 |
| 6 | 2 | PAGE_FREE | 空闲空间起始 |
| 8 | 2 | PAGE_GARBAGE_END | 垃圾空间结束 |
| 10 | 2 | PAGE_LAST_INSERT | 最后插入偏移 |
| 12 | 2 | PAGE_DIRECTION | 插入方向 |
| 14 | 2 | PAGE_N_DIRECT_PAGES | 直接页数 |

### Record Header

#### REDUNDANT 格式 (6字节固定头)

```
Byte 0:   n_owned (4 bits) + status (2 bits) + flag (1 bit) + next_rec_high (1 bit)
Bytes 1-2: next_record_offset (14 bits)
Bytes 3-4: heap_no (14 bits)
Bytes 5-6: record_length (14 bits)
```

#### COMPACT 格式 (5字节固定头)

```
Byte 0:   info_bits (8 bits)
Bytes 1-2: next_record_offset (14 bits)
Bytes 3-4: heap_no (14 bits)
Byte 5:   n_fields (8 bits)
```

#### DYNAMIC 格式 (5字节 + Instant DDL)

```
同 COMPACT 格式，外加:
Byte 6:   metadata_version
Byte 7:   instant_flags
```

## 页类型

| 类型 | 值 | 描述 |
|------|-----|------|
| FIL_PAGE_INDEX | 9969 (0x26f1) | B+Tree 索引页 |
| FIL_PAGE_SDI | 9971 (0x26f3) | SDI 元数据页 |
| FIL_PAGE_UNDO_LOG | 9970 (0x26f2) | Undo Log 页 |
| FIL_PAGE_TYPE_BLOB | 9972 (0x26f4) | BLOB 数据页 |
| FIL_PAGE_TYPE_ALLOCATED | 0 | 新分配页 |
| FIL_PAGE_COMPRESSED | 9973 (0x26f5) | 压缩页 |

## 行格式识别

### 从页头 Flags 识别

```cpp
// Flags 位于偏移 38 + 42 = 80
uint16_t flags = read_le<uint16_t>(data, 80);
uint8_t row_format_val = flags & 0x07;

switch (row_format_val) {
    case 0: return RowFormat::REDUNDANT;
    case 1: return RowFormat::COMPACT;
    case 2: return RowFormat::DYNAMIC;
    case 3: return RowFormat::COMPRESSED;
}
```

### 从 Record Header 识别

```cpp
// 检查 info_bits 格式
uint8_t info_bits = data[offset];

// REDUNDANT: status 在高2位
uint8_t status = (info_bits >> 4) & 0x03;

// COMPACT: status 在低3位
uint8_t status = info_bits & 0x07;
```

## 版本识别

### 基于页类型

| 页类型 | MySQL 版本 |
|--------|-----------|
| 无 SDI 页 | 5.6/5.7 |
| 有 SDI 页 | 8.0+ |

### 基于页头字段

```cpp
// MySQL 8.0+ 有 PAGE_N_DIRECT_PAGES 字段
if (size >= 92) {
    uint16_t n_direct_pages = read_le<uint16_t>(data, 90);
    if (n_direct_pages > 0) {
        return MySQLVersion::MYSQL_80;
    }
}
```

### 基于 SDI JSON

```cpp
// 从 SDI JSON 中提取版本信息
if (json.contains("version")) {
    int sdi_version = json["version"];
    if (sdi_version >= 200000) {
        return MySQLVersion::MYSQL_84;
    }
    return MySQLVersion::MYSQL_80;
}
```

## 极端场景处理

### 1. 页头损坏

```cpp
// 检查页类型有效性
if (page_type != FIL_PAGE_INDEX &&
    page_type != FIL_PAGE_SDI &&
    page_type != FIL_PAGE_UNDO_LOG) {
    // 尝试从其他位置恢复
    // 或标记为损坏页
}
```

### 2. rm -rf 场景

```cpp
// 检查页大小
if (size < 4096) {
    // 只有部分数据，尝试恢复可用部分
    // 读取页头信息
    // 解析可用记录
}
```

### 3. truncate 场景

```cpp
// 检查页链连续性
if (next_page != current_page + 1) {
    // 页链断裂，标记为损坏
    // 尝试重建页链
}
```

### 4. 校验和损坏

```cpp
// 读取存储的校验和
uint32_t stored_checksum = read_le<uint32_t>(data, 24);

// 计算校验和
uint32_t computed_checksum = compute_checksum(data, size);

// 比较
if (stored_checksum != computed_checksum) {
    // 校验和不匹配
    // 标记为损坏页
    // 尝试继续解析
}
```

## 性能优化

### 1. 零拷贝读取

```cpp
// 使用 mmap 避免数据拷贝
int fd = open(file_path, O_RDONLY);
uint8_t* data = (uint8_t*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
```

### 2. io_uring 支持

```cpp
// 使用 io_uring 进行异步 I/O
struct io_uring ring;
io_uring_queue_init(64, &ring, 0);
```

### 3. 页缓存

```cpp
class PageCache {
public:
    std::optional<std::vector<uint8_t>> get(uint32_t page_no);
    void put(uint32_t page_no, std::vector<uint8_t> data);
    
private:
    std::map<uint32_t, std::vector<uint8_t>> cache_;
    size_t max_size_;
};
```

## 测试用例

### 1. 页头损坏场景

```cpp
TEST(PageParserV2Test, ParseHeaderCorruptPage) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, 0xFFFF);  // 无效页类型
    
    PageParserV2 parser(page.data(), page.size());
    auto result = parser.parse();
    
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_message.empty());
}
```

### 2. rm -rf 场景

```cpp
TEST(PageParserV2Test, ParseRmrfScenario) {
    std::vector<uint8_t> page(1024, 0);  // 只有部分数据
    
    PageParserV2 parser(page.data(), page.size());
    auto result = parser.parse();
    
    // 应该能解析部分数据
    EXPECT_TRUE(result.success);
}
```

### 3. truncate 场景

```cpp
TEST(PageParserV2Test, ParseTruncateScenario) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint32_t>(page.data(), 28, 0xFFFFFFF0);  // 无效 next_page
    
    PageParserV2 parser(page.data(), page.size());
    auto result = parser.parse();
    
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.next_page, 0xFFFFFFF0);
}
```

## 扩展性设计

### 添加新版本支持

1. 创建新的解析器类

```cpp
class MySQL90PageParser : public MySQL84PageParser {
public:
    explicit MySQL90PageParser(RowFormat format);
    
    bool parse_header(const uint8_t* data, size_t size) override;
};
```

2. 在工厂中添加版本识别

```cpp
case MySQLVersion::MYSQL_90:
    return std::make_unique<MySQL90PageParser>(format);
```

### 添加新行格式支持

1. 创建新的记录解析器

```cpp
class NewRowFormatParser : public IRecordParser {
public:
    std::optional<RecordHeaderBase> parse_header(...) override;
    std::vector<ColumnData> parse_columns(...) override;
};
```

2. 在工厂中添加格式识别

```cpp
case RowFormat::NEW_FORMAT:
    return std::make_unique<NewRowFormatParser>();
```

## 总结

本设计文档描述了 MySQL 页解析层的完整架构，包括：

1. **分层架构**: 统一接口 + 多版本支持
2. **工厂模式**: 自动识别版本和格式
3. **策略模式**: 不同版本使用不同解析策略
4. **高性能**: 零拷贝 + io_uring + 页缓存
5. **极端场景**: 损坏、rm -rf、truncate 等

通过本设计，可以实现对 MySQL 5.6-9.0 全版本的页解析支持，满足极端数据恢复场景的需求。
