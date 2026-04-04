#!/bin/bash

# Detailed analysis of record structure in page 4
echo "=== Analyzing page 4 record structure ==="

# Find all records by looking for pattern: next_offset after heap_no
# Records are linked via next_record_offset field
# Parse starting from supremum and following next offsets backwards

# Read page 4 and parse record chain
echo "=== Reading page 4 ==="
xxd /tmp/test_users.ibd | head -1

echo ""
echo "=== Page header at offset 36-60 ==="
xxd -s 65536 -l 60 /tmp/test_users.ibd

echo ""
echo "=== Records chain analysis ==="
# Records start around 0x46 (offset within page)
# infimum at 0x60, supremum at 0x72

echo "=== Supremum record at 0x72 ==="
xxd -s $((65536 + 0x72)) -l 30 /tmp/test_users.ibd

echo ""
echo "=== Looking for next record from supremum ==="
# From 0x72, the next offset should point to previous record in chain

# Check the record structure in the chain
# Record format (COMPACT):
# - 1 byte: info_bits
# - 2 bytes: next_record_offset (points to next record in heap order)
# - 2 bytes: heap_no

# Looking at pattern 000e 8000:
# 00 = info_bits
# 0e 00 = next_offset (14 bytes, LE: 0x000e = 14)
# 80 00 = heap_no (128 in LE)
