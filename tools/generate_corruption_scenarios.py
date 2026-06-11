#!/usr/bin/env python3
"""
MySQL IBD Corruption Scenario Generator
========================================
生成各种数据损坏场景的测试文件，用于验证损坏检测和修复功能

支持的损坏场景：
1. 页头损坏 (Page Header Corruption)
2. 页类型无效 (Invalid Page Type)
3. 校验和错误 (Checksum Mismatch)
4. 页链断裂 (Page Chain Broken)
5. 记录头损坏 (Record Header Corruption)
6. INFIMUM/SUPREMUM 损坏
7. BLOB 链损坏 (BLOB Chain Corruption)
8. 混合损坏场景 (Mixed Corruption)
"""

import struct
import os
import argparse
from typing import List, Optional


# InnoDB Constants
FIL_PAGE_TYPE_INDEX = 9969      # 0x26F1
FIL_PAGE_TYPE_SDI = 9971        # 0x26F3
FIL_PAGE_TYPE_UNDO_LOG = 9970   # 0x26F2
FIL_PAGE_TYPE_BLOB = 9972       # 0x26F4
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

    struct.pack_into('<H', header, 0, n_dir_slots)
    struct.pack_into('<H', header, 2, heap_top)
    struct.pack_into('<H', header, 4, n_heap & 0x7FFF)
    struct.pack_into('<H', header, 6, free_offset)
    struct.pack_into('<H', header, 8, garbage_end)
    struct.pack_into('<H', header, 10, last_insert)
    struct.pack_into('<H', header, 12, direction)
    struct.pack_into('<H', header, 14, n_direct_pages)
    struct.pack_into('<H', header, 16, header_version)

    return bytes(header)


def create_infimum_record() -> bytes:
    """创建 INFIMUM 系统记录 (13 bytes)"""
    record = bytearray(13)
    record[0] = REC_STATUS_INFIMUM
    struct.pack_into('<H', record, 1, 54)
    struct.pack_into('<H', record, 3, 0)
    record[5] = 1
    record[6] = 6
    return bytes(record)


def create_supremum_record() -> bytes:
    """创建 SUPREMUM 系统记录 (8 bytes)"""
    record = bytearray(8)
    record[0] = REC_STATUS_SUPREMUM
    struct.pack_into('<H', record, 1, 0xFFFF)
    struct.pack_into('<H', record, 3, 1)
    return bytes(record)


def create_user_record_compact(
    values: List[bytes],
    is_deleted: bool = False,
    is_min_rec: bool = False
) -> bytes:
    """创建 COMPACT 行格式用户记录"""
    record = bytearray()

    info_bits = REC_STATUS_ORDINARY
    if is_deleted:
        info_bits |= REC_INFO_DELETED_FLAG
    if is_min_rec:
        info_bits |= REC_INFO_MIN_REC_FLAG
    record.append(info_bits)

    record.extend(b'\x00\x00')
    record.extend(b'\x00\x00')

    n_fields = len(values)
    record[0] |= (n_fields << 3) & 0xF8
    record.append((n_fields >> 2) & 0x0F)
    record.append(5)

    null_bitmap = 0
    null_count = 0
    for val in values:
        if val is None:
            null_bitmap |= (1 << null_count)
        null_count += 1

    if null_count > 0:
        record.append(null_bitmap)

    for val in values:
        if val is None:
            continue
        if len(val) < 256:
            record.append(len(val))
        else:
            record.append(0xFF)
            record.append((len(val) >> 8) & 0xFF)
        record.extend(val)

    next_offset = len(record)
    struct.pack_into('<H', record, 1, next_offset)

    return bytes(record)


