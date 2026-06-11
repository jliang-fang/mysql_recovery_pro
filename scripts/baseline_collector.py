#!/usr/bin/env python3
"""
Baseline Collector for MySQL 8.0 IBD Files
Extracts metadata and generates SHA-256 hashes for recovery validation
"""

import os
import sys
import struct
import hashlib
import json
import argparse
from typing import Dict, List, Any, Optional, Tuple
from pathlib import Path


class BaselineCollector:
    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.baseline: Dict[str, Any] = {
            "file_info": {},
            "schema": {},
            "page_info": [],
            "row_hashes": []
        }
    
    def read_page(self, data: bytes, page_no: int) -> Optional[bytes]:
        """Read a specific page from the file data"""
        start = page_no * self.page_size
        end = start + self.page_size
        if end > len(data):
            return None
        return data[start:end]
    
    def parse_page_header(self, page: bytes) -> Dict[str, Any]:
        """Parse InnoDB page header (offset 0-37)"""
        header = page[:38]
        return {
            "page_offset": page_no,
            "page_type": struct.unpack(">H", header[0:2])[0] if len(header) >= 2 else 0,
            "page_level": struct.unpack(">H", header[38:40])[0] if len(header) >= 40 else 0,
            "record_count": struct.unpack(">H", header[54:56])[0] if len(header) >= 56 else 0,
            "next_page": struct.unpack(">I", header[26:30])[0] if len(header) >= 30 else 0,
            "prev_page": struct.unpack(">I", header[30:34])[0] if len(header) >= 34 else 0,
            "free_space": struct.unpack(">H", header[10:12])[0] if len(header) >= 12 else 0,
            "garbage_space": struct.unpack(">H", header[12:14])[0] if len(header) >= 14 else 0
        }
    
    def parse_sdi_page(self, page: bytes) -> Dict[str, Any]:
        """Parse SDI (Serialized Dictionary Information) page"""
        result = {
            "page_type": "SDI",
            "sdi_version": 0,
            "seq_id": 0,
            "object_type": "",
            "object_def": ""
        }
        
        # SDI header starts at offset 38
        if len(page) < 100:
            return result
        
        # SDI format: version (2), seq_id (4), crc32 (4), flags (2), object_type_len (2), object_def_len (8)
        sdi_version = struct.unpack(">H", page[38:40])[0] if len(page) >= 40 else 0
        seq_id = struct.unpack(">I", page[40:44])[0] if len(page) >= 44 else 0
        
        result["sdi_version"] = sdi_version
        result["seq_id"] = seq_id
        
        return result
    
    def extract_record_hashes(self, data: bytes) -> List[Dict[str, Any]]:
        """Extract all records and compute SHA-256 hashes"""
        hashes = []
        
        # Read all data pages (skipFSP header, IBUF bitmap, SDI pages initially)
        total_pages = len(data) // self.page_size
        
        for page_no in range(10, min(total_pages, 500)):  # Skip first pages, sample up to 500
            page = self.read_page(data, page_no)
            if not page:
                continue
            
            page_type = struct.unpack(">H", page[0:2])[0] if len(page) >= 2 else 0
            
            # InnoDB data pages have type 0x45BC (17852) for B+Tree leaf
            if page_type == 0x45BC or page_type == 0x26F1:
                header_info = self.parse_page_header(page)
                
                # Try to extract record offsets from infimum/supremum
                # InnoDB record heap starts at offset 96 for newer versions
                if len(page) >= 120:
                    heap_top = struct.unpack("<H", page[96:98])[0] if len(page) >= 98 else 0
                    
                    # Extract records between infimum (96) and heap_top
                    record_start = 96
                    record_count = 0
                    
                    while record_start < heap_top and record_start + 20 < len(page):
                        # Record header: next_rec (2), extra_size (1), fields_mask (varies)
                        if page[record_start] == 0x00 and page[record_start + 1] == 0x00:
                            record_start += 2
                            continue
                        
                        # Compute record hash from bytes
                        rec_start = record_start
                        rec_len = min(256, len(page) - record_start)
                        
                        if rec_len > 0:
                            record_data = page[rec_start:rec_start + rec_len]
                            record_hash = hashlib.sha256(record_data).hexdigest()
                            
                            hashes.append({
                                "page_no": page_no,
                                "record_offset": record_start,
                                "record_size": rec_len,
                                "sha256": record_hash
                            })
                            record_count += 1
                            record_start += rec_len + 5  # Move to next potential record
                        
                        if record_count > 100:  # Limit records per page
                            break
        
        return hashes
    
    def collect(self, filepath: str) -> Dict[str, Any]:
        """Collect baseline information from IBD file"""
        with open(filepath, 'rb') as f:
            data = f.read()
        
        self.baseline["file_info"] = {
            "path": filepath,
            "size": len(data),
            "total_pages": len(data) // self.page_size,
            "md5": hashlib.md5(data).hexdigest(),
            "sha256": hashlib.sha256(data).hexdigest()
        }
        
        # Read key pages
        for page_no in [0, 1, 2, 3, 4, 5]:  # FSP, IBUF, ISI, SDI, etc.
            page = self.read_page(data, page_no)
            if page:
                page_type = struct.unpack(">H", page[0:2])[0] if len(page) >= 2 else 0
                self.baseline["page_info"].append({
                    "page_no": page_no,
                    "page_type": f"0x{page_type:04X}",
                    "page_type_decimal": page_type
                })
                
                # Special handling for SDI page (page 3 or similar)
                if page_type == 0x45BD or page_no == 3:  # SDI page type
                    sdi_info = self.parse_sdi_page(page)
                    self.baseline["sdi_info"] = sdi_info
        
        # Extract record hashes
        self.baseline["row_hashes"] = self.extract_record_hashes(data)
        
        # Summary statistics
        self.baseline["summary"] = {
            "total_files": 1,
            "total_pages": len(data) // self.page_size,
            "total_records_sampled": len(self.baseline["row_hashes"]),
            "unique_hash_count": len(set(h["sha256"] for h in self.baseline["row_hashes"]))
        }
        
        return self.baseline


def main():
    parser = argparse.ArgumentParser(description="Collect baseline information from MySQL IBD file")
    parser.add_argument("--ibd", required=True, help="Path to .ibd file")
    parser.add_argument("--output", default="baseline.json", help="Output JSON file")
    parser.add_argument("--page-size", type=int, default=16384, help="Page size (default: 16384)")
    
    args = parser.parse_args()
    
    collector = BaselineCollector(page_size=args.page_size)
    baseline = collector.collect(args.ibd)
    
    with open(args.output, 'w') as f:
        json.dump(baseline, f, indent=2)
    
    print(f"Baseline collected: {args.output}")
    print(f"Total records sampled: {baseline['summary']['total_records_sampled']}")
    print(f"Unique hashes: {baseline['summary']['unique_hash_count']}")


if __name__ == "__main__":
    main()
