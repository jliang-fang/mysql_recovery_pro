# MySQL Recovery Pro v2.1.0 - Enhanced Edition

A high-performance offline data recovery tool for MySQL InnoDB storage engine with CLI and GUI interfaces.

## Features

### Core Features
- **Truncate Recovery**: Recover data from truncated tables
- **rm -rf Recovery**: Recover data from deleted files
- **Header Repair**: Repair corrupted page headers
- **BLOB Recovery**: Recover BLOB data with chain reconstruction
- **Undo Log Parsing**: Transaction rollback support
- **Redo Log Replay**: Redo log replay for recovery
- **TDE Support**: Transparent Data Encryption decryption

### Advanced Features
- **Corruption Detection**: Detect and report various types of corruption
- **Page Chain Repair**: Repair broken page chains
- **Record Chain Repair**: Repair broken record chains
- **Undo Log Parsing**: Parse undo logs for flashback
- **Redo Log Analysis**: Analyze redo logs for recovery planning
- **Performance Benchmarks**: Measure recovery performance

### CLI Features
- **Full Disk Scanning**: Scan entire data directories for tables
- **Table Name Search**: Search for tables by name with wildcards
- **Bulk Recovery**: Recover multiple tables at once
- **Auto Schema Inference**: Automatically infer table structure from data
- **JSON Export**: Export scan/search results to JSON format
- **Recursive Directory Scanning**: Scan directories with configurable depth
- **Verbose/Debug Logging**: Detailed output for troubleshooting

### GUI Features
- **File Information Display**: View file details and page statistics
- **Multiple Recovery Modes**: Default, Truncate, rm -rf, Header repair
- **Device Scanning**: Scan raw devices and partitions
- **Multi-format Export**: CSV, SQL, JSON formats
- **Progress Tracking**: Real-time progress and status updates
- **Batch Processing**: Process multiple files in sequence
- **Real-time Logging**: Monitor recovery progress
- **Pause/Resume**: Control recovery operations

### Enhanced Features (v2.1.0)
- **Full Disk Scanning**: Scan entire data directories for tables
- **Table Name Search**: Search for tables by name with wildcards
- **Bulk Recovery**: Recover multiple tables at once
- **Auto Schema Inference**: Automatically infer table structure from data
- **JSON Export**: Export scan/search results to JSON format
- **Recursive Directory Scanning**: Scan directories with configurable depth

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Application Layer                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │  Recovery    │  │   Repair     │  │    GUI       │      │
│  │   Engine     │  │   Engine     │  │   Interface  │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
├─────────────────────────────────────────────────────────────┤
│                     Page Parser Layer (V2)                   │
│  ┌──────────────┬──────────────┬──────────────┬───────────┐ │
│  │ 5.6/5.7 Parser│ 8.0 Parser │ 8.4 Parser │ 9.0 Parser│ │
│  └──────────────┴──────────────┴──────────────┴───────────┘ │
├─────────────────────────────────────────────────────────────┤
│                     InnoDB Module                            │
│  ┌──────────────┬──────────────┬──────────────┬───────────┐ │
│  │  B+Tree      │  Undo Log    │  Redo Log    │   SDI     │ │
│  │  Parser      │  Parser      │  Parser      │ Manager   │ │
│  └──────────────┴──────────────┴──────────────┴───────────┘ │
├─────────────────────────────────────────────────────────────┤
│                     Recovery Module                          │
│  ┌──────────────┬──────────────┬──────────────┬───────────┐ │
│  │ Truncate     │  rm -rf      │   BLOB       │   Disk    │ │
│  │ Recovery     │  Recovery    │  Recovery    │ Scanner   │ │
│  └──────────────┴──────────────┴──────────────┴───────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## Building

### Prerequisites
- CMake 3.20+
- C++20 compatible compiler (GCC 10+, Clang 12+, MSVC 2019+)
- nlohmann/json library
- zlib library
- GTest (optional, for testing)
- Qt5/Qt6 (optional, for GUI)

### Build Instructions

```bash
# Clone the repository
git clone <repository-url>
cd mysql_recovery_pro

# Create build directory
mkdir build && cd build

# Configure with CMake (CLI only)
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
make -j$(nproc)

# Run tests (if GTest is available)
make test
```

### Build with GUI

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DGUI_ENABLED=ON
make -j$(nproc)
```

**GUI Requirements:**
- Qt6 or Qt5 development libraries
- Qt6::Core, Qt6::Gui, Qt6::Widgets (or Qt5 equivalents)

## Usage

### Command Line Interface

```bash
# Basic recovery from specific .ibd file
./mysql_recovery_pro --ibd=/path/to/table.ibd --output=/tmp/recovered

