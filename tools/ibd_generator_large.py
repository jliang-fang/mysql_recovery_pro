#!/usr/bin/env python3
"""
MySQL IBD Large Scale Test Data Generator
=========================================
生成大规模 MySQL IBD 测试数据（百万级别记录）
用于测试极端场景下的数据恢复能力

支持的场景：
1. 正常数据（完整恢复基准）
2. 截断场景（尾部数据丢失）
3. 随机坏块（部分页损坏）
4. 页链断裂（page chain broken）
5. 页头损坏（page header corrupted）
"""

import struct
import os
import sys
import random
import argparse
import hashlib
import json
from typing import List, Tuple, Dict, Optional
from dataclasses import dataclass
from tqdm import tqdm

# InnoDB 常量
FIL_PAGE_TYPE_INDEX = 9969      # 0x26F1
FIL_PAGE_TYPE_SDI = 9971        # 0x26F3
FIL_PAGE_TYPE_UNDO_LOG = 9970   # 0x26F2
FIL_PAGE_TYPE_ALLOCATED = 0

UNIV_PAGE_SIZE = 16384
FIL_HEADER_SIZE = 38


def calculate_sha256(data: bytes) -> str:
    """计算数据的 SHA-256 哈希值"""
    return hashlib.sha256(data).hexdigest()


def calculate_records_hash(records: List[bytes]) -> str:
    """计算所有记录的 SHA-256 哈希值"""
    hasher = hashlib.sha256()
    for i, rec in enumerate(records):
        hasher.update(rec)
        hasher.update(str(i).encode())  # 包含索引以区分顺序
    return hasher.hexdigest()


def calculate_csv_hash(csv_content: str) -> str:
    """计算 CSV 内容的 SHA-256 哈希值"""
    return hashlib.sha256(csv_content.encode()).hexdigest()


def calculate_ibd_hash(ibd_path: str) -> str:
    """计算 IBD 文件的 SHA-256 哈希值"""
    sha256_hash = hashlib.sha256()
    with open(ibd_path, 'rb') as f:
        for chunk in iter(lambda: f.read(8192), b''):
            sha256_hash.update(chunk)
    return sha256_hash.hexdigest()


def generate_csv_reference(
    num_records: int,
    output_path: str
) -> str:
    """生成 CSV 参考文件（用于恢复验证）"""
    print(f"[*] Generating CSV reference file: {output_path}")
    with open(output_path, 'w') as f:
        f.write("id,name,email,created_at\n")
        for i in tqdm(range(num_records), desc="Writing CSV"):
            id_val = i + 1
            name = f"user_{id_val:08d}"
            email = f"user_{id_val:08d}@example.com"
            created_at = f"2024-01-{(i % 28) + 1:02d} 10:00:00"
            f.write(f"{id_val},{name},{email},{created_at}\n")

    csv_hash = calculate_csv_hash(open(output_path, 'r').read())
    print(f"[+] CSV reference saved: {output_path}")
    print(f"[+] CSV hash: {csv_hash}")
    return csv_hash


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

    struct.pack_into('<I', header, 0, prev_page)
    struct.pack_into('<I', header, 4, next_page)
    struct.pack_into('<Q', header, 8, lsn)
    struct.pack_into('<H', header, 16, page_type)
    struct.pack_into('<I', header, 18, space_id)
    struct.pack_into('<H', header, 22, page_offset)
    struct.pack_into('<Q', header, 24, file_flush_lsn)

    return bytes(header)


