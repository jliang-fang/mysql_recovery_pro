#!/usr/bin/env python3
"""
Fuzz testing script for MySQL 8.0 Recovery Engine
Supports libFuzzer and AFL fuzzing engines
"""

import subprocess
import os
import sys
import tempfile
import shutil
from pathlib import Path

# Project root
PROJECT_ROOT = Path(__file__).parent.parent

# Fuzz targets
FUZZ_TARGETS = {
    "page_parser": "tools/fuzz_page_parser.cpp",
    "blob_pointer": "tools/fuzz_blob_pointer.cpp",
    "sdi_json": "tools/fuzz_sdi_json.cpp",
}

# Build flags for libFuzzer
FUZZ_FLAGS = [
    "-fsanitize=fuzzer,address",
    "-Iinclude",
    "-std=c++20",
    "-O1",
]

# Build flags for AFL
AFL_FLAGS = [
    "-Iinclude",
    "-std=c++20",
    "-O1",
]


def build_fuzz_target(target_name: str, engine: str = "libfuzzer") -> Path:
    """Build a fuzz target with the specified engine"""
    source_file = PROJECT_ROOT / FUZZ_TARGETS[target_name]
    output_dir = PROJECT_ROOT / "build" / "fuzz"
    output_dir.mkdir(parents=True, exist_ok=True)
    
    if engine == "libfuzzer":
        output_file = output_dir / f"fuzz_{target_name}"
        flags = FUZZ_FLAGS
    elif engine == "afl":
        output_file = output_dir / f"fuzz_{target_name}_afl"
        flags = AFL_FLAGS
    else:
        raise ValueError(f"Unknown engine: {engine}")
    
    # Check if source file exists
    if not source_file.exists():
        print(f"Error: Source file not found: {source_file}")
        return None
    
    # Build command
    cmd = ["clang++"] + flags + [str(source_file), "-o", str(output_file)]
    
    print(f"Building {target_name} with {engine}...")
    print(f"Command: {' '.join(cmd)}")
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=str(PROJECT_ROOT))
        if result.returncode != 0:
            print(f"Build failed: {result.stderr}")
            return None
        print(f"Built successfully: {output_file}")
        return output_file
    except FileNotFoundError:
        print(f"Error: clang++ not found. Please install clang++ with libFuzzer support.")
        return None


def run_fuzz_target(target_path: Path, corpus_dir: Path = None, timeout: int = 60):
    """Run a fuzz target with optional corpus directory"""
    if not target_path.exists():
        print(f"Error: Target not found: {target_path}")
        return False
    
    cmd = [str(target_path)]
    if corpus_dir:
        cmd.append(str(corpus_dir))
    
    print(f"Running fuzz target: {target_path}")
    print(f"Command: {' '.join(cmd)}")
    print(f"Timeout: {timeout} seconds")
    
    try:
        result = subprocess.run(cmd, timeout=timeout, capture_output=True, text=True)
        print(f"Exit code: {result.returncode}")
        if result.stdout:
            print(f"Output: {result.stdout}")
        if result.stderr:
            print(f"Errors: {result.stderr}")
        return result.returncode == 0
    except subprocess.TimeoutExpired:
        print("Fuzzing timed out (expected for long-running fuzz jobs)")
        return True
    except Exception as e:
        print(f"Error running fuzz target: {e}")
        return False


