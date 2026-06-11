#!/usr/bin/env python3
"""
Advanced Chaos Monkey - Comprehensive InnoDB File Corruption Simulator
Purpose: Simulate extreme real-world database disaster scenarios for recovery testing
Author: jliang.fang (Global MySQL Kernel QA Expert)
Version: 4.0.0 - Torn Pages, SDI Annihilation, Purge Race, Swiss Cheese Corruption

Scenarios:
- Torn pages (torn_page): Simulate write-thy断电, corrupt last 8K of 16K page
- Metadata annihilation (metadata_annihilation): Wipe .frm (5.x) or SDI (8.x) 
- Purge race condition (purge_race): Drop table, immediately kill -9 process
- Swiss cheese corruption (swiss_cheese): Randomly corrupt 20% of page headers
- Grid corruption (grid_corruption): Corrupt every Nth page in grid pattern
- CRC32 sabotage (crc_sabotage): Replace valid checksums with invalid values
- Page type confusion (type_confusion): Swap page types for chaos
- LSN corruption (lsn_corruption): Mangle log sequence numbers
- Undo log destruction (undo_destroy): Corrupt rollback segment headers
- BLOB chain attack (blob_attack): Break BLOB pointer chains
"""

import os
import sys
import struct
import random
import hashlib
import argparse
import json
from typing import List, Tuple, Dict, Optional, Set
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
import time


class CorruptionType(Enum):
    """Types of corruption that can be applied"""
    TORN_PAGE = "torn_page"
    METADATA_ANNIHILATION = "metadata_annihilation"
    PURGE_RACE = "purge_race"
    SWISS_CHEESE = "swiss_cheese"
    GRID_CORRUPTION = "grid_corruption"
    CRC_SABOTAGE = "crc_sabotage"
    TYPE_CONFUSION = "type_confusion"
    LSN_CORRUPTION = "lsn_corruption"
    UNDO_DESTROY = "undo_destroy"
    BLOB_ATTACK = "blob_attack"
    REPEATABLE_CORRUPTION = "repeatable"
    RANDOM_OVERWRITE = "random_overwrite"
    PAGE_TRUNCATION = "page_truncation"
    HEADER_ZERO = "header_zero"
    FOOTER_ZERO = "footer_zero"


# MySQL InnoDB Constants
FIL_PAGE_TYPE_INDEX = 0x26F1   # B+Tree Index
FIL_PAGE_TYPE_SDI = 0x26F3     # SDI (MySQL 8.0+)
FIL_PAGE_TYPE_UNDO_LOG = 0x26F2
FIL_PAGE_TYPE_BLOB = 0x26F4
FIL_PAGE_TYPE_ALLOCATED = 0
FIL_PAGE_TYPE_IBUF_FREE_LIST = 0x26F0
FIL_PAGE_TYPE_IBUF_BITMAP = 0x26F5
FIL_PAGE_TYPE_SYS = 0x26F6
FIL_PAGE_TYPE_TRX_SYS = 0x26F7
FIL_PAGE_TYPE_FSP_HDR = 0x26F8
FIL_PAGE_TYPE_XDES = 0x26F9
FIL_PAGE_TYPE_BLOB_FRAGMENT = 0x26FA
FIL_PAGE_TYPE_PAGE_COMPRESSED = 0x26FB
FIL_PAGE_TYPE_PAGE_COMPRESSED_FRAGMENT = 0x26FC
FIL_PAGE_TYPE_INDEX_OLD = 0x4567  # Old format

# Page type names for reporting
PAGE_TYPE_NAMES = {
    FIL_PAGE_TYPE_INDEX: "INDEX",
    FIL_PAGE_TYPE_SDI: "SDI",
    FIL_PAGE_TYPE_UNDO_LOG: "UNDO_LOG",
    FIL_PAGE_TYPE_BLOB: "BLOB",
    FIL_PAGE_TYPE_ALLOCATED: "ALLOCATED",
    FIL_PAGE_TYPE_IBUF_FREE_LIST: "IBUF_FREE_LIST",
    FIL_PAGE_TYPE_IBUF_BITMAP: "IBUF_BITMAP",
    FIL_PAGE_TYPE_SYS: "SYS",
    FIL_PAGE_TYPE_TRX_SYS: "TRX_SYS",
    FIL_PAGE_TYPE_FSP_HDR: "FSP_HDR",
    FIL_PAGE_TYPE_XDES: "XDES",
    FIL_PAGE_TYPE_BLOB_FRAGMENT: "BLOB_FRAGMENT",
    FIL_PAGE_TYPE_PAGE_COMPRESSED: "PAGE_COMPRESSED",
    FIL_PAGE_TYPE_PAGE_COMPRESSED_FRAGMENT: "PAGE_COMPRESSED_FRAGMENT",
    FIL_PAGE_TYPE_INDEX_OLD: "INDEX_OLD",
}


