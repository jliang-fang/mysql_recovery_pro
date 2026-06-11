#!/usr/bin/env python3
# =============================================================================
# MySQL 5.6 Corruption Scenarios Suite
# Author: jliang.fang (Global MySQL Kernel QA Expert)
# Purpose: Simulate various corruption scenarios for MySQL 5.6 recovery testing
#          This suite tests the mysql_recovery_pro tool's ability to recover
#          from different types of data corruption
# =============================================================================

import os
import sys
import struct
import random
import argparse
import hashlib
import shutil
from pathlib import Path
from typing import List, Dict, Tuple, Optional
from dataclasses import dataclass
from datetime import datetime

# MySQL InnoDB constants
FIL_PAGE_TYPE_INDEX = 0x45BC  # MySQL 5.6 B+Tree
FIL_PAGE_TYPE_SDI = 0x26F3    # MySQL 8.0 SDI
FIL_PAGE_TYPE_UNDO_LOG = 0x26F2
FIL_PAGE_TYPE_BLOB = 0x26F4
FIL_PAGE_TYPE_ALLOCATED = 0

# Page offsets
FIL_PAGE_PREV = 0
FIL_PAGE_NEXT = 4
FIL_PAGE_LSN = 8
FIL_PAGE_SPACE_ID = 16
FIL_PAGE_OFFSET = 20
FIL_PAGE_CHECKSUM = 24
FIL_PAGE_TYPE = 16

FILE_PAGE_DATA = 38


@dataclass
class CorruptionResult:
    """Result of a corruption injection operation"""
    success: bool
    scenario: str
    pages_corrupted: List[int]
    corruption_type: str
    output_file: str
    hash_before: str
    hash_after: str
    records_affected: int


