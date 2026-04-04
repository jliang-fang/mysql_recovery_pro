#!/usr/bin/env python3
"""
Simplified Test Data Generator for MySQL Recovery Pro
Generates test .ibd files for various MySQL versions and corruption scenarios
"""

import os
import sys
import struct
import random
import argparse
from typing import List, Dict, Tuple, Optional

# MySQL InnoDB constants
FIL_PAGE_TYPE_INDEX = 0x26F1  # 9969
FIL_PAGE_TYPE_SDI = 0x26F3    # 9971
FIL_PAGE_TYPE_UNDO_LOG = 0x26F2  # 9970
FIL_PAGE_TYPE_BLOB = 0x26F4   # 9972
FIL_PAGE_TYPE_ALLOCATED = 0

# Record status bits (lowest 3 bits of info_bits)
REC_STATUS_ORDINARY = 0x00   # ordinary record
REC_STATUS_NODE_PTR = 0x01   # node pointer record
REC_STATUS_INFIMUM = 0x02    # infimum system record
REC_STATUS_SUPREMUM = 0x03   # supremum system record

REC_INFO_MIN_REC_FLAG = 0x10
REC_INFO_DELETED_FLAG = 0x20


def write_le_uint16(data: bytearray, offset: int, value: int) -> None:
    """Write little-endian uint16"""
    data[offset] = value & 0xFF
    data[offset + 1] = (value >> 8) & 0xFF


def write_le_uint32(data: bytearray, offset: int, value: int) -> None:
    """Write little-endian uint32"""
    data[offset] = value & 0xFF
    data[offset + 1] = (value >> 8) & 0xFF
    data[offset + 2] = (value >> 16) & 0xFF
    data[offset + 3] = (value >> 24) & 0xFF


def write_le_uint64(data: bytearray, offset: int, value: int) -> None:
    """Write little-endian uint64"""
    for i in range(8):
        data[offset + i] = (value >> (i * 8)) & 0xFF


def create_btree_page_simple(page_no: int, num_records: int = 10,
                             page_size: int = 16384, level: int = 0,
                             prev_page: int = 0xFFFFFFFF, next_page: int = 0xFFFFFFFF) -> bytearray:
    """Create a simple B+Tree index page"""
    page = bytearray(page_size)
    
    # FIL Header (38 bytes)
    write_le_uint32(page, 0, prev_page)
    write_le_uint32(page, 4, next_page)
    write_le_uint64(page, 8, 0)           # LSN
    write_le_uint32(page, 16, 0)          # space_id
    write_le_uint32(page, 20, page_no)
    write_le_uint32(page, 24, 0)          # checksum
    write_le_uint32(page, 28, 0)          # checksum
    write_le_uint16(page, 36, 0)          # arch_log_no
    
    # Page Type (offset 16 in FIL header - NOT 50!)
    write_le_uint16(page, 16, FIL_PAGE_TYPE_INDEX)
    
    # Page Header (offset 38, size 16 bytes for basic header)
    # Page header starts at offset 38 (after FIL header)
    # Page header size is 16 bytes in basic format
    write_le_uint16(page, 38, 2)          # n_dir_slots (offset 38+0=38)
    write_le_uint16(page, 40, 54 + 20)    # heap_top (offset 38+2=40)
    write_le_uint16(page, 42, 2 | 0x8000) # n_heap (offset 38+4=42)
    write_le_uint16(page, 44, 54)         # free_offset (offset 38+6=44)
    # The rest of page header (offsets 46-53) should be zero for basic format
    
    # Page Level is stored in page header at offset 38+4=42, but we use n_heap there
    # For simplicity, we'll just set it to 0 in the n_heap field (level is stored differently)
    # Actually, page level is at offset 42 which is where n_heap should be
    # Let's fix this: page level goes at offset 42, n_heap at offset 42 too - they overlap!
    # In InnoDB, page level is at offset 42 (page header + 4)
    # n_heap is at offset 42 (page header + 4) - they share the same offset!
    # This is correct - n_heap includes the level in the high bits
    
    # INFIMUM record (offset 54)
    # info_bits should be REC_STATUS_INFIMUM (0x02) only - status bits
    # REC_INFO_MIN_REC_FLAG (0x10) is a separate flag bit, not part of status
    page[54] = REC_STATUS_INFIMUM
    write_le_uint16(page, 55, 54 + 13)    # next_record_offset = 61
    write_le_uint16(page, 57, 0)          # heap_no = 0
    write_le_uint16(page, 59, 13)         # record_length = 13

    # SUPREMUM record (offset 61)
    page[54 + 13] = REC_STATUS_SUPREMUM  # offset 61
    write_le_uint16(page, 54 + 14, 0x3FFF)  # next_record_offset (15-bit)
    write_le_uint16(page, 54 + 16, 1)       # heap_no (at 64-65, not 63-64!)
    write_le_uint16(page, 54 + 18, 13)      # record_length (at 66-67, not 65-66!)
    
    # User records
    current_offset = 54 + 13 + 13  # After INFIMUM and SUPREMUM
    record_count = 0
    
    for i in range(num_records):
        if current_offset + 32 >= page_size - 100:
            break
        
        # Simple record: 6 byte header + 26 byte data
        page[current_offset] = REC_STATUS_ORDINARY
        write_le_uint16(page, current_offset + 1, 0x3FFF)  # next_record_offset
        write_le_uint16(page, current_offset + 3, record_count + 2)  # heap_no
        write_le_uint16(page, current_offset + 5, 26)      # record_length
        
        # Data: id (8 bytes) + name (8 bytes) + email (8 bytes) + datetime (2 bytes)
        write_le_uint64(page, current_offset + 7, i + 1)   # id
        name_bytes = f'user_{i}'.encode('utf-8')[:8]
        page[current_offset + 15:current_offset + 15 + len(name_bytes)] = name_bytes
        email_bytes = f'user{i}@ex.com'.encode('utf-8')[:8]
        page[current_offset + 23:current_offset + 23 + len(email_bytes)] = email_bytes
        
        # Update next_record_offset
        next_offset = current_offset + 32
        write_le_uint16(page, current_offset + 1, next_offset)
        
        current_offset = next_offset
        record_count += 1
    
    # Update heap_top
    write_le_uint16(page, 40, current_offset)
    
    # Update n_heap
    write_le_uint16(page, 42, (record_count + 2) | 0x8000)
    
    return page