def create_page(
    page_type: int = FIL_PAGE_TYPE_INDEX,
    space_id: int = 1,
    page_offset: int = 0,
    records: Optional[List[bytes]] = None,
    page_level: int = 0,
    mysql_version: int = 80
) -> bytes:
    """创建完整的 InnoDB 页"""
    page = bytearray(UNIV_PAGE_SIZE)

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
    btr_header = bytearray(12)
    struct.pack_into('<I', btr_header, 0, page_level + 1)
    struct.pack_into('<Q', btr_header, 4, 0x123456789ABCDEF0)
    page[38:50] = btr_header

    # 3. Create PAGE_HEADER
    page_header = create_page_header_80(
        heap_top=54,
        n_heap=2,
        page_level=page_level
    )
    page[38:62] = page_header

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
    struct.pack_into('<H', page, PAGE_HEAP_TOP, heap_top)
    struct.pack_into('<H', page, PAGE_N_HEAP, 2 + len(records))

    # 6. Create page directory
    dir_slots = bytearray()
    dir_slots.extend(struct.pack('<H', 54))
    dir_slots.extend(struct.pack('<H', 54+13))

    if records:
        offset = 54 + len(infimum) + len(supremum)
        for record in records:
            dir_slots.extend(struct.pack('<H', offset))
            offset += len(record)

    dir_offset = heap_top
    page[dir_offset:dir_offset+len(dir_slots)] = dir_slots

    n_dir_slots = 2 + len(records) if records else 2
    struct.pack_into('<H', page, PAGE_N_DIR_SLOTS, n_dir_slots)

    # 7. Create page tail
    lsn = struct.unpack('<Q', fil_header[8:16])[0]
    page[-8:] = struct.pack('<Q', lsn)

    return bytes(page)


def create_blob_page(
    space_id: int = 1,
    page_offset: int = 100,
    next_page: int = 0xFFFFFFFF,
    total_length: int = 100,
    current_length: int = 100,
    data: bytes = b'X' * 100
) -> bytes:
    """创建 BLOB 页"""
    page = bytearray(UNIV_PAGE_SIZE)

    # FIL_HEADER
    fil_header = create_fil_header(
        prev_page=0xFFFFFFFF,
        next_page=next_page,
        lsn=1,
        page_type=FIL_PAGE_TYPE_BLOB,
        space_id=space_id,
        page_offset=page_offset
    )
    page[0:38] = fil_header

    # BLOB header at offset 38
    struct.pack_into('<I', page, 38, next_page)
    struct.pack_into('<I', page, 42, total_length)
    struct.pack_into('<I', page, 46, current_length)

    # Data
    page[50:50+len(data)] = data

    # Page tail
    page[-8:] = b'\x00' * 8

    return bytes(page)


def corrupt_page_header(page: bytearray, corruption_level: str = 'critical') -> bytearray:
    """损坏页头"""
    if corruption_level == 'critical':
        # 完全损坏页头
        page[38:62] = b'\xFF' * 24
    elif corruption_level == 'warning':
        # 部分损坏
        page[PAGE_N_DIR_SLOTS] = 0xFF
        page[PAGE_HEAP_TOP] = 0xFF
    return page


def corrupt_page_type(page: bytearray, new_type: int = 0xFFFF) -> bytearray:
    """损坏页类型"""
    struct.pack_into('<H', page, 16, new_type)
    return page


def corrupt_checksum(page: bytearray, new_checksum: int = 0xDEADBEEF) -> bytearray:
    """损坏校验和"""
    struct.pack_into('<I', page, 4, new_checksum)
    struct.pack_into('<I', page, 24, new_checksum)
    return page


def corrupt_page_chain(page: bytearray, new_next: int = 0x12345678) -> bytearray:
    """损坏页链"""
    struct.pack_into('<I', page, 4, new_next)
    return page


def corrupt_infimum(page: bytearray, new_type: int = 0x05) -> bytearray:
    """损坏 INFIMUM 记录"""
    page[54] = new_type
    return page


def corrupt_supremum(page: bytearray, new_type: int = 0x06) -> bytearray:
    """损坏 SUPREMUM 记录"""
    page[54 + 13] = new_type
    return page


