#!/usr/bin/env python3
"""
MySQL IBD Test Data Generator
=============================
生成 MySQL 5.6/5.7/8.0 的测试 IBD 文件，用于验证页解析器

支持的测试场景：
1. Compact 行格式测试
2. Redundant 行格式测试  
3. 坏块场景测试
4. Truncate 场景测试
5. SDI 页测试 (8.0+)
"""

import struct
import os
import random
import argparse
from typing import List, Tuple

# InnoDB 常量
FIL_PAGE_TYPE_INDEX = 9969      # 0x26F1
FIL_PAGE_TYPE_SDI = 9971        # 0x26F3
FIL_PAGE_TYPE_UNDO_LOG = 9970   # 0x26F2
FIL_PAGE_TYPE_ALLOCATED = 0

UNIV_PAGE_SIZE = 16384
FIL_HEADER_SIZE = 38

# Page Header Offsets (relative to page start)
PAGE_N_DIR_SLOTS = 38 + 0
PAGE_HEAP_TOP = 38 + 2
PAGE_N_HEAP = 38 + 4
PAGE_FREE = 38 + 6
PAGE_GARBAGE_END = 38 + 8
PAGE_LAST_INSERT = 38 + 10
PAGE_DIRECTION = 38 + 12
PAGE_N_DIRECT_PAGES = 38 + 14

# Record Constants
REC_INFO_BITS_MASK = 0x07
REC_INFO_DELETED_FLAG = 0x08
REC_INFO_MIN_REC_FLAG = 0x10
REC_INFO_EXTERN_FLAG = 0x20

REC_STATUS_ORDINARY = 0x00
REC_STATUS_NODE_PTR = 0x01
REC_STATUS_INFIMUM = 0x02
REC_STATUS_SUPREMUM = 0x03


def create_fil_header(
    prev_page: int = 0xFFFFFFFF,
    next_page: int = 0xFFFFFFFF,
    lsn: int = 1,
    page_type: int = FIL_PAGE_TYPE_INDEX,
    space_id: int = 1,
    page_offset: int = 0,
    file_flush_lsn: int = 0
) -> bytes:
    """创建 FIL_HEADER (38 bytes)"""
    header = bytearray(38)
    
    # FIL_PAGE_PREV (offset 0-3)
    struct.pack_into('<I', header, 0, prev_page)
    
    # FIL_PAGE_NEXT (offset 4-7)
    struct.pack_into('<I', header, 4, next_page)
    
    # FIL_PAGE_LSN (offset 8-15)
    struct.pack_into('<Q', header, 8, lsn)
    
    # FIL_PAGE_TYPE (offset 16-17)
    struct.pack_into('<H', header, 16, page_type)
    
    # FIL_PAGE_SPACE_ID (offset 18-21)
    struct.pack_into('<I', header, 18, space_id)
    
    # FIL_PAGE_SPACE_OFFSET (offset 22-23)
    struct.pack_into('<H', header, 22, page_offset)
    
    # FIL_PAGE_FILE_FLUSH_LSN (offset 24-31, 8 bytes in 8.0+)
    struct.pack_into('<Q', header, 24, file_flush_lsn)
    
    return bytes(header)


def create_page_header_56_57(
    n_dir_slots: int = 2,
    heap_top: int = 120,  # After INFIMUM/SUPREMUM + headers
    n_heap: int = 2,      # INFIMUM + SUPREMUM
    free_offset: int = 0,
    garbage_end: int = 0,
    last_insert: int = 0,
    direction: int = 0,
    n_direct_pages: int = 0,
    page_level: int = 0,
    max_trx_id: int = 0,
    header_version: int = 0
) -> bytes:
    """创建 MySQL 5.6/5.7 Page Header (16 bytes)"""
    header = bytearray(16)
    
    # PAGE_N_DIR_SLOTS
    struct.pack_into('<H', header, 0, n_dir_slots)
    
    # PAGE_HEAP_TOP
    struct.pack_into('<H', header, 2, heap_top)
    
    # PAGE_N_HEAP (15-bit, bit 15 is REC_INFO_NEW_CHUNK_FLAG)
    struct.pack_into('<H', header, 4, n_heap & 0x7FFF)
    
    # PAGE_FREE
    struct.pack_into('<H', header, 6, free_offset)
    
    # PAGE_GARBAGE_END
    struct.pack_into('<H', header, 8, garbage_end)
    
    # PAGE_LAST_INSERT
    struct.pack_into('<H', header, 10, last_insert)
    
    # PAGE_DIRECTION
    struct.pack_into('<H', header, 12, direction)
    
    # PAGE_N_DIRECT_PAGES
    struct.pack_into('<H', header, 14, n_direct_pages)
    
    return bytes(header)


