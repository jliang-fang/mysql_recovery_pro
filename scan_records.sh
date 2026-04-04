#!/bin/bash

# Look for patterns that might be records
# Records have format: 
# - 1 byte info_bits
# - 2 bytes next_record_offset
# - 2 bytes heap_no
# - 1 byte n_fields (for REDUNDANT)
# or for COMPACT:
# - 1 byte info_bits  
# - 2 bytes next_record
# - 2 bytes heap_no
# - variable length fields with length prefixes

echo "=== Looking for record-like structures in page 4 ==="
xxd -s 65536 -l 500 /tmp/test_users.ibd

echo ""
echo "=== Scanning entire file for 0x80 markers (potential FIXED NULL BIT) ==="
# Records often start with a byte that indicates null bitmap status
od -An -tx1 /tmp/test_users.ibd | grep -i "80" | head -30
