#!/usr/bin/env bash
# extract_sdi_python_test.sh — Contract-parity tests for tools/extract_sdi.py
#
# Phase B2: verifies the Python SDI extractor fails closed on adversarial
# samples (over-ratio / truncated stream / trailing data / bad JSON), and
# accepts a valid SDI page. All pages are synthesized by the embedded Python.
set -euo pipefail

TOOL="/app/mysql_recovery_pro/tools/extract_sdi.py"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

python3 - "$WORK" "$TOOL" <<'ENDGEN'
import json, os, struct, subprocess, sys, zlib

work = sys.argv[1]
tool = sys.argv[2]

def build_ibd(entries):
    """Build multi-page .ibd from (filename, sdi_payload_bytes) entries."""
    for filename, sdi_payload in entries:
        page = bytearray(16384)
        struct.pack_into('<I', page, 0, 0x4573)  # FIL_PAGE_SDI
        page[38:38 + len(sdi_payload)] = sdi_payload
        with open(os.path.join(work, filename), 'wb') as f:
            f.write(page)

def raw_deflate(data):
    """Raw DEFLATE (no zlib wrapper) — matches C++ SDIManager contract."""
    co = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
    return co.compress(data) + co.flush()

def sdi_header_bytes(type_, id_, uncompressed_len, compressed_len, raw_data):
    """Build SDI header (20 bytes) + compressed data."""
    hdr = struct.pack('<I', type_)
    hdr += struct.pack('<Q', id_)
    hdr += struct.pack('<I', uncompressed_len)
    hdr += struct.pack('<I', compressed_len)
    return hdr + raw_data

# 1. Valid SDI
good_json = json.dumps({"mysqld_version_id": 80040, "dd_object_type": "Table", "dd_object_id": 1234}).encode()
good_comp = raw_deflate(good_json)
valid_payload = sdi_header_bytes(1, 1234, len(good_json), len(good_comp), good_comp)
build_ibd([("valid.ibd", valid_payload)])

# 2. Over-ratio: highly repetitive -> ratio >> 20
big = b"A" * 8000
big_comp = raw_deflate(big)
assert len(big) // len(big_comp) > 20, "ratio must exceed 20"
over_payload = sdi_header_bytes(1, 1235, len(big), len(big_comp), big_comp)
build_ibd([("overratio.ibd", over_payload)])

# 3. Truncated stream: half the compressed data (no zlib eof)
mid = good_comp[:len(good_comp)//2]
trunc_payload = sdi_header_bytes(1, 1236, len(good_json), len(mid), mid)
build_ibd([("truncated.ibd", trunc_payload)])

# 4. Trailing data: valid deflate + garbage after
tail = good_comp + b"\xde\xad\xbe\xef"
trail_payload = sdi_header_bytes(1, 1237, len(good_json), len(tail), tail)
build_ibd([("trailing.ibd", trail_payload)])

# 5. Bad JSON: valid UTF-8, invalid JSON
bad_json = b'{"this is": not json at all'
bad_comp = raw_deflate(bad_json)
bad_payload = sdi_header_bytes(1, 1238, len(bad_json), len(bad_comp), bad_comp)
build_ibd([("badjson.ibd", bad_payload)])

# 6. Non-SDI page (no FIL_PAGE_SDI marker)
empty_page = bytearray(16384)
with open(os.path.join(work, "nosdi.ibd"), 'wb') as f:
    f.write(empty_page)

# Now run the tests
fail_count = 0
pass_count = 0

def run_case(name, expect_ok):
    global fail_count, pass_count
    outpath = os.path.join(work, name + ".out")
    errpath = os.path.join(work, name + ".err")
    ibdpath = os.path.join(work, name + ".ibd")
    with open(outpath, 'w') as out, open(errpath, 'w') as err:
        rc = subprocess.call([tool, ibdpath], stdout=out, stderr=err)
    if rc == 0:
        if expect_ok:
            pass_count += 1; print(f"PASS: {name} accepted")
        else:
            fail_count += 1; print(f"FAIL: {name} must be rejected (exit 0)")
            with open(errpath) as f: print("    " + f.read().strip())
    else:
        if expect_ok:
            fail_count += 1; print(f"FAIL: {name} must be accepted")
            with open(errpath) as f: print("    " + f.read().strip())
        else:
            pass_count += 1; print(f"PASS: {name} rejected")

run_case("valid", True)
run_case("overratio", False)
run_case("truncated", False)
run_case("trailing", False)
run_case("badjson", False)
run_case("nosdi", False)

# Sanity: adversarial samples must not leak SDI payload content
for name in ["overratio", "truncated", "trailing", "badjson", "nosdi"]:
    with open(os.path.join(work, name + ".out")) as f:
        content = f.read()
    if "dd_object_id" in content:
        fail_count += 1
        print(f"FAIL: {name} leaked SDI payload")
    else:
        pass_count += 1
        print(f"PASS: {name} no SDI leak")

print(f"\nResults: {pass_count} passed, {fail_count} failed")
sys.exit(1 if fail_count else 0)
ENDGEN