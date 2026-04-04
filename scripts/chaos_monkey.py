#!/usr/bin/env python3
"""
Chaos Monkey - MySQL InnoDB File Corruption Simulator
用于模拟真实世界的数据库灾难场景
"""

import os
import sys
import random
import struct
import hashlib
import argparse
from typing import List, Tuple, Optional
from dataclasses import dataclass
from pathlib import Path


@dataclass
class PageHeader:
    """InnoDB Page Header Structure"""
    prev_page: int = 0xFFFFFFFF
    next_page: int = 0xFFFFFFFF
    lsn: int = 0
    space_id: int = 0
    page_no: int = 0
    checksum: int = 0
    arch_log_no: int = 0


@dataclass
class FileStats:
    """File statistics"""
    total_pages: int = 0
    page_size: int = 0
    file_size: int = 0
    page_types: dict = None
    
    def __post_init__(self):
        if self.page_types is None:
            self.page_types = {}


# MySQL InnoDB constants
FIL_PAGE_TYPE_INDEX = 0x26F1  # B+Tree Index
FIL_PAGE_TYPE_SDI = 0x26F3    # SDI (MySQL 8.0+)
FIL_PAGE_TYPE_UNDO_LOG = 0x26F2
FIL_PAGE_TYPE_BLOB = 0x26F4
FIL_PAGE_TYPE_ALLOCATED = 0


def read_le_uint16(data: bytes, offset: int) -> int:
    """Read little-endian uint16"""
    return struct.unpack('<H', data[offset:offset+2])[0]


def read_le_uint32(data: bytes, offset: int) -> int:
    """Read little-endian uint32"""
    return struct.unpack('<I', data[offset:offset+4])[0]


def read_le_uint64(data: bytes, offset: int) -> int:
    """Read little-endian uint64"""
    return struct.unpack('<Q', data[offset:offset+8])[0]


def write_le_uint16(data: bytearray, offset: int, value: int) -> None:
    """Write little-endian uint16"""
    struct.pack_into('<H', data, offset, value & 0xFFFF)


def write_le_uint32(data: bytearray, offset: int, value: int) -> None:
    """Write little-endian uint32"""
    struct.pack_into('<I', data, offset, value & 0xFFFFFFFF)


def write_le_uint64(data: bytearray, offset: int, value: int) -> None:
    """Write little-endian uint64"""
    struct.pack_into('<Q', data, offset, value)


def analyze_file(filepath: str) -> FileStats:
    """Analyze an .ibd file and return statistics"""
    stats = FileStats()
    
    with open(filepath, 'rb') as f:
        data = f.read()
    
    stats.file_size = len(data)
    
    # Detect page size (first page)
    if len(data) >= 16384:
        # Check for common page sizes
        for page_size in [4096, 8192, 16384, 32768, 65536]:
            if len(data) >= page_size:
                # Check page type at offset 16 (FIL_PAGE_TYPE)
                page_type = read_le_uint16(data, 16)
                if page_type in [FIL_PAGE_TYPE_INDEX, FIL_PAGE_TYPE_SDI, 
                                FIL_PAGE_TYPE_UNDO_LOG, FIL_PAGE_TYPE_BLOB]:
                    stats.page_size = page_size
                    break
    
    if stats.page_size == 0:
        stats.page_size = 16384  # Default
    
    stats.total_pages = len(data) // stats.page_size
    
    # Analyze page types
    for page_no in range(min(stats.total_pages, 100)):  # Analyze first 100 pages
        offset = page_no * stats.page_size
        if offset + 52 <= len(data):
            page_type = read_le_uint16(data, offset + 16)
            stats.page_types[page_type] = stats.page_types.get(page_type, 0) + 1
    
    return stats


def calculate_page_checksum(data: bytes, page_size: int) -> int:
    """Calculate InnoDB page checksum (CRC32)"""
    import zlib
    return zlib.crc32(data[:page_size-4]) & 0xFFFFFFFF


