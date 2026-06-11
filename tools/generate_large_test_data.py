#!/usr/bin/env python3
"""
Large Scale Test Data Generator for MySQL Recovery Pro
======================================================
生成可用于真实 MySQL 恢复测试的 IBD 文件

使用方法：
1. 先创建测试数据库和表
2. 使用此脚本生成高质量的测试数据
3. 进行各种损坏场景测试
"""

import struct
import os
import random
import argparse
from typing import List, Tuple
from tqdm import tqdm


# InnoDB Constants
FIL_PAGE_TYPE_INDEX = 0x26F1
FIL_PAGE_TYPE_SDI = 0x26F3
FIL_PAGE_TYPEALLOCATED = 0

UNIV_PAGE_SIZE = 16384
FIL_PAGE_OFFSET = 4
FIL_PAGE_LSN = 8
FIL_PAGE_TYPE = 16
FIL_PAGE_SPACE_ID = 18
FIL_PAGE_SPACE_OFFSET = 22


def create_fil_header(
    prev_page: int = 0xFFFFFFFF,
    next_page: int = 0xFFFFFFFF,
    lsn: int = 0,
    page_type: int = FIL_PAGE_TYPE_INDEX,
    space_id: int = 1,
    page_offset: int = 0,
    checksum_a: int = 0,
    checksum_b: int = 0
) -> bytes:
    """Create FIL_HEADER (38 bytes)"""
    header = bytearray(38)

    struct.pack_into('<I', header, 0, prev_page)
    struct.pack_into('<I', header, 4, next_page)
    struct.pack_into('<Q', header, 8, lsn)
    struct.pack_into('<H', header, 16, page_type)
    struct.pack_into('<I', header, 18, space_id)
    struct.pack_into('<H', header, 22, page_offset)
    struct.pack_into('<Q', header, 24, 0)  # file_flush_lsn

    return bytes(header)


def create_page_header_compact(
    heap_top: int = 128,
    n_heap: int = 2,
    free_offset: int = 0,
    page_level: int = 0,
    n_dir_slots: int = 2,
    page_offset: int = 0
) -> bytes:
    """Create Page Header for Compact/Dynamic row format (24 bytes)"""
    header = bytearray(24)

    struct.pack_into('<H', header, 0, n_dir_slots)       # PAGE_N_DIR_SLOTS
    struct.pack_into('<H', header, 2, heap_top)          # PAGE_HEAP_TOP
    struct.pack_into('<H', header, 4, n_heap)            # PAGE_N_HEAP
    struct.pack_into('<H', header, 6, free_offset)       # PAGE_FREE
    struct.pack_into('<H', header, 8, 0)                 # PAGE_GARBAGE_END
    struct.pack_into('<H', header, 10, 0)                # PAGE_LAST_INSERT
    struct.pack_into('<H', header, 12, 0)                # PAGE_DIRECTION
    struct.pack_into('<H', header, 14, 0)                # PAGE_N_DIRECT_PAGES
    struct.pack_into('<H', header, 16, page_level)       # PAGE_LEVEL
    struct.pack_into('<I', header, 18, 0)                # PAGE_MAX_TRX_ID (4 bytes)

    return bytes(header)


def encode_varchar(data: bytes) -> bytes:
    """Encode VARCHAR with 1-byte length prefix"""
    if len(data) >= 127:
        # Use 2-byte prefix for larger strings
        return bytes([0x80 | (len(data) >> 8), len(data) & 0xFF]) + data
    return bytes([len(data)]) + data


def create_compact_record_header(
    deleted: bool = False,
    n_owned: int = 0,
    heap_no: int = 2,
    n_fields: int = 4,
    sql_null_bytes: bytes = b'\x00',
    length_bytes: bytes = b'\x00\x00\x00',
    next_rec_off: int = 0
) -> bytes:
    """Create Compact record header"""
    header = bytearray(5)

    # 1 byte: info bits
    info = 0
    if deleted:
        info |= 0x08  # REC_INFO_DELETED_FLAG
    info |= (n_fields - 1) << 3
    header[0] = info

    # 2 bytes: heap_no
    struct.pack_into('<H', header, 1, heap_no)

    return bytes(header)


