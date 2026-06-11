#!/usr/bin/env python3
"""
MySQL Recovery Pro - Header Repair & SDI Nuclear Chaos Suite
Purpose: Page header corruption, SDI page destruction, and recovery validation
Author: jliang.fang (Global MySQL Kernel QA Expert)
Version: 2.1.0 - Header/SDI Recovery Validation
"""

import os
import sys
import struct
import random
import hashlib
import json
import argparse
from typing import List, Dict, Tuple, Optional, Set
from dataclasses import dataclass, field
from pathlib import Path
from datetime import datetime


# MySQL InnoDB Constants
# Page Types
FIL_PAGE_TYPE_INDEX = 0x26F1      # B+Tree Index
FIL_PAGE_TYPE_SDI = 0x26F3        # SQL Data Dictionary - MySQL 8.0+
FIL_PAGE_TYPE_UNDO_LOG = 0x26F2   # Undo Log
FIL_PAGE_TYPE_BLOB = 0x26F4       # BLOB
FIL_PAGE_TYPE_ALLOCATED = 0

# Page Header Offsets (52 bytes)
FIL_PAGE_PREV = 0                 # Previous page number (4 bytes)
FIL_PAGE_NEXT = 4                 # Next page number (4 bytes)
FIL_PAGE_LSN = 8                  # LSN (8 bytes)
FIL_PAGE_TYPE = 16                # Page type (2 bytes) - OVERLAPS with space_id
FIL_PAGE_SPACE_ID = 16            # Space ID (4 bytes) - OVERLAPS with page_type
FIL_PAGE_OFFSET = 20              # Page number (4 bytes)
FIL_PAGE_CHECKSUM = 24            # Checksum (4 bytes) - DOUBLE WRITTEN
FIL_PAGE_ARCH_LOG_NO = 36         # Archive log number (4 bytes)

# Filter Header Offset
FIL_PAGE_DATA = 38

# Record Types
REC_STATUS_ORDINARY = 0x00
REC_STATUS_NODE_PTR = 0x01
REC_STATUS_INFIMUM = 0x02
REC_STATUS_SUPREMUM = 0x03


@dataclass
class PageHeader:
    """InnoDB Page Header Structure"""
    prev_page: int = 0xFFFFFFFF
    next_page: int = 0xFFFFFFFF
    lsn: int = 0
    space_id: int = 0
    page_no: int = 0
    page_type: int = FIL_PAGE_TYPE_INDEX
    checksum: int = 0
    arch_log_no: int = 0
    
    def to_bytes(self) -> bytes:
        data = bytearray(52)
        struct.pack_into('<I', data, 0, self.prev_page)
        struct.pack_into('<I', data, 4, self.next_page)
        struct.pack_into('<Q', data, 8, self.lsn)
        struct.pack_into('<I', data, 16, self.space_id)
        struct.pack_into('<I', data, 20, self.page_no)
        struct.pack_into('<I', data, 24, self.checksum)
        struct.pack_into('<I', data, 28, self.checksum)  # Double checksum
        struct.pack_into('<H', data, 36, self.arch_log_no)
        return bytes(data)
    
    @classmethod
    def from_bytes(cls, data: bytes, offset: int = 0) -> 'PageHeader':
        return cls(
            prev_page=struct.unpack_from('<I', data, offset + 0)[0],
            next_page=struct.unpack_from('<I', data, offset + 4)[0],
            lsn=struct.unpack_from('<Q', data, offset + 8)[0],
            space_id=struct.unpack_from('<I', data, offset + 16)[0],
            page_no=struct.unpack_from('<I', data, offset + 20)[0],
            page_type=struct.unpack_from('<H', data, offset + 16)[0],
            checksum=struct.unpack_from('<I', data, offset + 24)[0],
            arch_log_no=struct.unpack_from('<H', data, offset + 36)[0]
        )


