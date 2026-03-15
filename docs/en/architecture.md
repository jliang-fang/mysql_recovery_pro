# Architecture Design

## Core Modules

### 1. FileMapper
- Cross-platform file mapping (mmap/Win32)
- RAII resource management
- Move semantics support

### 2. PageReader
- CRC32C checksum validation (MySQL 8.0 default)
- INNODB/OLD checksum compatibility
- Performance-optimized lookup table implementation

### 3. BPlusTreeParser
- Directory slot parsing
- Record offset extraction
- Health status checking

## Data Flow

```
.ibd File → FileMapper → PageReader → BPlusTreeParser → Records
                                              ↓
                                         BlobPointer → BLOB Data
```

## Extensibility Points

### Adding New MySQL Versions

```cpp
// Add version-specific constants in page_defs.hpp
constexpr uint16_t MYSQL_8_1_FEATURE_FLAG = 0x8000;
```

### Adding New Row Formats

```cpp
enum class RowFormat {
    COMPACT,
    DYNAMIC,
    REDUNDANT,
    COMPRESSED  // Future support
};
```
