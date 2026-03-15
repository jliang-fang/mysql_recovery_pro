# MySQL 8.0 Recovery Engine

A high-performance C++ library for offline MySQL 8.0+ data recovery.

## Features

- **InnoDB Page Parsing**: Full support for COMPACT/REDUNDANT row formats
- **B+Tree Traversal**: Recursive index traversal with page chain validation
- **BLOB Recovery**: Multi-page BLOB reconstruction with checksum verification
- **Cross-platform**: Linux/Windows support with unified API

## Architecture

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│   FileMapper    │────▶│  PageReader      │────▶│  BPlusTreeParser│
└─────────────────┘     └──────────────────┘     └─────────────────┘
                                                      │
                                                      ▼
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  BlobPointer    │◀────│  SchemaDefs      │◀────│  RecordParser   │
└─────────────────┘     └──────────────────┘     └─────────────────┘
```

## Quick Start

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)

# Recover table data
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered
```

## API Reference

### PageReader

```cpp
class PageReader {
public:
    std::vector<uint8_t> read_page(uint32_t page_no);
    static bool validate_checksum(const std::vector<uint8_t>& page_data);
};
```

### BPlusTreeParser

```cpp
class BPlusTreeParser {
public:
    bool parse() noexcept;
    std::vector<std::map<std::string, std::string>> parse_records(
        const catalog::TableSchema& schema) const;
};
```

## Testing

```bash
# Run unit tests
ctest --verbose

# Generate test data
python3 tools/ibd_generator.py test_table
```