def create_compact_record(
    record_id: int,
    name: str,
    email: str,
    created_at: str,
    heap_no: int = 2,
    next_rec_off: int = 0
) -> bytes:
    """Create a Compact format record"""
    record = bytearray()

    # 1. Record header (5 bytes)
    # Byte 0: info bits (deleted=0, n_owned=0, heap_no_past=0, n_fields)
    # n_fields = 4 bits at position 3-6
    header_byte0 = (3 << 3) | 0x00  # n_fields=4, no other flags
    record.append(header_byte0)

    # Bytes 1-2: heap_no (little-endian) - extend buffer first
    record.extend(b'\x00\x00')  # Make room for heap_no
    struct.pack_into('<H', record, 1, heap_no)

    # 2. Row header (for DYNAMIC format)
    # NULL bitmap (1 byte for 4 columns)
    row_header = bytearray()
    row_header.append(0x00)  # No NULL values

    # Length bytes for VARCHAR fields (3 bytes each for DYNAMIC format)
    name_bytes = name.encode()
    email_bytes = email.encode()
    row_header.append(len(name_bytes))
    row_header.append((len(name_bytes) >> 8) if len(name_bytes) >= 127 else 0)
    row_header.append(len(email_bytes))
    row_header.append((len(email_bytes) >> 8) if len(email_bytes) >= 127 else 0)

    record.extend(bytes(row_header))

    # 3. Data fields
    # id (BIGINT, 8 bytes)
    record.extend(struct.pack('<Q', record_id))

    # name (VARCHAR with 2-byte length)
    if len(name) >= 127:
        record.append(0x80 | (len(name) >> 8))
        record.append(len(name) & 0xFF)
    else:
        record.append(len(name))
    record.extend(name_bytes)

    # email (VARCHAR with 2-byte length)
    if len(email) >= 127:
        record.append(0x80 | (len(email) >> 8))
        record.append(len(email) & 0xFF)
    else:
        record.append(len(email))
    record.extend(email_bytes)

    # created_at (DATETIME, stored as 5-byte packed value)
    # Simple packed datetime
    record.extend(b'\x00\x00\x00\x00\x00')

    return bytes(record)


def create_infimum_record() -> bytes:
    """Create INFIMUM record (special system record)"""
    record = bytearray()

    # Record header
    header = bytearray(5)
    header[0] = 0x02 | (3 << 3)  # REC_STATUS_INFIMUM | n_fields=4
    struct.pack_into('<H', header, 1, 0)  # heap_no = 0
    record.extend(bytes(header))

    # Row header
    record.extend(b'\x00')  # NULL bitmap
    record.extend(b'\x00\x00\x00')  # length bytes

    # Data: "infimum\0"
    record.extend(b'\x07infimum')
    record.extend(b'\x07infimum')
    record.extend(b'\x00\x00\x00\x00\x00')

    return bytes(record)


def create_supremum_record() -> bytes:
    """Create SUPREMUM record (special system record)"""
    record = bytearray()

    # Record header
    header = bytearray(5)
    header[0] = 0x03 | (3 << 3)  # REC_STATUS_SUPREMUM | n_fields=4
    struct.pack_into('<H', header, 1, 1)  # heap_no = 1
    record.extend(bytes(header))

    # Row header
    record.extend(b'\x00')  # NULL bitmap
    record.extend(b'\x00\x00\x00')  # length bytes

    # Data: "supremum\0"
    record.extend(b'\x08supremum')
    record.extend(b'\x08supremum')
    record.extend(b'\x00\x00\x00\x00\x00')

    return bytes(record)


