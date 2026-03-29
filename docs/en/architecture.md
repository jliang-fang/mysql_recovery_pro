# MySQL 8.x Recovery Engine - Architecture Design Document

## 1. Overview

This project is a high-performance offline MySQL 8.x data recovery engine developed in C++, aiming to build a world-leading, offline-capable deep data recovery tool for MySQL 5.6, 5.7, 8.0, 8.1, 8.4, and 9.0.

### 1.1 Core Capabilities

- **InnoDB Page Parsing**: Full support for COMPACT/REDUNDANT/DYNAMIC/COMPRESSED row formats
- **B+Tree Traversal**: Recursive index traversal with page chain validation
- **BLOB Recovery**: Multi-page BLOB reconstruction with checksum validation
- **Redo Log Replay**: Redo log parsing and replay for all InnoDB row formats
- **Undo Log Parsing**: Transaction rollback support
- **SDI Metadata Parsing**: Space Descriptor Index parsing and table structure reconstruction
- **Cross-platform Support**: Linux/Windows unified API

### 1.2 Technology Stack

- **Language**: C++20
- **Build System**: CMake 3.20+
- **Testing Framework**: Google Test
- **JSON Processing**: nlohmann/json
- **Compression Library**: zlib (SDI decompression)

## 2. Module Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         MySQL 8.x Recovery Engine                   │
├─────────────────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────┐ │
│  │   FileMapper │  │  PageReader  │  │ BPlusTreeParser││SchemaDefs│ │
│  └──────────────┘  └──────────────┘  └──────────────┘  └──────────┘ │
│         │                │                  │               │        │
│         ▼                ▼                  ▼               ▼        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────┐ │
│  │   FileMapper │  │ CRC32C/INNODB│  │  RecordParser  │  │Catalog  │ │
│  │  (mmap/Win32)│  │  Checksum    │  │  BLOB Parser   │  │Table    │ │
│  └──────────────┘  └──────────────┘  └──────────────┘  └──────────┘ │
│         │                │                  │               │        │
│         ▼                ▼                  ▼               ▼        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────┐ │
│  │  RedoLog     │  │  UndoLog     │  │  Recovery      │  │Exporter  │ │
│  │  Parser      │  │  Parser      │  │  Engine        │  │Module    │ │
│  └──────────────┘  └──────────────┘  └──────────────┘  └──────────┘ │
│         │                │                  │               │        │
│         ▼                ▼                  ▼               ▼        │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                    CLI/GUI Frontend                          │   │
│  └──────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 3. Core Module Details

### 3.1 FileMapper

**Responsibility**: Cross-platform file mapping

**Features**:
- Linux: Uses `mmap()` for memory mapping
- Windows: Uses `CreateFileMappingA()` and `MapViewOfFile()`
- RAII resource management
- Move semantics support

**Key Interfaces**:
```cpp
class FileMapper {
public:
    explicit FileMapper(const std::string& path);
    ~FileMapper();

    bool is_valid() const;
    const uint8_t* data() const;
    size_t size() const;
};
```

### 3.2 PageReader

**Responsibility**: InnoDB page checksum calculation and validation

**Features**:
- CRC32C checksum validation (MySQL 8.0 default)
- INNODB checksum compatibility (MySQL 5.6/5.7)
- OLD checksum compatibility (MySQL 5.5)
- Performance-optimized lookup table implementation

**Key Interfaces**:
```cpp
class PageReader {
public:
    static bool validate_checksum(const std::vector<uint8_t>& page_data,
                                  bool strict_mode = true,
                                  uint32_t* out_stored = nullptr,
                                  uint32_t* out_computed = nullptr);

    static uint32_t compute_crc32c(const std::vector<uint8_t>& page_data);
    static uint32_t compute_innodb_checksum(const std::vector<uint8_t>& page_data);
    static uint32_t compute_old_checksum(const std::vector<uint8_t>& page_data);
};
```

### 3.3 BPlusTreeParser

**Responsibility**: B+Tree index page parsing

**Features**:
- Page header parsing
- Record offset extraction
- Health status checking
- INFIMUM/SUPREMUM record validation
- Directory slot parsing

**Key Interfaces**:
```cpp
class BPlusTreeParser {
public:
    explicit BPlusTreeParser(const std::vector<uint8_t>& page_data);

    bool parse() noexcept;
    bool is_valid_page_type() const;

    std::vector<std::map<std::string, std::string>> parse_records(
        const catalog::TableSchema& schema) const;

    // Redo replay support
    void apply_redo_record(const RedoLogRecord& record);
    void update_page_checksum();

    // Field offset calculation
    static std::optional<uint16_t> calculate_dynamic_field_offset(
        const uint8_t* record_data,
        uint16_t field_no,
        size_t record_size,
        bool is_compact_format);
};
```

### 3.4 SchemaDefs

**Responsibility**: Table structure definition and metadata management

**Features**:
- ColumnInfo: Column definition
- IndexInfo: Index definition
- TableSchema: Table-level metadata
- JSON serialization/deserialization

**Key Interfaces**:
```cpp
struct ColumnInfo {
    std::string name;
    uint32_t mysql_type_id;
    bool is_nullable;
    uint32_t column_length;
    uint32_t charset_id;
    // ... more fields
};

struct TableSchema {
    std::string table_name;
    uint32_t row_format;
    uint32_t page_size;
    std::map<uint32_t, ColumnInfo> columns;
    std::vector<IndexInfo> indexes;
    // ... more fields
};
```

### 3.5 RedoLogParser

**Responsibility**: Redo Log parsing

**Features**:
- Parse Redo Log record headers
- Support all InnoDB Redo Log types
- Redo replay support