class MySQL56CorruptionSuite:
    """MySQL 5.6 corruption scenarios suite for testing recovery"""

    def __init__(self, ibd_file: str, page_size: int = 16384, seed: int = 42):
        self.ibd_file = ibd_file
        self.page_size = page_size
        self.seed = seed
        self.original_hash = ""
        self.corruption_log: List[Dict] = []
        self._load_file()

    def _load_file(self) -> bool:
        """Load IBD file and compute hash"""
        try:
            if not os.path.exists(self.ibd_file):
                print(f"ERROR: File not found: {self.ibd_file}")
                return False

            with open(self.ibd_file, 'rb') as f:
                self.file_data = bytearray(f.read())

            self.size = len(self.file_data)
            self.page_count = self.size // self.page_size
            self.original_hash = hashlib.sha256(self.file_data).hexdigest()

            print(f"Loaded {self.ibd_file}")
            print(f"  Size: {self.size:,} bytes ({self.size / (1024*1024):.2f} MB)")
            print(f"  Pages: {self.page_count:,}")
            print(f"  Page size: {self.page_size}")
            print(f"  Hash: {self.original_hash[:32]}...")

            return True
        except Exception as e:
            print(f"ERROR loading file: {e}")
            return False

    def _calculate_page_checksum(self, page_data: bytes) -> int:
        """Calculate CRC32 checksum for page"""
        import binascii
        return binascii.crc32(page_data) & 0xffffffff

    def _fix_checksum(self, page: bytearray) -> None:
        """Recalculate and fix page checksum"""
        page_no = struct.unpack_from('<I', page, FIL_PAGE_OFFSET)[0]
        lsn = struct.unpack_from('<Q', page, FIL_PAGE_LSN)[0]

        # Calculate checksum over page without checksum fields
        checksum_area = page[:FIL_PAGE_CHECKSUM]
        data_area = page[FIL_PAGE_CHECKSUM + 8:]  # Skip checksum + double write
        combined = checksum_area + data_area

        checksum = self._calculate_page_checksum(combined)
        struct.pack_into('<I', page, FIL_PAGE_CHECKSUM, checksum)
        struct.pack_into('<I', page, FIL_PAGE_CHECKSUM + 4, checksum)

    def _get_page_type(self, page: bytes) -> int:
        """Extract page type from page data"""
        return struct.unpack_from('<H', page, FIL_PAGE_TYPE)[0]

    def set_page_type(self, page: bytearray, page_type: int) -> None:
        """Set page type in page data"""
        struct.pack_into('<H', page, FIL_PAGE_TYPE, page_type)

    def corruption_torn_pages(self, num_pages: int = 100, 
                             output_file: str = None) -> CorruptionResult:
        """Simulate torn pages (incomplete writes due to power loss)
        
        This simulates the common scenario where a write is interrupted,
        leaving the page header partially written with garbage.
        """
        print(f"\n=== Corruption: Torn Pages (Power Loss Simulation) ===")
        print(f"Corrupting {num_pages} page headers...")

        random.seed(self.seed)
        pages_to_corrupt = random.sample(range(self.page_count), min(num_pages, self.page_count))

        for page_num in pages_to_corrupt:
            offset = page_num * self.page_size
            page = self.file_data[offset:offset + self.page_size]

            # Corrupt the first 20-40 bytes (FIL header area)
            corrupt_start = FIL_PAGE_PREV  # Start of header
            corrupt_end = min(FIL_PAGE_TYPE + 10, self.page_size)

            # Overwrite with random data
            for i in range(corrupt_start, corrupt_end):
                page[i] = random.randint(0, 255)

            # Fix checksum to make page appear valid
            self._fix_checksum(page)
            self.file_data[offset:offset + self.page_size] = page

        output_path = output_file or f"{self.ibd_file}.torn"
        self._write_file(output_path)

        return CorruptionResult(
            success=True,
            scenario="torn_pages",
            pages_corrupted=pages_to_corrupt,
            corruption_type="header_corruption",
            output_file=output_path,
            hash_before=self.original_hash,
            hash_after=hashlib.sha256(self.file_data).hexdigest(),
            records_affected=len(pages_to_corrupt) * (self.page_size // 128)
        )

    def corruption_fragmented_file(self, num_fragments: int = 100,
                                   fragment_size: int = 1024,
                                   output_file: str = None) -> CorruptionResult:
        """Simulate rm -rf style fragmentation
        
        This simulates files that have been partially deleted or corrupted,
        leaving scattered fragments of data.
        """
        print(f"\n=== Corruption: Fragmented File (rm -rf Simulation) ===")
        print(f"Creating {num_fragments} fragments of {fragment_size} bytes each...")

        random.seed(self.seed + 1)
        output_path = output_file or f"{self.ibd_file}.fragmented"

        # Read all fragments
        fragments = []
        for i in range(num_fragments):
            if i * fragment_size >= self.size:
                break
            offset = i * fragment_size
            length = min(fragment_size, self.size - offset)
            data = bytes(self.file_data[offset:offset + length])
            fragments.append((offset, data))

        # Shuffle fragments
        random.shuffle(fragments)

        # Create fragmented file with gaps
        new_size = max(f[0] + len(f[1]) for f in fragments) + 1024 * 1024
        new_data = bytearray(new_size)

        # Fill with recognizable pattern first
        for i in range(0, len(new_data), 4096):
            struct.pack_into('<I', new_data, i, 0xDEADBEEF)

        # Place fragments
        for offset, data in fragments:
            new_data[offset:offset + len(data)] = data

        with open(output_path, 'wb') as f:
            f.write(new_data)

        return CorruptionResult(
            success=True,
            scenario="fragmented_file",
            pages_corrupted=list(range(len(fragments))),
            corruption_type="file_fragmentation",
            output_file=output_path,
            hash_before=self.original_hash,
            hash_after=hashlib.sha256(new_data).hexdigest(),
            records_affected=len(fragments)
        )

    def corruption_blob_chain_break(self, num_breaks: int = 10,
                                    output_file: str = None) -> CorruptionResult:
        """Simulate BLOB chain breakage
        
        This simulates corruption where BLOB data spans multiple pages
        and some intermediate pages are corrupted.
        """
        print(f"\n=== Corruption: BLOB Chain Break ===")
        print(f"Breaking BLOB chain at {num_breaks} locations...")

        random.seed(self.seed + 2)
        output_path = output_file or f"{self.ibd_file}.blob_broken"

        # Simulate BLOB pages by corrupting page type and chain links
        # In MySQL 5.6, BLOB pages are linked via page chain
        pages_to_corrupt = random.sample(range(1, self.page_count - 1), num_breaks)

        for page_num in pages_to_corrupt:
            offset = page_num * self.page_size
            page = bytearray(self.file_data[offset:offset + self.page_size])

            # Corrupt page type to make it unrecognizable
            self.set_page_type(page, random.randint(0, 65535))

            # Corrupt chain pointers (prev/next page in chain)
            struct.pack_into('<I', page, FIL_PAGE_PREV, random.randint(0, 0xFFFFFFFF))
            struct.pack_into('<I', page, FIL_PAGE_NEXT, random.randint(0, 0xFFFFFFFF))

            # Update checksum
            self._fix_checksum(page)
            self.file_data[offset:offset + self.page_size] = page

        with open(output_path, 'wb') as f:
            f.write(self.file_data)

        return CorruptionResult(
            success=True,
            scenario="blob_chain_break",
            pages_corrupted=pages_to_corrupt,
            corruption_type="blob_chain",
            output_file=output_path,
            hash_before=self.original_hash,
            hash_after=hashlib.sha256(self.file_data).hexdigest(),
            records_affected=num_breaks
        )

    def corruption_btree_nonleaf(self, num_nodes: int = 50,
                                 output_file: str = None) -> CorruptionResult:
        """Simulate B+Tree non-leaf node corruption
        
        This simulates corruption in B+Tree index pages where
        the node pointers are damaged.
        """
        print(f"\n=== Corruption: B+Tree Non-leaf Node Corruption ===")
        print(f"Corrupting {num_nodes} B+Tree nodes...")

        random.seed(self.seed + 3)
        output_path = output_file or f"{self.ibd_file}.btree_broken"

        # Simulate index pages by corrupting page type and pointers
        pages_to_corrupt = random.sample(range(1, self.page_count - 1), min(num_nodes, self.page_count - 2))

        for page_num in pages_to_corrupt:
            offset = page_num * self.page_size
            page = bytearray(self.file_data[offset:offset + self.page_size])

            # Change page type to something invalid
            self.set_page_type(page, random.choice([0x45BD, 0x45BE, 0x1234, 0xFFFF]))

            # Corrupt BTree header fields
            # PAGE_BTR_SEG_LEAF (offset 56)
            struct.pack_into('<I', page, 56, random.randint(0, 0xFFFFFFFF))

            # Update checksum
            self._fix_checksum(page)
            self.file_data[offset:offset + self.page_size] = page

        with open(output_path, 'wb') as f:
            f.write(self.file_data)

        return CorruptionResult(
            success=True,
            scenario="btree_nonleaf",
            pages_corrupted=pages_to_corrupt,
            corruption_type="btree_index",
            output_file=output_path,
            hash_before=self.original_hash,
            hash_after=hashlib.sha256(self.file_data).hexdigest(),
            records_affected=num_nodes
        )

    def corruption_page_mismatch(self, num_mismatches: int = 100,
                                 output_file: str = None) -> CorruptionResult:
        """Simulate page number mismatches
        
        This simulates scenario where page numbers don't match their
        physical location in the file.
        """
        print(f"\n=== Corruption: Page Number Mismatch ===")
        print(f"Corrupting page numbers on {num_mismatches} pages...")

        random.seed(self.seed + 4)
        output_path = output_file or f"{self.ibd_file}.mismatch"

        pages_to_corrupt = random.sample(range(self.page_count), num_mismatches)

        for page_num in pages_to_corrupt:
            offset = page_num * self.page_size
            page = self.file_data[offset:offset + self.page_size]

            # Write wrong page number
            wrong_page = random.randint(0, self.page_count * 2)
            struct.pack_into('<I', page, FIL_PAGE_OFFSET, wrong_page)

            # Fix checksum
            self._fix_checksum(page)
            self.file_data[offset:offset + self.page_size] = page

        with open(output_path, 'wb') as f:
            f.write(self.file_data)

        return CorruptionResult(
            success=True,
            scenario="page_mismatch",
            pages_corrupted=pages_to_corrupt,
            corruption_type="page_number",
            output_file=output_path,
            hash_before=self.original_hash,
            hash_after=hashlib.sha256(self.file_data).hexdigest(),
            records_affected=num_mismatches
        )

    def corruption_checksum(self, num_pages: int = 100,
                           output_file: str = None) -> CorruptionResult:
        """Simulate checksum corruption
        
        This simulates data corruption where the checksum doesn't match
        the data (but page structure is otherwise intact).
        """
        print(f"\n=== Corruption: Checksum Mismatch ===")
        print(f"Corrupting checksums on {num_pages} pages...")

        random.seed(self.seed + 5)
        output_path = output_file or f"{self.ibd_file}.checksum"

        pages_to_corrupt = random.sample(range(self.page_count), num_pages)

        for page_num in pages_to_corrupt:
            offset = page_num * self.page_size
            page = self.file_data[offset:offset + self.page_size]

            # Corrupt checksum with wrong value
            struct.pack_into('<I', page, FIL_PAGE_CHECKSUM, random.randint(0, 0xFFFFFFFF))
            struct.pack_into('<I', page, FIL_PAGE_CHECKSUM + 4, random.randint(0, 0xFFFFFFFF))

            self.file_data[offset:offset + self.page_size] = page

        with open(output_path, 'wb') as f:
            f.write(self.file_data)

        return CorruptionResult(
            success=True,
            scenario="checksum_mismatch",
            pages_corrupted=pages_to_corrupt,
            corruption_type="checksum",
            output_file=output_path,
            hash_before=self.original_hash,
            hash_after=hashlib.sha256(self.file_data).hexdigest(),
            records_affected=num_pages
        )

    def corruption_random_bytes(self, num_corruptions: int = 1000,
                                output_file: str = None) -> CorruptionResult:
        """Simulate random byte corruption
        
        This is the most aggressive corruption scenario, randomly
        overwriting bytes throughout the file.
        """
        print(f"\n=== Corruption: Random Byte Corruption ===")
        print(f"Corrupting {num_corruptions} random bytes...")

        random.seed(self.seed + 6)
        output_path = output_file or f"{self.ibd_file}.random"

        # Corrupt random bytes throughout the file
        bytes_to_corrupt = random.sample(range(self.size), min(num_corruptions, self.size))

        for byte_pos in bytes_to_corrupt:
            self.file_data[byte_pos] = random.randint(0, 255)

        with open(output_path, 'wb') as f:
            f.write(self.file_data)

        return CorruptionResult(
            success=True,
            scenario="random_bytes",
            pages_corrupted=[],
            corruption_type="random",
            output_file=output_path,
            hash_before=self.original_hash,
            hash_after=hashlib.sha256(self.file_data).hexdigest(),
            records_affected=num_corruptions
        )

    def corruption_truncate(self, truncate_percent: float = 0.1,
                           output_file: str = None) -> CorruptionResult:
        """Simulate partial file truncation
        
        This simulates what happens when a file is truncated,
        like after a TRUNCATE TABLE command.
        """
        print(f"\n=== Corruption: Truncation ===")
        print(f"Truncating file to {truncate_percent * 100:.1f}% of original size...")

        output_path = output_file or f"{self.ibd_file}.truncated"

        new_size = int(self.size * (1 - truncate_percent))
        truncated_data = self.file_data[:new_size]

        with open(output_path, 'wb') as f:
            f.write(truncated_data)

        return CorruptionResult(
            success=True,
            scenario="truncation",
            pages_corrupted=[],
            corruption_type="truncation",
            output_file=output_path,
            hash_before=self.original_hash,
            hash_after=hashlib.sha256(truncated_data).hexdigest(),
            records_affected=int(self.page_count * truncate_percent)
        )

    def corruption_dd_overwrite(self, dd_count: int = 10,
                               dd_size: int = 40,
                               output_file: str = None) -> CorruptionResult:
        """Simulate dd command style corruption
        
        This uses specific dd-style overwrites to corrupt specific
        areas of the page header.
        """
        print(f"\n=== Corruption: dd-style Overwrite ===")
        print(f"Performing {dd_count} dd-style overwrites of {dd_size} bytes each...")

        random.seed(self.seed + 7)
        output_path = output_file or f"{self.ibd_file}.dd_corrupt"

        pages_to_corrupt = random.sample(range(self.page_count), dd_count)

        for page_num in pages_to_corrupt:
            offset = page_num * self.page_size
            # Simulate dd bs=1 count=40 seek=xxx
            for i in range(dd_size):
                self.file_data[offset + i] = 0xFF

        with open(output_path, 'wb') as f:
            f.write(self.file_data)

        return CorruptionResult(
            success=True,
            scenario="dd_overwrite",
            pages_corrupted=pages_to_corrupt,
            corruption_type="dd_style",
            output_file=output_path,
            hash_before=self.original_hash,
            hash_after=hashlib.sha256(self.file_data).hexdigest(),
            records_affected=dd_count
        )

    def _write_file(self, output_path: str) -> None:
        """Write corrupted data to file"""
        with open(output_path, 'wb') as f:
            f.write(self.file_data)
        print(f"  Output: {output_path}")

    def audit_corruption(self, output_dir: str = "./corruption_audit") -> None:
        """Generate a corruption audit report"""
        os.makedirs(output_dir, exist_ok=True)
        audit_file = os.path.join(output_dir, "corruption_audit.json")
        
        audit_data = {
            "original_file": self.ibd_file,
            "original_hash": self.original_hash,
            "page_size": self.page_size,
            "page_count": self.page_count,
            "corruptions": self.corruption_log
        }

        with open(audit_file, 'w') as f:
            json.dump(audit_data, f, indent=2)

        print(f"\nCorruption audit written to: {audit_file}")


def run_all_corruption_scenarios(ibd_file: str, output_dir: str = "./corruption_output"):
    """Run all corruption scenarios on a given IBD file"""
    print("=" * 70)
    print("MySQL 5.6 Corruption Scenarios Suite")
    print("=" * 70)
    print(f"Input file: {ibd_file}")
    print(f"Output directory: {output_dir}")
    print()

    os.makedirs(output_dir, exist_ok=True)

    # Create corruption suite
    corruptor = MySQL56CorruptionSuite(ibd_file)

    # Copy original file to output directory
    original_copy = os.path.join(output_dir, "original.ibd")
    shutil.copy2(ibd_file, original_copy)

    scenarios = []

    # Run all scenarios
    results = []

    # 1. Torn pages
    result = corruptor.corruption_torn_pages(num_pages=100)
    results.append(result)
    scenarios.append(result.output_file)
    print(f"  ✓ Torn pages: {len(result.pages_corrupted)} pages")

    # 2. Fragmented file
    result = corruptor.corruption_fragmented_file(num_fragments=100)
    results.append(result)
    scenarios.append(result.output_file)
    print(f"  ✓ Fragmented: {len(result.pages_corrupted)} fragments")

    # 3. BLOB chain break
    result = corruptor.corruption_blob_chain_break(num_breaks=50)
    results.append(result)
    scenarios.append(result.output_file)
    print(f"  ✓ BLOB chain: {len(result.pages_corrupted)} breaks")

    # 4. B+Tree corruption
    result = corruptor.corruption_btree_nonleaf(num_nodes=50)
    results.append(result)
    scenarios.append(result.output_file)
    print(f"  ✓ B+Tree: {len(result.pages_corrupted)} nodes")

    # 5. Page mismatch
    result = corruptor.corruption_page_mismatch(num_mismatches=100)
    results.append(result)
    scenarios.append(result.output_file)
    print(f"  ✓ Page mismatch: {len(result.pages_corrupted)} pages")

    # 6. Checksum corruption
    result = corruptor.corruption_checksum(num_pages=100)
    results.append(result)
    scenarios.append(result.output_file)
    print(f"  ✓ Checksum: {len(result.pages_corrupted)} pages")

    # 7. Random bytes
    result = corruptor.corruption_random_bytes(num_corruptions=5000)
    results.append(result)
    scenarios.append(result.output_file)
    print(f"  ✓ Random bytes: {result.records_affected} bytes")

    # 8. Truncation
    result = corruptor.corruption_truncate(truncate_percent=0.1)
    results.append(result)
    scenarios.append(result.output_file)
    print(f"  ✓ Truncation: {result.records_affected} pages affected")

    # 9. dd-style overwrite
    result = corruptor.corruption_dd_overwrite(dd_count=10, dd_size=40)
    results.append(result)
    scenarios.append(result.output_file)
    print(f"  ✓ dd overwrite: {len(result.pages_corrupted)} pages")

    # Print summary
    print()
    print("=" * 70)
    print("Corruption Scenarios Summary")
    print("=" * 70)
    print(f"Original file: {original_copy}")
    print(f"Original hash: {results[0].hash_before[:32]}...")
    print()
    print("Corrupted files created:")
    for i, result in enumerate(results, 1):
        print(f"  {i}. {os.path.basename(result.output_file)}")
        print(f"     Type: {result.corruption_type}")
        print(f"     Affected: {result.records_affected:,} records")
    print()
    print("Generated scenarios can betested with mysql_recovery_pro using:")
    print("  --header-repair    for torn pages, checksum corruption")
    print("  --rmrf-recovery    for fragmented files")
    print("  --blob-recovery    for BLOB chain breaks")
    print("  --rebuild-chain    for B+Tree corruption")
    print("  --truncate-recovery for truncated files")
    print()


def main():
    parser = argparse.ArgumentParser(
        description='MySQL 5.6 Corruption Scenarios Suite'
    )
    parser.add_argument('ibd_file', help='Input IBD file to corrupt')
    parser.add_argument('--output-dir', default='./corruption_output',
                       help='Output directory for corrupted files')
    parser.add_argument('--scenario', choices=[
        'torn', 'fragmented', 'blob', 'btree', 'mismatch', 
        'checksum', 'random', 'truncate', 'dd', 'all'
    ], default='all', help='Specific scenario to run')
    parser.add_argument('--num-pages', type=int, default=100,
                       help='Number of pages to corrupt')
    parser.add_argument('--seed', type=int, default=42,
                       help='Random seed for reproducibility')

    args = parser.parse_args()

    if args.scenario == 'all':
        run_all_corruption_scenarios(args.ibd_file, args.output_dir)
    else:
        corruptor = MySQL56CorruptionSuite(args.ibd_file, seed=args.seed)

        if args.scenario == 'torn':
            result = corruptor.corruption_torn_pages(args.num_pages)
        elif args.scenario == 'fragmented':
            result = corruptor.corruption_fragmented_file(args.num_pages)
        elif args.scenario == 'blob':
            result = corruptor.corruption_blob_chain_break(args.num_pages // 10)
        elif args.scenario == 'btree':
            result = corruptor.corruption_btree_nonleaf(args.num_pages // 2)
        elif args.scenario == 'mismatch':
            result = corruptor.corruption_page_mismatch(args.num_pages)
        elif args.scenario == 'checksum':
            result = corruptor.corruption_checksum(args.num_pages)
        elif args.scenario == 'random':
            result = corruptor.corruption_random_bytes(args.num_pages * 10)
        elif args.scenario == 'truncate':
            result = corruptor.corruption_truncate(truncate_percent=0.1)
        elif args.scenario == 'dd':
            result = corruptor.corruption_dd_overwrite(args.num_pages // 10, 40)

        print(f"\nCorruption complete: {result.output_file}")


if __name__ == '__main__':
    main()
