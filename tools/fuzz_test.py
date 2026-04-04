#!/usr/bin/env python3
"""
Fuzz testing script for MySQL 8.0 Recovery Engine
Uses libFuzzer-style fuzzing for InnoDB page parsing
"""

import os
import sys
import subprocess
import tempfile
import shutil
import signal
import time

# Fuzzing timeout in seconds
FUZZ_TIMEOUT = 60

def create_fuzz_corpus():
    """Create initial corpus for fuzzing"""
    corpus_dir = tempfile.mkdtemp(prefix="mysql_recovery_corpus_")
    
    # Create sample InnoDB page data for fuzzing
    # Each file represents a different page type
    
    # 1. Valid B+Tree index page
    valid_index_page = bytearray(16384)
    # FIL Header (38 bytes)
    valid_index_page[0:4] = (0).to_bytes(4, 'little')  # prev_page
    valid_index_page[4:8] = (0).to_bytes(4, 'little')  # next_page
    valid_index_page[8:16] = (1000).to_bytes(8, 'little')  # LSN
    valid_index_page[16:20] = (0).to_bytes(4, 'little')  # space_id
    valid_index_page[20:24] = (0).to_bytes(4, 'little')  # page_no
    valid_index_page[24:28] = (0).to_bytes(4, 'little')  # checksum
    valid_index_page[28:32] = (0).to_bytes(4, 'little')  # checksum
    # Page type (FIL_PAGE_INDEX = 9969)
    valid_index_page[38:40] = (9969).to_bytes(2, 'little')
    # Page header
    valid_index_page[40:42] = (2).to_bytes(2, 'little')  # n_dir_slots
    valid_index_page[42:44] = (100).to_bytes(2, 'little')  # heap_top
    valid_index_page[44:46] = (3).to_bytes(2, 'little')  # n_heap
    valid_index_page[46:48] = (54).to_bytes(2, 'little')  # free_offset
    
    with open(os.path.join(corpus_dir, "valid_index_page.bin"), "wb") as f:
        f.write(valid_index_page)
    
    # 2. Valid SDI page
    valid_sdi_page = bytearray(16384)
    valid_sdi_page[38:40] = (9971).to_bytes(2, 'little')  # FIL_PAGE_SDI
    with open(os.path.join(corpus_dir, "valid_sdi_page.bin"), "wb") as f:
        f.write(valid_sdi_page)
    
    # 3. Valid Undo Log page
    valid_undo_page = bytearray(16384)
    valid_undo_page[38:40] = (9970).to_bytes(2, 'little')  # FIL_PAGE_UNDO_LOG
    with open(os.path.join(corpus_dir, "valid_undo_page.bin"), "wb") as f:
        f.write(valid_undo_page)
    
    # 4. Corrupted page (for fuzzing error handling)
    corrupted_page = bytearray(16384)
    corrupted_page[38:40] = (9969).to_bytes(2, 'little')  # Invalid checksum
    with open(os.path.join(corpus_dir, "corrupted_page.bin"), "wb") as f:
        f.write(corrupted_page)
    
    # 5. Truncated page
    truncated_page = bytearray(100)
    truncated_page[38:40] = (9969).to_bytes(2, 'little')
    with open(os.path.join(corpus_dir, "truncated_page.bin"), "wb") as f:
        f.write(truncated_page)
    
    # 6. BLOB pointer page
    blob_page = bytearray(16384)
    blob_page[38:40] = (9972).to_bytes(2, 'little')  # FIL_PAGE_TYPE_BLOB
    with open(os.path.join(corpus_dir, "blob_page.bin"), "wb") as f:
        f.write(blob_page)
    
    return corpus_dir

def run_fuzz_test(binary_path, corpus_dir, output_dir, timeout=FUZZ_TIMEOUT):
    """Run fuzz test on the specified binary"""
    print(f"[FUZZ] Starting fuzz test on {binary_path}")
    print(f"[FUZZ] Corpus directory: {corpus_dir}")
    print(f"[FUZZ] Output directory: {output_dir}")
    
    # Create output directory
    os.makedirs(output_dir, exist_ok=True)
    
    # Run libFuzzer
    cmd = [
        binary_path,
        corpus_dir,
        "-timeout=1",
        "-jobs=1",
        f"-output_dir={output_dir}"
    ]
    
    print(f"[FUZZ] Running: {' '.join(cmd)}")
    
    try:
        result = subprocess.run(
            cmd,
            timeout=timeout,
            capture_output=True,
            text=True
        )
        print(f"[FUZZ] Exit code: {result.returncode}")
        print(f"[FUZZ] Stdout: {result.stdout}")
        if result.stderr:
            print(f"[FUZZ] Stderr: {result.stderr}")
        return result.returncode == 0
    except subprocess.TimeoutExpired:
        print(f"[FUZZ] Fuzz test timed out after {timeout} seconds")
        return True  # Timeout is expected for fuzzing
    except Exception as e:
        print(f"[FUZZ] Error running fuzz test: {e}")
        return False

def main():
    """Main fuzz testing entry point"""
    print("=" * 60)
    print("MySQL 8.0 Recovery Engine - Fuzz Testing")
    print("=" * 60)
    
    # Check if we're in the build directory
    build_dir = os.path.join(os.path.dirname(__file__), "build")
    if not os.path.exists(build_dir):
        print(f"[ERROR] Build directory not found: {build_dir}")
        sys.exit(1)
    
    # Find the binary to fuzz
    binary_path = os.path.join(build_dir, "mysql_recovery_pro")
    if not os.path.exists(binary_path):
        print(f"[ERROR] Binary not found: {binary_path}")
        sys.exit(1)
    
    # Create corpus
    print("[FUZZ] Creating fuzz corpus...")
    corpus_dir = create_fuzz_corpus()
    print(f"[FUZZ] Corpus created at: {corpus_dir}")
    
    # Run fuzz test
    output_dir = os.path.join(build_dir, "fuzz_output")
    success = run_fuzz_test(binary_path, corpus_dir, output_dir)
    
    # Cleanup
    print(f"[FUZZ] Cleaning up...")
    shutil.rmtree(corpus_dir, ignore_errors=True)
    
    if success:
        print("[FUZZ] Fuzz test completed successfully")
        sys.exit(0)
    else:
        print("[FUZZ] Fuzz test failed")
        sys.exit(1)

if __name__ == "__main__":
    main()