# Truncate recovery
./mysql_recovery_pro --ibd=/path/to/table.ibd --truncate-recovery --output=/tmp/recovered

# rm -rf recovery
./mysql_recovery_pro --ibd=/path/to/table.ibd --rmrf-recovery --output=/tmp/recovered

# Header repair
./mysql_recovery_pro --ibd=/path/to/table.ibd --check-headers --output=/tmp/repaired

# Check for corruption
./mysql_recovery_pro --ibd=/path/to/table.ibd --check-corruption --output=/tmp/corruption

# Repair corrupted pages
./mysql_recovery_pro --ibd=/path/to/table.ibd --repair --output=/tmp/repaired

# BLOB recovery
./mysql_recovery_pro --ibd=/path/to/table.ibd --blob-recovery --output=/tmp/recovered

# Export to SQL
./mysql_recovery_pro --ibd=/path/to/table.ibd --format=sql --output=/tmp/recovered

# Scan entire data directory for tables
./mysql_recovery_pro --data-dir=/var/lib/mysql --scan --output=/tmp/scanned

# Search for table by name (supports wildcards * and ?)
./mysql_recovery_pro --data-dir=/var/lib/mysql --table-name="users*" --search

# Bulk recovery from rm -rf scenario
./mysql_recovery_pro --data-dir=/var/lib/mysql --rmrf-recovery --bulk --output=/tmp/recovered

# Verbose mode for detailed output
./mysql_recovery_pro --ibd=/path/to/table.ibd --output=/tmp/recovered --verbose

# Debug mode for troubleshooting
./mysql_recovery_pro --data-dir=/var/lib/mysql --scan --output=/tmp/scanned --debug
```

### GUI Usage

1. **Open File**: Select an .ibd file using File -> Open
2. **Recovery**: Choose recovery mode in the Recovery tab
3. **Options**: Enable BLOB recovery, set output path and format
4. **Start**: Click 'Start Recovery' to begin
5. **Results**: View results in the Results tab
6. **Device Scan**: Use Device Scan tab to scan raw devices
7. **Batch**: Add multiple files to the Batch tab for bulk recovery
8. **Log**: Monitor recovery progress in the Log tab

### Recovery Modes

#### Default Recovery
Standard recovery mode that attempts to recover all valid records.

#### Truncate Recovery
Recovers data from truncated tables by scanning B+Tree pages for valid records.

#### rm -rf Recovery
Recovers data from deleted files by scanning the file system or raw devices.

#### Header Repair
Repairs corrupted page headers and checksums.

## File Structure

```
mysql_recovery_pro/
├── include/              # Header files
│   ├── dbrecovery/       # Main library headers
│   │   ├── catalog/      # Catalog definitions
│   │   ├── common/       # Common utilities
│   │   ├── crypt/        # Encryption support
│   │   ├── disk/         # Disk scanning
│   │   ├── engine/       # Recovery engine
│   │   ├── export/       # Export functionality
│   │   ├── innodb/       # InnoDB parsing
│   │   ├── recovery/     # Recovery modules
│   │   ├── repair/       # Repair modules
│   │   └── utils/        # Utilities
│   └── page/             # Page parser headers
├── src/                  # Source files
│   ├── engine/           # Engine implementations
│   ├── export/           # Export implementations
│   ├── innodb/           # InnoDB implementations
│   ├── recovery/         # Recovery implementations
│   ├── repair/           # Repair implementations
│   ├── utils/            # Utility implementations
│   └── page/             # Page parser implementations
├── docs/                 # Documentation
├── gui/                  # GUI source files
├── scripts/              # Build and test scripts
├── tests/                # Test files
└── tools/                # Utility tools
```

## Supported MySQL Versions

- MySQL 5.5/5.6/5.7
- MySQL 8.0/8.4/9.0

## Supported Row Formats

- REDUNDANT
- COMPACT
- DYNAMIC
- COMPRESSED

## Recovery Scenarios

1. **Truncated Tables**: Recover data after `TRUNCATE TABLE`
2. **Deleted Files**: Recover data after `rm -rf`
3. **Corrupted Headers**: Repair corrupted page headers
4. **BLOB Recovery**: Recover BLOB/TEXT data
5. **Partial Recovery**: Recover partial data from damaged files
6. **Undo Log**: Transaction rollback support
7. **Redo Log**: Redo log replay for point-in-time recovery

### Enhanced Recovery Scenarios (v2.1.0)

1. **Data Directory Scan**: Scan entire MySQL data directory to find all tables
2. **Table Name Search**: Search for specific tables by name with wildcard support
3. **Bulk Recovery**: Recover multiple tables from rm -rf scenario at once
4. **Auto Schema Inference**: Automatically infer table structure when SDI is unavailable
5. **JSON Export**: Export scan/search results to JSON for further processing

### CLI Recovery Scenarios

1. **Scan Data Directory**: `./mysql_recovery_pro --data-dir=/var/lib/mysql --scan --output=/tmp/scanned`
2. **Search for Table**: `./mysql_recovery_pro --data-dir=/var/lib/mysql --table-name="users*" --search`
3. **Bulk Recovery**: `./mysql_recovery_pro --data-dir=/var/lib/mysql --rmrf-recovery --bulk --output=/tmp/recovered`
4. **Verbose Logging**: `./mysql_recovery_pro --ibd=file.ibd --output=/tmp --verbose`
5. **Debug Mode**: `./mysql_recovery_pro --data-dir=/var/lib/mysql --scan --debug`

### GUI Recovery Scenarios

1. **Batch Processing**: Add multiple files to the Batch tab and start recovery
2. **Real-time Monitoring**: Watch progress in the Log tab
3. **Pause/Resume**: Control recovery operations with Pause/Resume buttons
4. **Format Selection**: Choose output format (CSV, SQL, JSON) in the Recovery tab

## Performance

The tool is optimized for high-performance recovery:

- **Zero-copy reading**: Uses mmap for file access
- **Parallel processing**: Multi-threaded recovery
- **Efficient parsing**: Optimized InnoDB page parsing
- **Memory pooling**: Custom memory pools for allocation efficiency

## Testing

```bash
# Run all tests
make test