**Key Interfaces**:
```cpp
class RedoLogParser {
public:
    std::optional<RedoLogRecord> parse_record(const uint8_t* data, size_t len);
    std::vector<RedoLogRecord> parse_file(const std::string& path);
};
```

### 3.6 UndoLogParser

**Responsibility**: Undo Log parsing

**Features**:
- Parse Undo Log records
- Transaction rollback support

**Key Interfaces**:
```cpp
class UndoLogParser {
public:
    std::optional<UndoLogRecord> parse_record(const uint8_t* data, size_t len);
    std::vector<UndoLogRecord> parse_file(const std::string& path);
};
```

### 3.7 SDIManager

**Responsibility**: SDI (Space Descriptor Index) parsing

**Features**:
- Parse SDI header
- ZLIB decompression (raw deflate)
- Table structure extraction

**Key Interfaces**:
```cpp
class SDIManager {
public:
    static std::optional<std::string> extract_sdi_json(
        const uint8_t* payload, size_t payload_len);

    static catalog::TableSchema parse_sdi_record(
        const uint8_t* payload, size_t payload_len);
};
```

### 3.8 BlobPointer

**Responsibility**: BLOB pointer parsing and page chain management

**Features**:
- BLOB pointer parsing (20 bytes)
- Multi-page BLOB page chain construction
- Circular reference detection
- Page chain integrity validation

**Key Interfaces**:
```cpp
struct BlobPointer {
    uint32_t space_id;
    uint32_t page_no;
    uint32_t offset;
    uint32_t len;
    uint32_t total_len;
    std::vector<std::pair<uint32_t, uint32_t>> chain_pages;

    bool is_complete() const;
    bool is_valid(const std::function<...>& page_reader, bool strict_mode) const;
};

struct BlobPageChain {
    std::vector<std::pair<uint32_t, uint32_t>> pages;

    void add_page(uint32_t space_id, uint32_t page_no);
    bool is_valid(const std::function<...>& page_reader, bool strict_mode) const;
    bool has_circular_reference() const;
};
```

### 3.9 Exporter Module

**Responsibility**: Data export functionality

**Features**:
- CSV export
- SQL export
- IBD export (repaired tablespace)

**Key Interfaces**:
```cpp
class CSVExporter : public DataExporter {
public:
    explicit CSVExporter(const std::vector<std::string>& headers);
    bool export_record(const std::map<std::string, std::string>& record) override;
    std::string get_result() const override;
};

class SQLOutputter : public DataExporter {
public:
    explicit SQLOutputter(const std::string& table_name);
    bool export_record(const std::map<std::string, std::string>& record) override;
    std::string get_result() const override;
};

class IBDExporter : public DataExporter {
public:
    explicit IBDExporter(uint32_t page_size = 16384);
    bool export_to_ibd(const std::string& file_path,
                      const std::map<std::string, std::string>& table_schema);
};
```

## 4. Data Flow

```
.ibd File → FileMapper → PageReader → BPlusTreeParser → Records
                                              ↓
                                         BlobPointer → BLOB Data
                                              ↓
                                         SchemaDefs → Table Schema
                                              ↓
                                         RecoveryEngine → Recovered Data
                                              ↓
                                         Exporter Module → CSV/SQL/IBD
```

## 5. Extension Points

### 5.1 Adding New MySQL Versions

```cpp
// Add version-specific constants in page_defs.hpp
constexpr uint16_t MYSQL_8_1_FEATURE_FLAG = 0x8000;
```

### 5.2 Adding New Row Formats

```cpp
enum class RowFormat {
    COMPACT,
    DYNAMIC,
    REDUNDANT,
    COMPRESSED,  // Future support
    CUSTOM,      // Custom format
};
```

### 5.3 Adding New Export Formats

```cpp
class ExporterModule {
public:
    virtual bool export_to_format(const std::vector<...>& records,
                                  const std::string& path) = 0;
};

class CSVExporter : public ExporterModule { ... };
class SQLOutputter : public ExporterModule { ... };
class IBDExporter : public ExporterModule { ... };
```

## 6. Performance Optimization

### 6.1 CRC32C Lookup Table

```cpp
static constexpr auto crc32c_table = generate_crc32c_table();
// Compile-time generation, zero runtime overhead
```

### 6.2 Memory Mapping

Use mmap to avoid extra memory copies

### 6.3 Zero-copy View

```cpp
std::span<const uint8_t> get_page_view(uint32_t page_no);
```

## 7. Testing Strategy

### 7.1 Unit Tests

- Each core module has independent unit tests
- Coverage of normal scenarios, edge cases, and error handling

### 7.2 Integration Tests

- Complete recovery flow testing
- Multi-page BLOB recovery testing
- Redo log replay testing

### 7.3 Fuzz Testing

- Use libFuzzer or AFL for fuzz testing
- Test file parser robustness

## 8. Future Extensions

### 8.1 Supported MySQL Versions

- MySQL 5.6/5.7 (REDUNDANT/COMPACT)
- MySQL 8.0/8.1/8.2/8.3/8.4/8.5 (DYNAMIC/COMPRESSED)
- MySQL 9.0 (Future versions)

### 8.2 Supported Disaster Scenarios

- Truncate recovery
- rm -rf recovery
- File header corruption recovery
- Tablespace corruption recovery
- Redo log application

### 8.3 GUI Features

- File selection and preview
- Page-level visualization
- Record-level preview
- Export configuration

## 9. References

- MySQL 8.0 Source Code: `include/page0page.h`, `include/fil0fil.h`
- InnoDB Internal Structure: https://dev.mysql.com/doc/refman/8.0/en/innodb-file-space.html
- B+Tree Algorithm: https://dev.mysql.com/doc/refman/8.0/en/innodb-index-types.html