@dataclass
class FileStats:
    """Statistics about an .ibd file"""
    filepath: str = ""
    file_size: int = 0
    page_size: int = 16384
    total_pages: int = 0
    page_types: Dict[int, int] = field(default_factory=dict)
    index_pages: List[int] = field(default_factory=list)
    sdi_pages: List[int] = field(default_factory=list)
    undo_pages: List[int] = field(default_factory=list)
    blob_pages: List[int] = field(default_factory=list)


@dataclass
class CorruptionResult:
    """Result of a corruption operation"""
    scenario: str = ""
    input_hash: str = ""
    output_hash: str = ""
    corrupted_pages: List[int] = field(default_factory=list)
    corruption_types: List[str] = field(default_factory=list)
    statistics: Dict = field(default_factory=dict)
    timestamp: str = ""


def read_le_uint16(data: bytes, offset: int) -> int:
    """Read little-endian uint16 from bytes"""
    return struct.unpack('<H', data[offset:offset+2])[0]


def read_le_uint32(data: bytes, offset: int) -> int:
    """Read little-endian uint32 from bytes"""
    return struct.unpack('<I', data[offset:offset+4])[0]


def read_le_uint64(data: bytes, offset: int) -> int:
    """Read little-endian uint64 from bytes"""
    return struct.unpack('<Q', data[offset:offset+8])[0]


def write_le_uint16(data: bytearray, offset: int, value: int) -> None:
    """Write little-endian uint16 to bytearray"""
    struct.pack_into('<H', data, offset, value & 0xFFFF)


def write_le_uint32(data: bytearray, offset: int, value: int) -> None:
    """Write little-endian uint32 to bytearray"""
    struct.pack_into('<I', data, offset, value & 0xFFFFFFFF)


def write_le_uint64(data: bytearray, offset: int, value: int) -> None:
    """Write little-endian uint64 to bytearray"""
    struct.pack_into('<Q', data, offset, value)


def calculate_sha256(data: bytes) -> str:
    """Calculate SHA-256 hash of data"""
    return hashlib.sha256(data).hexdigest()


def detect_page_size(data: bytes) -> int:
    """Detect InnoDB page size from file structure"""
    for page_size in [4096, 8192, 16384, 32768, 65536]:
        if len(data) >= page_size:
            # Check page type at offset 16 (FIL_PAGE_TYPE)
            page_type = read_le_uint16(data, 16)
            if page_type in PAGE_TYPE_NAMES:
                return page_size
    return 16384  # Default


def analyze_ibd_file(filepath: str) -> FileStats:
    """Analyze an .ibd file and extract statistics"""
    stats = FileStats(filepath=filepath)
    
    with open(filepath, 'rb') as f:
        data = f.read()
    
    stats.file_size = len(data)
    stats.page_size = detect_page_size(data)
    stats.total_pages = len(data) // stats.page_size
    
    # Analyze page types
    for page_no in range(min(stats.total_pages, 500)):  # Analyze first 500 pages
        offset = page_no * stats.page_size
        if offset + 52 <= len(data):
            page_type = read_le_uint16(data, offset + 16)
            stats.page_types[page_type] = stats.page_types.get(page_type, 0) + 1
            
            # Categorize pages
            if page_type == FIL_PAGE_TYPE_INDEX:
                stats.index_pages.append(page_no)
            elif page_type == FIL_PAGE_TYPE_SDI:
                stats.sdi_pages.append(page_no)
            elif page_type == FIL_PAGE_TYPE_UNDO_LOG:
                stats.undo_pages.append(page_no)
            elif page_type == FIL_PAGE_TYPE_BLOB:
                stats.blob_pages.append(page_no)
    
    return stats


