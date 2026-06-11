#!/usr/bin/env python3
# =============================================================================
# MySQL Recovery Pro - Test Data Generator Engine
# Author: jliang.fang (Global MySQL Kernel QA & Chaos Engineering)
# Purpose: Generate realistic test data patterns for all recovery scenarios
# Target: MySQL 5.0 - 9.x, all InnoDB page formats
# =============================================================================

import os
import sys
import struct
import random
import hashlib
import argparse
import subprocess
import json
import time
from pathlib import Path
from typing import Dict, List, Tuple, Optional, BinaryIO
from dataclasses import dataclass
import sqlite3

# InnoDB Constants
PAGE_SIZE_4K = 4096
PAGE_SIZE_8K = 8192
PAGE_SIZE_16K = 16384
PAGE_SIZE_32K = 32768
PAGE_SIZE_64K = 65536

PAGE_TYPE_INDEX = 8
PAGE_TYPE_BLOB = 17
PAGE_TYPE_FSP_HDR = 15
PAGE_TYPE_IBUF_FREE_LIST_NODE = 1

FIL_PAGE_DATA = 38  # Start of FIL_PAGE_DATA within a page

# ==================== Row Format Structures ====================

@dataclass
class MySQLRow:
    """Represents a single row in an InnoDB table"""
    record_type: int  # 0 = normal, 1 = node pointer, 2 =_infimum, 3 = _supremum
    heap_no: int
    n_owned: int
    next_rec: int  # Offset to next record
    length: int    # Record length
    data: bytes    # Record payload


@dataclass
class BLOBHeader:
    """BLOB page header structure"""
    next_page: int   # Next BLOB page in chain
    prev_page: int   # Previous BLOB page in chain
    len: int         # Length of BLOB data in this page
    unused: bytes    # Unused bytes