def create_page_header_80(
    n_dir_slots: int = 2,
    heap_top: int = 120,
    n_heap: int = 2,
    free_offset: int = 0,
    garbage_end: int = 0,
    last_insert: int = 0,
    direction: int = 0,
    n_direct_pages: int = 0,
    page_level: int = 0,
    header_version: int = 1
) -> bytes:
    """创建 MySQL 8.0+ Page Header (24 bytes)"""
    header = bytearray(24)
    
    # PAGE_N_DIR_SLOTS
    struct.pack_into('<H', header, 0, n_dir_slots)
    
    # PAGE_HEAP_TOP
    struct.pack_into('<H', header, 2, heap_top)
    
    # PAGE_N_HEAP
    struct.pack_into('<H', header, 4, n_heap & 0x7FFF)
    
    # PAGE_FREE
    struct.pack_into('<H', header, 6, free_offset)
    
    # PAGE_GARBAGE_END
    struct.pack_into('<H', header, 8, garbage_end)
    
    # PAGE_LAST_INSERT
    struct.pack_into('<H', header, 10, last_insert)
    
    # PAGE_DIRECTION
    struct.pack_into('<H', header, 12, direction)
    
    # PAGE_N_DIRECT_PAGES
    struct.pack_into('<H', header, 14, n_direct_pages)
    
    # PAGE_HEADER_VERSION (8.0+)
    struct.pack_into('<H', header, 16, header_version)
    
    return bytes(header)


def create_infimum_record() -> bytes:
    """创建 INFIMUM 系统记录 (13 bytes)"""
    record = bytearray(13)
    
    # info_bits = REC_STATUS_INFIMUM (0x02)
    record[0] = REC_STATUS_INFIMUM
    
    # next_record_offset = 54 (SUPREMUM offset)
    struct.pack_into('<H', record, 1, 54)
    
    # heap_no = 0
    struct.pack_into('<H', record, 3, 0)
    
    # n_fields = 1
    record[5] = 1
    
    # extra_bytes = 6 (Redundant) or 5 (Compact)
    record[6] = 6
    
    # rest is zero
    return bytes(record)


def create_supremum_record() -> bytes:
    """创建 SUPREMUM 系统记录 (8 bytes)"""
    record = bytearray(8)
    
    # info_bits = REC_STATUS_SUPREMUM (0x03)
    record[0] = REC_STATUS_SUPREMUM
    
    # next_record_offset = 0xFFFF (end of chain)
    struct.pack_into('<H', record, 1, 0xFFFF)
    
    # heap_no = 1
    struct.pack_into('<H', record, 3, 1)
    
    return bytes(record)


