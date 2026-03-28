# MySQL 8.0 Recovery Engine

[English](./README.md) | [中文](../zh/README.md)

A high-performance C++ library for offline MySQL 8.0+ data recovery.

## 🚀 Features

- **InnoDB Page Parsing**: Full support for COMPACT/REDUNDANT/DYNAMIC/COMPRESSED row formats
- **B+Tree Traversal**: Recursive index traversal with page chain validation
- **BLOB Recovery**: Multi-page BLOB reconstruction with checksum verification
- **Redo Log Replay**: Redo log parsing and replay for all InnoDB row formats
- **Undo Log Parsing**: Transaction rollback support
- **SDI Metadata Parsing**: Space Descriptor Index parsing and table structure reconstruction
- **Cross-platform**: Linux/Windows unified API
- **CLI/GUI**: Command-line tool and graphical interface

## 📊 Architecture

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│   FileMapper    │────▶│  PageReader      │────▶│  BPlusTreeParser│
└─────────────────┘     └──────────────────┘     └─────────────────┘
                                                      │
                                                      ▼
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  BlobPointer    │◀────│  SchemaDefs      │◀────│  RecordParser   │
└─────────────────┘     └──────────────────┘     └─────────────────┘
                                                      │
                                                      ▼
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  RedoLog        │     │  UndoLog         │     │  Recovery       │
│  Parser         │     │  Parser          │     │  Engine         │
└─────────────────┘     └──────────────────┘     └─────────────────┘
                                                      │
                                                      ▼
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  ExportModule   │     │  CLI/GUI         │     │  TestSuite      │
│  (CSV/SQL/ibd)  │     │  Frontend        │     │  (GTest)        │
└─────────────────┘     └──────────────────┘     └─────────────────┘
```

## 📦 Quick Start

### Build

```bash
# Clone project
cd /app/mysql8_recovery

# Create build directory
mkdir -p build && cd build

# Configure CMake
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
make -j$(nproc)

# Run tests
ctest --verbose
```

### CLI Tool

```bash
# Recover table data
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered

# Specify table ID
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --table-id=12345

# Show help
./mysql8_recover --help
```

### GUI Tool

```bash
# Launch GUI
cd build/gui
./mysql8_recovery_gui
```

## 📚 API Reference

### PageReader

```cpp
class PageReader {
public:
    static bool validate_checksum(const std::vector<uint8_t>& page_data,
                                  bool strict_mode = true,
                                  uint32_t* out_stored = nullptr,
                                  uint32_t* out_computed = nullptr);
    
    static uint32_t compute_crc32c(const std::vector<uint8_t>& page_data);
    static uint32_t compute_innodb_checksum(const std::vector<uint8_t>& page_data);
};
```

### BPlusTreeParser

```cpp
class BPlusTreeParser {
public:
    bool parse() noexcept;
    std::vector<std::map<std::string, std::string>> parse_records(
        const catalog::TableSchema& schema) const;
    
    void apply_redo_record(const RedoLogRecord& record);
    void update_page_checksum();
};
```

### RecoveryEngine

```cpp
class RecoveryEngine {
public:
    bool open(const std::string& file_path);
    void close();
    
    std::vector<std::map<std::string, std::string>> recover_truncate(
        const catalog::TableSchema& schema);
    
    bool export_to_csv(const std::vector<std::map<std::string, std::string>>& records,
                       const std::string& csv_path);
    bool export_to_sql(const std::vector<std::map<std::string, std::string>>& records,
                       const std::string& table_name,
                       const std::string& sql_path);
};
```

## 🧪 Testing

```bash
# Run all tests
ctest --verbose

# Run specific test
./build/tests/test_page_header
./build/tests/test_redo_log
./build/tests/test_blob_recovery

# Generate test data
python3 tools/ibd_generator.py test_table
```

## 📖 Documentation

- [Architecture](./architecture.md) - Detailed architecture documentation
- [User Guide](./user_guide.md) - Complete user guide
- [Principle Document](./) - Principle explanations

## 🔧 Technology Stack

- **Language**: C++20
- **Build System**: CMake 3.20+
- **Testing Framework**: Google Test
- **JSON Processing**: nlohmann/json
- **Compression Library**: zlib

## 🌍 Supported MySQL Versions

| Version | Row Formats | Checksums | SDI | Status |
|---------|-------------|-----------|-----|--------|
| 5.6     | REDUNDANT, COMPACT | INNODB, OLD | None | ✅ Basic Support |
| 5.7     | REDUNDANT, COMPACT | INNODB, CRC32C | None | ✅ Basic Support |
| 8.0     | REDUNDANT, COMPACT, DYNAMIC, COMPRESSED | CRC32C | Supported | ✅ Full Support |
| 8.1+    | All 8.0 formats | CRC32C | Supported | ✅ Full Support |
| 9.0+    | All 8.0 formats | CRC32C | Supported | 🔄 Future Support |

## 📊 Supported Disaster Scenarios

| Scenario | Recovery Capability | Notes |
|----------|---------------------|-------|
| Truncate | ✅ Full Recovery | Based on page chain and Redo Log |
| rm -rf | ✅ Partial Recovery | Requires uncovered data pages |
| File header corruption | ✅ Repair | Rebuild file header |
| Tablespace corruption | ✅ Partial Recovery | Based on available pages |
| Redo Log application | ✅ Full Recovery | Forward recovery |
| BLOB corruption | ✅ Partial Recovery | Based on page chain |

## 🎯 Project Goals

1. **World-class**: Achieve top-level capabilities in low-level data parsing
2. **Offline-capable**: Recover data without database connection
3. **Industrial-grade**: Meet strict industrial delivery standards
4. **Cross-platform**: Linux/Windows unified API
5. **User-friendly**: CLI and GUI dual support

## 🤝 Contribution Guidelines

Issues and Pull Requests are welcome!

### Code Style

- Follow Google C++ Style Guide
- Use clang-format for formatting
- Add unit tests

### Commit Convention

```
type(scope): description

body

footer
```

Types: feat, fix, docs, style, refactor, test, chore

## 📄 License

MIT License

## 👨‍💻 Author

**jliang.fang** - Chief Database Kernel Architect

---

<div align="center">
  <p>Building the world's leading MySQL data recovery tool</p>
</div>