def create_sdi_page(page_no: int, table_id: int = 12345,
                    table_name: str = "test_table",
                    page_size: int = 16384) -> bytearray:
    """Create an SDI page (MySQL 8.0+)"""
    page = create_btree_page_simple(page_no, num_records=1, page_size=page_size)
    
    # SDI payload starts at offset 38
    write_le_uint64(page, 46, table_id)   # table_id
    
    return page


def create_undo_log_page(page_no: int, trx_id: int = 12345,
                         page_size: int = 16384) -> bytearray:
    """Create an undo log page"""
    page = bytearray(page_size)
    
    # FIL Header
    write_le_uint32(page, 0, 0xFFFFFFFF)
    write_le_uint32(page, 4, 0xFFFFFFFF)
    write_le_uint64(page, 8, 0)
    write_le_uint32(page, 16, 0)
    write_le_uint32(page, 20, page_no)
    write_le_uint32(page, 24, 0)
    write_le_uint32(page, 28, 0)
    write_le_uint16(page, 36, 0)
    
    # Page Type
    write_le_uint16(page, 50, FIL_PAGE_TYPE_UNDO_LOG)
    
    # Page Header
    write_le_uint16(page, 38, 2)
    write_le_uint16(page, 40, 54 + 100)
    write_le_uint16(page, 42, 2 | 0x8000)
    write_le_uint16(page, 44, 54)
    
    # Undo Log Header
    write_le_uint64(page, 38 + 0, trx_id)   # trx_id
    write_le_uint64(page, 38 + 8, 0)        # roll_ptr
    write_le_uint32(page, 38 + 20, 100)     # free_space
    
    return page


def create_blob_page(page_no: int, total_len: int = 100,
                     page_size: int = 16384) -> bytearray:
    """Create a BLOB page"""
    page = bytearray(page_size)
    
    # FIL Header
    write_le_uint32(page, 0, 0xFFFFFFFF)
    write_le_uint32(page, 4, 0xFFFFFFFF)
    write_le_uint64(page, 8, 0)
    write_le_uint32(page, 16, 0)
    write_le_uint32(page, 20, page_no)
    write_le_uint32(page, 24, 0)
    write_le_uint32(page, 28, 0)
    write_le_uint16(page, 36, 0)
    
    # Page Type
    write_le_uint16(page, 50, FIL_PAGE_TYPE_BLOB)
    
    # BLOB Header
    write_le_uint32(page, 38, 0xFFFFFFFF)   # next_page
    write_le_uint32(page, 42, total_len)    # total_length
    write_le_uint32(page, 46, total_len)    # current_length
    
    # BLOB Data
    blob_data = bytes([0x42] * min(total_len, page_size - 38 - 12))
    page[50:50 + len(blob_data)] = blob_data
    
    return page


