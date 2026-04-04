# MySQL 5.6-9.0 Multi-Version Compatible Page Parser Architecture Design Document

**Version**: v2.0.0  
**Author**: jliang.fang  
**Date**: 2026-03-29  
**Status**: Design Complete

---

## 1. Overview

This document details the page parser architecture design for the MySQL Recovery Pro project, which aims to support InnoDB page parsing for the full range of MySQL versions from 5.6 to 9.0.

### 1.1 Design Goals

1. **Multi-Version Compatibility**: Support MySQL 5.6/5.7/8.0/8.1/8.4/9.0 full series
2. **Automatic Detection**: Automatically identify MySQL version and row format through page features
3. **Strategy Pattern**: Use strategy pattern for version-specific parsing
4. **Factory Pattern**: Use factory pattern to create parser instances
5. **High Performance**: Zero-copy reading, support for io_uring

### 1.2 Architecture Principles

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
│  (CLI/GUI/REST API)                                         │
├─────────────────────────────────────────────────────────────┤
│              Recovery Engine (Core Recovery Engine)          │
├─────────────────────────────────────────────────────────────┤
│              Storage Abstraction Layer (Storage Abstraction) │
├─────────────────────────────────────────────────────────────┤
│              Page Parser Layer (Page Parser Layer - This)   │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  IPageParserStrategy (Strategy Interface)             │   │
│  │  ├─ GenericPageParser (Generic Implementation)        │   │
│  │  ├─ MySQL56PageParser (MySQL 5.6/5.7)                 │   │
│  │  ├─ MySQL80PageParser (MySQL 8.0)                     │   │
│  │  ├─ MySQL84PageParser (MySQL 8.4+)                    │   │
│  │  └─ MySQL90PageParser (MySQL 9.0)                     │   │
│  └──────────────────────────────────────────────────────┘   │
│              ┌───────────────────────────────────────┐      │
│              │  PageParserFactory (Factory Class)     │      │
│              │  - detect_mysql_version()              │      │
│              │  - detect_row_format()                 │      │
│              │  - create_parser()                     │      │
│              └───────────────────────────────────────┘      │
├─────────────────────────────────────────────────────────────┤
│              Storage Layer (Storage Layer)                   │
│  (ibd/dat/raw disk)                                         │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. MySQL Version Differences Analysis

### 2.1 System Dictionary Differences

| Feature | MySQL 5.6/5.7 | MySQL 8.0+ |
|---------|---------------|------------|
| System Dictionary Location | ibdata1 (shared) | mysql.ibd (independent) |
| Metadata Format | System Tables | SDI (Serialized Dictionary Info) |
| SDI Support | ❌ Not Supported | ✅ Supported |

### 2.2 Row Format Differences

| Row Format | MySQL Version | Record Header Size | Field Offset Calculation |
|------------|---------------|-------------------|-------------------------|
| REDUNDANT | 4.1-5.0 | 6 bytes | Reverse calculation from end |
| COMPACT | 5.0+ | 5 bytes | Reverse calculation from end |
| DYNAMIC | 5.7+ | 5 bytes | Dynamic calculation |
| COMPRESSED | 5.7+ | 5 bytes | Dynamic calculation |

### 2.3 Page Header Field Differences

| Field | MySQL 5.6/5.7 | MySQL 8.0+ |
|-------|---------------|------------|
| PAGE_N_DIRECT_PAGES | ❌ Not exists | ✅ Exists |
| Instant DDL Metadata | ❌ Not exists | ✅ Exists |
| SDI | ❌ Not exists | ✅ Exists |

---

## 3. Core Architecture Design

### 3.1 Strategy Interface Definition

```cpp
class IPageParserStrategy {
public:
    virtual ~IPageParserStrategy() = default;

    // Parse page header
    virtual bool parse_header(const uint8_t* data, size_t size) = 0;

    // Parse records
    virtual std::vector<Record> parse_records(const uint8_t* data, size_t size) = 0;

    // Get page type
    virtual uint16_t get_page_type() const = 0;

    // Get row format
    virtual RowFormat get_row_format() const = 0;

    // Get MySQL version
    virtual MySQLVersion get_mysql_version() const = 0;

    // Validate page health
    virtual bool validate_health() const = 0;

    // Get page level (B+Tree height)
    virtual uint16_t get_page_level() const = 0;

    // Parse SDI metadata
    virtual std::optional<catalog::TableSchema> parse_sdi_metadata(
        const uint8_t* data, size_t size) = 0;

    // Get page chain info
    virtual std::pair<uint32_t, uint32_t> get_page_chain() const = 0;
};
```