def create_user_record_compact(
    values: List[bytes],
    is_deleted: bool = False,
    is_min_rec: bool = False
) -> bytes:
    """
    创建 COMPACT 行格式用户记录
    
    Record format:
    - 1 byte: info_bits
    - 2 bytes: next_record_offset
    - 2 bytes: heap_no
    - n bytes: column data
    """
    record = bytearray()
    
    # info_bits
    info_bits = REC_STATUS_ORDINARY
    if is_deleted:
        info_bits |= REC_INFO_DELETED_FLAG
    if is_min_rec:
        info_bits |= REC_INFO_MIN_REC_FLAG
    record.append(info_bits)
    
    # next_record_offset (will be set later)
    record.extend(b'\x00\x00')
    
    # heap_no
    record.extend(b'\x00\x00')
    
    # n_fields (high 5 bits in info_bits, low 4 bits in next byte)
    n_fields = len(values)
    record[0] |= (n_fields << 3) & 0xF8
    record.append((n_fields >> 2) & 0x0F)
    
    # extra_bytes = 5 (Compact)
    record.append(5)
    
    # null bitmap (1 bit per nullable column)
    null_bitmap = 0
    null_count = 0
    for val in values:
        if val is None:
            null_bitmap |= (1 << null_count)
        null_count += 1
    
    if null_count > 0:
        record.append(null_bitmap)
    
    # column data
    for val in values:
        if val is None:
            continue
        # Variable length: 1-2 bytes length + data
        if len(val) < 256:
            record.append(len(val))
        else:
            record.append(0xFF)
            record.append((len(val) >> 8) & 0xFF)
        record.extend(val)
    
    # Set next_record_offset
    next_offset = len(record)
    struct.pack_into('<H', record, 1, next_offset)
    
    return bytes(record)


def create_user_record_redundant(
    values: List[bytes],
    is_deleted: bool = False,
    is_min_rec: bool = False
) -> bytes:
    """
    创建 REDUNDANT 行格式用户记录
    
    Record format:
    - 1 byte: info_bits
    - 2 bytes: next_record_offset
    - 2 bytes: heap_no
    - 1 byte: n_fields
    - n bytes: column lengths
    - n bytes: column data
    """
    record = bytearray()
    
    # info_bits
    info_bits = REC_STATUS_ORDINARY
    if is_deleted:
        info_bits |= REC_INFO_DELETED_FLAG
    if is_min_rec:
        info_bits |= REC_INFO_MIN_REC_FLAG
    record.append(info_bits)
    
    # next_record_offset
    record.extend(b'\x00\x00')
    
    # heap_no
    record.extend(b'\x00\x00')
    
    # n_fields
    record.append(len(values))
    
    # extra_bytes = 6 (Redundant)
    record.append(6)
    
    # column lengths (2 bytes each for Redundant)
    for val in values:
        if val is None:
            record.extend(b'\xFF\xFF')  # NULL marker
        else:
            record.extend(struct.pack('<H', len(val)))
    
    # column data
    for val in values:
        if val is not None:
            record.extend(val)
    
    # Set next_record_offset
    next_offset = len(record)
    struct.pack_into('<H', record, 1, next_offset)
    
    return bytes(record)


def create_btr_header(page_level: int = 0) -> bytes:
    """创建 BTR_HEADER (12 bytes)"""
    header = bytearray(12)
    
    # n_levels (number of levels in tree) - 4 bytes
    struct.pack_into('<I', header, 0, page_level + 1)
    
    # index_id (8 bytes)
    struct.pack_into('<Q', header, 4, 0x123456789ABCDEF0)
    
    return bytes(header)


