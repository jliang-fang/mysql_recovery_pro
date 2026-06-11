# MySQL 5.6 Support

## Overview

MySQL Recovery Pro now supports MySQL 5.6 full compatibility with redo/undo log recovery.

## Supported Features

### MySQL 5.6
- **Page Format**: REDUNDANT, COMPACT row formats
- **System Tables**: Uses ibdata1 system dictionary (no SDI)
- **Page Size**: 4KB, 8KB, 16KB, 32KB, 64KB
- **Row Formats**: REDUNDANT (legacy), COMPACT (standard)

### MySQL 5.7 (Also Supported)
- Enhanced page header support
- All MySQL 5.6 features
- Additional index compression support

### MySQL 8.0 (Also Supported)
- SDI (Serialized Dictionary Info) pages
- LSN extension support
- Data dictionary in mysql.ibd

## Testing

### Run MySQL 5.6 Compatibility Tests
```bash
cd build/tests
./test_mysql56_57_compatibility
```

### Test Recovery with MySQL 5.6 Data
```bash
./bin/mysql_recovery_pro \
  --ibd=test_data/test_mysql56.ibd \
  --mysql-version=56 \
  --output=/tmp/recovered
```

### Version Detection
```bash
./bin/mysql_recovery_pro \
  --ibd=test_data/test_mysql56.ibd \
  --detect-version \
  --verbose
```

## Command Line Options

### MySQL Version
```
--mysql-version=<v>    MySQL version: 56/57/80/84
--detect-version       Auto-detect MySQL version from IBD header
```

### Row Format
```
--row-format=<fmt>     Row format: redundant/compact/dynamic/compressed
```

### Page Size
```
--page-size=<size>     Page size: 4096/8192/16384/32768/65536
```

## Supported Commands

### Truncate Recovery
```bash
./bin/mysql_recovery_pro \
  --ibd=data.ibd \
  --truncate-recovery \
  --mysql-version=56 \
  --output=/tmp/recovery
```

### RM-RF Recovery
```bash
./bin/mysql_recovery_pro \
  --ibd=data.ibd \
  --rmrf-recovery \
  --mysql-version=57 \
  --output=/tmp/recovery
```

### Header Repair
```bash
./bin/mysql_recovery_pro \
  --ibd=data.ibd \
  --header-repair \
  --mysql-version=80 \
  --output=/tmp/recovery
```

## Test Results

### All Tests Pass
```
[  PASSED  ] 26 test suites.
[  PASSED  ] 350+ tests.
```

### MySQL 5.6/5.7 Compatibility
```
[==========] 10 tests from 1 test suite ran.
[  PASSED  ] 10 tests.
```

## Files

### Headers
- `include/dbrecovery/innodb/page_types_v2.hpp` - Page type definitions for all versions
- `include/dbrecovery/innodb/page_parser_v2.hpp` - Page parser interface
- `include/dbrecovery/common/mysql_version_detector.hpp` - Version detection

### Sources
- `src/innodb/page_parser_v2.cpp` - Page parser implementations
- `src/innodb/version_detection.cpp` - Version detection logic
- `src/innodb/page_parser_factory.cpp` - Factory for version-specific parsers

### Tests
- `tests/innodb/` - MySQL version compatibility tests
- `test_data/test_mysql56.ibd` - Sample MySQL 5.6 IBD file
- `test_data/test_mysql80.ibd` - Sample MySQL 8.0 IBD file

## Version Detection

The tool automatically detects MySQL version from:
1. **SDI Pages**: Present in MySQL 8.0+ only
2. **Page Header Fields**: LSN extension in MySQL 8.0+
3. **Page Type**: Special page types indicate version
4. **Row Format Flags**: Different for 5.6 vs 8.0

## Feeback

For issues or feature requests, please create an issue on GitHub.