def corrupt_record_chain(page: bytearray, offset: int = 100) -> bytearray:
    """损坏记录链"""
    if offset + 1 < len(page):
        struct.pack_into('<H', page, offset + 1, 0x0000)
    return page


def corrupt_blob_chain(blob_page: bytes, new_next: int = 100) -> bytes:
    """损坏 BLOB 链"""
    page = bytearray(blob_page)
    struct.pack_into('<I', page, 38, new_next)
    return bytes(page)


def create_corruption_test_files(output_dir: str = '.'):
    """创建所有损坏场景测试文件"""
    os.makedirs(output_dir, exist_ok=True)

    print("=" * 60)
    print("MySQL IBD Corruption Scenario Generator")
    print("=" * 60)

    # 1. 页头损坏 (Critical)
    print("\n[1/12] Creating page header corruption scenario...")
    page = bytearray(create_page())
    page = corrupt_page_header(page, 'critical')
    with open(os.path.join(output_dir, 'test_header_corrupt_scenario.ibd'), 'wb') as f:
        f.write(bytes(page))
    print(f"    [+] test_header_corrupt_scenario.ibd ({len(page)} bytes)")

    # 2. 页类型无效 (Critical)
    print("\n[2/12] Creating invalid page type scenario...")
    page = bytearray(create_page())
    page = corrupt_page_type(page, 0xFFFF)
    with open(os.path.join(output_dir, 'test_page_type_invalid.ibd'), 'wb') as f:
        f.write(bytes(page))
    print(f"    [+] test_page_type_invalid.ibd ({len(page)} bytes)")

    # 3. 校验和错误 (Warning)
    print("\n[3/12] Creating checksum mismatch scenario...")
    page = bytearray(create_page())
    page = corrupt_checksum(page, 0xDEADBEEF)
    with open(os.path.join(output_dir, 'test_checksum_mismatch.ibd'), 'wb') as f:
        f.write(bytes(page))
    print(f"    [+] test_checksum_mismatch.ibd ({len(page)} bytes)")

    # 4. 页链断裂 (Warning)
    print("\n[4/12] Creating page chain broken scenario...")
    page = bytearray(create_page())
    page = corrupt_page_chain(page, 0x12345678)
    with open(os.path.join(output_dir, 'test_page_chain_broken.ibd'), 'wb') as f:
        f.write(bytes(page))
    print(f"    [+] test_page_chain_broken.ibd ({len(page)} bytes)")

    # 5. INFIMUM 损坏 (Critical)
    print("\n[5/12] Creating INFIMUM corruption scenario...")
    page = bytearray(create_page())
    page = corrupt_infimum(page, 0x05)
    with open(os.path.join(output_dir, 'test_infimum_corrupt.ibd'), 'wb') as f:
        f.write(bytes(page))
    print(f"    [+] test_infimum_corrupt.ibd ({len(page)} bytes)")

    # 6. SUPREMUM 损坏 (Critical)
    print("\n[6/12] Creating SUPREMUM corruption scenario...")
    page = bytearray(create_page())
    page = corrupt_supremum(page, 0x06)
    with open(os.path.join(output_dir, 'test_supremum_corrupt.ibd'), 'wb') as f:
        f.write(bytes(page))
    print(f"    [+] test_supremum_corrupt.ibd ({len(page)} bytes)")

    # 7. 记录链损坏 (Critical)
    print("\n[7/12] Creating record chain corruption scenario...")
    page = bytearray(create_page())
    page = corrupt_record_chain(page, 100)
    with open(os.path.join(output_dir, 'test_record_chain_corrupt.ibd'), 'wb') as f:
        f.write(bytes(page))
    print(f"    [+] test_record_chain_corrupt.ibd ({len(page)} bytes)")

    # 8. BLOB 链损坏 (Critical)
    print("\n[8/12] Creating BLOB chain corruption scenario...")
    blob_page = create_blob_page(page_offset=100)
    blob_page = corrupt_blob_chain(blob_page, 100)  # Circular reference
    with open(os.path.join(output_dir, 'test_blob_chain_corrupt.ibd'), 'wb') as f:
        f.write(bytes(blob_page))
    print(f"    [+] test_blob_chain_corrupt.ibd ({len(blob_page)} bytes)")

    # 9. 多页损坏场景 (Mixed)
    print("\n[9/12] Creating mixed corruption scenario (multiple pages)...")
    pages = []
    
    # Page 0: Normal index page
    page0 = bytearray(create_page(page_offset=0))
    pages.append(bytes(page0))
    
    # Page 1: Corrupted page header
    page1 = bytearray(create_page(page_offset=1))
    page1 = corrupt_page_header(page1, 'critical')
    pages.append(bytes(page1))
    
    # Page 2: Invalid page type
    page2 = bytearray(create_page(page_offset=2))
    page2 = corrupt_page_type(page2, 0xFFFF)
    pages.append(bytes(page2))
    
    # Page 3: Checksum mismatch
    page3 = bytearray(create_page(page_offset=3))
    page3 = corrupt_checksum(page3, 0xBADC0DE)
    pages.append(bytes(page3))
    
    # Page 4: Page chain broken
    page4 = bytearray(create_page(page_offset=4))
    page4 = corrupt_page_chain(page4, 0x99999999)
    pages.append(bytes(page4))
    
    with open(os.path.join(output_dir, 'test_mixed_corruption.ibd'), 'wb') as f:
        for p in pages:
            f.write(p)
    print(f"    [+] test_mixed_corruption.ibd ({len(pages) * UNIV_PAGE_SIZE} bytes, {len(pages)} pages)")

    # 10. 完整的损坏恢复场景 (Truncate + Header)
    print("\n[10/12] Creating truncate + header corruption scenario...")
    original_page = create_page()
    truncated = original_page[:8192]  # Truncate to half
    truncated_page = bytearray(truncated)
    truncated_page = corrupt_page_header(truncated_page, 'critical')
    with open(os.path.join(output_dir, 'test_truncate_header_corrupt.ibd'), 'wb') as f:
        f.write(bytes(truncated_page))
    print(f"    [+] test_truncate_header_corrupt.ibd ({len(truncated_page)} bytes)")

    # 11. rm -rf 恢复场景 (Deleted file simulation)
    print("\n[11/12] Creating rm -rf recovery scenario...")
    # Simulate a file that was deleted but data still exists on disk
    # Create a minimal valid page structure
    rmrf_page = bytearray(create_page())
    rmrf_page = corrupt_checksum(rmrf_page, 0x00000000)  # Zeroed checksum
    with open(os.path.join(output_dir, 'test_rmrf_scenario.ibd'), 'wb') as f:
        f.write(bytes(rmrf_page))
    print(f"    [+] test_rmrf_scenario.ibd ({len(rmrf_page)} bytes)")

    # 12. SDI 损坏场景
    print("\n[12/12] Creating SDI corruption scenario...")
    sdi_page = bytearray(create_page(page_type=FIL_PAGE_TYPE_SDI, page_offset=1))
    sdi_page = corrupt_page_header(sdi_page, 'critical')
    with open(os.path.join(output_dir, 'test_sdi_corrupt.ibd'), 'wb') as f:
        f.write(bytes(sdi_page))
    print(f"    [+] test_sdi_corrupt.ibd ({len(sdi_page)} bytes)")

    # 创建测试结果 JSON
    test_results = {
        "generator_version": "1.0.0",
        "generated_at": __import__('datetime').datetime.now().isoformat(),
        "page_size": UNIV_PAGE_SIZE,
        "scenarios": [
            {
                "name": "page_header_corruption",
                "file": "test_header_corrupt_scenario.ibd",
                "type": "critical",
                "description": "Corrupted page header with all zeros",
                "corruptions": ["PAGE_HEADER_CORRUPTED"]
            },
            {
                "name": "invalid_page_type",
                "file": "test_page_type_invalid.ibd",
                "type": "critical",
                "description": "Invalid page type 0xFFFF",
                "corruptions": ["PAGE_TYPE_INVALID"]
            },
            {
                "name": "checksum_mismatch",
                "file": "test_checksum_mismatch.ibd",
                "type": "warning",
                "description": "Invalid checksum value",
                "corruptions": ["PAGE_CHECKSUM_MISMATCH"]
            },
            {
                "name": "page_chain_broken",
                "file": "test_page_chain_broken.ibd",
                "type": "warning",
                "description": "Next page pointer points to invalid page",
                "corruptions": ["PAGE_CHAIN_BROKEN"]
            },
            {
                "name": "infimum_corruption",
                "file": "test_infimum_corrupt.ibd",
                "type": "critical",
                "description": "INFIMUM record type corrupted",
                "corruptions": ["RECORD_HEADER_CORRUPTED"]
            },
            {
                "name": "supremum_corruption",
                "file": "test_supremum_corrupt.ibd",
                "type": "critical",
                "description": "SUPREMUM record type corrupted",
                "corruptions": ["RECORD_HEADER_CORRUPTED"]
            },
            {
                "name": "record_chain_broken",
                "file": "test_record_chain_corrupt.ibd",
                "type": "critical",
                "description": "Record chain broken",
                "corruptions": ["RECORD_CHAIN_BROKEN"]
            },
            {
                "name": "blob_chain_corruption",
                "file": "test_blob_chain_corrupt.ibd",
                "type": "critical",
                "description": "BLOB chain circular reference",
                "corruptions": ["BLOB_CHAIN_BROKEN"]
            },
            {
                "name": "mixed_corruption",
                "file": "test_mixed_corruption.ibd",
                "type": "mixed",
                "description": "Multiple pages with different corruption types",
                "corruptions": ["PAGE_HEADER_CORRUPTED", "PAGE_TYPE_INVALID", 
                               "PAGE_CHECKSUM_MISMATCH", "PAGE_CHAIN_BROKEN"]
            },
            {
                "name": "truncate_header_corruption",
                "file": "test_truncate_header_corrupt.ibd",
                "type": "mixed",
                "description": "Truncated file with corrupted header",
                "corruptions": ["PAGE_HEADER_CORRUPTED", "PAGE_OVERFLOW_CORRUPTED"]
            },
            {
                "name": "rmrf_scenario",
                "file": "test_rmrf_scenario.ibd",
                "type": "warning",
                "description": "Simulated deleted file with zeroed checksum",
                "corruptions": ["PAGE_CHECKSUM_MISMATCH"]
            },
            {
                "name": "sdi_corruption",
                "file": "test_sdi_corrupt.ibd",
                "type": "critical",
                "description": "SDI page with corrupted header",
                "corruptions": ["SDI_CORRUPTED"]
            }
        ]
    }

    import json
    results_path = os.path.join(output_dir, 'corruption_test_results.json')
    with open(results_path, 'w') as f:
        json.dump(test_results, f, indent=2)
    print(f"\n[+] Test results saved to: {results_path}")

    print("\n" + "=" * 60)
    print("All corruption scenarios generated successfully!")
    print("=" * 60)
    print(f"\nOutput directory: {output_dir}")
    print(f"Total scenarios: {len(test_results['scenarios'])}")