### 3.2 Version Enum

```cpp
enum class MySQLVersion : uint8_t {
    UNKNOWN = 0,
    MYSQL_56 = 56,
    MYSQL_57 = 57,
    MYSQL_80 = 80,
    MYSQL_81 = 81,
    MYSQL_84 = 84,
    MYSQL_90 = 90,
};
```

### 3.3 Row Format Enum

```cpp
enum class RowFormat : uint8_t {
    UNKNOWN = 0,
    REDUNDANT = 1,    // MySQL 4.1-5.0
    COMPACT = 2,      // MySQL 5.0+
    DYNAMIC = 3,      // MySQL 5.7+
    COMPRESSED = 4,   // MySQL 5.7+
};
```

### 3.4 Page Type Constants

```cpp
// Defined in page_defs.hpp
constexpr uint16_t FIL_PAGE_INDEX       = 9969;  // B+Tree index page
constexpr uint16_t FIL_PAGE_SDI         = 9971;  // SDI metadata page
constexpr uint16_t FIL_PAGE_UNDO_LOG    = 9970;  // Undo log page
constexpr uint16_t FIL_PAGE_TYPE_BLOB   = 9972;  // BLOB page
constexpr uint16_t FIL_PAGE_TYPE_ALLOCATED = 0;  // Freshly allocated page
constexpr uint16_t FIL_PAGE_COMPRESSED  = 9973;  // Compressed page
```

---

## 4. Factory Pattern Implementation

### 4.1 Factory Class Definition

```cpp
class PageParserFactory {
public:
    // Create page parser
    static std::unique_ptr<IPageParserStrategy> create_parser(
        const uint8_t* data, size_t size);

    // Detect MySQL version
    static MySQLVersion detect_mysql_version(const uint8_t* data, size_t size);

    // Detect row format
    static RowFormat detect_row_format(const uint8_t* data, size_t size);

    // Detect version from page header
    static MySQLVersion detect_version_from_header(const uint8_t* data, size_t size);

    // Detect version from SDI page
    static MySQLVersion detect_version_from_sdi(const uint8_t* data, size_t size);
};
```

### 4.2 Version Detection Logic

```cpp
MySQLVersion PageParserFactory::detect_mysql_version(const uint8_t* data, size_t size) {
    // 1. First try to detect version from SDI page
    uint16_t page_type = get_page_type(data);
    if (page_type == FIL_PAGE_SDI) {
        return detect_version_from_sdi(data, size);
    }
    
    // 2. Detect version from page header
    return detect_version_from_header(data, size);
}
```

### 4.3 Row Format Detection Logic

```cpp
RowFormat PageParserFactory::detect_row_format(const uint8_t* data, size_t size) {
    // Extract row format from BTR header flags
    if (size < FIL_HEADER_SIZE + 44) {
        return RowFormat::UNKNOWN;
    }
    
    uint16_t flags = read_le<uint16_t>(data, FIL_HEADER_SIZE + 42);
    uint8_t row_format_val = flags & 0x07;
    
    switch (row_format_val) {
        case 0: return RowFormat::REDUNDANT;
        case 1: return RowFormat::COMPACT;
        case 2: return RowFormat::DYNAMIC;
        case 3: return RowFormat::COMPRESSED;
        default: return RowFormat::UNKNOWN;
    }
}
```

---

## 5. Version-Specific Parsers

### 5.1 GenericPageParser (Generic Implementation)