class IBDFileGenerator:
    """生成原始 InnoDB .ibd 文件 - bypass MySQL server"""
    
    def __init__(self, output_path: str, page_size: int = PAGE_SIZE_16K):
        self.output_path = Path(output_path)
        self.page_size = page_size
        self.n_pages = 0
        self.page_data = []
        self.records = []
        self.blob_pages = []
        
    def initialize_fsp_header(self) -> bytes:
        """Initialize FSP Header (Page 0)"""
        page = bytearray(self.page_size)
        
        # FIL Header
        struct.pack_into('<I', page, 0, 0)  # FIL_PAGE_PREV
        struct.pack_into('<I', page, 4, 0)  # FIL_PAGE_NEXT
        struct.pack_into('<H', page, 8, 0)  # FIL_PAGE_LSN_LO
        struct.pack_into('<H', page, 10, 0) # FIL_PAGE_LSN_HI
        struct.pack_into('<H', page, 12, 1) # FIL_PAGE_TYPE - FSP_HDR
        struct.pack_into('<I', page, 16, 0) # FIL_PAGE_FILE_FLUSH_LSN
        struct.pack_into('<H', page, 24, 0) # FIL_PAGE_SPACE_ID
        
        # FSP Header (offset 54-116)
        struct.pack_into('<I', page, 54, self.page_size - 54)  # FSP_SIZE
        struct.pack_into('<I', page, 58, 1)  # FSP_FREE
        struct.pack_into('<I', page, 62, 0)  # FSP_FREE_FRAG
        struct.pack_into('<I', page, 66, self.page_size - 54)  # FSP_FULL
        struct.pack_into('<I', page, 70, 0)  # FSP_LAST
        
        # XDES Array
        struct.pack_into('<I', page, 116, 0)  # FSP_XDES_0
        
        return page
    
    def initialize_inode_page(self) -> bytes:
        """Initialize Inode Entry Page"""
        page = bytearray(self.page_size)
        
        # FIL Header
        struct.pack_into('<I', page, 0, 0)  # FIL_PAGE_PREV
        struct.pack_into('<I', page, 4, 0)  # FIL_PAGE_NEXT
        struct.pack_into('<H', page, 8, 1)  # FIL_PAGE_TYPE - INDEX
        struct.pack_into('<H', page, 12, 0) # FIL_PAGE_SPACE_ID
        
        # Page Header
        struct.pack_into('<H', page, 38, 0)  # PAGE_LEVEL
        struct.pack_into('<H', page, 40, 0)  # PAGE_N_DIRECT
        struct.pack_into('<H', page, 42, 2)  # PAGE_N_HEAPS
        
        # Infimum/Supermum Records
        page[56:60] = struct.pack('<H', 0)   # Infimum next
        page[58:60] = struct.pack('<B', 2)   # Infimum type
        page[60:62] = struct.pack('<B', 0)   # Infimum len
        page[62:66] = struct.pack('<H', 96)  # Supremum next
        
        return page
    
    def initialize_index_page(self, level: int = 0) -> bytes:
        """Initialize an index (leaf or internal) page"""
        page = bytearray(self.page_size)
        
        # FIL Header
        struct.pack_into('<I', page, 0, 0)   # FIL_PAGE_PREV
        struct.pack_into('<I', page, 4, 0)   # FIL_PAGE_NEXT
        struct.pack_into('<H', page, 8, 1)   # FIL_PAGE_LSN_LO
        struct.pack_into('<H', page, 10, 0)  # FIL_PAGE_LSN_HI
        struct.pack_into('<H', page, 12, PAGE_TYPE_INDEX)  # FIL_PAGE_TYPE
        struct.pack_into('<I', page, 16, 0)  # FIL_PAGE_SPACE_ID
        
        # Page Header
        struct.pack_into('<H', page, 38, level)       # PAGE_LEVEL
        struct.pack_into('<H', page, 40, 0)           # PAGE_N_DIRECT
        struct.pack_into('<H', page, 42, 2)           # PAGE_N_HEAPS
        struct.pack_into('<I', page, 44, 96)          # PAGE_FREE
        struct.pack_into('<H', page, 48, 0)           # PAGE_GARBAGE
        struct.pack_into('<H', page, 50, 96)          # PAGE_LAST_INSERT
        struct.pack_into('<H', page, 52, 0)           # PAGE_DIRECTION
        struct.pack_into('<H', page, 54, 1)           # PAGE_N_DIR_ENTRIES
        
        # B+Tree Index ID (appendix)
        struct.pack_into('<Q', page, 56, random.randint(1, 0xFFFFFFFFFFFFFFFF))  # INDEX_ID
        
        # Infimum Record
        offset = 96
        page[offset:offset+2] = struct.pack('<H', 128)  # next_rec offset
        page[offset+2:offset+4] = struct.pack('<B', 2)  # type_infimum
        page[offset+4:offset+6] = struct.pack('<B', 0)  # len
        offset += 6
        
        # Supremum Record
        page[offset:offset+2] = struct.pack('<H', 0)    # next_rec = 0 (end)
        page[offset+2:offset+4] = struct.pack('<B', 3)  # type_supremum
        page[offset+4:offset+6] = struct.pack('<B', 0)  # len
        
        return page
    
    def create_blob_page_chain(self, total_blob_size: int) -> List[int]:
        """
        Create a chain of BLOB pages for large BLOB data (> 16KB)
        Returns list of page numbers containing BLOB data
        """
        blob_pages = []
        remaining = total_blob_size
        page_num = len(self.page_data)
        
        # BLOB pages start after index pages
        blob_data_offset = 128  # BLOB data starts at offset 128 in BLOB page
        
        while remaining > 0:
            page = bytearray(self.page_size)
            
            # FIL Header
            struct.pack_into('<I', page, 0, page_num - 1 if blob_pages else 0)  # prev
            struct.pack_into('<I', page, 4, 0)  # next (will be filled later)
            struct.pack_into('<H', page, 8, 1)  # LSN_LO
            struct.pack_into('<H', page, 10, 0) # LSN_HI
            struct.pack_into('<H', page, 12, PAGE_TYPE_BLOB)  # BLOB page type
            struct.pack_into('<I', page, 16, 0) # space_id
            
            # BLOB Header (offset 4)
            if blob_pages:
                prev_page = blob_pages[-1]
                struct.pack_into('<I', page, 4, prev_page)  # prev page
                # Update previous page's next pointer
                prev_page_data = self.page_data[prev_page]
                struct.pack_into('<I', prev_page_data, 4, page_num)
            
            # BLOB data
            data_size = min(remaining, self.page_size - blob_data_offset)
            blob_data = os.urandom(data_size)
            page[blob_data_offset:blob_data_offset + data_size] = blob_data
            
            # BLOB header (len at offset 52)
            struct.pack_into('<I', page, 52, data_size)
            
            blob_pages.append(page_num)
            self.page_data.append(page)
            self.n_pages += 1
            remaining -= data_size
            page_num += 1
        
        # Link last page to NULL
        if blob_pages:
            last_page = self.page_data[blob_pages[-1]]
            struct.pack_into('<I', last_page, 4, 0xFFFFFFFF)  # NULL next
        
        return blob_pages
    
    def create_index_page_with_blob_ref(self, page_num: int, blob_page: int) -> bytes:
        """Create an index page with BLOB reference in record"""
        page = self.initialize_index_page(level=0)
        
        # Create a record with BLOB reference
        # Format: [data] + [BLOB reference]
        blob_ref = struct.pack('<I', blob_page)  # First BLOB page
        
        record = bytearray()
        record += b'\x00' * 20  # Primary key (for example)
        record += blob_ref
        record += b'\x00' * 20  # Other columns
        
        # Record header
        record_header = bytearray()
        record_header += struct.pack('<H', 0)  # next_rec
        record_header += struct.pack('<B', 0)  # type = normal
        record_header += struct.pack('<B', len(record) >> 7)  # info_bits
        record_header += struct.pack('<H', len(record) & 0x7FFF)  # len
        
        final_record = bytes(record_header) + bytes(record)
        
        # Insert record into page
        offset = 96
        page[offset:offset + len(final_record)] = final_record
        page[offset + len(final_record):offset + len(final_record) + 2] = struct.pack('<H', 0)
        
        struct.pack_into('<H', page, 42, 4)  # PAGE_N_HEAPS = 4 (inf, sup, +2 records)
        
        return page
    
    def finalize(self) -> int:
        """Finalize and write IBD file"""
        # Create FSP Header (page 0)
        self.page_data.insert(0, self.initialize_fsp_header())
        self.n_pages = len(self.page_data)
        
        # Write file
        with open(self.output_path, 'wb') as f:
            for page in self.page_data:
                f.write(page)
        
        return self.n_pages
    
    def write_to_file(self, f: BinaryIO):
        """Write all pages to file"""
        for page in self.page_data:
            f.write(page)