def create_page(
    page_type: int = FIL_PAGE_TYPE_INDEX,
    space_id: int = 1,
    page_offset: int = 0,
    records: List[bytes] = None,
    page_level: int = 0,
    mysql_version: int = 57,
    row_format: str = 'compact'
) -> bytes:
    """创建完整的 InnoDB 页"""
    page = bytearray(UNIV_PAGE_SIZE)
    
    # Handle None records
    if records is None:
        records = []
    
    # 1. Create FIL_HEADER
    fil_header = create_fil_header(
        prev_page=0xFFFFFFFF,
        next_page=0xFFFFFFFF,
        lsn=1,
        page_type=page_type,
        space_id=space_id,
        page_offset=page_offset
    )
    page[0:38] = fil_header
    
    # 2. Create BTR_HEADER (after FIL_HEADER)
    btr_header = create_btr_header(page_level)
    page[38:50] = btr_header  # Fixed: 12 bytes, not 8
    
    # 3. Create PAGE_HEADER
    if mysql_version >= 80:
        page_header = create_page_header_80(
            heap_top=54,  # INFIMUM at 54
            n_heap=2,
            page_level=page_level
        )
    else:
        page_header = create_page_header_56_57(
            heap_top=54,
            n_heap=2,
            page_level=page_level
        )
    page[38:54] = page_header
    
    # 4. Create system records (INFIMUM/SUPREMUM)
    infimum = create_infimum_record()
    supremum = create_supremum_record()
    
    page[54:54+len(infimum)] = infimum
    page[54+len(infimum):54+len(infimum)+len(supremum)] = supremum
    
    # 5. Add user records
    current_offset = 54 + len(infimum) + len(supremum)
    
    if records:
        for record in records:
            page[current_offset:current_offset+len(record)] = record
            current_offset += len(record)
    
    # Update heap_top
    heap_top = current_offset
    if mysql_version >= 80:
        struct.pack_into('<H', page, PAGE_HEAP_TOP, heap_top)
        struct.pack_into('<H', page, PAGE_N_HEAP, 2 + len(records))
    else:
        struct.pack_into('<H', page, PAGE_HEAP_TOP, heap_top)
        struct.pack_into('<H', page, PAGE_N_HEAP, 2 + len(records))
    
    # 6. Create page directory (2-byte slots pointing to records)
    # INFIMUM at offset 54, SUPREMUM at offset 54+13=67
    dir_slots = bytearray()
    dir_slots.extend(struct.pack('<H', 54))      # INFIMUM
    dir_slots.extend(struct.pack('<H', 54+13))   # SUPREMUM
    
    # Add user record slots
    if records:
        offset = 54 + len(infimum) + len(supremum)
        for record in records:
            dir_slots.extend(struct.pack('<H', offset))
            offset += len(record)
    
    # Directory slots go after the last record
    dir_offset = heap_top
    page[dir_offset:dir_offset+len(dir_slots)] = dir_slots
    
    # Update n_dir_slots
    n_dir_slots = 2 + len(records) if records else 2
    if mysql_version >= 80:
        struct.pack_into('<H', page, PAGE_N_DIR_SLOTS, n_dir_slots)
    else:
        struct.pack_into('<H', page, PAGE_N_DIR_SLOTS, n_dir_slots)
    
    # 7. Create FIL_PAGE_END_LSN (page tail)
    # LSN reversed (last 8 bytes of page)
    lsn = struct.unpack('<Q', fil_header[8:16])[0]
    page[-8:] = struct.pack('<Q', lsn)
    
    # 8. Create checksum (simplified - use zeros for now)
    # In production, use CRC32C
    struct.pack_into('<I', page, 4, 0)   # checksum_a
    struct.pack_into('<I', page, 8, 0)   # checksum_b
    struct.pack_into('<I', page, 24, 0)  # checksum_a (duplicate)
    struct.pack_into('<I', page, 28, 0)  # checksum_b (duplicate)
    
    return bytes(page)


def create_sdi_page(
    space_id: int = 1,
    page_offset: int = 1
) -> bytes:
    """创建 SDI 页 (MySQL 8.0+)"""
    page = bytearray(UNIV_PAGE_SIZE)
    
    # 1. FIL_HEADER
    fil_header = create_fil_header(
        prev_page=0xFFFFFFFF,
        next_page=0xFFFFFFFF,
        lsn=1,
        page_type=FIL_PAGE_TYPE_SDI,
        space_id=space_id,
        page_offset=page_offset
    )
    page[0:38] = fil_header
    
    # 2. Page Header (24 bytes for 8.0+)
    page_header = create_page_header_80(
        heap_top=128,
        n_heap=1
    )
    page[38:62] = page_header
    
    # 3. SDI Signature (8 bytes at offset 64)
    sdi_sig = b'\x08SDI0000'
    page[64:72] = sdi_sig
    
    # 4. SDI JSON data (simplified)
    sdi_data = b'{"type": "table", "table_name": "test_table"}'
    page[72:72+len(sdi_data)] = sdi_data
    
    # 5. Page tail
    page[-8:] = b'\x00' * 8
    
    return bytes(page)


