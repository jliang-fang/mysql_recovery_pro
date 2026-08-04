#!/usr/bin/env python3
"""
Phase 5: Generate Test Data for Disaster Recovery Testing

Creates mock InnoDB test data files for all MySQL versions
(5.6, 5.7, 8.0, 8.4, 9.0) with known record counts, used
to validate disaster recovery end-to-end.
"""

import argparse
import json
import os
import struct
import sys


def make_fil_header(page_type: int = 0x45BF, page_no: int = 0) -> bytes:
    """Create a minimal InnoDB FIL header (38 bytes)."""
    checksum = 0x12345678
    lsn = 0x00000123456789
    return struct.pack(">II", checksum, page_no) + \
           b"\x00\x00\x00\x00\x00\x00\x00\x00" + \
           struct.pack(">Q", lsn) + \
           struct.pack(">H", page_type) + \
           b"\x00\x00\x00\x00\x00\x00\x00\x00" + \
           struct.pack(">I", 1)  # space_id


def make_page_header(n_recs: int = 0) -> bytes:
    """Create a minimal InnoDB page header (36 bytes for INDEX page)."""
    return struct.pack("<H", 256) + \  # heap_top
           b"\x00" * 10 + \
           struct.pack("<H", n_recs) + \  # PAGE_N_RECS
           b"\x00" * 18


def make_sdi_page(index_id: int = 1) -> bytes:
    """Create a minimal SDI page."""
    page = bytearray(16384)
    page[0:38] = make_fil_header(0x45BE, 0)
    # SDI header: index_id (8 bytes LE)
    struct.pack_into("<Q", page, 38, index_id)
    return bytes(page)


def make_ibd_data_page(page_no: int, records: list) -> bytes:
    """Create a minimal InnoDB data page with records."""
    page = bytearray(16384)
    page[0:38] = make_fil_header(0x45BF, page_no)
    page[38:74] = make_page_header(len(records))

    # Place records starting at offset 120
    offset = 120
    for rec in records:
        data = rec.encode("utf-8") if isinstance(rec, str) else rec
        if offset + len(data) + 4 > 16376:
            break
        # Record header: 2-byte length + 1-byte info + data
        struct.pack_into("<H", page, offset, len(data))
        page[offset + 2] = 0x00  # info bits
        page[offset + 3:offset + 3 + len(data)] = data
        offset += len(data) + 4

    # Page directory
    page[16382] = 0x02  # n_slots = 2
    page[16383] = 0x00
    page[16380] = 0x78  # SUPREMUM offset
    page[16381] = 0x00

    return bytes(page)


def make_ibd_file(version: str, num_rows: int, output_dir: str) -> str:
    """Create a mock .ibd file with the given number of rows."""
    filename = os.path.join(output_dir, f"test_mysql{version}.ibd")
    with open(filename, "wb") as f:
        # Space header (page 0)
        page0 = bytearray(16384)
        page0[0:38] = make_fil_header(0x45B0, 0)  # FSP_HDR
        f.write(page0)

        # SDI page (page 1)
        f.write(make_sdi_page(1))

        # Data pages
        rows_per_page = 5
        pages_needed = max(1, (num_rows + rows_per_page - 1) // rows_per_page)
        for page_no in range(2, 2 + pages_needed):
            start = (page_no - 2) * rows_per_page
            end = min(start + rows_per_page, num_rows)
            records = [f"row_{i}_data_{'A' * 20}" for i in range(start, end)]
            f.write(make_ibd_data_page(page_no, records))

        # Pad to at least 1MB
        remaining = max(0, 1048576 - os.path.getsize(filename))
        if remaining > 0:
            f.write(b"\x00" * remaining)

    return filename


def main():
    parser = argparse.ArgumentParser(
        description="Generate test data for disaster recovery matrix"
    )
    parser.add_argument("--output-dir", default="./phase5_results/test_data",
                        help="Output directory for test data")
    parser.add_argument("--num-rows", type=int, default=1000,
                        help="Number of rows per table")
    parser.add_argument("--versions", nargs="+",
                        default=["5.6", "5.7", "8.0", "8.4", "9.0"],
                        help="MySQL versions to generate data for")
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    for version in args.versions:
        clean_ver = version.replace(".", "")
        ibd_file = make_ibd_file(clean_ver, args.num_rows, args.output_dir)
        meta = {
            "version": version,
            "num_rows": args.num_rows,
            "ibd_file": ibd_file,
            "ibd_size": os.path.getsize(ibd_file),
        }
        meta_file = os.path.join(args.output_dir, f"meta_mysql{clean_ver}.json")
        with open(meta_file, "w") as f:
            json.dump(meta, f, indent=2)
        print(f"MySQL {version}: {ibd_file} ({meta['ibd_size']} bytes)")

    print(f"\nAll test data generated in {args.output_dir}")


if __name__ == "__main__":
    main()