class MySQL50RowGenerator:
    """MySQL 5.0 Row Generator (Compact Redundant Format)"""
    
    def generate_row(self, row_id: int, blob_size: int = 0) -> bytes:
        """Generate a single row in MySQL 5.0 format"""
        record = bytearray()
        
        # Row ID (6 bytes)
        record += struct.pack('>Q', row_id)[2:]
        
        # Transaction ID (6 bytes)
        record += struct.pack('>Q', 1)[2:]
        
        # Roll pointer (7 bytes)
        record += b'\x00' * 7
        
        # Column data (example: VARCHAR(255))
        col1 = f"row_{row_id:08d}".encode()
        record += struct.pack('<B', len(col1))
        record += col1
        
        # BLOB reference if needed
        if blob_size > 0:
            record += b'\x00' * 20  # BLOB pointer placeholder
        
        # Record header
        header = bytearray()
        header += struct.pack('<H', 0)       # next_rec
        header += struct.pack('<B', 0)       # type = normal
        header += struct.pack('<B', 0)       # info_bits
        header += struct.pack('<H', len(record) & 0x7FFF)  # len
        
        return bytes(header) + bytes(record)


class MySQL80RowGenerator:
    """MySQL 8.0 Row Generator (Dynamic Format with SDI support)"""
    
    def generate_row(self, row_id: int, blob_size: int = 0) -> bytes:
        """Generate a single row in MySQL 8.0 Dynamic format"""
        record = bytearray()
        
        # Hidden columns
        record += struct.pack('>Q', row_id)  # DB_ROW_ID
        record += struct.pack('>Q', 0)       # DB_TRX_ID
        
        # Column data
        col1 = f"row_{row_id:08d}".encode()
        record += struct.pack('<B', len(col1))
        record += col1
        
        # BLOB reference
        if blob_size > 0:
            record += b'\x00' * 20  # BLOB pointer
        
        # Record header
        header = bytearray()
        header += struct.pack('<H', 0)       # next_rec
        header += struct.pack('<B', 0)       # type
        header += struct.pack('<B', 0)       # info_bits
        header += struct.pack('<H', len(record) & 0x7FFF)
        
        return bytes(header) + bytes(record)


class SDIGenerator:
    """SDI (Space Definition Information) Page Generator for MySQL 8.0+"""
    
    @staticmethod
    def generate_sdi_page(table_name: str, schema_name: str = "test") -> bytes:
        """Generate a SDI page (Page 3)"""
        import json
        
        sdi_data = {
            "dictionary": {
                "object": {
                    "Column": [
                        {
                            "name": "id",
                            "type": "INT",
                            "nullable": False,
                            "charset": "utf8mb4",
                            "length": 11
                        },
                        {
                            "name": "data",
                            "type": "VARCHAR",
                            "nullable": True,
                            "charset": "utf8mb4",
                            "length": 255
                        },
                        {
                            "name": "blob_data",
                            "type": "BLOB",
                            "nullable": True,
                            "charset": "binary",
                            "length": 65535
                        }
                    ],
                    "Table": {
                        "name": table_name,
                        "schema": schema_name,
                        "row_format": "Dynamic",
                        "Create_options": "row_format=DYNAMIC"
                    }
                }
            }
        }
        
        sdi_json = json.dumps(sdi_data, separators=(',', ':'))
        
        # SDI page structure
        page_size = 16384
        page = bytearray(page_size)
        
        # FIL Header
        struct.pack_into('<H', page, 12, 1)  # FIL_PAGE_TYPE = INDEX
        
        # SDI header
        page[38:42] = b'SDI\x00'  # SDI marker
        struct.pack_into('<I', page, 42, 1)  # SDI version
        struct.pack_into('<I', page, 46, len(sdi_json))  # SDI length
        
        # SDI data
        page[128:128 + len(sdi_json)] = sdi_json.encode()
        
        return page