class HeaderCorruptionEngine:
    """Generate various page header corruption scenarios"""
    
    def __init__(self, page_size: int = 16384, seed: int = 42):
        self.page_size = page_size
        self.seed = seed
        random.seed(seed)
        self.corruption_log: List[Dict] = []
    
    def corrupt_all_page_headers(self, data: bytes) -> Tuple[bytes, Dict]:
        """
        corruption Scenario: All Page Headers
        Corrupt page_type, checksum, page_no across all pages
        """
        corrupted = bytearray(data)
        corrupted_pages = []
        
        total_pages = len(data) // self.page_size
        
        for page_no in range(total_pages):
            offset = page_no * self.page_size
            
            if offset + 52 > len(data):
                break
            
            # Record original values
            original_header = PageHeader.from_bytes(data, offset)
            
            # Corrupt page type
            struct.pack_into('<H', corrupted, offset + 16, 0xFFFF)
            
            # Corrupt checksum
            struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
            struct.pack_into('<I', corrupted, offset + 28, 0xBADC0DE)
            
            # Corrupt page number
            struct.pack_into('<I', corrupted, offset + 20, page_no ^ 0xDEADBEEF)
            
            # Corrupt LSN
            struct.pack_into('<Q', corrupted, offset + 8, 0)
            
            # Corrupt prev/next page pointers
            struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)
            struct.pack_into('<I', corrupted, offset + 4, 0xFEEDBEEF)
            
            corrupted_pages.append({
                "page_no": page_no,
                "original_header": original_header.to_dict() if hasattr(original_header, 'to_dict') else {
                    "page_type": hex(original_header.page_type),
                    "page_no": original_header.page_no,
                    "checksum": format(original_header.checksum, '08x')
                },
                "corrupted_fields": ["page_type", "checksum", "page_no", "lsn", "prev_page", "next_page"]
            })
        
        result = {
            "scenario": "all_headers_corrupted",
            "total_pages": total_pages,
            "corrupted_pages": len(corrupted_pages),
            "corruption_type": "all_fields",
            "corrupted_pages_detail": corrupted_pages
        }
        
        self.corruption_log.append(result)
        return bytes(corrupted), result
    
    def corrupt_random_headers(self, data: bytes, num_corrupt: int = 5) -> Tuple[bytes, Dict]:
        """Corrupt random page headers"""
        corrupted = bytearray(data)
        corrupted_pages = []
        
        total_pages = len(data) // self.page_size
        pages_to_corrupt = random.sample(range(total_pages), min(num_corrupt, total_pages))
        
        for page_no in pages_to_corrupt:
            offset = page_no * self.page_size
            
            if offset + 52 > len(data):
                continue
            
            # Corrupt specific fields
            corruption_type = random.choice([
                "page_type_only",
                "checksum_only", 
                "page_no_only",
                "chain_only",  # prev/next pointers
                "all_critical"
            ])
            
            if corruption_type in ["page_type_only", "all_critical"]:
                struct.pack_into('<H', corrupted, offset + 16, 0x9999)
            
            if corruption_type in ["checksum_only", "all_critical"]:
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xBEEFDEAD)
            
            if corruption_type in ["page_no_only", "all_critical"]:
                struct.pack_into('<I', corrupted, offset + 20, page_no ^ 0x55555555)
            
            if corruption_type in ["chain_only", "all_critical"]:
                struct.pack_into('<I', corrupted, offset + 0, 0xFFFFFFFF)
                struct.pack_into('<I', corrupted, offset + 4, 0xFFFFFFFF)
            
            corrupted_pages.append({
                "page_no": page_no,
                "corruption_type": corruption_type
            })
        
        result = {
            "scenario": "random_headers_corrupted",
            "total_pages": total_pages,
            "corrupted_pages": len(corrupted_pages),
            "corrupted_page_nos": [p["page_no"] for p in corrupted_pages]
        }
        
        self.corruption_log.append(result)
        return bytes(corrupted), result
    
    def corrupt_page_type_only(self, data: bytes, 
                                target_page_type: int = 0xFFFF) -> Tuple[bytes, Dict]:
        """Corrupt only the page type field"""
        corrupted = bytearray(data)
        
        # Corrupt all index pages
        for offset in range(0, len(data), self.page_size):
            if offset + 52 > len(data):
                break
            
            # Only corrupt pages that look like index pages
            page_type = struct.unpack_from('<H', data, offset + 16)[0]
            if page_type == FIL_PAGE_TYPE_INDEX:
                struct.pack_into('<H', corrupted, offset + 16, target_page_type)
        
        result = {
            "scenario": "page_type_corrupted",
            "page_type_target": hex(target_page_type),
            " corruption_detected": True
        }
        
        self.corruption_log.append(result)
        return bytes(corrupted), result


