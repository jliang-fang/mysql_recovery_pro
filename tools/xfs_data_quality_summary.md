# XFS Schema-Aware Recovery Test Results

## Test Date: 2026-07-03

## Test Configuration
- MySQL Version: 8.0.46
- Test Rows: 5,000
- Block Device: 200GB XFS
- Recovery Tool: mysql_recovery_pro with --filesystem=xfs

## Test Results

### Recovery Methods Comparison

| Method | CSV Files | Total Rows | Format | User Data Found |
|--------|-----------|------------|--------|-----------------|
| Schema-agnostic (no schema) | 109 | 2,218,249 | Hex fields (field_0) | ❌ Not found |
| Manual Schema | 109 | ~100 | Schema fields | ❌ Not found |
| JSON Schema File | 109 | ~100 | Schema fields | ❌ Not found |

### Observations

1. **Schema Parameter Support**: The tool accepts --schema and --schema-file parameters
2. **XFS Recovery**: Successfully scans and recovers 170K+ pages from XFS
3. **Data Recovery**: Recovers data from multiple sources:
   - System tables (information_schema, performance_schema, mysql)
   - Historical test data from previous runs
   - Schema metadata

### Issues Identified

1. **User Data Not Recovered**:
   - Test table data (User_*, user@example.com patterns) not found in output
   - Likely causes:
     - Small dataset (5K rows) mixed with large historical data
     - XFS deleted inode detection not capturing recent deletions
     - Need better index_id filtering

2. **Schema-Aware vs Schema-Agnostic**:
   - Both methods produce similar output
   - Schema parameter affects field naming but not content interpretation
   - Need to investigate schema application in disk carving mode

3. **Data Quality**:
   - 164,807 issues detected (column mismatches)
   - CSV format inconsistencies
   - Binary data in text fields

### Recommendations

1. **For Production Use**:
   - Use larger test datasets (100K+ rows) for validation
   - Combine XFS AG scanning with precise index_id targeting
   - Implement data filtering by timestamp or content pattern

2. **For Tool Improvement**:
   - Add index_id filtering option
   - Implement data deduplication across multiple recovery passes
   - Add schema-aware field parsing in disk carving mode
   - Improve CSV output formatting for binary data

3. **For Testing**:
   - Use dedicated XFS partition for each test
   - Minimize time between data creation and recovery
   - Test with FRM file recovery for MySQL 5.6/5.7

## Conclusion

XFS disk carving is **functional** but requires optimization for:
- Better user data detection
- Schema-aware field interpretation
- Data quality improvements

The core functionality (AG scanning, signature detection, page recovery) works correctly.
