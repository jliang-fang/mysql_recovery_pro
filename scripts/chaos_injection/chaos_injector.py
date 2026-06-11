#!/usr/bin/env python3
# =============================================================================
# MySQL Recovery Pro - IBD Chaos Injector
# Author: jliang.fang (Global MySQL Kernel QA & Chaos Engineering)
# Purpose: Inject various corruption scenarios into IBD files for chaos testing
# =============================================================================

import os
import sys
import struct
import random
import argparse
import hashlib
from pathlib import Path
from typing import Optional, List, Tuple
from dataclasses import dataclass


# ==================== Constants ====================

PAGE_SIZE = 16384

# InnoDB Page Types
FIL_PAGE_TYPE_ALLOCATED = 0
FIL_PAGE_UNDO_LOG = 2
FIL_PAGE_INDEX = 9969  # 0x26f1
FIL_PAGE_TYPE_BLOB = 9972  # 0x26f4
FIL_PAGE_TYPE_ZBLOB = 9973  # 0x26f5
FIL_PAGE_TYPE_ZBLOB2 = 10470  # 0x28e6
FIL_PAGE_TYPE_UNKNOWN = 65535

# Page Type Offsets
FIL_PAGE_TYPE_ABS = 16  # Offset of page type in FIL header
FIL_PAGE_OFFSET_ABS = 4  # Offset of page number in FIL header
FIL_PAGE_PREV_ABS = 0  # Offset of previous page number
FIL_PAGE_NEXT_ABS = 4  # Offset of next page number
FIL_PAGE_LSN_ABS = 8  # Offset of LSN
FIL_PAGE_SPACE_ID_ABS = 16  # Offset of space_id
FIL_PAGE_CHECKSUM_ABS = 24  # Offset of checksum

# Page Header Offsets
FIL_PAGE_DATA_ABS = 38  # Start of page data

# Record offsets within page data
REC_NEXT_OFFSET = 0  # Next record offset
REC_HEAP_NO_OFFSET = 4  # Heap number
REC_INFO_BITS_OFFSET = 6  # Info bits
REC_N_OWNED_OFFSET = 7  # Number of owned records
REC_REC_STATUS_OFFSET = 8  # Record status
REC_NOrd_OFFSET = 9  # Number of fields
REC_BIG_REC_THRESHOLD = 127  # Big record threshold


@dataclass
class IBDFile:
    """Represents an IBD file with useful metadata"""
    path: str
    size: int
    page_count: int
    page_size: int
    file_hash: str


@dataclass
class ChaosResult:
    """Result of a chaos injection operation"""
    success: bool
    scenario: str
    pages_corrupted: List[int]
    output_file: str
    hash_input: str
    hash_output: str
    pages: List[int]