def create_index_page(
    records: List[bytes],
    page_offset: int = 0,
    space_id: int = 1,
    lsn: int = 0,
    prev_page: int = 0xFFFFFFFF,
    next_page: int = 0xFFFFFFFF,
    page_level: int = 0
) -> bytes:
    """Create an index page with records"""
    page = bytearray(UNIV_PAGE_SIZE)

    # 1. FIL_HEADER (38 bytes)
    # FIL_PAGE_PREV (offset 0-3)
    struct.pack_into('<I', page, 0, prev_page)
    # FIL_PAGE_NEXT (offset 4-7)
    struct.pack_into('<I', page, 4, next_page)
    # FIL_PAGE_LSN (offset 8-15)
    struct.pack_into('<Q', page, 8, lsn)
    # FIL_PAGE_TYPE (offset 16-17)
    struct.pack_into('<H', page, 16, FIL_PAGE_TYPE_INDEX)
    # FIL_PAGE_SPACE_ID (offset 18-21)
    struct.pack_into('<I', page, 18, space_id)
    # FIL_PAGE_SPACE_OFFSET (offset 22-23) - THIS IS THE ERROR!
    # The page_offset value in page header is at offset 22-23 in FIL_HEADER
    # But we're writing the page number here which is 0 for FSP_HDR
    struct.pack_into('<H', page, 22, 0)
    # FIL_PAGE_FILE_FLUSH_LSN (offset 24-31)
    struct.pack_into('<Q', page, 24, 0)

    # 2. Page Header (24 bytes) at offset 38
    # The page header structure has:
    # - At offset 4 (BTR header): PAGE_N_HEAP, which also serves as BTR_N_LEVELS
    #   For leaf pages (page_level=0), PAGE_N_HEAP should be set correctly
    # - The parser reads BTR_N_LEVELS_ABS=42 which is the same as PAGE_N_HEAP offset
    # - In standard InnoDB, PAGE_N_HEAP at offset 4 contains both information:
    #   * Lower 10 bits: number of records in heap
    #   * Upper 6 bits: page level (for BTR_N_LEVELS)
    # But our parser reads it as a plain uint16_t for page level
    # So we need to store page_level directly at offset 42

    # PAGE_N_DIR_SLOTS (offset 38)
    struct.pack_into('<H', page, 38, 2)
    # PAGE_HEAP_TOP (offset 40)
    heap_top = 96
    struct.pack_into('<H', page, 40, heap_top)
    # PAGE_N_HEAP / BTR_N_LEVELS (offset 42)
    # For leaf pages with page_level=0, this should be the number of records
    # The parser reads this as page_level, so we need to combine them
    # PAGE_N_HEAP (lower 10 bits) + PAGE_LEVEL * 1024 (upper 6 bits)
    # For page_level=0: PAGE_N_HEAP = 2 + len(records), page_level = 0
    page_n_heap = 2 + len(records)
    # For leaf pages, page_level is 0, so we just write page_n_heap
    # But if page_level > 0, we need to encode it
    if page_level == 0:
        # Leaf page: page_level is 0, so just store PAGE_N_HEAP
        struct.pack_into('<H', page, 42, page_n_heap)
    else:
        # Non-leaf page: encode both
        # InnoDB stores: (page_level << 10) | (page_n_heap & 0x3FF)
        combined = (page_level << 10) | (page_n_heap & 0x3FF)
        struct.pack_into('<H', page, 42, combined)

    # Continue with remaining page header fields
    # PAGE_FREE (offset 44)
    struct.pack_into('<H', page, 44, 0)
    # PAGE_GARBAGE_END (offset 46)
    struct.pack_into('<H', page, 46, 0)
    # PAGE_LAST_INSERT (offset 48)
    struct.pack_into('<H', page, 48, 0)
    # PAGE_DIRECTION (offset 50)
    struct.pack_into('<H', page, 50, 0)
    # PAGE_N_DIRECT_PAGES (offset 52)
    struct.pack_into('<H', page, 52, 0)
    # PAGE_LEVEL (offset 54) - alias for BTR_N_LEVELS (same value)
    struct.pack_into('<H', page, 54, page_level)
    # PAGE_MAX_TRX_ID (offset 56)
    struct.pack_into('<I', page, 56, 0)

    # 3. Record data area starts at offset 96
    # Add INFIMUM at offset 96
    infimum = create_infimum_record()
    infimum_len = len(infimum)
    page[96:96 + infimum_len] = infimum
    heap_top = 96 + infimum_len

    # Add user records
    for rec in records:
        if heap_top + len(rec) > UNIV_PAGE_SIZE - 8 - 100:
            break
        record_len = len(rec)
        page[heap_top:heap_top + record_len] = rec
        heap_top += record_len

    # Add SUPREMUM
    supremum = create_supremum_record()
    page[heap_top:heap_top + len(supremum)] = supremum
    heap_top += len(supremum)

    # Update PAGE_HEAP_TOP
    struct.pack_into('<H', page, 40, heap_top)

    # 4. Page tail (last 8 bytes)
    page[-8:] = b'\x00' * 8

    return bytes(page)