def create_corrupted_page(
    corruption_type: str = 'header',
    page_type: int = FIL_PAGE_TYPE_INDEX,
    space_id: int = 1,
    page_offset: int = 0
) -> bytes:
    """创建损坏的页用于测试"""
    page = bytearray(create_page(
        page_type=page_type,
        space_id=space_id,
        page_offset=page_offset,
        records=[]
    ))
    
    if corruption_type == 'header':
        # Corrupt page header
        page[38:54] = b'\xFF' * 16
    elif corruption_type == 'checksum':
        # Corrupt checksum
        struct.pack_into('<I', page, 4, 0xDEADBEEF)
        struct.pack_into('<I', page, 24, 0xDEADBEEF)
    elif corruption_type == 'type':
        # Corrupt page type
        struct.pack_into('<H', page, 16, 0xFFFF)
    elif corruption_type == 'data':
        # Corrupt record data
        page[100:200] = b'\xFF' * 100
    
    return bytes(page)


def create_truncated_page(
    original_page: bytes,
    truncate_size: int = 8192
) -> bytes:
    """创建截断的页"""
    return original_page[:truncate_size]


def generate_test_files(output_dir: str = '.'):
    """生成所有测试文件"""
    os.makedirs(output_dir, exist_ok=True)
    
    # 1. Compact 行格式测试页
    print("[*] Generating Compact row format test page...")
    compact_records = [
        create_user_record_compact([b'John', b'Doe', b'30']),
        create_user_record_compact([b'Jane', b'Smith', b'25']),
        create_user_record_compact([b'Bob', b'Johnson', b'35']),
    ]
    compact_page = create_page(
        records=compact_records,
        row_format='compact'
    )
    with open(os.path.join(output_dir, 'test_compact.ibd'), 'wb') as f:
        f.write(compact_page)
    print(f"    [+] Created test_compact.ibd ({len(compact_page)} bytes)")
    
    # 2. Redundant 行格式测试页
    print("[*] Generating Redundant row format test page...")
    redundant_records = [
        create_user_record_redundant([b'John', b'Doe', b'30']),
        create_user_record_redundant([b'Jane', b'Smith', b'25']),
        create_user_record_redundant([b'Bob', b'Johnson', b'35']),
    ]
    redundant_page = create_page(
        records=redundant_records,
        row_format='redundant'
    )
    with open(os.path.join(output_dir, 'test_redundant.ibd'), 'wb') as f:
        f.write(redundant_page)
    print(f"    [+] Created test_redundant.ibd ({len(redundant_page)} bytes)")
    
    # 3. 坏块测试页
    print("[*] Generating corrupted page test file...")
    corrupted_page = create_corrupted_page('header')
    with open(os.path.join(output_dir, 'test_header_corrupt.ibd'), 'wb') as f:
        f.write(corrupted_page)
    print(f"    [+] Created test_header_corrupt.ibd ({len(corrupted_page)} bytes)")
    
    # 4. SDI 页测试 (8.0+)
    print("[*] Generating SDI page test file...")
    sdi_page = create_sdi_page()
    with open(os.path.join(output_dir, 'test_sdi.ibd'), 'wb') as f:
        f.write(sdi_page)
    print(f"    [+] Created test_sdi.ibd ({len(sdi_page)} bytes)")
    
    # 5. 截断测试页
    print("[*] Generating truncated page test file...")
    original_page = create_page()
    truncated_page = create_truncated_page(original_page, 8192)
    with open(os.path.join(output_dir, 'test_truncated.ibd'), 'wb') as f:
        f.write(truncated_page)
    print(f"    [+] Created test_truncated.ibd ({len(truncated_page)} bytes)")
    
    # 6. 多版本混合测试
    print("[*] Generating multi-version test files...")
    
    # MySQL 5.6 页
    page_56 = create_page(mysql_version=56, row_format='compact')
    with open(os.path.join(output_dir, 'test_mysql56.ibd'), 'wb') as f:
        f.write(page_56)
    print(f"    [+] Created test_mysql56.ibd")
    
    # MySQL 8.0 页
    page_80 = create_page(mysql_version=80, row_format='dynamic')
    with open(os.path.join(output_dir, 'test_mysql80.ibd'), 'wb') as f:
        f.write(page_80)
    print(f"    [+] Created test_mysql80.ibd")
    
    # 7. 完整的 IBD 文件模拟
    print("[*] Generating complete IBD file simulation...")
    
    # Page 0: FSP_HDR
    fsp_hdr = create_page(
        page_type=0x45BF,
        page_offset=0,
        mysql_version=80
    )
    
    # Page 1: IBUF_BITMAP
    ibuf_bitmap = create_page(
        page_type=0x45BD,
        page_offset=1,
        mysql_version=80
    )
    
    # Page 2: INODE
    inode = create_page(
        page_type=0x45BB,
        page_offset=2,
        mysql_version=80
    )
    
    # Page 3: INDEX (root page)
    index_records = [
        create_user_record_compact([b'Key1', b'Value1']),
        create_user_record_compact([b'Key2', b'Value2']),
    ]
    root_page = create_page(
        page_type=FIL_PAGE_TYPE_INDEX,
        page_offset=3,
        page_level=0,
        records=index_records,
        mysql_version=80,
        row_format='compact'
    )
    
    # Write as a single file
    with open(os.path.join(output_dir, 'test_complete.ibd'), 'wb') as f:
        f.write(fsp_hdr)
        f.write(ibuf_bitmap)
        f.write(inode)
        f.write(root_page)
        # Pad to 64KB
        f.write(b'\x00' * (65536 - len(fsp_hdr) - len(ibuf_bitmap) - len(inode) - len(root_page)))
    print(f"    [+] Created test_complete.ibd")
    
    print("\n[+] All test files generated successfully!")
    print(f"    Output directory: {output_dir}")