def corrupt_page_header(data: bytearray, page_no: int, page_size: int, 
                       corruption_type: str) -> bytearray:
    """Corrupt a specific page header"""
    offset = page_no * page_size
    
    if offset + 52 > len(data):
        return data
    
    # Corrupt page type
    if corruption_type == "page_type":
        write_le_uint16(data, offset + 16, 0xFFFF)  # Invalid page type
    
    # Corrupt checksum
    elif corruption_type == "checksum":
        write_le_uint32(data, offset + 24, 0xDEADBEEF)
        write_le_uint32(data, offset + 28, 0xDEADBEEF)
    
    # Corrupt page number
    elif corruption_type == "page_no":
        write_le_uint32(data, offset + 20, 0xDEADBEEF)
    
    # Corrupt LSN
    elif corruption_type == "lsn":
        write_le_uint64(data, offset + 8, 0)
    
    # Corrupt prev/next page pointers
    elif corruption_type == "chain":
        write_le_uint32(data, offset + 0, 0xDEADBEEF)  # prev_page
        write_le_uint32(data, offset + 4, 0xDEADBEEF)  # next_page
    
    # Corrupt all header fields
    elif corruption_type == "all":
        write_le_uint32(data, offset + 0, 0xDEADBEEF)  # prev_page
        write_le_uint32(data, offset + 4, 0xDEADBEEF)  # next_page
        write_le_uint64(data, offset + 8, 0)           # LSN
        write_le_uint32(data, offset + 16, 0xFFFF)     # space_id
        write_le_uint32(data, offset + 20, 0xDEADBEEF) # page_no
        write_le_uint32(data, offset + 24, 0xDEADBEEF) # checksum
        write_le_uint32(data, offset + 28, 0xDEADBEEF) # checksum
        write_le_uint16(data, offset + 36, 0xFFFF)     # arch_log_no
    
    return data