class IBDChaosInjector:
    """Chaos injector for IBD files"""

    def __init__(self, ibd_file: str, page_size: int = PAGE_SIZE):
        self.ibd_file = ibd_file
        self.page_size = page_size
        self.ibd = self._load_file()
        self.original_hash = self._calculate_hash()

    def _load_file(self) -> Optional[IBDFile]:
        """Load IBD file and compute metadata"""
        try:
            path = Path(self.ibd_file)
            if not path.exists():
                print(f"ERROR: File not found: {self.ibd_file}")
                return None

            size = path.stat().st_size
            page_count = size // self.page_size

            with open(self.ibd_file, 'rb') as f:
                data = f.read()

            return IBDFile(
                path=self.ibd_file,
                size=size,
                page_count=page_count,
                page_size=self.page_size,
                file_hash=hashlib.sha256(data).hexdigest()
            )
        except Exception as e:
            print(f"ERROR: Failed to load file: {e}")
            return None

    def _calculate_hash(self) -> str:
        """Calculate hash of original file"""
        if not self.ibd:
            return ""
        return self.ibd.file_hash

    def _read_page(self, page_num: int) -> Optional[bytes]:
        """Read a specific page from the file"""
        if not self.ibd:
            return None
        if page_num < 0 or page_num >= self.ibd.page_count:
            return None

        offset = page_num * self.page_size
        with open(self.ibd_file, 'rb') as f:
            f.seek(offset)
            return f.read(self.page_size)

    def _write_page(self, page_num: int, data: bytes) -> bool:
        """Write a page back to the file"""
        if not self.ibd:
            return False
        if page_num < 0 or page_num >= self.ibd.page_count:
            return False
        if len(data) != self.page_size:
            return False

        offset = page_num * self.page_size
        with open(self.ibd_file, 'rb+') as f:
            f.seek(offset)
            f.write(data)
        return True

    def _get_page_type(self, page_data: bytes) -> int:
        """Extract page type from page data"""
        if len(page_data) < 18:
            return FIL_PAGE_TYPE_UNKNOWN
        return struct.unpack('<H', page_data[FIL_PAGE_TYPE_ABS:FIL_PAGE_TYPE_ABS + 2])[0]

    def _set_page_type(self, page_data: bytearray, page_type: int) -> None:
        """Set page type in page data"""
        struct.pack_into('<H', page_data, FIL_PAGE_TYPE_ABS, page_type)

    def _get_page_number(self, page_data: bytes) -> int:
        """Extract page number from page data"""
        if len(page_data) < 24:
            return 0
        return struct.unpack('<I', page_data[FIL_PAGE_OFFSET_ABS:FIL_PAGE_OFFSET_ABS + 4])[0]

    def _get_next_page(self, page_data: bytes) -> int:
        """Get next page in chain"""
        if len(page_data) < 8:
            return 0xFFFFFFFF
        return struct.unpack('<I', page_data[FIL_PAGE_NEXT_ABS:FIL_PAGE_NEXT_ABS + 4])[0]

    def _get_prev_page(self, page_data: bytes) -> int:
        """Get previous page in chain"""
        if len(page_data) < 4:
            return 0xFFFFFFFF
        return struct.unpack('<I', page_data[FIL_PAGE_PREV_ABS:FIL_PAGE_PREV_ABS + 4])[0]

    # ==================== Chaos Injection Methods ====================

    def corrupt_page_header(self, page_num: int, corrupt_type: str = "all") -> bool:
        """
        Corrupt page header fields
        Types: all, page_type, checksum, offset, lsn, prev_next
        """
        if not self.ibd:
            return False

        page = bytearray(self._read_page(page_num) or b'\x00' * self.page_size)

        if corrupt_type in ("all", "page_type"):
            # Corrupt page type - write invalid values
            self._set_page_type(page, random.choice([
                0xFFFF,  # Invalid
                0x0000,  # Allocated
                0x0001,  # Unknown
                0x9999,  # Invalid
            ]))
            print(f"  [Page {page_num}] Corrupted page_type")

        if corrupt_type in ("all", "checksum"):
            # Corrupt checksum at offset 24
            struct.pack_into('<I', page, FIL_PAGE_CHECKSUM_ABS, 0xDEADBEEF)
            print(f"  [Page {page_num}] Corrupted checksum")

        if corrupt_type in ("all", "offset"):
            # Corrupt page number
            struct.pack_into('<I', page, FIL_PAGE_OFFSET_ABS, 0xFFFFFFFF)
            print(f"  [Page {page_num}] Corrupted page_offset")

        if corrupt_type in ("all", "lsn"):
            # Corrupt LSN
            struct.pack_into('<Q', page, FIL_PAGE_LSN_ABS, 0xDEADBEEFCAFEBABE)
            print(f"  [Page {page_num}] Corrupted LSN")

        if corrupt_type in ("all", "prev_next"):
            # Corrupt prev/next page pointers
            struct.pack_into('<I', page, FIL_PAGE_PREV_ABS, 0xFFFFFFFF)
            struct.pack_into('<I', page, FIL_PAGE_NEXT_ABS, 0xFFFFFFFF)
            print(f"  [Page {page_num}] Corrupted prev/next pointers")

        return self._write_page(page_num, bytes(page))

    def tear_page(self, page_num: int, tear_ratio: float = 0.5) -> bool:
        """Tear/corrupt a page by zeroing out part of it"""
        if not self.ibd:
            return False

        page = bytearray(self._read_page(page_num) or b'\x00' * self.page_size)
        tear_size = int(self.page_size * tear_ratio)
        start = random.randint(0, self.page_size - tear_size)
        page[start:start + tear_size] = b'\x00' * tear_size
        return self._write_page(page_num, bytes(page))

    def corrupt_page_trailer(self, page_num: int) -> bool:
        """Corrupt page trailer (last 8 bytes - FSLR)"""
        if not self.ibd:
            return False

        page = bytearray(self._read_page(page_num) or b'\x00' * self.page_size)
        # Corrupt the page trailer (last 8 bytes)
        page[-8:] = bytes([random.randint(0, 255) for _ in range(8)])
        return self._write_page(page_num, bytes(page))

    def corrupt_all_checksums(self) -> int:
        """Corrupt checksums on all pages"""
        if not self.ibd:
            return 0

        corrupted = 0
        for page_num in range(self.ibd.page_count):
            page = bytearray(self._read_page(page_num) or b'\x00' * self.page_size)
            struct.pack_into('<I', page, FIL_PAGE_CHECKSUM_ABS, 0xDEADBEEF)
            if self._write_page(page_num, bytes(page)):
                corrupted += 1
        return corrupted

    def corrupt_page_trailers(self) -> int:
        """Corrupt page trailers on all pages"""
        if not self.ibd:
            return 0

        corrupted = 0
        for page_num in range(self.ibd.page_count):
            page = bytearray(self._read_page(page_num) or b'\x00' * self.page_size)
            page[-8:] = bytes([random.randint(0, 255) for _ in range(8)])
            if self._write_page(page_num, bytes(page)):
                corrupted += 1
        return corrupted

    def corrupt_page_type_random(self, page_num: int) -> bool:
        """Set random page type on a page"""
        if not self.ibd:
            return False

        page = bytearray(self._read_page(page_num) or b'\x00' * self.page_size)
        valid_types = [
            FIL_PAGE_TYPE_ALLOCATED,
            FIL_PAGE_UNDO_LOG,
            FIL_PAGE_INDEX,
            FIL_PAGE_TYPE_BLOB,
            FIL_PAGE_TYPE_ZBLOB,
            FIL_PAGE_TYPE_ZBLOB2,
        ]
        # Add invalid types for chaos
        valid_types.extend([0xFFFF, 0x0000, 0x1111, 0x2222, 0x3333])
        self._set_page_type(page, random.choice(valid_types))
        return self._write_page(page_num, bytes(page))

    def corrupt_page_types_batch(self, num_pages: int = None) -> List[int]:
        """Corrupt page types on multiple pages"""
        if not self.ibd:
            return []

        pages = []
        page_count = num_pages if num_pages else self.ibd.page_count // 4
        page_count = min(page_count, self.ibd.page_count)

        for _ in range(page_count):
            page_num = random.randint(0, self.ibd.page_count - 1)
            if self.corrupt_page_type_random(page_num):
                pages.append(page_num)
        return pages

    def corrupt_sdi_page(self) -> bool:
        """Corrupt SDI page (typically page 3 in MySQL 8.0+)"""
        if not self.ibd:
            return False

        if self.ibd.page_count <= 3:
            print("  No SDI page (page 3) available")
            return False

        return self.corrupt_page_header(3, "all")

    def corrupt_blob_chain(self, blob_pages: List[int] = None, break_ratio: float = 0.5) -> List[int]:
        """Corrupt BLOB chain pointers"""
        if not self.ibd:
            return []

        pages = []

        # If no specific pages, find BLOB pages
        if not blob_pages:
            for page_num in range(self.ibd.page_count):
                page = self._read_page(page_num)
                if page and self._get_page_type(page) == FIL_PAGE_TYPE_BLOB:
                    blob_pages.append(page_num)

        for page_num in blob_pages:
            if random.random() < break_ratio:
                page = bytearray(self._read_page(page_num) or b'\x00' * self.page_size)
                # Corrupt BLOB chain pointers (next_bpage at offset 38)
                struct.pack_into('<I', page, 38, 0xFFFFFFFF)
                if self._write_page(page_num, bytes(page)):
                    pages.append(page_num)
                    print(f"  [Page {page_num}] Corrupted BLOB chain pointer")

        return pages

    def break_page_chain(self) -> int:
        """Break the B+Tree page chain"""
        if not self.ibd:
            return 0

        broken = 0
        for page_num in range(1, self.ibd.page_count):
            page = bytearray(self._read_page(page_num) or b'\x00' * self.page_size)
            # Break the chain by setting inconsistent prev/next pointers
            if page_num > 0:
                struct.pack_into('<I', page, FIL_PAGE_PREV_ABS, random.randint(0, page_num - 2) if page_num > 1 else 0xFFFFFFFF)
            if page_num < self.ibd.page_count - 1:
                struct.pack_into('<I', page, FIL_PAGE_NEXT_ABS, random.randint(page_num + 2, self.ibd.page_count - 1) if page_num < self.ibd.page_count - 2 else 0xFFFFFFFF)
            if self._write_page(page_num, bytes(page)):
                broken += 1
        return broken

    def corrupt_record_chain(self, page_num: int) -> bool:
        """Corrupt record chain on a page"""
        if not self.ibd:
            return False

        page = bytearray(self._read_page(page_num) or b'\x00' * self.page_size)
        # Corrupt the infimum and supremum record pointers
        page[54:62] = b'\xFF' * 8  # INFIMUM
        page[67:75] = b'\xFF' * 8  # SUPREMUM
        return self._write_page(page_num, bytes(page))

    def corrupt_directory(self, page_num: int) -> bool:
        """Corrupt page directory"""
        if not self.ibd:
            return False

        page = bytearray(self._read_page(page_num) or b'\x00' * self.page_size)
        # Corrupt directory slots at offset 76 (after page header)
        # Page directory format: [slot1, slot2, ...]
        if len(page) > 78:
            # Corrupt first few directory slots
            for i in range(4):
                struct.pack_into('<H', page, 76 + i * 2, random.randint(54, 8000))
        return self._write_page(page_num, bytes(page))

    def create_page_corruption_batch(self, num_pages: int, scenarios: List[str] = None) -> ChaosResult:
        """
        Create a batch of page corruptions and run all scenarios
        Returns a ChaosResult with hash information
        """
        if not self.ibd:
            return ChaosResult(
                success=False,
                scenario="batch_corruption",
                pages_corrupted=[],
                output_file=self.ibd_file,
                hash_input=self.original_hash,
                hash_output="",
                pages=[]
            )

        # Create a backup of original hash
        original_hash = self.original_hash

        # Default scenarios
        if not scenarios:
            scenarios = ["corrupt_header", "corrupt_checksum", "tear", "corrupt_type"]

        pages_corrupted = []

        for page_num in range(min(num_pages, self.ibd.page_count)):
            scenario = random.choice(scenarios)
            if scenario == "corrupt_header":
                self.corrupt_page_header(page_num, "all")
                pages_corrupted.append(page_num)
            elif scenario == "corrupt_checksum":
                self.corrupt_page_type_random(page_num)
                self.corrupt_page_trailer(page_num)
                pages_corrupted.append(page_num)
            elif scenario == "tear":
                self.tear_page(page_num, 0.3)
                pages_corrupted.append(page_num)
            elif scenario == "corrupt_type":
                self.corrupt_page_type_random(page_num)
                pages_corrupted.append(page_num)

        # Calculate output hash
        with open(self.ibd_file, 'rb') as f:
            output_hash = hashlib.sha256(f.read()).hexdigest()

        return ChaosResult(
            success=True,
            scenario="batch_corruption",
            pages_corrupted=pages_corrupted,
            output_file=self.ibd_file,
            hash_input=original_hash,
            hash_output=output_hash,
            pages=pages_corrupted
        )

    def corrupt_all_headers(self) -> int:
        """Corrupt headers on all pages"""
        if not self.ibd:
            return 0

        corrupted = 0
        for page_num in range(self.ibd.page_count):
            if self.corrupt_page_header(page_num, "all"):
                corrupted += 1
        return corrupted

    def corrupt_all_headers_random(self) -> int:
        """Corrupt headers randomly on all pages (50% of pages)"""
        if not self.ibd:
            return 0

        corrupted = 0
        for page_num in range(self.ibd.page_count):
            if random.random() < 0.5:
                if self.corrupt_page_header(page_num, "all"):
                    corrupted += 1
        return corrupted