def create_page_header_80(
    n_dir_slots: int = 2,
    heap_top: int = 128,
    n_heap: int = 2,
    free_offset: int = 0,
    garbage_end: int = 0,
    last_insert: int = 0,
    direction: int = 0,
    n_direct_pages: int = 0,
    page_level: int = 0,
    max_trx_id: int = 0,
    header_version: int = 0
) -> bytes:
    """创建 Page Header (24 bytes for MySQL 8.0+)"""
    header = bytearray(24)

    # PAGE_N_DIR_SLOTS (offset 0)
    struct.pack_into('<H', header, 0, n_dir_slots)
    # PAGE_HEAP_TOP (offset 2)
    struct.pack_into('<H', header, 2, heap_top)
    # PAGE_N_HEAP (offset 4)
    struct.pack_into('<H', header, 4, n_heap)
    # PAGE_FREE (offset 6)
    struct.pack_into('<H', header, 6, free_offset)
    # PAGE_GARBAGE_END (offset 8)
    struct.pack_into('<H', header, 8, garbage_end)
    # PAGE_LAST_INSERT (offset 10)
    struct.pack_into('<H', header, 10, last_insert)
    # PAGE_DIRECTION (offset 12)
    struct.pack_into('<H', header, 12, direction)
    # PAGE_N_DIRECT_PAGES (offset 14)
    struct.pack_into('<H', header, 14, n_direct_pages)
    # PAGE_LEVEL (offset 16)
    struct.pack_into('<H', header, 16, page_level)
    # PAGE_MAX_TRX_ID (offset 18-25, 8 bytes) - but buffer is only 24 bytes
    # So we only pack the lower 4 bytes at offset 18
    if max_trx_id > 0xFFFFFFFF:
        # Truncate to fit
        struct.pack_into('<I', header, 18, max_trx_id & 0xFFFFFFFF)
    else:
        struct.pack_into('<I', header, 18, max_trx_id & 0xFFFFFFFF)

    return bytes(header)


def create_compact_record_header(
    deleted: bool = False,
    n_fields: int = 4,
    heap_no: int = 2,
    next_rec: int = 0
) -> bytes:
    """创建 Compact 格式记录头 (5 bytes)"""
    header = bytearray(5)
    bits = 0
    if deleted:
        bits |= 0x08
    bits |= (n_fields - 1) << 3
    header[0] = bits
    struct.pack_into('<H', header, 1, heap_no)
    struct.pack_into('<H', header, 3, next_rec)
    return bytes(header)


def encode_varlen_string(data: bytes) -> bytes:
    """编码可变长度字符串 (1-2 bytes length + data)"""
    if len(data) < 127:
        return bytes([len(data)]) + data
    else:
        return bytes([0x80 | (len(data) >> 8), len(data) & 0xFF]) + data


def create_user_record_compact(
    values: List[bytes],
    deleted: bool = False,
    heap_no: int = 2,
    next_rec: int = 0
) -> bytes:
    """创建 Compact 格式用户记录"""
    record = bytearray()

    # 1. Record header
    header = create_compact_record_header(
        deleted=deleted,
        n_fields=4,
        heap_no=heap_no,
        next_rec=next_rec
    )
    record.extend(header)

    # 2. Row header (for dynamic row format)
    row_header = bytearray(5)
    # NULL bitmap (1 byte for 4 columns, all not null)
    row_header[0] = 0x00
    # length bytes for VARCHAR fields (3 bytes each)
    row_header[1] = len(values[0]) if values[0] else 0  # name length
    row_header[2] = len(values[1]) if len(values) > 1 and values[1] else 0  # email length
    row_header[3] = len(values[2]) if len(values) > 2 and values[2] else 0  # extra length
    row_header[4] = 0
    record.extend(row_header)

    # 3. Data fields
    # id (LONGLONG, 8 bytes)
    if values[0] and values[0].isdigit():
        record.extend(struct.pack('<Q', int(values[0])))
    else:
        record.extend(struct.pack('<Q', 0))

    # name (VARCHAR)
    if len(values) > 1 and values[1]:
        record.extend(encode_varlen_string(values[1]))

    # email (VARCHAR)
    if len(values) > 2 and values[2]:
        record.extend(encode_varlen_string(values[2]))

    # created_at (DATETIME, 5 bytes)
    if len(values) > 3 and values[3]:
        # Simple datetime encoding
        record.extend(b'\x00\x00\x00\x00\x00')

    return bytes(record)