def generate_single_scenario(scenario: str, output_dir: str = '.'):
    """生成单个损坏场景"""
    os.makedirs(output_dir, exist_ok=True)
    
    if scenario == 'header':
        page = bytearray(create_page())
        page = corrupt_page_header(page, 'critical')
        path = os.path.join(output_dir, 'test_header_corrupt_scenario.ibd')
        with open(path, 'wb') as f:
            f.write(bytes(page))
        print(f"Created: {path}")
    elif scenario == 'type':
        page = bytearray(create_page())
        page = corrupt_page_type(page, 0xFFFF)
        path = os.path.join(output_dir, 'test_page_type_invalid.ibd')
        with open(path, 'wb') as f:
            f.write(bytes(page))
        print(f"Created: {path}")
    elif scenario == 'checksum':
        page = bytearray(create_page())
        page = corrupt_checksum(page, 0xDEADBEEF)
        path = os.path.join(output_dir, 'test_checksum_mismatch.ibd')
        with open(path, 'wb') as f:
            f.write(bytes(page))
        print(f"Created: {path}")
    elif scenario == 'chain':
        page = bytearray(create_page())
        page = corrupt_page_chain(page, 0x12345678)
        path = os.path.join(output_dir, 'test_page_chain_broken.ibd')
        with open(path, 'wb') as f:
            f.write(bytes(page))
        print(f"Created: {path}")
    elif scenario == 'infimum':
        page = bytearray(create_page())
        page = corrupt_infimum(page, 0x05)
        path = os.path.join(output_dir, 'test_infimum_corrupt.ibd')
        with open(path, 'wb') as f:
            f.write(bytes(page))
        print(f"Created: {path}")
    elif scenario == 'supremum':
        page = bytearray(create_page())
        page = corrupt_supremum(page, 0x06)
        path = os.path.join(output_dir, 'test_supremum_corrupt.ibd')
        with open(path, 'wb') as f:
            f.write(bytes(page))
        print(f"Created: {path}")
    elif scenario == 'record':
        page = bytearray(create_page())
        page = corrupt_record_chain(page, 100)
        path = os.path.join(output_dir, 'test_record_chain_corrupt.ibd')
        with open(path, 'wb') as f:
            f.write(bytes(page))
        print(f"Created: {path}")
    elif scenario == 'blob':
        blob_page = create_blob_page(page_offset=100)
        blob_page = corrupt_blob_chain(blob_page, 100)
        path = os.path.join(output_dir, 'test_blob_chain_corrupt.ibd')
        with open(path, 'wb') as f:
            f.write(bytes(blob_page))
        print(f"Created: {path}")
    elif scenario == 'mixed':
        create_corruption_test_files(output_dir)
    else:
        print(f"Unknown scenario: {scenario}")
        print("Available scenarios: header, type, checksum, chain, infimum, supremum, record, blob, mixed")


