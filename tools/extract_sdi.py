#!/usr/bin/env python3
"""Extract and decompress SDI from .ibd file (MySQL 8.0+)

Aligns with C++ SDIManager::extract_standard_payload contract:
- Header validation (type==1, id!=0, both lengths non-zero)
- 20:1 max compression ratio
- Stream integrity (no trailing data, complete decompression)
- Strict UTF-8 / valid JSON required
"""
import struct
import zlib
import sys
import json

SDI_PAGE_TYPE = 0x4573  # FIL_PAGE_SDI in MySQL 8.0
MAX_SDI_SIZE = 16 * 1024 * 1024  # SDI_MAX_UNCOMPRESSED_SIZE
MAX_COMPRESSION_RATIO = 20       # SDI_MAX_COMPRESSION_RATIO


def read_le_uint32(data, offset):
    return struct.unpack('<I', data[offset:offset + 4])[0]


def read_le_uint64(data, offset):
    return struct.unpack('<Q', data[offset:offset + 8])[0]


def extract_sdi_from_page(page_data):
    """Extract SDI payload from a single page with full C++ contract parity."""
    if len(page_data) < 38:
        return None

    # Check page type
    page_type = read_le_uint32(page_data, 0x00)
    if page_type != SDI_PAGE_TYPE:
        return None

    # SDI payload starts at offset 38 (after FIL header + FSP header)
    sdi_start = 38
    if len(page_data) < sdi_start + 20:  # min header size
        return None

    # Read SDI header
    type_ = read_le_uint32(page_data, sdi_start)
    id_ = read_le_uint64(page_data, sdi_start + 4)
    uncompressed_len = read_le_uint32(page_data, sdi_start + 12)
    compressed_len = read_le_uint32(page_data, sdi_start + 16)

    # --- C++ contract checks ---

    # Header validation: type==1, id!=0, both lengths non-zero
    if type_ != 1 or id_ == 0 or uncompressed_len == 0 or compressed_len == 0:
        return None

    # Uncompressed size limit
    if uncompressed_len > MAX_SDI_SIZE:
        return None

    # 20:1 max compression ratio
    if uncompressed_len // compressed_len > MAX_COMPRESSION_RATIO:
        return None

    # Compressed data must fit within declared payload
    payload_len = len(page_data) - sdi_start - 20
    if compressed_len > payload_len:
        return None

    raw_data = page_data[sdi_start + 20: sdi_start + 20 + compressed_len]

    # Decompress with stream integrity check (raw deflate, -15 window bits)
    # Use decompressobj to check for trailing data (Z_STREAM_END + unused_data)
    try:
        decompressor = zlib.decompressobj(-zlib.MAX_WBITS)
        decompressed = decompressor.decompress(raw_data)
        # Check for complete stream (Z_STREAM_END reached)
        if decompressor.eof is False:
            raise ValueError("incomplete compressed stream (no end-of-stream marker)")
        # Check for trailing data after the compressed stream
        if decompressor.unused_data:
            raise ValueError(f"trailing data after compressed stream ({len(decompressor.unused_data)} bytes)")
        if len(decompressed) != uncompressed_len:
            raise ValueError(
                f"decompressed length mismatch: got {len(decompressed)}, expected {uncompressed_len}"
            )
    except Exception as e:
        print(f"[!] Decompression failed at page offset {sdi_start}: {e}", file=sys.stderr)
        return None

    # Strict UTF-8 decoding (no errors='replace' — must be valid)
    try:
        text = decompressed.decode('utf-8', errors='strict')
    except UnicodeDecodeError as e:
        print(f"[!] Invalid UTF-8 in SDI payload at offset {sdi_start}: {e}", file=sys.stderr)
        return None

    # Valid JSON required
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError as e:
        print(f"[!] Invalid JSON in SDI page at offset {sdi_start}: {e}", file=sys.stderr)
        return None

    return parsed


def extract_all_sdi(ibd_path):
    """Scan .ibd for all SDI pages and extract JSON."""
    PAGE_SIZE = 16384  # Default; adjust if needed (4K/8K/32K/64K)

    with open(ibd_path, 'rb') as f:
        data = f.read()

    results = []
    for offset in range(0, len(data), PAGE_SIZE):
        page = data[offset: offset + PAGE_SIZE]
        if len(page) < 38:
            continue

        sdi_json = extract_sdi_from_page(page)
        if sdi_json:
            results.append(sdi_json)

    return results


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python extract_sdi.py <file.ibd>", file=sys.stderr)
        sys.exit(1)

    ibd_path = sys.argv[1]
    sdis = extract_all_sdi(ibd_path)

    if not sdis:
        print(f"[!] No SDI found in {ibd_path}", file=sys.stderr)
        sys.exit(1)
    else:
        for i, sdi in enumerate(sdis):
            print(f"=== SDI #{i + 1} ===")
            print(json.dumps(sdi, indent=2))