def create_btree_page_chain(
    num_records: int,
    records_per_page: int = 100,
    space_id: int = 1
) -> List[bytes]:
    """Create a chain of B+Tree index pages"""
    pages = []

    # Calculate number of pages needed
    num_pages = (num_records + records_per_page - 1) // records_per_page

    # Create records
    all_records = []
    for i in range(num_records):
        record_id = i + 1
        name = f"user_{record_id:08d}"
        email = f"user_{record_id:08d}@example.com"
        created_at = f"2024-01-{(i % 28) + 1:02d} 10:00:00"

        rec = create_compact_record(
            record_id=record_id,
            name=name,
            email=email,
            created_at=created_at
        )
        all_records.append(rec)

    # Create pages
    for page_num in range(num_pages):
        start = page_num * records_per_page
        end = min(start + records_per_page, num_records)
        page_records = all_records[start:end]

        lsn = page_num + 1
        page = create_index_page(
            records=page_records,
            page_offset=page_num + 3,  # Pages start at offset 3
            space_id=space_id,
            lsn=lsn,
            prev_page=page_num + 2 if page_num > 0 else 0xFFFFFFFF,
            next_page=page_num + 4 if page_num < num_pages - 1 else 0xFFFFFFFF,
            page_level=0  # Leaf page
        )
        pages.append(page)

    return pages