def create_corpus_dir(corpus_name: str) -> Path:
    """Create a corpus directory with sample inputs"""
    corpus_dir = PROJECT_ROOT / "build" / "fuzz" / "corpora" / corpus_name
    corpus_dir.mkdir(parents=True, exist_ok=True)
    
    # Create sample inputs for each target
    if corpus_name == "page_parser":
        # Sample valid page header
        sample = bytearray(16384)  # Standard 16KB page
        sample[50:52] = (9969).to_bytes(2, 'little')  # FIL_PAGE_INDEX
        sample[38:40] = (16).to_bytes(2, 'little')    # n_dir_slots
        sample[40:42] = (100).to_bytes(2, 'little')   # heap_top
        sample[42:44] = (2).to_bytes(2, 'little')     # n_heap
        with open(corpus_dir / "valid_page.bin", "wb") as f:
            f.write(sample)
    
    elif corpus_name == "blob_pointer":
        # Sample valid BLOB pointer (20 bytes)
        sample = bytearray(20)
        sample[0:4] = (0).to_bytes(4, 'little')       # space_id
        sample[4:8] = (100).to_bytes(4, 'little')     # page_no
        sample[8:12] = (0).to_bytes(4, 'little')      # offset
        sample[12:16] = (50).to_bytes(4, 'little')    # len
        sample[16:20] = (50).to_bytes(4, 'little')    # total_len
        with open(corpus_dir / "valid_blob_ptr.bin", "wb") as f:
            f.write(sample)
    
    elif corpus_name == "sdi_json":
        # Sample SDI header (20 bytes) + minimal JSON
        sample = bytearray(50)
        sample[0:4] = (1).to_bytes(4, 'little')       # type
        sample[4:12] = (12345).to_bytes(8, 'little')  # id
        sample[12:16] = (100).to_bytes(4, 'little')   # uncompressed_len
        sample[16:20] = (10).to_bytes(4, 'little')    # compressed_len
        # Add minimal zlib-compressed JSON
        import zlib
        json_data = b'{"name":"test"}'
        compressed = zlib.compress(json_data, 1)
        sample[20:20+len(compressed)] = compressed
        with open(corpus_dir / "valid_sdi.bin", "wb") as f:
            f.write(sample)
    
    return corpus_dir


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 fuzz_test.py <command> [args]")
        print("")
        print("Commands:")
        print("  build <target> [libfuzzer|afl]  - Build a fuzz target")
        print("  run <target> [corpus]           - Run a fuzz target")
        print("  corpus <name>                   - Create sample corpus")
        print("  all                             - Build all fuzz targets")
        print("")
        print("Targets:")
        for name in FUZZ_TARGETS:
            print(f"  - {name}")
        sys.exit(1)
    
    command = sys.argv[1]
    
    if command == "build":
        if len(sys.argv) < 3:
            print("Error: Target name required")
            sys.exit(1)
        target = sys.argv[2]
        engine = sys.argv[3] if len(sys.argv) > 3 else "libfuzzer"
        
        if target not in FUZZ_TARGETS:
            print(f"Error: Unknown target '{target}'")
            sys.exit(1)
        
        result = build_fuzz_target(target, engine)
        if result:
            print(f"Build successful: {result}")
        else:
            print("Build failed")
            sys.exit(1)
    
    elif command == "run":
        if len(sys.argv) < 3:
            print("Error: Target name required")
            sys.exit(1)
        target = sys.argv[2]
        corpus = sys.argv[3] if len(sys.argv) > 3 else None
        
        if target not in FUZZ_TARGETS:
            print(f"Error: Unknown target '{target}'")
            sys.exit(1)
        
        # Build first if not already built
        output_dir = PROJECT_ROOT / "build" / "fuzz"
        target_path = output_dir / f"fuzz_{target}"
        
        if not target_path.exists():
            print("Target not built, building now...")
            result = build_fuzz_target(target)
            if not result:
                print("Build failed")
                sys.exit(1)
        
        # Create corpus if specified
        corpus_dir = None
        if corpus:
            corpus_dir = create_corpus_dir(corpus)
        
        run_fuzz_target(target_path, corpus_dir)
    
    elif command == "corpus":
        if len(sys.argv) < 3:
            print("Error: Corpus name required")
            sys.exit(1)
        corpus_name = sys.argv[2]
        corpus_dir = create_corpus_dir(corpus_name)
        print(f"Created corpus directory: {corpus_dir}")
        print(f"Files:")
        for f in corpus_dir.glob("*"):
            print(f"  - {f.name} ({f.stat().st_size} bytes)")
    
    elif command == "all":
        print("Building all fuzz targets...")
        for target in FUZZ_TARGETS:
            result = build_fuzz_target(target)
            if result:
                print(f"✓ {target}")
            else:
                print(f"✗ {target}")
    
    else:
        print(f"Error: Unknown command '{command}'")
        sys.exit(1)


if __name__ == "__main__":
    main()