def corrupt_torn_page(data: bytearray, page_no: int, page_size: int,
                      corruption_percent: float = 50.0) -> bytearray:
    """
    Torn Page Corruption: Simulate write-thy断电
    Corrupt the last N% of a page to simulate incomplete write
    """
    offset = page_no * page_size
    if offset + page_size > len(data):
        return data
    
    # Calculate how much to corrupt (last N%)
    corrupt_size = int(page_size * (corruption_percent / 100.0))
    corrupt_start = offset + page_size - corrupt_size
    
    # Overwrite with random bytes or zeros
    corruption_pattern = random.choice(['zeros', 'ones', 'random'])
    
    for i in range(corrupt_start, min(corrupt_start + corrupt_size, len(data))):
        if corruption_pattern == 'zeros':
            data[i] = 0
        elif corruption_pattern == 'ones':
            data[i] = 255
        else:
            data[i] = random.randint(0, 255)
    
    return data


def corrupt_metadata_annihilation(data: bytearray, page_size: int,
                                  version: str = "5.7") -> Tuple[bytearray, List[int]]:
    """
    Metadata Annihilation: Remove metadata essential for table recovery
    - MySQL 5.x: Wipe .frm related info (though .frm is separate file)
    - MySQL 8.x: Erase SDI pages and system dict
    - MySQL 9.x: Erase SDI and vector metadata
    """
    corrupted_pages = []
    
    #Find and corrupt SDI pages (MySQL 8.0+)
    for page_no in range(len(data) // page_size):
        offset = page_no * page_size
        if offset + 52 <= len(data):
            page_type = read_le_uint16(data, offset + 16)
            
            if page_type == FIL_PAGE_TYPE_SDI:
                # Wipe entire SDI page with zeros
                for i in range(offset, offset + page_size):
                    data[i] = 0
                corrupted_pages.append(page_no)
    
    # Also corrupt some system pages if available
    if len(data) >= page_size * 2:
        # Corrupt page 1 (possibly FSP_HDR)
        for i in range(page_size, min(page_size * 2, len(data))):
            data[i] = 0
        corrupted_pages.append(1)
    
    return data, corrupted_pages


def corrupt_purge_race(data: bytearray, page_size: int,
                       num_corruptions: int = 5) -> Tuple[bytearray, List[int]]:
    """
    Purge Race Condition: Simulate DROP TABLE / TRUNCATE followed by immediate kill -9
    This scenarios tests recovery of zombie/dangling records
    """
    corrupted_pages = []
    
    # Corrupt record headers to simulate deleted but not purged records
    # Mark records as deleted with corrupted next pointers
    
    for _ in range(num_corruptions):
        page_no = random.randint(1, len(data) // page_size - 1)
        offset = page_no * page_size + 38  # Page header
        
        if offset + 20 <= len(data):
            # Corrupt heap_top to simulate Purge Race state
            write_le_uint16(data, offset + 2, random.randint(0, 65535))
            # Corrupt number of directory slots
            write_le_uint16(data, offset + 6, random.randint(0, 100))
            corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_swiss_cheese(data: bytearray, page_size: int,
                         corruption_ratio: float = 0.20,
                         seed: int = None) -> Tuple[bytearray, Dict]:
    """
    Swiss Cheese Corruption: Randomly corrupt 20% of pages
    Simulates silent disk corruption across the file
    """
    if seed:
        random.seed(seed)
    
    corrupted = {
        "total_corrupted": 0,
        "per_type": {},
        "page_numbers": []
    }
    
    total_pages = len(data) // page_size
    num_corruptions = int(total_pages * corruption_ratio)
    
    # Randomly select pages to corrupt
    available_pages = list(range(1, total_pages))
    num_corruptions = min(num_corruptions, len(available_pages))
    
    pages_to_corrupt = random.sample(available_pages, num_corruptions)
    
    for page_no in pages_to_corrupt:
        offset = page_no * page_size
        
        # Determine corruption type for this page
        corruption_type = random.choice([
            'zeros', 'ones', 'random', 'header_only'
        ])
        
        corrupted["per_type"][corruption_type] = corrupted["per_type"].get(corruption_type, 0) + 1
        corrupted["total_corrupted"] += 1
        corrupted["page_numbers"].append(page_no)
        
        if corruption_type == 'header_only':
            # Corrupt only the page header (first 50 bytes)
            corrupt_start = offset
            corrupt_end = min(offset + 50, len(data))
        else:
            # Corrupt random portion of page
            corrupt_start = offset + random.randint(0, page_size // 2)
            corrupt_end = min(offset + page_size, len(data))
        
        for i in range(corrupt_start, corrupt_end):
            if corruption_type == 'zeros':
                data[i] = 0
            elif corruption_type == 'ones':
                data[i] = 255
            else:
                data[i] = random.randint(0, 255)
    
    return data, corrupted


def corrupt_grid(data: bytearray, page_size: int,
                 grid_spacing: int = 10,
                 corruption_type: str = "all") -> Tuple[bytearray, List[int]]:
    """
    Grid Corruption: Corrupt every Nth page in a predictable pattern
    Useful for testing recovery of structured corruption
    """
    corrupted_pages = []
    
    for page_no in range(0, len(data) // page_size, grid_spacing):
        offset = page_no * page_size
        
        if corruption_type == "all":
            # Corrupt entire page
            for i in range(offset, min(offset + page_size, len(data))):
                data[i] = random.randint(0, 255)
        elif corruption_type == "header":
            # Corrupt header only
            for i in range(offset, min(offset + 50, len(data))):
                data[i] = random.randint(0, 255)
        elif corruption_type == "checksum":
            # Corrupt checksum fields
            if offset + 28 <= len(data):
                write_le_uint32(data, offset + 24, random.randint(0, 4294967295))
                write_le_uint32(data, offset + 28, random.randint(0, 4294967295))
        
        corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_crc_sabotage(data: bytearray, page_size: int,
                         num_corruptions: int = 10) -> Tuple[bytearray, List[int]]:
    """
    CRC32 Sabotage: Replace valid checksums with invalid values
    Tests checksum validation recovery
    """
    corrupted_pages = []
    
    for _ in range(num_corruptions):
        page_no = random.randint(1, len(data) // page_size - 1)
        offset = page_no * page_size
        
        if offset + 32 <= len(data):
            # Corrupt both checksum fields
            write_le_uint32(data, offset + 24, random.randint(0, 4294967295))
            write_le_uint32(data, offset + 28, random.randint(0, 4294967295))
            corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_type_confusion(data: bytearray, page_size: int,
                           num_corruptions: int = 5) -> Tuple[bytearray, List[int]]:
    """
    Type Confusion: Swap page types to confuse recovery logic
    Tests recovery with incorrect page type information
    """
    corrupted_pages = []
    valid_types = [FIL_PAGE_TYPE_INDEX, FIL_PAGE_TYPE_SDI, 
                   FIL_PAGE_TYPE_UNDO_LOG, FIL_PAGE_TYPE_BLOB]
    
    for _ in range(num_corruptions):
        page_no = random.randint(1, len(data) // page_size - 1)
        offset = page_no * page_size
        
        if offset + 20 <= len(data):
            # Read current page type
            current_type = read_le_uint16(data, offset + 16)
            
            # Replace with different valid type
            new_type = random.choice(valid_types)
            while new_type == current_type and len(valid_types) > 1:
                new_type = random.choice(valid_types)
            
            write_le_uint16(data, offset + 16, new_type)
            corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_lsn(data: bytearray, page_size: int,
                num_corruptions: int = 10) -> Tuple[bytearray, List[int]]:
    """
    LSN Corruption: Mangle Log Sequence Numbers
    Tests recovery with invalid transaction log positions
    """
    corrupted_pages = []
    
    for _ in range(num_corruptions):
        page_no = random.randint(1, len(data) // page_size - 1)
        offset = page_no * page_size
        
        if offset + 20 <= len(data):
            # LSN starts at offset 8 (8 bytes)
            write_le_uint64(data, offset + 8, random.randint(0, 18446744073709551615))
            corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_undo_destroy(data: bytearray, page_size: int) -> Tuple[bytearray, List[int]]:
    """
    Undo Log Destruction: Corrupt undo segment headers
    Tests recovery with corrupted rollback segments
    """
    corrupted_pages = []
    
    # Find undo log pages and corrupt their headers
    for page_no in range(len(data) // page_size):
        offset = page_no * page_size
        
        if offset + 52 <= len(data):
            page_type = read_le_uint16(data, offset + 16)
            
            if page_type == FIL_PAGE_TYPE_UNDO_LOG:
                # Corrupt undo log header
                if offset + 36 <= len(data):
                    #undo_state field at offset 36
                    data[offset + 36] = random.randint(0, 255)
                    corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_blob_attack(data: bytearray, page_size: int,
                        num_corruptions: int = 3) -> Tuple[bytearray, List[int]]:
    """
    BLOB Chain Attack: Break BLOB pointer chains
    Tests recovery of fragmented BLOB data
    """
    corrupted_pages = []
    
    # Find BLOB pages and corrupt the chain pointers
    for _ in range(num_corruptions):
        page_no = random.randint(1, len(data) // page_size - 1)
        offset = page_no * page_size
        
        if offset + page_size > len(data):
            continue
        
        # Look for BLOB chain pointers in page
        # BLOB pointers typically include prev/next page references
        
        # Corrupt some page structure to simulate broken chain
        corrupt_type = random.choice(['zeros', 'ones', 'random'])
        
        for i in range(offset + 100, min(offset + 1000, len(data))):
            if corrupt_type == 'zeros':
                data[i] = 0
            elif corrupt_type == 'ones':
                data[i] = 255
            else:
                data[i] = random.randint(0, 255)
        
        corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_page_truncation(data: bytearray, page_size: int,
                            num_pages: int = 1) -> Tuple[bytearray, List[int]]:
    """
    Page Truncation: Truncate file to remove pages
    Simulates severe data loss scenario
    """
    corrupted_pages = []
    
    # Remove last N pages by truncating file
    new_size = len(data) - (num_pages * page_size)
    if new_size > page_size:  # Keep at least one page
        data = data[:new_size]
        for i in range(num_pages):
            corrupted_pages.append(len(data) // page_size + i)
    
    return data, corrupted_pages


def corrupt_header_zero(data: bytearray, page_size: int,
                        num_corruptions: int = 5) -> Tuple[bytearray, List[int]]:
    """Corrupt page headers by zeroing them out"""
    corrupted_pages = []
    
    for _ in range(num_corruptions):
        page_no = random.randint(1, len(data) // page_size - 1)
        offset = page_no * page_size
        
        # Zero out first 50 bytes (header)
        for i in range(offset, min(offset + 50, len(data))):
            data[i] = 0
        
        corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_footer_zero(data: bytearray, page_size: int,
                        num_corruptions: int = 5) -> Tuple[bytearray, List[int]]:
    """Corrupt page footers by zeroing them out"""
    corrupted_pages = []
    
    for _ in range(num_corruptions):
        page_no = random.randint(1, len(data) // page_size - 1)
        offset = page_no * page_size
        
        # Zero out last 200 bytes (footer typically contains trailer checksum area)
        for i in range(max(offset, len(data) - 200), len(data)):
            data[i] = 0
        
        corrupted_pages.append(page_no)
    
    return data, corrupted_pages


def corrupt_random_overwrite(data: bytearray, page_size: int,
                             num_corruptions: int = 10,
                             corruption_size_range: Tuple[int, int] = (1, 100)) -> Tuple[bytearray, List[int]]:
    """Randomly overwrite bytes at random locations"""
    corrupted_pages = set()
    
    for _ in range(num_corruptions):
        page_no = random.randint(1, len(data) // page_size - 1)
        offset = page_no * page_size + random.randint(0, page_size - 1)
        
        size = random.randint(*corruption_size_range)
        for i in range(offset, min(offset + size, len(data))):
            data[i] = random.randint(0, 255)
        
        corrupted_pages.add(page_no)
    
    return data, list(corrupted_pages)


def corrupt_repeatable(data: bytearray, page_size: int,
                       seed: int = 12345) -> Tuple[bytearray, List[int]]:
    """
    Repeatable Corruption: Deterministic corruption for testing
    Same seed produces same corruption pattern
    """
    random.seed(seed)
    return corrupt_random_overwrite(data, page_size, num_corruptions=20)


def create_corruption_scenario(scenario: str, data: bytearray, page_size: int,
                               **kwargs) -> Tuple[bytearray, Dict]:
    """Create a specific corruption scenario"""
    result = {
        "scenario": scenario,
        "corrupted_pages": [],
        "corruption_types": [],
        "statistics": {},
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    }
    
    # Scenario-specific parameters
    page_no = kwargs.get('page_no', None)
    corruption_percent = kwargs.get('corruption_percent', 50.0)
    corruption_ratio = kwargs.get('corruption_ratio', 0.20)
    num_corruptions = kwargs.get('num_corruptions', 5)
    version = kwargs.get('version', '8.0')
    corruption_type = kwargs.get('corruption_type', 'all')
    seed = kwargs.get('seed', None)
    
    if scenario == 'torn_page':
        # Single page torn corruption
        if page_no is None:
            page_no = random.randint(1, len(data) // page_size - 1)
        data = corrupt_torn_page(data, page_no, page_size, corruption_percent)
        result["corrupted_pages"] = [page_no]
        result["corruption_types"] = ['torn_page']
        result["statistics"]["corruption_percent"] = corruption_percent
        
    elif scenario == 'metadata_annihilation':
        data, corrupted = corrupt_metadata_annihilation(data, page_size, version)
        result["corrupted_pages"] = corrupted
        result["corruption_types"] = ['metadata_annihilation'] * len(corrupted)
        result["statistics"]["version"] = version
        
    elif scenario == 'purge_race':
        data, corrupted = corrupt_purge_race(data, page_size, num_corruptions)
        result["corrupted_pages"] = corrupted
        result["corruption_types"] = ['purge_race'] * len(corrupted)
        
    elif scenario == 'swiss_cheese':
        if seed:
            random.seed(seed)
        data, stats = corrupt_swiss_cheese(data, page_size, corruption_ratio)
        result["corrupted_pages"] = stats.get("page_numbers", [])
        result["corruption_types"] = list(stats.get("per_type", {}).keys())
        result["statistics"] = stats
        
    elif scenario == 'grid_corruption':
        grid_spacing = kwargs.get('grid_spacing', 10)
        data, corrupted = corrupt_grid(data, page_size, grid_spacing, corruption_type)
        result["corrupted_pages"] = corrupted
        result["corruption_types"] = ['grid'] * len(corrupted)
        result["statistics"]["grid_spacing"] = grid_spacing
        
    elif scenario == 'crc_sabotage':
        data, corrupted = corrupt_crc_sabotage(data, page_size, num_corruptions)
        result["corrupted_pages"] = corrupted
        result["corruption_types"] = ['crc_sabotage'] * len(corrupted)
        
    elif scenario == 'type_confusion':
        data, corrupted = corrupt_type_confusion(data, page_size, num_corruptions)
        result["corrupted_pages"] = corrupted
        result["corruption_types"] = ['type_confusion'] * len(corrupted)
        
    elif scenario == 'lsn_corruption':
        data, corrupted = corrupt_lsn(data, page_size, num_corruptions)
        result["corrupted_pages"] = corrupted
        result["corruption_types"] = ['lsn_corruption'] * len(corrupted)
        
    elif scenario == 'undo_destroy':
        data, corrupted = corrupt_undo_destroy(data, page_size)
        result["corrupted_pages"] = corrupted
        result["corruption_types"] = ['undo_destroy'] * len(corrupted)
        
    elif scenario == 'blob_attack':
        data, corrupted = corrupt_blob_attack(data, page_size, num_corruptions)
        result["corrupted_pages"] = corrupted
        result["corruption_types"] = ['blob_attack'] * len(corrupted)
        
    elif scenario == 'page_truncation':
        num_pages = kwargs.get('num_pages', 1)
        data, corrupted = corrupt_page_truncation(data, page_size, num_pages)
        result["corrupted_pages"] = corrupted
        result["corruption_types"] = ['page_truncation'] * len(corrupted)
        result["statistics"]["pages_removed"] = num_pages
        
    elif scenario == 'header_zero':
        data, corrupted = corrupt_header_zero(data, page_size, num_corruptions)
        result["corrupted_pages"] = corrupted
        result["corruption_types"] = ['header_zero'] * len(corrupted)
        
    elif scenario == 'footer_zero':
        data, corrupted = corrupt_footer_zero(data, page_size, num_corruptions)
        result["corrupted_pages"] = corrupted
        result["corruption_types"] = ['footer_zero'] * len(corrupted)
        
    elif scenario == 'random_overwrite':
        data, corrupted = corrupt_random_overwrite(data, page_size, num_corruptions)
        result["corrupted_pages"] = corrupted
        result["corruption_types"] = ['random_overwrite'] * len(corrupted)
        
    elif scenario == 'repeatable':
        data, corrupted = corrupt_repeatable(data, page_size, seed or 12345)
        result["corrupted_pages"] = corrupted
        result["corruption_types"] = ['repeatable'] * len(corrupted)
        
    elif scenario == 'all_scenarios':
        # Apply all corruption scenarios
        scenarios = [
            ('torn_page', {'page_no': 5, 'corruption_percent': 80}),
            ('crc_sabotage', {'num_corruptions': 10}),
            ('type_confusion', {'num_corruptions': 5}),
            ('lsn_corruption', {'num_corruptions': 10}),
            ('grid_corruption', {'grid_spacing': 20}),
        ]
        
        for scen, scen_kwargs in scenarios:
            data, scen_result = create_corruption_scenario(scen, data, page_size, **scen_kwargs)
            result["corrupted_pages"].extend(scen_result["corrupted_pages"])
            result["corruption_types"].extend(scen_result["corruption_types"])
            result["statistics"].update(scen_result.get("statistics", {}))
        
    else:
        # Default: swiss cheese with 10% corruption
        data, stats = corrupt_swiss_cheese(data, page_size, 0.10)
        result["corrupted_pages"] = stats.get("page_numbers", [])
        result["corruption_types"] = list(stats.get("per_type", {}).keys())
        result["statistics"] = stats
    
    return data, result


class AdvancedChaosMonkey:
    """Advanced corruption orchestrator for comprehensive testing"""
    
    def __init__(self, verbose: bool = False):
        self.verbose = verbose
        self.results = []
    
    def analyze_file(self, filepath: str) -> FileStats:
        """Analyze input file"""
        return analyze_ibd_file(filepath)
    
    def apply_corruption(self, input_path: str, output_path: str,
                        scenario: str, **kwargs) -> CorruptionResult:
        """Apply corruption and generate result"""
        
        # Read input file
        with open(input_path, 'rb') as f:
            data = bytearray(f.read())
        
        # Detect page size
        page_size = detect_page_size(data)
        
        # Analyze before corruption
        input_hash = calculate_sha256(data)
        stats = analyze_ibd_file(input_path)
        
        # Apply corruption
        corrupted_data, scenario_info = create_corruption_scenario(
            scenario, data, page_size, **kwargs
        )
        
        # Write output file
        with open(output_path, 'wb') as f:
            f.write(corrupted_data)
        
        # Calculate output hash
        output_hash = calculate_sha256(corrupted_data)
        
        # Build result
        result = CorruptionResult(
            scenario=scenario,
            input_hash=input_hash,
            output_hash=output_hash,
            corrupted_pages=scenario_info.get("corrupted_pages", []),
            corruption_types=scenario_info.get("corruption_types", []),
            statistics=scenario_info.get("statistics", {}),
            timestamp=scenario_info.get("timestamp", "")
        )
        
        self.results.append(result)
        
        if self.verbose:
            self._print_result(result, stats)
        
        return result
    
    def _print_result(self, result: CorruptionResult, before_stats: FileStats):
        """Print detailed corruption result"""
        print(f"\n{'='*60}")
        print(f"Corruption Scenario: {result.scenario}")
        print(f"{'='*60}")
        print(f"Input File: {before_stats.filepath}")
        print(f"File Size: {before_stats.file_size:,} bytes")
        print(f"Page Size: {before_stats.page_size}")
        print(f"Total Pages: {before_stats.total_pages:,}")
        print(f"\nPage Type Distribution:")
        for ptype, count in sorted(before_stats.page_types.items(), key=lambda x: -x[1]):
            name = PAGE_TYPE_NAMES.get(ptype, f"UNKNOWN({ptype})")
            print(f"  {name}: {count}")
        
        print(f"\nCorruption Applied:")
        print(f"  Corrupted Pages: {len(result.corrupted_pages)}")
        print(f"  Corruption Types: {result.corruption_types}")
        print(f"  Input Hash (SHA-256): {result.input_hash}")
        print(f"  Output Hash (SHA-256): {result.output_hash}")
        print(f"  Timestamp: {result.timestamp}")
        
        if result.corrupted_pages:
            print(f"\nCorrupted Page Numbers (first 20):")
            print(f"  {result.corrupted_pages[:20]}")
            if len(result.corrupted_pages) > 20:
                print(f"  ... and {len(result.corrupted_pages) - 20} more")
        
        if result.statistics:
            print(f"\nStatistics:")
            for key, value in result.statistics.items():
                print(f"  {key}: {value}")


def generate_corruption_report(results: List[CorruptionResult], output_path: str):
    """Generate JSON report of all corruption scenarios"""
    report = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "total_scenarios": len(results),
        "scenarios": []
    }
    
    for result in results:
        report["scenarios"].append({
            "scenario": result.scenario,
            "corrupted_pages": len(result.corrupted_pages),
            "corruption_types": result.corruption_types,
            "input_hash": result.input_hash,
            "output_hash": result.output_hash,
            "statistics": result.statistics,
            "timestamp": result.timestamp
        })
    
    with open(output_path, 'w') as f:
        json.dump(report, f, indent=2)
    
    print(f"\nCorruption report saved to: {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description='Advanced Chaos Monkey - InnoDB File Corruption Simulator'
    )
    parser.add_argument('--input', '-i', required=True,
                       help='Input .ibd file path')
    parser.add_argument('--output', '-o', required=True,
                       help='Output corrupted .ibd file path')
    parser.add_argument('--scenario', '-s', required=True,
                       choices=[
                           'torn_page',
                           'metadata_annihilation',
                           'purge_race',
                           'swiss_cheese',
                           'grid_corruption',
                           'crc_sabotage',
                           'type_confusion',
                           'lsn_corruption',
                           'undo_destroy',
                           'blob_attack',
                           'page_truncation',
                           'header_zero',
                           'footer_zero',
                           'random_overwrite',
                           'repeatable',
                           'all_scenarios'
                       ],
                       help='Corruption scenario to apply')
    parser.add_argument('--page-no', type=int, default=None,
                       help='Specific page number to corrupt (for torn_page)')
    parser.add_argument('--corruption-percent', type=float, default=50.0,
                       help='Percentage of page to corrupt (0-100, default: 50)')
    parser.add_argument('--corruption-ratio', type=float, default=0.20,
                       help='Ratio of pages to corrupt (0.0-1.0, default: 0.20)')
    parser.add_argument('--num-corruptions', type=int, default=5,
                       help='Number of corruptions to apply')
    parser.add_argument('--version', default='8.0',
                       choices=['5.0', '5.5', '5.6', '5.7', '8.0', '8.4', '9.0'],
                       help='MySQL version for scenario (default: 8.0)')
    parser.add_argument('--grid-spacing', type=int, default=10,
                       help='Grid spacing for grid corruption')
    parser.add_argument('--corruption-type', default='all',
                       choices=['all', 'header', 'checksum'],
                       help='Corruption type for grid')
    parser.add_argument('--seed', type=int, default=None,
                       help='Random seed for reproducibility')
    parser.add_argument('--verbose', '-v', action='store_true',
                       help='Verbose output with detailed analysis')
    parser.add_argument('--report', type=str, default=None,
                       help='Output JSON report file path')
    
    args = parser.parse_args()
    
    # Set random seed for reproducibility
    if args.seed:
        random.seed(args.seed)
    
    # Create chaos monkey
    monkey = AdvancedChaosMonkey(verbose=args.verbose)
    
    # Analyze input file
    print(f"Analyzing input file: {args.input}")
    stats = monkey.analyze_file(args.input)
    
    if args.verbose:
        print(f"\nFile Analysis Results:")
        print(f"  File Size: {stats.file_size:,} bytes")
        print(f"  Page Size: {stats.page_size}")
        print(f"  Total Pages: {stats.total_pages:,}")
        print(f"  Index Pages: {len(stats.index_pages)}")
        print(f"  SDI Pages: {len(stats.sdi_pages)}")
        print(f"  Undo Pages: {len(stats.undo_pages)}")
        print(f"  Blob Pages: {len(stats.blob_pages)}")
    
    # Apply corruption
    result = monkey.apply_corruption(
        args.input, args.output,
        args.scenario,
        page_no=args.page_no,
        corruption_percent=args.corruption_percent,
        corruption_ratio=args.corruption_ratio,
        num_corruptions=args.num_corruptions,
        version=args.version,
        corruption_type=args.corruption_type,
        grid_spacing=args.grid_spacing,
        seed=args.seed
    )
    
    # Generate report if requested
    if args.report:
        generate_corruption_report([result], args.report)
    
    # Print summary
    print(f"\n{'='*60}")
    print(f"SUMMARY")
    print(f"{'='*60}")
    print(f"Input:  {args.input} (SHA-256: {result.input_hash[:32]}...)")
    print(f"Output: {args.output} (SHA-256: {result.output_hash[:32]}...)")
    print(f"Pages Corrupted: {len(result.corrupted_pages)}")
    print(f"Scenario: {result.scenario}")


if __name__ == '__main__':
    main()