def main():
    parser = argparse.ArgumentParser(
        description='MySQL IBD Test Data Generator'
    )
    parser.add_argument(
        '-o', '--output',
        default='.',
        help='Output directory for test files'
    )
    parser.add_argument(
        '--compact',
        action='store_true',
        help='Generate only Compact format test file'
    )
    parser.add_argument(
        '--redundant',
        action='store_true',
        help='Generate only Redundant format test file'
    )
    parser.add_argument(
        '--corrupted',
        action='store_true',
        help='Generate only corrupted page test file'
    )
    parser.add_argument(
        '--sdi',
        action='store_true',
        help='Generate only SDI page test file'
    )
    parser.add_argument(
        '--truncated',
        action='store_true',
        help='Generate only truncated page test file'
    )
    
    args = parser.parse_args()
    
    if args.compact:
        records = [
            create_user_record_compact([b'John', b'Doe', b'30']),
            create_user_record_compact([b'Jane', b'Smith', b'25']),
        ]
        page = create_page(records=records, row_format='compact')
        with open(os.path.join(args.output, 'test_compact.ibd'), 'wb') as f:
            f.write(page)
        print(f"[+] Created test_compact.ibd ({len(page)} bytes)")
    elif args.redundant:
        records = [
            create_user_record_redundant([b'John', b'Doe', b'30']),
            create_user_record_redundant([b'Jane', b'Smith', b'25']),
        ]
        page = create_page(records=records, row_format='redundant')
        with open(os.path.join(args.output, 'test_redundant.ibd'), 'wb') as f:
            f.write(page)
        print(f"[+] Created test_redundant.ibd ({len(page)} bytes)")
    elif args.corrupted:
        page = create_corrupted_page('header')
        with open(os.path.join(args.output, 'test_corrupted.ibd'), 'wb') as f:
            f.write(page)
        print(f"[+] Created test_corrupted.ibd ({len(page)} bytes)")
    elif args.sdi:
        page = create_sdi_page()
        with open(os.path.join(args.output, 'test_sdi.ibd'), 'wb') as f:
            f.write(page)
        print(f"[+] Created test_sdi.ibd ({len(page)} bytes)")
    elif args.truncated:
        original = create_page()
        truncated = create_truncated_page(original, 8192)
        with open(os.path.join(args.output, 'test_truncated.ibd'), 'wb') as f:
            f.write(truncated)
        print(f"[+] Created test_truncated.ibd ({len(truncated)} bytes)")
    else:
        generate_test_files(args.output)


if __name__ == '__main__':
    main()