def main():
    parser = argparse.ArgumentParser(
        description='MySQL IBD Corruption Scenario Generator',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python generate_corruption_scenarios.py
  python generate_corruption_scenarios.py --scenario header --output ./test_data
  python generate_corruption_scenarios.py --scenario mixed
        """
    )
    
    parser.add_argument(
        '-o', '--output',
        default='.',
        help='Output directory for test files (default: current directory)'
    )
    
    parser.add_argument(
        '-s', '--scenario',
        choices=['header', 'type', 'checksum', 'chain', 'infimum', 'supremum', 'record', 'blob', 'mixed'],
        help='Generate single scenario instead of all'
    )
    
    parser.add_argument(
        '--list',
        action='store_true',
        help='List all available scenarios'
    )
    
    args = parser.parse_args()
    
    if args.list:
        print("Available corruption scenarios:")
        print("  header   - Page header corruption")
        print("  type     - Invalid page type")
        print("  checksum - Checksum mismatch")
        print("  chain    - Page chain broken")
        print("  infimum  - INFIMUM record corruption")
        print("  supremum - SUPREMUM record corruption")
        print("  record   - Record chain corruption")
        print("  blob     - BLOB chain corruption")
        print("  mixed    - All scenarios combined")
        return
    
    if args.scenario:
        generate_single_scenario(args.scenario, args.output)
    else:
        create_corruption_test_files(args.output)


if __name__ == '__main__':
    main()