```cpp
class GenericPageParser : public IPageParserStrategy {
public:
    explicit GenericPageParser(MySQLVersion version, RowFormat format);

    bool parse_header(const uint8_t* data, size_t size) override;
    std::vector<Record> parse_records(const uint8_t* data, size_t size) override;
    uint16_t get_page_type() const override;
    RowFormat get_row_format() const override;
    MySQLVersion get_mysql_version() const override;
    bool validate_health() const override;
    uint16_t get_page_level() const override;
    std::optional<catalog::TableSchema> parse_sdi_metadata(
        const uint8_t* data, size_t size) override;
    std::pair<uint32_t, uint32_t> get_page_chain() const override;

protected:
    // Methods that can be overridden by subclasses
    virtual bool parse_fil_header(const uint8_t* data, size_t size);
    virtual bool parse_page_header(const uint8_t* data, size_t size);
    virtual bool parse_btr_header(const uint8_t* data, size_t size);
    virtual std::vector<uint16_t> parse_record_offsets(const uint8_t* data, size_t size);
    virtual Record parse_record(const uint8_t* data, size_t offset, size_t size);
};
```

### 5.2 MySQL56PageParser (MySQL 5.6/5.7)

```cpp
class MySQL56PageParser : public GenericPageParser {
public:
    explicit MySQL56PageParser(RowFormat format);

    bool parse_header(const uint8_t* data, size_t size) override;
    std::vector<Record> parse_records(const uint8_t* data, size_t size) override;
    std::optional<catalog::TableSchema> parse_sdi_metadata(
        const uint8_t* data, size_t size) override; // Returns nullopt (no SDI)
};
```

**Features**:
- Supports REDUNDANT/COMPACT row formats
- Uses ibdata1 system dictionary
- No SDI (metadata stored in system tables)

### 5.3 MySQL80PageParser (MySQL 8.0)

```cpp
class MySQL80PageParser : public GenericPageParser {
public:
    explicit MySQL80PageParser(RowFormat format);

    bool parse_header(const uint8_t* data, size_t size) override;
    std::vector<Record> parse_records(const uint8_t* data, size_t size) override;
    std::optional<catalog::TableSchema> parse_sdi_metadata(
        const uint8_t* data, size_t size) override;

    // MySQL 8.0 specific: Instant DDL support
    bool parse_instant_ddl_metadata(const uint8_t* data, size_t size);
    uint64_t get_metadata_version() const { return metadata_version_; }

private:
    uint64_t metadata_version_;
};
```

**Features**:
- Supports REDUNDANT/COMPACT/DYNAMIC/COMPRESSED row formats
- Uses independent mysql.ibd system dictionary
- Supports SDI (Serialized Dictionary Info)
- Supports Instant DDL

### 5.4 MySQL84PageParser (MySQL 8.4+)

```cpp
class MySQL84PageParser : public MySQL80PageParser {
public:
    explicit MySQL84PageParser(RowFormat format);

    bool parse_header(const uint8_t* data, size_t size) override;
    std::optional<catalog::TableSchema> parse_sdi_metadata(
        const uint8_t* data, size_t size) override;
};
```

**Features**:
- Supports all MySQL 8.0 row formats
- Enhanced SDI support
- Better compression support

### 5.5 MySQL90PageParser (MySQL 9.0)

```cpp
class MySQL90PageParser : public MySQL84PageParser {
public:
    explicit MySQL90PageParser(RowFormat format);

    bool parse_header(const uint8_t* data, size_t size) override;
};
```

**Features**:
- Supports all MySQL 8.x row formats
- Reserved for new features

---

## 6. Unified Entry Design

### 6.1 PageParserV2 (Unified Parser)

```cpp
class PageParserV2 {
public:
    explicit PageParserV2(const uint8_t* data, size_t size);

    // Parse page
    PageParseResult parse();

    // Get parse result
    const PageParseResult& result() const { return result_; }
    PageParseResult& result() { return result_; }

    // Get parser
    IPageParserStrategy* parser() { return parser_.get(); }
    const IPageParserStrategy* parser() const { return parser_.get(); }

    // Re-parse (with specified version)
    PageParseResult parse_as(MySQLVersion version);

private:
    const uint8_t* data_;
    size_t size_;
    PageParseResult result_;
    std::unique_ptr<IPageParserStrategy> parser_;
};
```

### 6.2 Parse Result Structure

```cpp
struct PageParseResult {
    bool success;
    std::string error_message;
    uint16_t page_type;
    uint16_t page_level;
    RowFormat row_format;
    MySQLVersion version;
    uint32_t page_offset;
    uint32_t prev_page;
    uint32_t next_page;
    std::vector<Record> records;
    std::optional<catalog::TableSchema> schema;
    bool checksum_valid;
    uint32_t checksum_stored;
    uint32_t checksum_computed;
};
```