# Run specific test
./test_recovery_engine
./test_corruption_scenarios
./test_page_types_v2
./test_corruption_detector
```

## Documentation

- [Architecture Guide](docs/en/architecture.md)
- [User Guide](docs/en/user_guide.md)
- [Developer Guide](docs/en/developer_guide.md)
- [Benchmark Guide](docs/en/benchmark_guide.md)

## Contributing

Contributions are welcome! Please follow these guidelines:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Run tests
5. Submit a pull request

## License

MIT License - See LICENSE file for details.

## Author

- **Jliang Fang** - Initial work

## Acknowledgments

- MySQL InnoDB documentation
- Percona XtraBackup
- MySQL source code

## Changelog

### v2.1.0 - Enhanced Recovery Features (Current)
- Integrated all enhanced features into CLI and GUI interfaces
- Added full disk scanning for data directories
- Added table name search with wildcard support
- Added bulk recovery mode for multiple tables
- Added auto schema inference from record data
- Added JSON export for scan/search results
- Added recursive directory scanning with configurable depth
- Added raw device scanning for disk-level recovery
- Added verbose and debug logging modes
- Added batch processing in GUI
- Added real-time logging in GUI
- Added pause/resume controls in GUI
- Improved rm -rf recovery with better page detection
- Enhanced CLI with new modes: --scan, --search, --bulk
- Enhanced GUI with Batch and Log tabs

### v2.0.0 - Enhanced Edition
- Added GUI enhancements with device scanning
- Implemented corruption detection and repair
- Added BLOB chain reconstruction
- Implemented Undo Log parsing
- Implemented Redo Log replay
- Added TDE decryption support
- Added performance benchmarks
- Improved documentation

### v1.0.0 - Initial Release
- Basic recovery functionality
- Page parsing support
- BLOB recovery
- rm -rf recovery
- Header repair

## Support

For support, please:
1. Check the documentation
2. Run with verbose logging
3. Submit an issue with detailed information

## Roadmap

### Completed

#### Phase 1: Page Parser (Complete)
- [x] Week 1: Page Header auto-detection + Page Type recognition
- [x] Week 2: Row format recognition + Compact/Dynamic parsing

#### Phase 2: B+Tree Analysis (In Progress)
- [x] Week 3: B+Tree traversal + Chain break detection
- [x] Week 4: Heuristic scanning + Bad block inference

#### Phase 3: Advanced Recovery (Planned)
- [ ] Week 5: Undo Log parsing + Physical flashback
- [ ] Week 6: SDI metadata extraction + Instant DDL
- [ ] Week 7: TDE decryption + Keyring loading

#### Phase 4: Performance & Platform (Planned)
- [ ] Week 8: io_uring integration + Cross-platform adaptation

### Future Enhancements

- [ ] Support for more MySQL versions
- [ ] Transaction isolation support
- [ ] Incremental recovery
- [ ] Cloud storage integration
- [ ] Web interface