def generate_test_tables():
    """Generate all test tables for chaos testing"""
    generator = IBDFileGenerator("test_tables/test.ibd", PAGE_SIZE_16K)
    
    # Create BLOB chain (32KB total)
    blob_pages = generator.create_blob_chain_chain(32768)
    
    # Create index page with BLOB reference
    index_page = generator.create_index_page_with_blob_ref(1, blob_pages[0])
    generator.page_data.append(index_page)
    
    # Add more rows
    for i in range(100):
        row = MySQL80RowGenerator().generate_row(i + 1)
        generator.records.append(row)
    
    n_pages = generator.finalize()
    
    print(f"Generated IBD file with {n_pages} pages")
    print(f"BLOB pages: {len(blob_pages)}")
    
    return generator


def generate_heavy_blob_ibd(output_path: str, blob_size: int = 1024 * 1024):
    """Generate an IBD file with a massive BLOB (> 1MB)"""
    generator = IBDFileGenerator(output_path, PAGE_SIZE_16K)
    
    # Create BLOB chain (1MB+)
    blob_pages = generator.create_blob_chain(blob_size)
    
    # Add index page
    index_page = generator.create_index_page_with_blob_ref(1, blob_pages[0])
    generator.page_data.append(index_page)
    
    n_pages = generator.finalize()
    
    print(f"Generated file: {output_path}")
    print(f"Total pages: {n_pages}")
    print(f"BLOB pages: {len(blob_pages)}")
    
    return generator


def generate_sdi_corrupted_file(ibd_path: str, page_size: int = 16384):
    """Generate an IBD file with corrupted SDI page"""
    generator = IBDFileGenerator(ibd_path, page_size)
    
    # Create normal pages
    for i in range(10):
        page = generator.initialize_index_page(level=0)
        generator.page_data.append(page)
    
    # Create corrupted SDI page at position 3
    sdi_page = bytearray(page_size)
    struct.pack_into('<H', sdi_page, 12, PAGE_TYPE_INDEX)
    # Corrupt SDI header
    sdi_page[38:42] = b'SDX\x00'  # Wrong marker
    sdi_page[42:46] = b'\x00\x00\x00\x00'  # Corrupt version
    generator.page_data.insert(3, sdi_page)
    
    n_pages = generator.finalize()
    
    return generator


def generate_deep_chain_ibd(output_path: str, depth: int = 5):
    """Generate a B+Tree with deep chain for rebuilding test"""
    generator = IBDFileGenerator(output_path, PAGE_SIZE_16K)
    
    # Create internal nodes
    for level in range(depth, -1, -1):
        page = generator.initialize_index_page(level=level)
        generator.page_data.append(page)
    
    n_pages = generator.finalize()
    
    return generator


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="MySQL Recovery Pro - Test Data Generator"
    )
    parser.add_argument("--output", required=True, help="Output IBD file path")
    parser.add_argument("--type", choices=[
        "normal", "heavy-blob", "sdi-corrupt", "deep-chain",
        "all"
    ], default="normal", help="Test data type")
    parser.add_argument("--blob-size", type=int, default=32768,
                       help="BLOB size in bytes (for heavy-blob)")
    parser.add_argument("--page-size", type=int, default=16384,
                       help="Page size in bytes")
    
    args = parser.parse_args()
    
    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    
    if args.type == "normal":
        generate_test_tables()
    elif args.type == "heavy-blob":
        generate_heavy_blob_ibd(args.output, args.blob_size)
    elif args.type == "sdi-corrupt":
        generate_sdi_corrupted_file(args.output, args.page_size)
    elif args.type == "deep-chain":
        generate_deep_chain_ibd(args.output, depth=5)
    elif args.type == "all":
        generate_test_tables()
        generate_heavy_blob_ibd(args.output.replace(".ibd", "_heavy.ibd"), 1024 * 1024)
        generate_sdi_corrupted_file(args.output.replace(".ibd", "_sdi.ibd"), args.page_size)