---

## 7. Utility Classes

### 7.1 PageScanner (Page Scanner)

```cpp
class PageScanner {
public:
    explicit PageScanner(size_t page_size = 16384);

    // Scan all pages in file
    std::vector<PageParseResult> scan_file(const std::string& file_path);

    // Scan pages in memory buffer
    std::vector<PageParseResult> scan_buffer(const uint8_t* data, size_t size);

    // Detect InnoDB page
    static bool is_innodb_page(const uint8_t* data, size_t size);

    // Get page size
    size_t page_size() const { return page_size_; }

private:
    size_t page_size_;
};
```

### 7.2 PageChainReconstructor (Page Chain Reconstructor)

```cpp
class PageChainReconstructor {
public:
    explicit PageChainReconstructor();

    // Add page
    void add_page(uint32_t page_no, const std::vector<uint8_t>& data);

    // Reconstruct page chain
    bool reconstruct();

    // Get reconstructed page chain
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages() const;

    // Get page chain statistics
    struct ChainStats {
        uint32_t total_pages;
        uint32_t valid_chain;
        uint32_t broken_chain;
        uint32_t circular_reference;
    };
    ChainStats stats() const;

private:
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pages_;
    std::map<uint32_t, std::vector<uint8_t>> page_map_;
    ChainStats stats_;
};
```

### 7.3 PageHealthChecker (Page Health Checker)

```cpp
class PageHealthChecker {
public:
    struct HealthInfo {
        bool is_valid;
        std::string status_message;
        uint32_t checksum_expected;
        uint32_t checksum_actual;
        bool checksum_match;
        bool has_infimum;
        bool has_supremum;
        uint16_t n_records;
        uint16_t n_dir_slots;
        uint16_t page_level;
        uint32_t next_page;
        uint32_t prev_page;
        uint16_t free_space;
    };

    static HealthInfo check(const uint8_t* data, size_t size);

private:
    static bool validate_checksum(const uint8_t* data, size_t size, 
                                  uint32_t* stored, uint32_t* computed);
    static bool validate_infimum_supremum(const uint8_t* data, size_t size);
    static bool validate_directory(const uint8_t* data, size_t size);
};
```

---

## 8. Usage Examples

### 8.1 Basic Usage

```cpp
#include "dbrecovery/innodb/page_parser_v2.hpp"

// Read page data
std::ifstream file("table.ibd", std::ios::binary);
std::vector<uint8_t> page_data(16384);
file.read(reinterpret_cast<char*>(page_data.data()), 16384);

// Create parser (auto-detects version)
dbrecovery::innodb::PageParserV2 parser(page_data.data(), page_data.size());
auto result = parser.parse();

// Get parse result
if (result.success) {
    std::cout << "Page Type: " << result.page_type << std::endl;
    std::cout << "MySQL Version: " << static_cast<int>(result.version) << std::endl;
    std::cout << "Row Format: " << static_cast<int>(result.row_format) << std::endl;
    std::cout << "Records: " << result.records.size() << std::endl;
    
    // Get table schema (if SDI available)
    if (result.schema.has_value()) {
        auto schema = result.schema.value();
        std::cout << "Table: " << schema.table_name << std::endl;
    }
}
```

### 8.2 File Scanning

```cpp
dbrecovery::innodb::PageScanner scanner(16384);
auto results = scanner.scan_file("table.ibd");

for (const auto& result : results) {
    if (result.success) {
        std::cout << "Page " << result.page_offset << ": "
                  << dbrecovery::innodb::PageTypeDetector::page_type_to_string(
                         result.page_type) << std::endl;
    }
}
```

### 8.3 Parse with Specified Version

```cpp
// Force use MySQL 8.0 parser
dbrecovery::innodb::PageParserV2 parser(page_data.data(), page_data.size());
auto result = parser.parse_as(dbrecovery::innodb::MySQLVersion::MYSQL_80);
```

---

## 9. Extensibility Design

### 9.1 Adding New Version Support

To add support for a new MySQL version:

