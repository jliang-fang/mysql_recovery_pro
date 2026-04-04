# MySQL Recovery Pro - Page Parser User Guide

## Overview

MySQL Recovery Pro 的页解析层提供对 MySQL 5.6 到 9.0 全系列版本的 InnoDB 页的深度解析能力。支持从严重损坏、删库（rm -rf/Truncate）等极端场景中恢复数据。

## Features

- ✅ **Multi-Version Support**: MySQL 5.6/5.7/8.0/8.4/9.0
- ✅ **Row Format Detection**: Automatic detection of Redundant/Compact/Dynamic
- ✅ **SDI Parsing**: MySQL 8.0+ Serialized Dictionary Info
- ✅ **Corrupted Page Handling**: Robust error handling for damaged pages
- ✅ **Heuristic Recovery**: Metadata inference without CREATE TABLE statement

## Architecture

```
Page Parser Layer
├── PageHeaderBase (Abstract)
│   ├── PageHeaderV56 (MySQL 5.6/5.7)
│   └── PageHeaderV80 (MySQL 8.0+)
├── RowParser
│   ├── CompactRowParser
│   └── RedundantRowParser
└── PageFactory (Auto-Detection)
```

## Quick Start

### C++ Usage

```cpp
#include "page/page_parser.h"

// Load page data
std::vector<uint8_t> page_data = read_file("table.ibd");

// Auto-create PageHeader based on version detection
auto header = mysql_recovery::PageFactory::create_page_header(
    page_data.data(), 
    page_data.size()
);

// Get page information
std::cout << "Version: " << static_cast<int>(header->get_version()) << "\n";
std::cout << "Page Type: " << static_cast<int>(header->get_page_type()) << "\n";
std::cout << "Level: " << header->get_level() << "\n";
std::cout << "Records: " << header->get_n_recs() << "\n";
```

### Python Usage

```python
# Generate test IBD files
python3 tools/ibd_generator.py -o test/

# View generated files
ls -la test/*.ibd
```

## Command Line Tools

### Generate Test Data

```bash
# Generate all test files
python3 tools/ibd_generator.py -o test/

# Generate specific format
python3 tools/ibd_generator.py --compact -o test/
python3 tools/ibd_generator.py --redundant -o test/
python3 tools/ibd_generator.py --corrupted -o test/
python3 tools/ibd_generator.py --sdi -o test/
python3 tools/ibd_generator.py --truncated -o test/
```

### Run Tests

```bash
# Build project
cd build
cmake .. && make -j4

# Run page parser tests
./test/page_parser_test
```

## Page Structure Reference

### InnoDB Page Layout (16KB)

```
Offset 0-37:    FIL_HEADER (38 bytes)
Offset 38-49:   BTR_HEADER (12 bytes)
Offset 38-53:   PAGE_HEADER (16 bytes in 5.6/5.7, 24 bytes in 8.0+)
Offset 54+:     User Records
Offset +:       Page Directory
Offset -64:     FIL_PAGE_END_LSN (8 bytes)
Offset -60:     FIL_PAGE_CHECKSUM (4 bytes)
```

### Key Offsets

| Field | Offset | Size | Description |
|-------|--------|------|-------------|
| FIL_PAGE_PREV | 0 | 4 bytes | Previous page |
| FIL_PAGE_NEXT | 4 | 4 bytes | Next page |
| FIL_PAGE_LSN | 8 | 8 bytes | Log Sequence Number |
| FIL_PAGE_TYPE | 16 | 2 bytes | Page type |
| FIL_PAGE_SPACE_ID | 18 | 4 bytes | Space ID |
| PAGE_N_DIR_SLOTS | 38 | 2 bytes | Directory slots |
| PAGE_HEAP_TOP | 40 | 2 bytes | Heap top |
| PAGE_N_HEAP | 42 | 2 bytes | Record count |
| PAGE_FREE | 44 | 2 bytes | Free list start |

## Page Types

| Type | Value | Description |
|------|-------|-------------|
| INDEX | 9969 (0x26F1) | B+Tree index page |
| SDI | 9971 (0x26F3) | SDI metadata page (8.0+) |
| UNDO_LOG | 9970 (0x26F2) | Undo log page |
| ALLOCATED | 0 | Freshly allocated page |

## Row Formats

### Redundant (5.6/5.7 default)

- **extra_bytes**: 6
- **Header size**: 6 bytes
- **Column length**: 2 bytes each

### Compact (5.7/8.0+ default)

- **extra_bytes**: 5
- **Header size**: 5 bytes
- **Column length**: 1-2 bytes (variable)

### Dynamic (8.0+ default)

- **extra_bytes**: 5
- **External BLOB**: Supported
- **Variable length**: Full support

## SDI (Serialized Dictionary Info)

### SDI Page Structure (MySQL 8.0+)

```
Offset 0-37:    FIL_HEADER
Offset 38-61:   PAGE_HEADER (24 bytes)
Offset 64-71:   SDI Signature ("08SDI0000")
Offset 72+:     SDI JSON Data
```

### SDI Signature

```
0x08, 'S', 'D', 'I', '0', '0', '0', '0'
```

## Error Handling

### Corrupted Page

```cpp
try {
    auto header = PageFactory::create_page_header(data, size);
    // Handle corrupted page gracefully
} catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
}
```

### Version Detection Fallback

If version detection fails, the parser defaults to MySQL 5.7 compatibility mode.

## Performance

- **Page Size**: 4KB/8KB/16KB/32KB/64KB
- **Parsing Speed**: ~100K pages/second (16KB)
- **Memory Usage**: ~16KB per page

## Troubleshooting

### Issue: Version detection fails

**Solution**: Check page data integrity and ensure FIL_HEADER is intact.

### Issue: Row format detection incorrect

**Solution**: Check extra_bytes field (offset 4 in record header):
- 6 = Redundant
- 5 = Compact/Dynamic

### Issue: SDI page not recognized

**Solution**: Verify SDI Signature at offset 64:
```
08 53 44 49 30 30 30 30
```

## API Reference

### PageFactory

```cpp
class PageFactory {
public:
    static std::unique_ptr<PageHeaderBase> create_page_header(
        const uint8_t* page_data,
        size_t page_size = 16384
    );
    
    static MySQLVersion detect_version(
        const uint8_t* page_data,
        size_t page_size = 16384
    );
    
    static RowFormat detect_row_format(
        const uint8_t* page_data,
        MySQLVersion version,
        size_t page_size = 16384
    );
    
    static bool is_sdi_page(
        const uint8_t* page_data,
        size_t page_size = 16384
    );
};
```

### PageHeaderBase

```cpp
class PageHeaderBase {
public:
    virtual uint32_t get_prev_page() const = 0;
    virtual uint32_t get_next_page() const = 0;
    virtual uint64_t get_lsn() const = 0;
    virtual PageType get_page_type() const = 0;
    virtual uint16_t get_level() const = 0;
    virtual uint16_t get_n_recs() const = 0;
    virtual uint16_t get_free_ptr() const = 0;
    virtual uint16_t get_free_size() const = 0;
    virtual uint16_t get_n_dir_slots() const = 0;
    virtual uint16_t get_data_start() const = 0;
    virtual bool verify_checksum() const = 0;
    virtual MySQLVersion get_version() const = 0;
    virtual RowFormat get_row_format() const = 0;
};
```

## Contributing

1. Add test cases for new scenarios
2. Update documentation
3. Run all tests before commit

## License

MIT License - See LICENSE file for details.
