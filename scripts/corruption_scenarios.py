#!/usr/bin/env python3
"""
Corruption Scenarios for MySQL Recovery Pro Testing
模拟各种数据库故障场景以测试99.9%恢复率
Author: Qwen Code
Version: 2.0.0 - 99.9% Recovery Validation
"""

import os
import sys
import struct
import random
import argparse
from typing import List, Tuple, Dict, Optional
from dataclasses import dataclass
from pathlib import Path
from datetime import datetime


# MySQL InnoDB constants
FIL_PAGE_TYPE_INDEX = 0x26F1
FIL_PAGE_TYPE_SDI = 0x26F3
FIL_PAGE_TYPE_UNDO_LOG = 0x26F2
FIL_PAGE_TYPE_BLOB = 0x26F4
FIL_PAGE_TYPE_ALLOCATED = 0


def read_le_uint16(data: bytes, offset: int) -> int:
    return struct.unpack('<H', data[offset:offset+2])[0]


def read_le_uint32(data: bytes, offset: int) -> int:
    return struct.unpack('<I', data[offset:offset+4])[0]


def read_le_uint64(data: bytes, offset: int) -> int:
    return struct.unpack('<Q', data[offset:offset+8])[0]


def write_le_uint16(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into('<H', data, offset, value & 0xFFFF)


def write_le_uint32(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into('<I', data, offset, value & 0xFFFFFFFF)


def write_le_uint64(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into('<Q', data, offset, value)


@dataclass
class FileStats:
    total_pages: int = 0
    page_size: int = 0
    file_size: int = 0
    page_types: Dict[int, int] = None

    def __post_init__(self):
        if self.page_types is None:
            self.page_types = {}


def analyze_file(filepath: str) -> FileStats:
    """Analyze an .ibd file and return statistics"""
    stats = FileStats()

    with open(filepath, 'rb') as f:
        data = f.read()

    stats.file_size = len(data)

    # Detect page size
    for page_size in [4096, 8192, 16384, 32768, 65536]:
        if len(data) >= page_size:
            page_type = read_le_uint16(data, 16)
            if page_type in [FIL_PAGE_TYPE_INDEX, FIL_PAGE_TYPE_SDI,
                           FIL_PAGE_TYPE_UNDO_LOG, FIL_PAGE_TYPE_BLOB]:
                stats.page_size = page_size
                break

    if stats.page_size == 0:
        stats.page_size = 16384

    stats.total_pages = len(data) // stats.page_size

    # Analyze page types
    for page_no in range(min(stats.total_pages, 100)):
        offset = page_no * stats.page_size
        if offset + 52 <= len(data):
            page_type = read_le_uint16(data, offset + 16)
            stats.page_types[page_type] = stats.page_types.get(page_type, 0) + 1

    return stats


class CorruptionEngine:
    """Implements various corruption scenarios for testing"""

    def __init__(self, seed: int = 42):
        self.seed = seed
        random.seed(seed)

    def corrupt_random_pages(self, data: bytearray, num_pages: int, 
                             page_size: int, exclude_pages: List[int] = None) -> Tuple[bytearray, List[int]]:
        """Corrupt random pages by zeroing them out"""
        if exclude_pages is None:
            exclude_pages = [0]  # Always exclude root page
        
        total_pages = len(data) // page_size
        available_pages = [i for i in range(total_pages) if i not in exclude_pages]
        
        if num_pages >= len(available_pages):
            num_pages = len(available_pages) - 1
        
        corrupted_pages = random.sample(available_pages, num_pages)
        
        result = bytearray(data)
        for page_no in corrupted_pages:
            offset = page_no * page_size
            for i in range(page_size):
                result[offset + i] = 0
        
        return result, corrupted_pages

    def corrupt_page_headers(self, data: bytearray, page_size: int, 
                             page_numbers: List[int] = None) -> Tuple[bytearray, List[int]]:
        """Corrupt page headers (type, checksum, etc.)"""
        total_pages = len(data) // page_size
        
        if page_numbers is None:
            # Corrupt 5% of pages
            num_corrupt = max(1, total_pages * 5 // 100)
            page_numbers = random.sample(range(1, total_pages), num_corrupt)
        
        result = bytearray(data)
        corrupted_pages = []
        
        for page_no in page_numbers:
            offset = page_no * page_size
            
            if offset + 52 > len(data):
                continue
            
            # Corrupt various header fields randomly
            corruption_type = random.choice(['type', 'checksum', 'chain', 'all'])
            
            if corruption_type == 'type':
                write_le_uint16(result, offset + 16, 0xFFFF)
            elif corruption_type == 'checksum':
                write_le_uint32(result, offset + 24, 0xDEADBEEF)
                write_le_uint32(result, offset + 28, 0xDEADBEEF)
            elif corruption_type == 'chain':
                write_le_uint32(result, offset + 0, 0xDEADBEEF)  # prev_page
                write_le_uint32(result, offset + 4, 0xDEADBEEF)  # next_page
            elif corruption_type == 'all':
                write_le_uint16(result, offset + 16, 0xFFFF)
                write_le_uint32(result, offset + 24, 0xDEADBEEF)
                write_le_uint32(result, offset + 28, 0xDEADBEEF)
            
            corrupted_pages.append(page_no)
        
        return result, corrupted_pages

    def corrupt_infimum_records(self, data: bytearray, page_size: int,
                                page_numbers: List[int] = None) -> Tuple[bytearray, List[int]]:
        """Corrupt INFIMUM records in pages"""
        total_pages = len(data) // page_size
        
        if page_numbers is None:
            num_corrupt = max(1, total_pages * 5 // 100)
            page_numbers = random.sample(range(1, total_pages), num_corrupt)
        
        result = bytearray(data)
        corrupted_pages = []
        
        for page_no in page_numbers:
            offset = page_no * page_size
            if offset + 100 > len(data):
                continue
            
            # Corrupt INFIMUM record at offset 54
            result[offset + 54] = 0xFF
            corrupted_pages.append(page_no)
        
        return result, corrupted_pages

    def corrupt_supremum_records(self, data: bytearray, page_size: int,
                                 page_numbers: List[int] = None) -> Tuple[bytearray, List[int]]:
        """Corrupt SUPREMUM records in pages"""
        total_pages = len(data) // page_size
        
        if page_numbers is None:
            num_corrupt = max(1, total_pages * 5 // 100)
            page_numbers = random.sample(range(1, total_pages), num_corrupt)
        
        result = bytearray(data)
        corrupted_pages = []
        
        for page_no in page_numbers:
            offset = page_no * page_size
            if offset + 70 > len(data):
                continue
            
            # Corrupt SUPREMUM record (after INFIMUM)
            result[offset + 67] = 0xFF
            corrupted_pages.append(page_no)
        
        return result, corrupted_pages

    def break_page_chain(self, data: bytearray, page_size: int,
                         page_numbers: List[int] = None) -> Tuple[bytearray, List[int]]:
        """Break page chain by corrupting prev/next pointers"""
        total_pages = len(data) // page_size
        
        if page_numbers is None:
            num_corrupt = max(1, total_pages * 3 // 100)
            page_numbers = random.sample(range(1, total_pages), num_corrupt)
        
        result = bytearray(data)
        corrupted_pages = []
        
        for page_no in page_numbers:
            offset = page_no * page_size
            if offset + 8 > len(data):
                continue
            
            # Corrupt prev_page and next_page pointers
            write_le_uint32(result, offset + 0, 0xDEADBEEF)
            write_le_uint32(result, offset + 4, 0xDEADBEEF)
            corrupted_pages.append(page_no)
        
        return result, corrupted_pages

    def corrupt_record_chain(self, data: bytearray, page_size: int,
                             page_numbers: List[int] = None) -> Tuple[bytearray, List[int]]:
        """Corrupt record chain pointers"""
        total_pages = len(data) // page_size
        
        if page_numbers is None:
            num_corrupt = max(1, total_pages * 5 // 100)
            page_numbers = random.sample(range(1, total_pages), num_corrupt)
        
        result = bytearray(data)
        corrupted_pages = []
        
        for page_no in page_numbers:
            offset = page_no * page_size
            if offset + 70 > len(data):
                continue
            
            # Corrupt multiple record chain pointers
            for rec_offset in range(54, min(offset + 200, page_size - 10), 32):
                write_le_uint16(result, rec_offset + 1, 0xFFFF)
            
            corrupted_pages.append(page_no)
        
        return result, corrupted_pages

    def corrupt_metadata(self, data: bytearray, page_size: int) -> Tuple[bytearray, List[int]]:
        """Corrupt metadata pages (file header, first page)"""
        result = bytearray(data)
        corrupted_pages = []
        
        # Corrupt page 0 (root page header)
        if len(data) >= page_size:
            write_le_uint16(result, 16, 0xFFFF)  # Corrupt page type
            write_le_uint32(result, 24, 0xDEADBEEF)  # Corrupt checksum
            corrupted_pages.append(0)
        
        # Corrupt SDI page if exists
        total_pages = len(data) // page_size
        if total_pages > 2:
            sdi_page = total_pages - 1
            offset = sdi_page * page_size
            if offset + 52 <= len(data):
                write_le_uint16(result, offset + 16, 0xFFFF)
                corrupted_pages.append(sdi_page)
        
        return result, corrupted_pages

    def truncate_file(self, data: bytearray, page_size: int, 
                      truncate_ratio: float = 0.3) -> Tuple[bytearray, List[int]]:
        """Truncate file to simulate file truncation"""
        truncate_pages = int(len(data) / page_size * truncate_ratio)
        truncate_size = truncate_pages * page_size
        
        result = data[:max(0, len(data) - truncate_size)]
        corrupted_pages = list(range(len(data) // page_size - truncate_pages, len(data) // page_size))
        
        return result, corrupted_pages

    def corrupt_checksums(self, data: bytearray, page_size: int,
                          page_numbers: List[int] = None) -> Tuple[bytearray, List[int]]:
        """Corrupt checksums in pages"""
        total_pages = len(data) // page_size
        
        if page_numbers is None:
            num_corrupt = max(1, total_pages * 10 // 100)
            page_numbers = random.sample(range(1, total_pages), num_corrupt)
        
        result = bytearray(data)
        corrupted_pages = []
        
        for page_no in page_numbers:
            offset = page_no * page_size
            if offset + 32 > len(data):
                continue
            
            write_le_uint32(result, offset + 24, 0xDEADBEEF)
            write_le_uint32(result, offset + 28, 0xDEADBEEF)
            corrupted_pages.append(page_no)
        
        return result, corrupted_pages

    def corrupt_btree_level(self, data: bytearray, page_size: int,
                           level_pages: int = 100) -> Tuple[bytearray, List[int]]:
        """Corrupt B+Tree level pages (higher levels in the tree)"""
        total_pages = len(data) // page_size
        
        # Corrupt pages at higher levels (e.g., pages 1000-1100)
        start_page = min(1000, total_pages - level_pages - 1)
        page_numbers = list(range(start_page, start_page + level_pages))
        
        result = bytearray(data)
        corrupted_pages = page_numbers[:]
        
        for page_no in page_numbers:
            offset = page_no * page_size
            if offset + 52 > len(data):
                continue
            
            write_le_uint16(result, offset + 16, 0xFFFF)
        
        return result, corrupted_pages


def run_corruption_scenario(input_path: str, output_path: str, scenario: str,
                            seed: int = 42, page_size: int = 16384,
                            **kwargs) -> Dict:
    """Run a specific corruption scenario"""
    
    corruption_engine = CorruptionEngine(seed=seed)
    
    # Read input file
    with open(input_path, 'rb') as f:
        data = f.read()
    
    stats = analyze_file(input_path)
    print(f"Input file: {input_path}")
    print(f"  Total pages: {stats.total_pages:,}")
    print(f"  Page size: {stats.page_size}")
    print(f"  Scenario: {scenario}")
    
    # Apply corruption
    scenarios = {
        'random_pages': lambda: corruption_engine.corrupt_random_pages(
            data, kwargs.get('num_pages', 100), page_size
        ),
        'page_headers': lambda: corruption_engine.corrupt_page_headers(
            data, page_size, kwargs.get('page_numbers', None)
        ),
        'infimum': lambda: corruption_engine.corrupt_infimum_records(
            data, page_size, kwargs.get('page_numbers', None)
        ),
        'supremum': lambda: corruption_engine.corrupt_supremum_records(
            data, page_size, kwargs.get('page_numbers', None)
        ),
        'chain': lambda: corruption_engine.break_page_chain(
            data, page_size, kwargs.get('page_numbers', None)
        ),
        'record_chain': lambda: corruption_engine.corrupt_record_chain(
            data, page_size, kwargs.get('page_numbers', None)
        ),
        'metadata': lambda: corruption_engine.corrupt_metadata(data, page_size),
        'truncate': lambda: corruption_engine.truncate_file(
            data, page_size, kwargs.get('truncate_ratio', 0.3)
        ),
        'checksum': lambda: corruption_engine.corrupt_checksums(
            data, page_size, kwargs.get('page_numbers', None)
        ),
        'btree_level': lambda: corruption_engine.corrupt_btree_level(
            data, page_size, kwargs.get('level_pages', 100)
        ),
    }
    
    if scenario not in scenarios:
        raise ValueError(f"Unknown scenario: {scenario}. Available: {list(scenarios.keys())}")
    
    corrupted_data, corrupted_pages = scenarios[scenario]()
    
    # Write output
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, 'wb') as f:
        f.write(corrupted_data)
    
    result = {
        'scenario': scenario,
        'input_file': input_path,
        'output_file': output_path,
        'original_size': len(data),
        'corrupted_size': len(corrupted_data),
        'corrupted_pages': len(corrupted_pages),
        'corrupted_page_list': corrupted_pages[:100],  # Limit for JSON
        'corruption_type': scenarios[scenario].__name__,
        'timestamp': datetime.now().isoformat(),
        'page_size': page_size,
        'seed': seed,
        'success': True
    }
    
    print(f"Output file: {output_path}")
    print(f"  Corrupted pages: {len(corrupted_pages)}")
    print(f"  Corruption list (first 100): {corrupted_pages[:100]}")
    
    return result


def main():
    parser = argparse.ArgumentParser(
        description="Corruption Scenarios for MySQL Recovery Testing"
    )
    parser.add_argument("--input", "-i", required=True, help="Input .ibd file")
    parser.add_argument("--output", "-o", required=True, help="Output corrupted .ibd file")
    parser.add_argument("--scenario", "-s", required=True,
                        choices=['random_pages', 'page_headers', 'infimum', 'supremum',
                                'chain', 'record_chain', 'metadata', 'truncate', 
                                'checksum', 'btree_level'],
                        help="Corruption scenario")
    parser.add_argument("--seed", type=int, default=42, help="Random seed (default: 42)")
    parser.add_argument("--page-size", type=int, default=16384,
                        choices=[4096, 8192, 16384, 32768, 65536],
                        help="Page size (default: 16384)")
    parser.add_argument("--num-pages", type=int, default=100,
                        help="Number of pages to corrupt (default: 100)")
    parser.add_argument("--truncate-ratio", type=float, default=0.3,
                        help="Truncate ratio (default: 0.3)")
    parser.add_argument("--level-pages", type=int, default=100,
                        help="Number of B-tree level pages to corrupt (default: 100)")
    parser.add_argument("--json-output", type=str,
                        help="Output JSON file with corruption details")
    
    args = parser.parse_args()
    
    result = run_corruption_scenario(
        input_path=args.input,
        output_path=args.output,
        scenario=args.scenario,
        seed=args.seed,
        page_size=args.page_size,
        num_pages=args.num_pages,
        truncate_ratio=args.truncate_ratio,
        level_pages=args.level_pages
    )
    
    if args.json_output:
        import json
        os.makedirs(os.path.dirname(args.json_output), exist_ok=True)
        with open(args.json_output, 'w') as f:
            json.dump(result, f, indent=2)
        print(f"\nDetails saved to: {args.json_output}")


if __name__ == "__main__":
    main()