class SDICorruptionEngine:
    """Generate SDI (SQL Data Dictionary) corruption scenarios"""
    
    def __init__(self, page_size: int = 16384, seed: int = 42):
        self.page_size = page_size
        self.seed = seed
        random.seed(seed)
        self.sdi_pages: List[int] = []
    
    def find_sdi_pages(self, data: bytes) -> List[int]:
        """Find all SDI pages in the file"""
        sdi_pages = []
        
        for offset in range(0, len(data), self.page_size):
            if offset + 38 + 4 > len(data):
                break
            
            page_type = struct.unpack_from('<H', data, offset + 16)[0]
            
            if page_type == FIL_PAGE_TYPE_SDI:
                sdi_pages.append(offset // self.page_size)
        
        return sdi_pages
    
    def nuclear_sdi_strike(self, data: bytes) -> Tuple[bytes, Dict]:
        """
        SDI Nuclear Strike - Complete SDI page destruction
        Simulates MySQL 8.0+ SDI page loss scenario
        """
        corrupted = bytearray(data)
        sdi_pages = self.find_sdi_pages(data)
        original_sdi_data = {}
        
        result = {
            "scenario": "sdi_nuclear_strike",
            "sdi_pages_found": len(sdi_pages),
            "sdi_pages_erased": [],
            "metadata_loss": "complete"
        }
        
        for page_no in sdi_pages:
            offset = page_no * self.page_size
            
            # Save original SDI payload (excluding header)
            if offset + self.page_size <= len(data):
                original_payload = data[offset + 38:offset + self.page_size]
                original_sdi_data[page_no] = original_payload
                
                # ERASE SDI payload - zero everything after header
                corrupted[offset + 38:offset + self.page_size] = (
                    b'\x00' * (self.page_size - 38)
                )
                
                # Also corrupt the page type to make it undetectable
                struct.pack_into('<H', corrupted, offset + 16, 0x0000)
                
                result["sdi_pages_erased"].append(page_no)
        
        # If no SDI pages found, still create corruption scenario
        if len(sdi_pages) == 0:
            # Create a "fake" SDI page at position 3 (typical SDI location)
            if len(data) > 3 * self.page_size:
                offset = 3 * self.page_size
                
                # Convert to SDI page
                struct.pack_into('<H', corrupted, offset + 16, FIL_PAGE_TYPE_SDI)
                
                # Zero the payload
                corrupted[offset + 38:offset + self.page_size] = b'\x00' * (self.page_size - 38)
                
                result["sdi_pages_found"] = 1
                result["sdi_pages_erased"] = [3]
        
        return bytes(corrupted), result
    
    def partial_sdi_corruption(self, data: bytes) -> Tuple[bytes, Dict]:
        """Partially corrupt SDI page - some metadata recoverable"""
        corrupted = bytearray(data)
        sdi_pages = self.find_sdi_pages(data)
        
        result = {
            "scenario": "partial_sdi_corruption",
            "corruption_level": "partial",
            "recovery_possible": True
        }
        
        for page_no in sdi_pages:
            offset = page_no * self.page_size
            
            if offset + self.page_size <= len(data):
                # Corrupt part of SDI payload
                payload_start = offset + 38
                payload_end = offset + self.page_size
                
                # Zero middle portion of payload (simulate partial corruption)
                mid_start = payload_start + (payload_end - payload_start) // 3
                mid_end = payload_start + 2 * (payload_end - payload_start) // 3
                
                corrupted[mid_start:mid_end] = b'\x00' * (mid_end - mid_start)
        
        return bytes(corrupted), result
    
    def invert_sdi_checksums(self, data: bytes) -> Tuple[bytes, Dict]:
        """Invert SDI checksums to simulate CRC corruption"""
        corrupted = bytearray(data)
        sdi_pages = self.find_sdi_pages(data)
        
        result = {
            "scenario": "sdi_checksum_inverted",
            "checksum_error_count": len(sdi_pages),
            "recovery_possible": True
        }
        
        for page_no in sdi_pages:
            offset = page_no * self.page_size
            
            if offset + 32 <= len(data):
                # Invert checksum bytes
                for i in range(4):
                    corrupted[offset + 24 + i] ^= 0xFF
                    corrupted[offset + 28 + i] ^= 0xFF
        
        return bytes(corrupted), result


class HeaderRepairValidator:
    """Validate header repair functionality"""
    
    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.repair_log: List[Dict] = []
    
    def validate_header_repair(self, corrupted_data: bytes) -> Dict:
        """
        Validate if header repair can reconstruct page headers
        """
        repair_results = {
            "scenario": "header_repair_validation",
            "pages_scanned": 0,
            "headers_detected": [],
            "headers_repaired": 0,
            "headers_failed": 0,
            "repair_methods_used": []
        }
        
        total_pages = len(corrupted_data) // self.page_size
        
        for page_no in range(total_pages):
            offset = page_no * self.page_size
            
            if offset + 52 > len(corrupted_data):
                break
            
            repair_results["pages_scanned"] += 1
            
            # Try to detect header validity
            page_type = struct.unpack_from('<H', corrupted_data, offset + 16)[0]
            checksum = struct.unpack_from('<I', corrupted_data, offset + 24)[0]
            page_no_field = struct.unpack_from('<I', corrupted_data, offset + 20)[0]
            
            # Valid page type?
            valid_page_type = page_type in [
                FIL_PAGE_TYPE_INDEX,
                FIL_PAGE_TYPE_SDI,
                FIL_PAGE_TYPE_UNDO_LOG,
                FIL_PAGE_TYPE_BLOB
            ]
            
            # Checksum valid?
            checksum_match = checksum == struct.unpack_from('<I', corrupted_data, offset + 28)[0]
            
            # Page number matches offset?
            page_no_consistent = page_no_field == page_no
            
            if not valid_page_type:
                # Attempt repair via heuristic
                repair_methods = self._attempt_header_repair(corrupted_data, page_no, offset)
                repair_results["repair_methods_used"].extend(repair_methods)
                
                if repair_methods:
                    repair_results["headers_repaired"] += 1
                else:
                    repair_results["headers_failed"] += 1
            
            elif not checksum_match:
                repair_results["repair_methods_used"].append("checksum_rebuild")
                repair_results["headers_repaired"] += 1
            
            # Track detected headers for analysis
            repair_results["headers_detected"].append({
                "page_no": page_no,
                "original_page_type": page_type,
                "original_checksum": format(checksum, '08x'),
                "valid_page_type": valid_page_type,
                "valid_checksum": checksum_match
            })
        
        return repair_results
    
    def _attempt_header_repair(self, data: bytes, page_no: int, offset: int) -> List[str]:
        """
        Attempt to repair corrupted header using heuristics
        Returns list of repair methods attempted
        """
        methods = []
        data = bytearray(data)  # Convert to mutable

        # Method 1: Analyze record chain to infer page type
        if offset + 104 <= len(data):
            # Check for infimum/supremum record markers
            infimum_info = data[offset + 58]
            supremum_info = data[offset + 81]

            if infimum_info == REC_STATUS_INFIMUM and supremum_info == REC_STATUS_SUPREMUM:
                # This is definitely an index page
                methods.append("record_chain_analysis")
                # Correct page type
                struct.pack_into('<H', data, offset + 16, FIL_PAGE_TYPE_INDEX)

        # Method 2: Check page boundaries
        next_page_ptr = struct.unpack_from('<I', data, offset + 4)[0]
        prev_page_ptr = struct.unpack_from('<I', data, offset)[0]

        if next_page_ptr == 0xFFFFFFFF and prev_page_ptr == 0xFFFFFFFF:
            # First/last page pattern
            if page_no > 0:
                methods.append("boundary_analysis")

        # Method 3: Check for valid page header pattern
        if offset + 36 <= len(data):
            header_signature = data[offset:52]

            # Look for default values that indicate uninitialized header
            if header_signature[8:16] == b'\x00' * 8:  # LSN is zero
                methods.append("default_pattern_match")

        return methods
    
    def validate_sdi_recovery(self, corrupted_data: bytes) -> Dict:
        """
        Validate SDI recovery after nuclear strike
        """
        recovery = {
            "scenario": "sdi_recovery_validation",
            "sdi_pages_found": 0,
            "sdi_payload_intact": 0,
            "sdi_recoverable": False,
            "recovery_method": None
        }
        
        for offset in range(0, len(corrupted_data), self.page_size):
            if offset + 38 + 4 > len(corrupted_data):
                break
            
            # Try to find SDI pages
            page_type = struct.unpack_from('<H', corrupted_data, offset + 16)[0]
            
            if page_type == FIL_PAGE_TYPE_SDI:
                recovery["sdi_pages_found"] += 1
                
                # Check if payload is recoverable
                payload = corrupted_data[offset + 38:offset + self.page_size]
                
                # Check for JSON structure or DD markers
                has_json = b'{' in payload or b'"' in payload
                has_dd_marker = payload[:4] in [b'\x7b\x00\x00\x00', b'{']
                has_valid_prefix = payload[:2] in [b'{\x00', b'{\x01']
                
                if has_json or has_dd_marker or has_valid_prefix:
                    recovery["sdi_payload_intact"] += 1
                    recovery["recovery_method"] = "payload_scan"
                    recovery["sdi_recoverable"] = True
                elif payload == b'\x00' * (self.page_size - 38):
                    # Completely zeroed - heuristic recovery needed
                    recovery["recovery_method"] = "heuristic_pattern"
        
        return recovery


def generate_header_test_data(output_dir: str, num_pages: int = 100) -> Dict:
    """Generate test data for header repair testing"""
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    
    results = {
        "generated_at": datetime.now().isoformat(),
        "num_pages": num_pages,
        "page_size": 16384,
        "test_files": []
    }
    
    # Generate clean test file
    page_size = 16384
    file_data = bytearray(num_pages * page_size)
    
    for page_no in range(num_pages):
        offset = page_no * page_size
        
        # FIL header
        struct.pack_into('<I', file_data, offset + 0, page_no - 1 if page_no > 0 else 0xFFFFFFFF)
        struct.pack_into('<I', file_data, offset + 4, page_no + 1 if page_no < num_pages - 1 else 0xFFFFFFFF)
        struct.pack_into('<Q', file_data, offset + 8, page_no * page_size)
        struct.pack_into('<I', file_data, offset + 16, 0)  # space_id
        struct.pack_into('<I', file_data, offset + 20, page_no)
        struct.pack_into('<H', file_data, offset + 16, FIL_PAGE_TYPE_INDEX)
        struct.pack_into('<I', file_data, offset + 24, 0)  # checksum
        struct.pack_into('<I', file_data, offset + 28, 0)
        
        # Infimum record
        file_data[offset + 58] = REC_STATUS_INFIMUM
        struct.pack_into('<I', file_data, offset + 62, offset + 81)
        
        # Supremum record
        file_data[offset + 81] = REC_STATUS_SUPREMUM
        struct.pack_into('<I', file_data, offset + 85, 0)
    
    # Save clean file
    clean_path = output / "header_test_clean.ibd"
    with open(clean_path, 'wb') as f:
        f.write(bytes(file_data))
    
    results["test_files"].append({
        "filename": "header_test_clean.ibd",
        "corruption_level": "none"
    })
    
    # Generate corrupted versions
    engine = HeaderCorruptionEngine(page_size=page_size)
    
    # Scenario 1: All headers corrupted
    corrupted, _ = engine.corrupt_all_page_headers(bytes(file_data))
    all_corrupted_path = output / "header_test_all_corrupted.ibd"
    with open(all_corrupted_path, 'wb') as f:
        f.write(corrupted)
    results["test_files"].append({
        "filename": "header_test_all_corrupted.ibd",
        "corruption_level": "all_headers"
    })
    
    # Scenario 2: Random headers corrupted
    random_corrupted, _ = engine.corrupt_random_headers(bytes(file_data), num_corrupt=10)
    random_corrupted_path = output / "header_test_random_corrupted.ibd"
    with open(random_corrupted_path, 'wb') as f:
        f.write(random_corrupted)
    results["test_files"].append({
        "filename": "header_test_random_corrupted.ibd",
        "corruption_level": "random_headers"
    })
    
    # Scenario 3: Page type only corrupted
    type_corrupted, _ = engine.corrupt_page_type_only(bytes(file_data))
    type_corrupted_path = output / "header_test_type_corrupted.ibd"
    with open(type_corrupted_path, 'wb') as f:
        f.write(type_corrupted)
    results["test_files"].append({
        "filename": "header_test_type_corrupted.ibd",
        "corruption_level": "page_type_only"
    })
    
    # Generate SDI nuclear strike test data
    sdi_engine = SDICorruptionEngine(page_size=page_size)
    sdi_corrupted, _ = sdi_engine.nuclear_sdi_strike(bytes(file_data))
    sdi_corrupted_path = output / "header_test_sdi_nuclear.ibd"
    with open(sdi_corrupted_path, 'wb') as f:
        f.write(sdi_corrupted)
    results["test_files"].append({
        "filename": "header_test_sdi_nuclear.ibd",
        "corruption_level": "sdi_nuclear"
    })
    
    # Save metadata
    metadata_path = output / "header_test_metadata.json"
    with open(metadata_path, 'w') as f:
        json.dump(results, f, indent=2)
    
    print(f"[GEN] Generated {len(results['test_files'])} header test files to {output}")
    return results


def run_header_repair_tests(input_dir: str) -> Dict:
    """Run header repair validation tests"""
    input_path = Path(input_dir)
    
    results = {
        "run_timestamp": datetime.now().isoformat(),
        "test_results": []
    }
    
    # Find test files
    test_files = list(input_path.glob("header_test_*.ibd"))
    
    header_validator = HeaderRepairValidator()
    
    for test_file in test_files:
        with open(test_file, 'rb') as f:
            data = f.read()
        
        # Test header repair
        header_result = header_validator.validate_header_repair(data)
        
        # Test SDI recovery (if SDI-related test)
        sdi_result = {"scanned": False}
        if "sdi" in test_file.name.lower():
            sdi_result = header_validator.validate_sdi_recovery(data)
        
        results["test_results"].append({
            "test_file": test_file.name,
            "header_repair": header_result,
            "sdi_recovery": sdi_result
        })
    
    # Calculate summary
    total = len(results["test_results"])
    repairs_attempted = sum(r["header_repair"]["headers_repaired"] for r in results["test_results"])
    repairs_failed = sum(r["header_repair"]["headers_failed"] for r in results["test_results"])
    
    results["summary"] = {
        "total_tests": total,
        "repairs_attempted": repairs_attempted,
        "repairs_successful": repairs_attempted - repairs_failed,
        "overall_success_rate": (repairs_attempted - repairs_failed) / repairs_attempted * 100 if repairs_attempted > 0 else 0
    }
    
    return results


def main():
    parser = argparse.ArgumentParser(
        description="MySQL Recovery Pro - Header Repair & SDI Nuclear Chaos Suite",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Header Repair Test Scenarios:
  1. All Headers Corrupted - Corrupt all page headers
  2. Random Headers Corrupted - Selective corruption
  3. Page Type Only Corrupted - Specific header field test
  4. SDI Nuclear Strike - Complete SDI page destruction
  5. Partial SDI Corruption - Partial metadata loss

Usage Examples:
  python3 header_sdi_chaos.py --generate --output /tmp/header_tests
  python3 header_sdi_chaos.py --test --input /tmp/header_tests
        """
    )
    
    parser.add_argument('--generate', action='store_true',
                       help='Generate header/SDI test dataset')
    parser.add_argument('--test', action='store_true',
                       help='Run header/SDI chaos tests')
    parser.add_argument('--output', type=str, default='./header_sdi_test_output',
                       help='Output directory')
    parser.add_argument('--input', type=str, default=None,
                       help='Input directory with test files')
    parser.add_argument('--num-pages', type=int, default=100,
                       help='Number of pages to generate')
    
    args = parser.parse_args()
    
    if args.generate:
        result = generate_header_test_data(
            output_dir=args.output,
            num_pages=args.num_pages
        )
        print(json.dumps(result, indent=2))
    
    elif args.test:
        if not args.input:
            print("Error: --input required for testing")
            sys.exit(1)
        
        result = run_header_repair_tests(args.input)
        print(json.dumps(result, indent=2))
    
    else:
        print("Usage: python3 header_sdi_chaos.py [--generate|--test] [options]")
        sys.exit(1)


if __name__ == "__main__":
    main()