def create_page(
    records: List[bytes] = None,
    page_type: int = FIL_PAGE_TYPE_INDEX,
    space_id: int = 1,
    page_offset: int = 0,
    lsn: int = 1,
    next_page: int = 0xFFFFFFFF,
    prev_page: int = 0xFFFFFFFF
) -> bytes:
    """创建 InnoDB 数据页"""
    page = bytearray(UNIV_PAGE_SIZE)

    # 1. FIL_HEADER (38 bytes)
    fil_header = create_fil_header(
        prev_page=prev_page,
        next_page=next_page,
        lsn=lsn,
        page_type=page_type,
        space_id=space_id,
        page_offset=page_offset
    )
    page[0:38] = fil_header

    # 2. Page Header (24 bytes for 8.0+)
    page_header = create_page_header_80(
        heap_top=128,
        n_heap=2
    )
    page[38:62] = page_header

    # 3. Record data (starting at offset 96)
    current_offset = 96
    heap_top = 128

    if records:
        for i, rec in enumerate(records):
            rec_len = len(rec)
            if current_offset + rec_len > UNIV_PAGE_SIZE - 8:
                break  # No more space
            page[current_offset:current_offset + rec_len] = rec
            heap_top = current_offset + rec_len
            current_offset = heap_top

    # Update heap top in page header
    struct.pack_into('<H', page, 40, heap_top)
    struct.pack_into('<H', page, 42, 2 + len(records) if records else 2)

    # 4. Page tail (last 8 bytes)
    page[-8:] = b'\x00' * 8

    return bytes(page)


def create_btree_root_page(
    records: List[bytes],
    page_offset: int = 3,
    space_id: int = 1
) -> bytes:
    """创建 B+Tree 根页"""
    page = bytearray(UNIV_PAGE_SIZE)

    # FIL_HEADER
    fil_header = create_fil_header(
        prev_page=0xFFFFFFFF,
        next_page=0xFFFFFFFF,
        lsn=1,
        page_type=FIL_PAGE_TYPE_INDEX,
        space_id=space_id,
        page_offset=page_offset
    )
    page[0:38] = fil_header

    # Page Header
    page_header = create_page_header_80(
        n_dir_slots=2,
        heap_top=128,
        n_heap=4,  # INFIMUM + SUPREMUM + 2 records
        page_level=0
    )
    page[38:62] = page_header

    # Directory slots (pointers to records)
    # INFIMUM (heap_no=0)
    # SUPREMUM (heap_no=1)
    # Record 1 (heap_no=2)
    # Record 2 (heap_no=3)

    # Record data
    current = 96
    for rec in records:
        if current + len(rec) > UNIV_PAGE_SIZE - 96:
            break
        page[current:current + len(rec)] = rec
        current += len(rec)

    struct.pack_into('<H', page, 40, current)
    struct.pack_into('<H', page, 42, 4)

    # Page tail
    page[-8:] = b'\x00' * 8

    return bytes(page)