def parse_scenarios(scenario_str: str) -> List[str]:
    """Parse scenario string into list of scenarios"""
    scenarios = [s.strip().lower() for s in scenario_str.split(',')]
    valid_scenarios = [
        "corrupt_header", "corrupt_checksum", "corrupt_type",
        "tear", "trailer", "chain", "blob", "sdi", "directory"
    ]
    return [s for s in scenarios if s in valid_scenarios]


def main():
    parser = argparse.ArgumentParser(
        description="MySQL Recovery Pro - IBD Chaos Injector",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Corrupt page header on page 0
  python3 scripts/chaos_injection/chaos_injector.py \\
      --ibd test_data/test.ibd --page-size 16384 \\
      --chaos corrupt-page-header --page-num 0

  # Tear a page (zero out 50%)
  python3 scripts/chaos_injection/chaos_injector.py \\
      --ibd test_data/test.ibd --page-size 16384 \\
      --chaos tear-page --page-num 5 --ratio 0.5

  # Corrupt all checksums
  python3 scripts/chaos_injection/chaos_injector.py \\
      --ibd test_data/test.ibd --page-size 16384 \\
      --chaos corrupt-all-checksums

  # Batch mode - process all IBD files in directory
  python3 scripts/chaos_injection/chaos_injector.py \\
      --ibd-dir /path/to/test_data --batch --chaos tear-page
        """
    )

    parser.add_argument("--ibd", type=str, help="Path to IBD file")
    parser.add_argument("--ibd-dir", type=str, help="Directory containing IBD files")
    parser.add_argument("--page-size", type=int, default=16384, help="Page size (default: 16384)")
    parser.add_argument("--chaos", type=str, required=True,
                        help="Chaos scenario: corrupt-page-header, tear-page, corrupt-trailer, "
                             "corrupt-all-checksums, corrupt-all-headers, corrupt-all-headers-random, "
                             "corrupt-all-trailers, corrupt-sdi, corrupt-blob, break-page-chain, "
                             "corrupt-record-chain, corrupt-directory, corrupt-type-random, "
                             "corrupt-type-batch")
    parser.add_argument("--page-num", type=int, default=None, help="Page number to corrupt")
    parser.add_argument("--num-pages", type=int, default=None, help="Number of pages to corrupt")
    parser.add_argument("--ratio", type=float, default=0.5, help="Tear/Corruption ratio (0.0-1.0)")
    parser.add_argument("--seed", type=int, default=42, help="Random seed")
    parser.add_argument("--batch", action="store_true", help="Batch process all IBD files in directory")
    parser.add_argument("--output-dir", type=str, help="Output directory for batch processing")
    parser.add_argument("--verbose", action="store_true", help="Enable verbose output")

    args = parser.parse_args()
    random.seed(args.seed)

    if not args.ibd and not args.ibd_dir:
        parser.error("Either --ibd or --ibd-dir must be specified")
        sys.exit(1)

    # Process files
    if args.batch or args.ibd_dir:
        # Batch mode
        if args.ibd_dir:
            ibd_files = list(Path(args.ibd_dir).glob("*.ibd"))
        else:
            ibd_files = [Path(args.ibd)]

        if not ibd_files:
            print("ERROR: No IBD files found")
            sys.exit(1)

        output_dir = Path(args.output_dir) if args.output_dir else Path.cwd()
        output_dir.mkdir(parents=True, exist_ok=True)

        for ibd_file in ibd_files:
            print(f"\nProcessing: {ibd_file.name}")
            injector = IBDChaosInjector(str(ibd_file), args.page_size)

            if args.chaos == "corrupt-page-header":
                injector.corrupt_page_header(args.page_num or 0, "all")
            elif args.chaos == "tear-page":
                injector.tear_page(args.page_num or 0, args.ratio)
            elif args.chaos == "corrupt-trailer":
                injector.corrupt_page_trailer(args.page_num or 0)
            elif args.chaos == "corrupt-all-checksums":
                injector.corrupt_all_checksums()
            elif args.chaos == "corrupt-all-headers":
                injector.corrupt_all_headers()
            elif args.chaos == "corrupt-all-headers-random":
                injector.corrupt_all_headers_random()
            elif args.chaos == "corrupt-all-trailers":
                injector.corrupt_page_trailers()
            elif args.chaos == "corrupt-sdi":
                injector.corrupt_sdi_page()
            elif args.chaos == "corrupt-type-random":
                injector.corrupt_page_type_random(args.page_num or 0)
            elif args.chaos == "corrupt-type-batch":
                injector.corrupt_page_types_batch(args.num_pages)
            elif args.chaos == "corrupt-blob":
                injector.corrupt_blob_chain(None, args.ratio)
            elif args.chaos == "break-page-chain":
                injector.break_page_chain()
            elif args.chaos == "corrupt-record-chain":
                injector.corrupt_record_chain(args.page_num or 0)
            elif args.chaos == "corrupt-directory":
                injector.corrupt_directory(args.page_num or 0)

            print(f"  Completed: {args.chaos}")

    else:
        # Single file mode
        injector = IBDChaosInjector(args.ibd, args.page_size)

        if not injector.ibd:
            print(f"ERROR: Failed to load IBD file: {args.ibd}")
            sys.exit(1)

        pages = []
        print(f"Processing: {args.ibd}")
        print(f"Pages: {injector.ibd.page_count}, Page Size: {injector.page_size}")

        if args.chaos == "corrupt-page-header":
            injector.corrupt_page_header(args.page_num, "all")
            pages = [args.page_num]
        elif args.chaos == "tear-page":
            injector.tear_page(args.page_num, args.ratio)
            pages = [args.page_num]
        elif args.chaos == "corrupt-trailer":
            injector.corrupt_page_trailer(args.page_num)
            pages = [args.page_num]
        elif args.chaos == "corrupt-all-checksums":
            count = injector.corrupt_all_checksums()
            print(f"Corrupted {count} checksums")
        elif args.chaos == "corrupt-all-headers":
            count = injector.corrupt_all_headers()
            print(f"Corrupted {count} page headers")
        elif args.chaos == "corrupt-all-headers-random":
            count = injector.corrupt_all_headers_random()
            print(f"Corrupted {count} page headers (random 50%)")
        elif args.chaos == "corrupt-all-trailers":
            count = injector.corrupt_page_trailers()
            print(f"Corrupted {count} page trailers")
        elif args.chaos == "corrupt-sdi":
            injector.corrupt_sdi_page()
            pages = [3]
        elif args.chaos == "corrupt-type-random":
            injector.corrupt_page_type_random(args.page_num)
            pages = [args.page_num]
        elif args.chaos == "corrupt-type-batch":
            pages = injector.corrupt_page_types_batch(args.num_pages)
        elif args.chaos == "corrupt-blob":
            pages = injector.corrupt_blob_chain(None, args.ratio)
        elif args.chaos == "break-page-chain":
            count = injector.break_page_chain()
            print(f"Broke {count} page chain links")
        elif args.chaos == "corrupt-record-chain":
            injector.corrupt_record_chain(args.page_num)
            pages = [args.page_num]
        elif args.chaos == "corrupt-directory":
            injector.corrupt_directory(args.page_num)
            pages = [args.page_num]
        else:
            print(f"ERROR: Unknown chaos scenario: {args.chaos}")
            sys.exit(1)

        # Print results
        with open(args.ibd, 'rb') as f:
            output_hash = hashlib.sha256(f.read()).hexdigest()

        print()
        print("=" * 60)
        print("CHAOS INJECTION RESULTS")
        print("=" * 60)
        print(f"SCENARIO: {args.chaos}")
        print(f"PAGES_CORRUPTED: {len(pages)}")
        print(f"PAGE_NUMBERS: {','.join(map(str, pages))}")
        print(f"HASH_INPUT: {injector.original_hash}")
        print(f"HASH_OUTPUT: {output_hash}")
        print("=" * 60)


if __name__ == "__main__":
    main()