def generate_large_test_ibd(
    num_records: int = 1000000,
    records_per_page: int = 100,
    output_path: str = './test_large.ibd',
    with_corruption: bool = False,
    corruption_ratio: float = 0.01,
    with_truncation: bool = False,
    truncation_ratio: float = 0.1
):
    """Generate a large IBD file with test data"""
    print(f"[*] Generating {num_records:,} records...")
    print(f"    Records per page: {records_per_page}")

    # Create B+Tree page chain
    pages = create_btree_page_chain(
        num_records=num_records,
        records_per_page=records_per_page,
        space_id=1
    )

    total_pages = len(pages)
    print(f"[*] Created {total_pages} index pages")

    # Apply corruption if requested
    if with_corruption:
        num_corrupt = int(total_pages * corruption_ratio)
        corrupt_indices = random.sample(range(total_pages), num_corrupt)

        for idx in corrupt_indices:
            page = bytearray(pages[idx])
            # Corrupt page header
            corruption_type = random.choice(['header', 'checksum', 'data'])
            if corruption_type == 'header':
                page[38:54] = b'\xFF' * 16
            elif corruption_type == 'checksum':
                struct.pack_into('<I', page, 4, 0xDEADBEEF)
                struct.pack_into('<I', page, 24, 0xDEADBEEF)
            elif corruption_type == 'data':
                # Corrupt random record data
                data_start = 96
                data_end = len(page) - 8 - 100
                offset = random.randint(data_start, data_end - 100)
                page[offset:offset + 100] = b'\xFF' * 100
            pages[idx] = bytes(page)
        print(f"[*] Applied corruption to {num_corrupt} pages")

    # Apply truncation if requested
    if with_truncation:
        num_keep = int(total_pages * (1 - truncation_ratio))
        pages = pages[:num_keep]
        print(f"[*] Truncated to {num_keep} pages")

    # Calculate file size
    file_size = 3 * UNIV_PAGE_SIZE + sum(len(p) for p in pages)
    print(f"[*] Total file size: {file_size:,} bytes (~{file_size/1024/1024:.2f} MB)")

    # Write IBD file
    print(f"[*] Writing IBD file: {output_path}")

    with open(output_path, 'wb') as f:
        # Page 0: FSP_HDR
        fsp_hdr = bytearray(create_index_page(
            records=[],
            page_offset=0,
            space_id=1,
            lsn=0
        ))
        # Change page type to FSP_HDR (0x45BF)
        struct.pack_into('<H', fsp_hdr, 16, 0x45BF)
        f.write(fsp_hdr)

        # Page 1: IBUF_BITMAP
        ibuf = bytearray(create_index_page(
            records=[],
            page_offset=1,
            space_id=1,
            lsn=0
        ))
        struct.pack_into('<H', ibuf, 16, 0x45BD)  # IBUF_BITMAP
        f.write(ibuf)

        # Page 2: INODE
        inode = bytearray(create_index_page(
            records=[],
            page_offset=2,
            space_id=1,
            lsn=0
        ))
        struct.pack_into('<H', inode, 16, 0x45BB)  # INODE
        f.write(inode)

        # Index pages
        for page in pages:
            f.write(page)

    actual_size = os.path.getsize(output_path)
    print(f"[+] Created: {output_path} ({actual_size:,} bytes)")

    return {
        'records': num_records,
        'pages': total_pages,
        'file_size': actual_size,
        'corrupted': with_corruption,
        'truncated': with_truncation
    }


def main():
    parser = argparse.ArgumentParser(
        description='MySQL IBD Large Scale Test Data Generator'
    )
    parser.add_argument(
        '-n', '--records',
        type=int,
        default=1000000,
        help='Number of records to generate (default: 1000000)'
    )
    parser.add_argument(
        '-r', '--records-per-page',
        type=int,
        default=100,
        help='Records per page (default: 100)'
    )
    parser.add_argument(
        '-o', '--output',
        default='./test_large.ibd',
        help='Output IBD file path (default: ./test_large.ibd)'
    )
    parser.add_argument(
        '--corrupt',
        action='store_true',
        help='Add random corruption to pages'
    )
    parser.add_argument(
        '--corrupt-ratio',
        type=float,
        default=0.01,
        help='Corruption ratio (default: 0.01 = 1%%)'
    )
    parser.add_argument(
        '--truncate',
        action='store_true',
        help='Truncate the end of the page chain'
    )
    parser.add_argument(
        '--trunc-ratio',
        type=float,
        default=0.1,
        help='Truncation ratio (default: 0.1 = 10%%)'
    )

    args = parser.parse_args()

    print("=" * 60)
    print("MySQL IBD Large Scale Test Data Generator")
    print("=" * 60)
    print()

    result = generate_large_test_ibd(
        num_records=args.records,
        records_per_page=args.records_per_page,
        output_path=args.output,
        with_corruption=args.corrupt,
        corruption_ratio=args.corrupt_ratio,
        with_truncation=args.truncate,
        truncation_ratio=args.trunc_ratio
    )

    print()
    print(f"[+] Results:")
    print(f"    Records: {result['records']:,}")
    print(f"    Pages: {result['pages']}")
    print(f"    File Size: {result['file_size']:,} bytes")
    print(f"    Corrupted: {result['corrupted']}")
    print(f"    Truncated: {result['truncated']}")

    print()
    print("=" * 60)
    print("[+] Test data generation complete!")
    print("=" * 60)


if __name__ == '__main__':
    main()