def corrupt_random_pages(data: bytearray, page_size: int, num_corruptions: int = 3,
                        corruption_types: List[str] = None) -> Tuple[bytearray, List[int]]:
    """Corrupt random pages in the file"""
    if corruption_types is None:
        corruption_types = ["page_type", "checksum", "page_no", "chain", "all"]
    
    total_pages = len(data) // page_size
    corrupted_pages = []
    
    # Select random pages to corrupt (exclude page 0 if possible)
    available_pages = list(range(1, total_pages)) if total_pages > 1 else [0]
    num_corruptions = min(num_corruptions, len(available_pages))
    
    pages_to_corrupt = random.sample(available_pages, num_corruptions)
    
    for page_no in pages_to_corrupt:
        corruption_type = random.choice(corruption_types)
        data = corrupt_page_header(data, page_no, page_size, corruption_type)
        corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_page_range(data: bytearray, page_size: int, start_page: int, 
                      end_page: int, corruption_type: str = "all") -> bytearray:
    """Corrupt a range of pages"""
    for page_no in range(start_page, min(end_page, len(data) // page_size)):
        data = corrupt_page_header(data, page_no, page_size, corruption_type)
    return data


def corrupt_sdi_pages(data: bytearray, page_size: int) -> Tuple[bytearray, List[int]]:
    """Find and corrupt SDI pages"""
    corrupted_pages = []
    
    for page_no in range(len(data) // page_size):
        offset = page_no * page_size
        if offset + 52 <= len(data):
            page_type = read_le_uint16(data, offset + 16)
            if page_type == FIL_PAGE_TYPE_SDI:
                data = corrupt_page_header(data, page_no, page_size, "all")
                corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_undo_pages(data: bytearray, page_size: int) -> Tuple[bytearray, List[int]]:
    """Find and corrupt undo log pages"""
    corrupted_pages = []
    
    for page_no in range(len(data) // page_size):
        offset = page_no * page_size
        if offset + 52 <= len(data):
            page_type = read_le_uint16(data, offset + 16)
            if page_type == FIL_PAGE_TYPE_UNDO_LOG:
                data = corrupt_page_header(data, page_no, page_size, "all")
                corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_btree_chain(data: bytearray, page_size: int, 
                       num_breaks: int = 2) -> Tuple[bytearray, List[int]]:
    """Break the B+Tree page chain"""
    corrupted_pages = []
    
    # Find index pages
    index_pages = []
    for page_no in range(len(data) // page_size):
        offset = page_no * page_size
        if offset + 52 <= len(data):
            page_type = read_le_uint16(data, offset + 16)
            if page_type == FIL_PAGE_TYPE_INDEX:
                index_pages.append(page_no)
    
    # Break the chain at random points
    if len(index_pages) > 2:
        break_points = random.sample(index_pages[1:-1], min(num_breaks, len(index_pages) - 2))
        for page_no in break_points:
            offset = page_no * page_size
            write_le_uint32(data, offset + 4, 0xDEADBEEF)  # Corrupt next_page
            corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_record_chain(data: bytearray, page_size: int, 
                        num_corruptions: int = 3) -> Tuple[bytearray, List[Tuple[int, int]]]:
    """Corrupt record chains within pages"""
    corrupted = []
    
    for _ in range(num_corruptions):
        page_no = random.randint(1, len(data) // page_size - 1)
        offset = page_no * page_size + 38  # Page header
        
        if offset + 20 <= len(data):
            # Corrupt heap_top
            write_le_uint16(data, offset + 2, 0xFFFF)
            # Corrupt free_offset
            write_le_uint16(data, offset + 6, 0xFFFF)
            corrupted.append((page_no, offset))
    
    return data, corrupted


def corrupt_all_headers(data: bytearray, page_size: int) -> Tuple[bytearray, List[int]]:
    """Corrupt all page headers"""
    corrupted_pages = []
    
    for page_no in range(len(data) // page_size):
        offset = page_no * page_size
        if offset + 52 <= len(data):
            # Corrupt key fields
            write_le_uint32(data, offset + 0, 0xDEADBEEF)  # prev_page
            write_le_uint32(data, offset + 4, 0xDEADBEEF)  # next_page
            write_le_uint64(data, offset + 8, 0)           # LSN
            write_le_uint32(data, offset + 16, 0xFFFF)     # page_type
            write_le_uint32(data, offset + 20, 0xDEADBEEF) # page_no
            write_le_uint32(data, offset + 24, 0xDEADBEEF) # checksum
            write_le_uint32(data, offset + 28, 0xDEADBEEF) # checksum
            corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_file_fragments(data: bytearray, page_size: int, 
                          num_fragments: int = 5) -> Tuple[bytearray, List[Tuple[int, int]]]:
    """Create file fragments by zeroing out random sections"""
    fragments = []
    
    for _ in range(num_fragments):
        start_page = random.randint(1, len(data) // page_size - 5)
        num_pages = random.randint(1, 3)
        start_offset = start_page * page_size
        end_offset = start_offset + num_pages * page_size
        
        # Zero out the fragment
        for i in range(start_offset, min(end_offset, len(data))):
            data[i] = 0
        
        fragments.append((start_page, num_pages))
    
    return data, fragments


def corrupt_metadata(data: bytearray, page_size: int) -> Tuple[bytearray, List[int]]:
    """Corrupt metadata pages (page 0, SDI pages)"""
    corrupted_pages = []
    
    # Corrupt page 0 (file header)
    if page_size <= len(data):
        write_le_uint32(data, 0, 0xDEADBEEF)  # space_id
        write_le_uint32(data, 4, 0xDEADBEEF)  # page_size
        write_le_uint64(data, 8, 0)           # lsn
        corrupted_pages.append(0)
    
    # Corrupt SDI pages
    data, sdi_corrupted = corrupt_sdi_pages(data, page_size)
    corrupted_pages.extend(sdi_corrupted)
    
    return data, corrupted_pages


def create_corruption_scenario(scenario: str, data: bytearray, page_size: int) -> Tuple[bytearray, dict]:
    """Create a specific corruption scenario"""
    result = {
        "scenario": scenario,
        "corrupted_pages": [],
        "corruption_types": []
    }
    
    if scenario == "random_3_pages":
        data, result["corrupted_pages"] = corrupt_random_pages(
            data, page_size, num_corruptions=3
        )
        result["corruption_types"] = ["random"] * 3
    
    elif scenario == "random_5_pages":
        data, result["corrupted_pages"] = corrupt_random_pages(
            data, page_size, num_corruptions=5
        )
        result["corruption_types"] = ["random"] * 5
    
    elif scenario == "random_10_pages":
        data, result["corrupted_pages"] = corrupt_random_pages(
            data, page_size, num_corruptions=10
        )
        result["corruption_types"] = ["random"] * 10
    
    elif scenario == "all_headers":
        data, result["corrupted_pages"] = corrupt_all_headers(data, page_size)
        result["corruption_types"] = ["all"] * len(result["corrupted_pages"])
    
    elif scenario == "btree_chain_broken":
        data, result["corrupted_pages"] = corrupt_btree_chain(data, page_size, num_breaks=2)
        result["corruption_types"] = ["chain"] * len(result["corrupted_pages"])
    
    elif scenario == "metadata_lost":
        data, result["corrupted_pages"] = corrupt_metadata(data, page_size)
        result["corruption_types"] = ["metadata"] * len(result["corrupted_pages"])
    
    elif scenario == "file_fragments":
        data, result["corrupted_pages"] = corrupt_file_fragments(data, page_size, num_fragments=5)
        result["corruption_types"] = ["fragment"] * len(result["corrupted_pages"])
    
    elif scenario == "undo_log_corrupt":
        data, result["corrupted_pages"] = corrupt_undo_pages(data, page_size)
        result["corruption_types"] = ["undo"] * len(result["corrupted_pages"])
    
    elif scenario == "record_chain_broken":
        data, result["corrupted_pages"] = corrupt_record_chain(data, page_size, num_corruptions=3)
        result["corruption_types"] = ["record_chain"] * len(result["corrupted_pages"])
    
    elif scenario == "sdn_only":
        # SDI-only corruption
        data, result["corrupted_pages"] = corrupt_sdi_pages(data, page_size)
        result["corruption_types"] = ["sdi"] * len(result["corrupted_pages"])
    
    elif scenario == "header_only":
        # Only corrupt page headers (not type)
        data, result["corrupted_pages"] = corrupt_random_pages(
            data, page_size, num_corruptions=3,
            corruption_types=["checksum", "page_no", "chain"]
        )
        result["corruption_types"] = ["header"] * 3
    
    else:
        # Default: random corruption
        data, result["corrupted_pages"] = corrupt_random_pages(data, page_size)
        result["corruption_types"] = ["random"] * len(result["corrupted_pages"])
    
    return data, result


def generate_hash(data: bytes) -> str:
    """Generate SHA-256 hash of data"""
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(
        description='Chaos Monkey - MySQL InnoDB File Corruption Simulator'
    )
    parser.add_argument('--input', '-i', required=True,
                       help='Input .ibd file')
    parser.add_argument('--output', '-o', required=True,
                       help='Output corrupted .ibd file')
    parser.add_argument('--scenario', '-s', required=True,
                       choices=[
                           'random_3_pages', 'random_5_pages', 'random_10_pages',
                           'all_headers', 'btree_chain_broken', 'metadata_lost',
                           'file_fragments', 'undo_log_corrupt', 'record_chain_broken',
                           'sdn_only', 'header_only'
                       ],
                       help='Corruption scenario')
    parser.add_argument('--seed', type=int, default=None,
                       help='Random seed for reproducibility')
    parser.add_argument('--verbose', '-v', action='store_true',
                       help='Verbose output')
    
    args = parser.parse_args()
    
    # Set random seed
    if args.seed:
        random.seed(args.seed)
    
    # Read input file
    with open(args.input, 'rb') as f:
        data = bytearray(f.read())
    
    # Analyze file
    page_size = 16384
    if len(data) >= 16384:
        page_type = read_le_uint16(data, 16)
        if page_type in [FIL_PAGE_TYPE_INDEX, FIL_PAGE_TYPE_SDI]:
            # Try to detect page size from file structure
            for ps in [4096, 8192, 16384, 32768, 65536]:
                if len(data) % ps == 0:
                    page_size = ps
                    break
    
    # Apply corruption
    corrupted_data, scenario_info = create_corruption_scenario(
        args.scenario, data, page_size
    )
    
    # Write output file
    with open(args.output, 'wb') as f:
        f.write(corrupted_data)
    
    # Print results
    if args.verbose:
        print(f"Input file: {args.input}")
        print(f"Output file: {args.output}")
        print(f"File size: {len(data)} bytes")
        print(f"Page size: {page_size} bytes")
        print(f"Total pages: {len(data) // page_size}")
        print()
        print(f"Corruption scenario: {scenario_info['scenario']}")
        print(f"Corrupted pages: {len(scenario_info['corrupted_pages'])}")
        print(f"Corruption types: {scenario_info['corruption_types']}")
        print(f"Corrupted page numbers: {scenario_info['corrupted_pages']}")
        print()
        print(f"Input hash: {generate_hash(data)}")
        print(f"Output hash: {generate_hash(corrupted_data)}")
    else:
        # Simple output for automation
        print(f"SCENARIO:{scenario_info['scenario']}")
        print(f"PAGES_CORRUPTED:{len(scenario_info['corrupted_pages'])}")
        print(f"PAGE_NUMBERS:{','.join(map(str, scenario_info['corrupted_pages']))}")
        print(f"HASH_INPUT:{generate_hash(data)}")
        print(f"HASH_OUTPUT:{generate_hash(corrupted_data)}")


if __name__ == '__main__':
    main()