1. Create a new parser class (inherit from `GenericPageParser`)
2. Implement `parse_header()` and `parse_records()` methods
3. Add version check in `PageParserFactory::create_parser()`

```cpp
class MySQL91PageParser : public MySQL90PageParser {
public:
    explicit MySQL91PageParser(RowFormat format);

    bool parse_header(const uint8_t* data, size_t size) override;
    // ... other methods
};

// Add in PageParserFactory::create_parser()
case MySQLVersion::MYSQL_91:
    return std::make_unique<MySQL91PageParser>(format);
```

### 9.2 Adding New Row Format

To add support for a new row format:

1. Add new value to `RowFormat` enum
2. Update `PageParserFactory::detect_row_format()` method
3. Implement new format parsing logic in corresponding parser

---

## 10. Performance Optimization

### 10.1 Zero-Copy Reading

Use `mmap` or `io_uring` for zero-copy reading:

```cpp
// Linux: Use mmap
void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);

// Windows: Use CreateFileMapping
HANDLE hMapping = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
```

### 10.2 Caching Strategy

```cpp
class PageCache {
public:
    std::optional<std::vector<uint8_t>> get(uint32_t page_no);
    void put(uint32_t page_no, std::vector<uint8_t> page_data);
    CacheStats get_stats() const;
};
```

### 10.3 Parallel Parsing

```cpp
class ParallelPageParser {
public:
    std::vector<std::pair<uint32_t, bool>> parse_all(
        const std::function<std::unique_ptr<BPlusTreeParser>(const std::vector<uint8_t>&)>& parser_factory);
};
```

---

## 11. Testing Strategy

### 11.1 Unit Tests

```cpp
TEST(PageParserFactoryTest, DetectMySQL80FromSDI) {
    // Create SDI page data
    std::vector<uint8_t> sdi_page(16384, 0);
    write_le<uint16_t>(sdi_page.data(), 16, FIL_PAGE_SDI);
    
    auto version = PageParserFactory::detect_mysql_version(sdi_page.data(), sdi_page.size());
    EXPECT_EQ(version, MySQLVersion::MYSQL_80);
}

TEST(PageParserFactoryTest, DetectRowFormatFromFlags) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), FIL_HEADER_SIZE + 42, 0x0002); // DYNAMIC
    
    auto format = PageParserFactory::detect_row_format(page.data(), page.size());
    EXPECT_EQ(format, RowFormat::DYNAMIC);
}
```

### 11.2 Integration Tests

```cpp
TEST(PageParserV2Test, ParseMySQL80Page) {
    // Load MySQL 8.0 page data
    std::vector<uint8_t> page_data = load_test_page("mysql80_page.ibd");
    
    PageParserV2 parser(page_data.data(), page_data.size());
    auto result = parser.parse();
    
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.version, MySQLVersion::MYSQL_80);
    EXPECT_EQ(result.page_type, FIL_PAGE_INDEX);
}
```

---

## 12. Compatibility Matrix

| Feature | MySQL 5.6 | MySQL 5.7 | MySQL 8.0 | MySQL 8.1+ | MySQL 9.0+ |
|---------|-----------|-----------|-----------|------------|------------|
| REDUNDANT | ✅ | ✅ | ✅ | ✅ | ✅ |
| COMPACT | ✅ | ✅ | ✅ | ✅ | ✅ |
| DYNAMIC | ❌ | ❌ | ✅ | ✅ | ✅ |
| COMPRESSED | ❌ | ❌ | ✅ | ✅ | ✅ |
| SDI | ❌ | ❌ | ✅ | ✅ | ✅ |
| Instant DDL | ❌ | ❌ | ✅ | ✅ | ✅ |
| io_uring | ❌ | ❌ | ✅ | ✅ | ✅ |

---

## 13. References

- MySQL 8.0 Source Code: `include/page0page.h`
- MySQL 8.0 Source Code: `include/fil0fil.h`
- MySQL 8.0 Source Code: `include/dict0dict.h`
- InnoDB Internal Structure Documentation

---

## 14. Version History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| v1.0.0 | 2026-03-29 | jliang.fang | Initial version |
| v2.0.0 | 2026-03-29 | jliang.fang | Complete architecture design |

---

<div align="center">
  <p>Building the world's leading MySQL data recovery tool</p>
</div>