def generate_large_test_data(
    num_records: int = 1000000,
    records_per_page: int = 100,
    output_dir: str = '.',
    with_corruption: bool = False,
    corruption_ratio: float = 0.01,
    with_truncation: bool = False,
    truncation_ratio: float = 0.1,
    baseline_hash: Optional[str] = None,
    compute_hash: bool = True
) -> Dict:
    """生成大规模测试数据"""
    os.makedirs(output_dir, exist_ok=True)

    print(f"[*] Generating {num_records} records...")
    print(f"    Records per page: {records_per_page}")
    print(f"    Output directory: {output_dir}")

    # 生成所有记录
    records = []
    print("[*] Creating records...")
    for i in tqdm(range(num_records), desc="Creating records"):
        # Generate record data
        id_val = i + 1
        name = f"user_{id_val:08d}"
        email = f"user_{id_val:08d}@example.com"
        created_at = f"2024-01-{(i % 28) + 1:02d} 10:00:00"

        record = create_user_record_compact([
            str(id_val).encode(),
            name.encode(),
            email.encode(),
            created_at.encode()
        ])
        records.append(record)

    # 计算需要的页数
    total_pages = (num_records + records_per_page - 1) // records_per_page
    print(f"[*] Total pages needed: {total_pages}")

    # 创建页
    pages = []
    print("[*] Creating pages...")
    for page_idx in tqdm(range(total_pages), desc="Creating pages"):
        start_rec = page_idx * records_per_page
        end_rec = min(start_rec + records_per_page, num_records)
        page_records = records[start_rec:end_rec]

        page = create_page(
            records=page_records,
            page_offset=page_idx + 3,  # Root at page 3
            lsn=page_idx + 1
        )
        pages.append(page)

    # 应用损坏（如果启用）
    if with_corruption:
        print(f"[*] Applying corruption (ratio: {corruption_ratio})...")
        num_corrupt = int(total_pages * corruption_ratio)
        corrupt_pages = random.sample(range(len(pages)), num_corrupt)

        for page_idx in corrupt_pages:
            corruption_type = random.choice(['header', 'checksum', 'data'])
            if corruption_type == 'header':
                # Corrupt page header
                pages[page_idx] = pages[page_idx][:38] + b'\xFF' * 24 + pages[page_idx][62:]
            elif corruption_type == 'checksum':
                # Corrupt checksum
                struct.pack_into('<I', pages[page_idx], 4, 0xDEADBEEF)
                struct.pack_into('<I', pages[page_idx], 24, 0xDEADBEEF)
            elif corruption_type == 'data':
                # Corrupt record data
                offset = random.randint(96, len(pages[page_idx]) - 100)
                pages[page_idx] = pages[page_idx][:offset] + b'\xFF' * 100 + pages[page_idx][offset + 100:]

    # 截断数据（如果启用）
    if with_truncation:
        print(f"[*] Applying truncation (ratio: {truncation_ratio})...")
        num_keep = int(total_pages * (1 - truncation_ratio))
        pages = pages[:num_keep]

    # 创建完整的 IBD 文件
    ibd_path = os.path.join(output_dir, f"test_large_{num_records}.ibd")
    print(f"[*] Writing IBD file: {ibd_path}")

    with open(ibd_path, 'wb') as f:
        # Page 0: FSP_HDR
        fsp_hdr = create_page(
            page_type=0x45BF,
            page_offset=0,
            records=[]
        )
        f.write(fsp_hdr)

        # Page 1: IBUF_BITMAP
        ibuf = create_page(
            page_type=0x45BD,
            page_offset=1,
            records=[]
        )
        f.write(ibuf)

        # Page 2: INODE
        inode = create_page(
            page_type=0x45BB,
            page_offset=2,
            records=[]
        )
        f.write(inode)

        # Page 3+: Index pages
        for i, page_data in enumerate(pages):
            # Update page no
            page = bytearray(page_data)
            struct.pack_into('<H', page, 22, 3 + i)
            f.write(page)

    # 计算文件大小
    file_size = os.path.getsize(ibd_path)
    print(f"[+] Created: {ibd_path} ({file_size:,} bytes, ~{file_size/1024/1024:.2f} MB)")

    # 计算哈希值
    result = {
        'file': ibd_path,
        'records': num_records,
        'pages': total_pages,
        'file_size': file_size,
        'corrupted': with_corruption,
        'truncated': with_truncation,
        'records_hash': '',
        'ibd_hash': '',
        'hash_match': None
    }

    if compute_hash:
        print("[*] Calculating SHA-256 hash...")
        records_hash = calculate_records_hash(records)
        ibd_hash = calculate_ibd_hash(ibd_path)
        print(f"[+] Records hash: {records_hash}")
        print(f"[+] IBD file hash: {ibd_hash}")
        result['records_hash'] = records_hash
        result['ibd_hash'] = ibd_hash

        if baseline_hash:
            result['hash_match'] = (records_hash == baseline_hash)
            if result['hash_match']:
                print(f"[+] Hash verification: PASSED")
            else:
                print(f"[!] Hash verification: FAILED")

        # 保存哈希信息到 JSON 文件
        hash_info_path = os.path.join(output_dir, f"test_large_{num_records}.hash.json")
        with open(hash_info_path, 'w') as f:
            json.dump({
                'records_hash': result['records_hash'],
                'ibd_hash': result['ibd_hash'],
                'num_records': num_records,
                'baseline_hash': baseline_hash
            }, f, indent=2)
        print(f"[+] Hash info saved to: {hash_info_path}")

    return result


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
        default='./test_data/large',
        help='Output directory (default: ./test_data/large)'
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
        help='Corruption ratio (0.01 = 1%%, default: 0.01)'
    )
    parser.add_argument(
        '--truncate',
        action='store_true',
        help='Truncate the end of the file'
    )
    parser.add_argument(
        '--trunc-ratio',
        type=float,
        default=0.1,
        help='Truncation ratio (0.1 = 10%%, default: 0.1)'
    )
    parser.add_argument(
        '--multi',
        type=int,
        default=1,
        help='Generate multiple files with different scenarios'
    )
    parser.add_argument(
        '--baseline-hash',
        type=str,
        default=None,
        help='Expected baseline SHA-256 hash for verification'
    )
    parser.add_argument(
        '--no-hash',
        action='store_true',
        help='Skip hash calculation (faster for very large datasets)'
    )
    parser.add_argument(
        '--generate-csv',
        action='store_true',
        help='Also generate CSV reference file for recovery verification'
    )

    args = parser.parse_args()

    print("=" * 60)
    print("MySQL IBD Large Scale Test Data Generator")
    print("=" * 60)
    print()

    for i in range(args.multi):
        scenario = ""

        # 不同的损坏场景
        with_corrupt = args.corrupt or (args.multi > 1 and i % 3 == 1)
        with_truncate = args.truncate or (args.multi > 1 and i % 3 == 2)

        output_subdir = args.output
        if args.multi > 1:
            output_subdir = os.path.join(args.output, f"scenario_{i}")
            scenario = f"_scenario_{i}"

        result = generate_large_test_data(
            num_records=args.records,
            records_per_page=args.records_per_page,
            output_dir=output_subdir,
            with_corruption=with_corrupt,
            corruption_ratio=args.corrupt_ratio,
            with_truncation=with_truncate,
            truncation_ratio=args.trunc_ratio,
            baseline_hash=args.baseline_hash,
            compute_hash=not args.no_hash
        )

        print()
        print(f"[+] Scenario {i}: {scenario}")
        print(f"    Records: {result['records']:,}")
        print(f"    Pages: {result['pages']}")
        print(f"    File: {result['file']}")
        print(f"    Size: {result['file_size']:,} bytes")
        print(f"    Corruption: {result['corrupted']}")
        print(f"    Truncation: {result['truncated']}")
        if result.get('records_hash'):
            print(f"    Records Hash: {result['records_hash']}")
            print(f"    IBD Hash: {result['ibd_hash']}")
        if result.get('hash_match') is not None:
            print(f"    Hash Match: {'YES' if result['hash_match'] else 'NO'}")
        print()

    # 生成 CSV 参考文件（如果请求）
    if args.generate_csv:
        csv_path = os.path.join(args.output, f"test_large_{args.records}.csv")
        csv_hash = generate_csv_reference(args.records, csv_path)
        print(f"[+] CSV reference hash: {csv_hash}")
        print()

    print("=" * 60)
    print("[+] All test files generated successfully!")
    print(f"    Output directory: {args.output}")
    print("=" * 60)


if __name__ == '__main__':
    main()