def generate_test_table(page_size: int = 16384, num_records: int = 100,
                        mysql_version: int = 80) -> bytearray:
    """Generate a complete test table with multiple pages"""
    # Calculate pages needed
    records_per_page = 10
    total_pages = max(3, (num_records + records_per_page - 1) // records_per_page + 2)
    
    # Create pages
    pages = []
    
    # Page 0: Root B+Tree page
    root_page = create_btree_page_simple(
        page_no=0, 
        num_records=records_per_page,
        page_size=page_size,
        level=0
    )
    pages.append(root_page)
    
    # Create leaf pages with records
    for i in range(1, total_pages - 1):
        page = create_btree_page_simple(
            page_no=i,
            num_records=records_per_page,
            page_size=page_size,
            prev_page=i - 1,
            next_page=i + 1,
            level=0
        )
        pages.append(page)
    
    # Last page
    if len(pages) > 1:
        last_page = create_btree_page_simple(
            page_no=len(pages) - 1,
            num_records=num_records - (len(pages) - 2) * records_per_page,
            page_size=page_size,
            prev_page=len(pages) - 2,
            next_page=0xFFFFFFFF,
            level=0
        )
        pages.append(last_page)
    
    # SDI page (MySQL 8.0+)
    if mysql_version >= 80:
        sdi_page = create_sdi_page(
            page_no=len(pages),
            table_id=12345,
            page_size=page_size
        )
        pages.append(sdi_page)
    
    # Combine pages
    result = bytearray()
    for page in pages:
        result.extend(page)
    
    return result


def generate_corrupted_page(original_page: bytearray, corruption_type: str) -> bytearray:
    """Generate a corrupted version of a page"""
    corrupted = bytearray(original_page)
    
    if corruption_type == "header_type":
        # Corrupt page type
        write_le_uint16(corrupted, 50, 0xFFFF)
    elif corruption_type == "header_checksum":
        # Corrupt checksum
        write_le_uint32(corrupted, 24, 0xDEADBEEF)
        write_le_uint32(corrupted, 28, 0xDEADBEEF)
    elif corruption_type == "infimum_corrupt":
        # Corrupt INFIMUM record
        corrupted[54] = 0xFF
    elif corruption_type == "supremum_corrupt":
        # Corrupt SUPREMUM record
        corrupted[54 + 13] = 0xFF
    elif corruption_type == "chain_broken":
        # Break page chain
        write_le_uint32(corrupted, 4, 0xFFFFFFF0)
    elif corruption_type == "record_chain_broken":
        # Break record chain
        write_le_uint16(corrupted, 55, 0xFFFF)
    elif corruption_type == "all_zero":
        # Zero out the page
        corrupted = bytearray(len(corrupted))
    elif corruption_type == "partial":
        # Truncate to partial size
        corrupted = corrupted[:len(corrupted) // 2]
    elif corruption_type == "random":
        # Random corruption
        for _ in range(10):
            pos = random.randint(0, len(corrupted) - 1)
            corrupted[pos] = random.randint(0, 255)
    
    return corrupted


def main():
    parser = argparse.ArgumentParser(description='Generate test .ibd files')
    parser.add_argument('--output-dir', default='test_data',
                        help='Output directory')
    parser.add_argument('--mysql-version', type=int, default=80,
                        choices=[55, 56, 57, 80, 84, 90],
                        help='MySQL version')
    parser.add_argument('--page-size', type=int, default=16384,
                        choices=[4096, 8192, 16384, 32768, 65536],
                        help='Page size')
    parser.add_argument('--num-records', type=int, default=100,
                        help='Number of records')
    parser.add_argument('--corruption', type=str, default=None,
                        choices=['header_type', 'header_checksum', 'infimum_corrupt',
                                 'supremum_corrupt', 'chain_broken', 'record_chain_broken',
                                 'all_zero', 'partial', 'random'],
                        help='Corruption type')
    
    args = parser.parse_args()
    
    os.makedirs(args.output_dir, exist_ok=True)
    
    # Generate test table
    if args.corruption:
        # Generate normal page first
        normal_page = generate_test_table(
            page_size=args.page_size,
            mysql_version=args.mysql_version,
            num_records=args.num_records
        )
        
        # Corrupt it
        corrupted_page = generate_corrupted_page(normal_page, args.corruption)
        
        output_path = os.path.join(
            args.output_dir,
            f'test_corrupted_{args.corruption}_v{args.mysql_version}_ps{args.page_size}.ibd'
        )
        
        with open(output_path, 'wb') as f:
            f.write(corrupted_page)
        
        print(f"Generated corrupted page: {output_path}")
    else:
        # Generate normal test table
        table_data = generate_test_table(
            page_size=args.page_size,
            mysql_version=args.mysql_version,
            num_records=args.num_records
        )
        
        output_path = os.path.join(
            args.output_dir,
            f'test_table_v{args.mysql_version}_ps{args.page_size}_rec{args.num_records}.ibd'
        )
        
        with open(output_path, 'wb') as f:
            f.write(table_data)
        
        print(f"Generated test table: {output_path}")
        print(f"  - MySQL version: {args.mysql_version}")
        print(f"  - Page size: {args.page_size}")
        print(f"  - Records: {args.num_records}")
        print(f"  - File size: {len(table_data)} bytes")


if __name__ == '__main__':
    main()
