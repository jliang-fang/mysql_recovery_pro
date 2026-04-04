#!/usr/bin/env python3
"""
Extreme Scenario Data Corruption Recovery Test Suite

This script tests the MySQL Recovery Pro tool with various extreme corruption scenarios:
1. Complete file truncation
2. Multiple page corruption
3. Header corruption
4. Checksum corruption
5. Page chain breaks
6. Record chain corruption
7. BLOB chain corruption
8. Mixed corruption scenarios
9. Empty/zero-filled files
10. Random data files
"""

import os
import sys
import struct
import random
import json
import subprocess
import tempfile
import shutil
from pathlib import Path

# Test constants
PAGE_SIZE = 16384
TEST_DIR = Path(__file__).parent
TOOL_PATH = Path(__file__).parent.parent / "build" / "mysql_recovery_pro"

class CorruptionTest:
    """Base class for corruption tests"""
    def __init__(self, name, description):
        self.name = name
        self.description = description
        self.test_file = None
        self.result = None
    
    def setup(self, temp_dir):
        """Create test file"""
        raise NotImplementedError
    
    def run_recovery(self, output_dir):
        """Run recovery on test file"""
        if not self.test_file or not self.test_file.exists():
            return {"error": "Test file not created"}
        
        cmd = [
            str(TOOL_PATH),
            f"--ibd={self.test_file}",
            f"--output={output_dir}",
            "--verbose"
        ]
        
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            return {
                "stdout": result.stdout,
                "stderr": result.stderr,
                "returncode": result.returncode
            }
        except subprocess.TimeoutExpired:
            return {"error": "Recovery timed out"}
        except Exception as e:
            return {"error": str(e)}
    
    def analyze_result(self, result):
        """Analyze recovery result for warnings/errors"""
        output = result.get("stdout", "") + result.get("stderr", "")
        
        warnings = []
        errors = []
        
        for line in output.split('\n'):
            line = line.strip()
            if '[WARN]' in line:
                warnings.append(line)
            elif '[ERROR]' in line:
                errors.append(line)
        
        return {
            "warnings": warnings,
            "errors": errors,
            "output_length": len(output)
        }
    
    def execute(self):
        """Execute full test"""
        temp_dir = tempfile.mkdtemp(prefix="extreme_test_")
        output_dir = os.path.join(temp_dir, "output")
        
        try:
            # Setup
            self.setup(temp_dir)
            
            # Run recovery
            result = self.run_recovery(output_dir)
            
            # Analyze
            analysis = self.analyze_result(result)
            
            self.result = {
                "test_name": self.name,
                "description": self.description,
                "test_file": str(self.test_file) if self.test_file else None,
                "file_size": self.test_file.stat().st_size if self.test_file else 0,
                "recovery_result": result,
                "analysis": analysis
            }
            
            return self.result
            
        finally:
            # Cleanup
            shutil.rmtree(temp_dir, ignore_errors=True)


class TruncationTest(CorruptionTest):
    """Test: Complete file truncation"""
    def __init__(self):
        super().__init__(
            "truncation",
            "Test recovery of completely truncated file"
        )
    
    def setup(self, temp_dir):
        # Create a valid IBD file first
        self.test_file = Path(temp_dir) / "truncated.ibd"
        
        # Write minimal valid header
        with open(self.test_file, 'wb') as f:
            # FIL Header (38 bytes)
            f.write(struct.pack('<I', 0xFFFFFFFF))  # prev_page
            f.write(struct.pack('<I', 0xFFFFFFFF))  # next_page
            f.write(struct.pack('<Q', 1))  # LSN
            f.write(struct.pack('<H', 9969))  # page_type (FIL_PAGE_INDEX)
            f.write(struct.pack('<I', 0))  # space_id
            f.write(struct.pack('<H', 0))  # page_no
            
            # Page Header (16 bytes minimum)
            f.write(struct.pack('<H', 2))  # n_dir_slots
            f.write(struct.pack('<H', 54 + 13 + 8))  # heap_top
            f.write(struct.pack('<H', 2 | 0x8000))  # n_heap
            f.write(struct.pack('<H', 54))  # free_offset
            
            # Truncate to very small size
            f.truncate(100)


class HeaderCorruptionTest(CorruptionTest):
    """Test: Header corruption"""
    def __init__(self):
        super().__init__(
            "header_corruption",
            "Test recovery with corrupted page header"
        )
    
    def setup(self, temp_dir):
        self.test_file = Path(temp_dir) / "header_corrupt.ibd"
        
        with open(self.test_file, 'wb') as f:
            # Write 16KB page
            f.write(b'\xFF' * PAGE_SIZE)
            
            # Corrupt page type at offset 16
            f.seek(16)
            f.write(struct.pack('<H', 0xFFFF))  # Invalid page type


class ChecksumMismatchTest(CorruptionTest):
    """Test: Checksum mismatch"""
    def __init__(self):
        super().__init__(
            "checksum_mismatch",
            "Test recovery with checksum mismatch"
        )
    
    def setup(self, temp_dir):
        self.test_file = Path(temp_dir) / "checksum_mismatch.ibd"
        
        with open(self.test_file, 'wb') as f:
            # Write valid page structure
            f.write(b'\x00' * PAGE_SIZE)
            
            # Set valid page type
            f.seek(16)
            f.write(struct.pack('<H', 9969))  # FIL_PAGE_INDEX
            
            # Corrupt checksum at offset 24
            f.seek(24)
            f.write(struct.pack('<I', 0xDEADBEEF))  # Invalid checksum


class PageChainBrokenTest(CorruptionTest):
    """Test: Page chain broken"""
    def __init__(self):
        super().__init__(
            "page_chain_broken",
            "Test recovery with broken page chain"
        )
    
    def setup(self, temp_dir):
        self.test_file = Path(temp_dir) / "chain_broken.ibd"
        
        with open(self.test_file, 'wb') as f:
            # Write multiple pages
            for _ in range(5):
                page = bytearray(PAGE_SIZE)
                
                # FIL Header
                struct.pack_into('<I', page, 0, 0xFFFFFFFF)  # prev_page
                struct.pack_into('<I', page, 4, 0xFFFFFFFF)  # next_page (broken)
                struct.pack_into('<Q', page, 8, 1)  # LSN
                struct.pack_into('<H', page, 16, 9969)  # page_type
                struct.pack_into('<I', page, 16, 0)  # space_id
                struct.pack_into('<H', page, 22, 0)  # page_no
                
                # Page Header
                struct.pack_into('<H', page, 38, 2)  # n_dir_slots
                struct.pack_into('<H', page, 40, 54 + 13 + 8)  # heap_top
                struct.pack_into('<H', page, 42, 2 | 0x8000)  # n_heap
                struct.pack_into('<H', page, 44, 54)  # free_offset
                
                f.write(page)


class RecordChainCorruptionTest(CorruptionTest):
    """Test: Record chain corruption"""
    def __init__(self):
        super().__init__(
            "record_chain_corruption",
            "Test recovery with corrupted record chain"
        )
    
    def setup(self, temp_dir):
        self.test_file = Path(temp_dir) / "record_chain_corrupt.ibd"
        
        with open(self.test_file, 'wb') as f:
            page = bytearray(PAGE_SIZE)
            
            # FIL Header
            struct.pack_into('<I', page, 0, 0xFFFFFFFF)
            struct.pack_into('<I', page, 4, 0xFFFFFFFF)
            struct.pack_into('<Q', page, 8, 1)
            struct.pack_into('<H', page, 16, 9969)
            struct.pack_into('<I', page, 16, 0)
            struct.pack_into('<H', page, 22, 0)
            
            # Page Header
            struct.pack_into('<H', page, 38, 2)
            struct.pack_into('<H', page, 40, 75)
            struct.pack_into('<H', page, 42, 2 | 0x8000)
            struct.pack_into('<H', page, 44, 54)
            
            # INFIMUM record (corrupted)
            page[54] = 0x05  # Invalid record type
            struct.pack_into('<H', page, 55, 67)  # next_offset
            
            # SUPREMUM record
            page[67] = 0x06  # Invalid record type
            struct.pack_into('<H', page, 68, 0xFFFF)
            
            # Page directory
            struct.pack_into('<H', page, 75, 54)
            struct.pack_into('<H', page, 77, 67)
            
            f.write(page)


class BLOBChainCorruptionTest(CorruptionTest):
    """Test: BLOB chain corruption"""
    def __init__(self):
        super().__init__(
            "blob_chain_corruption",
            "Test recovery with corrupted BLOB chain"
        )
    
    def setup(self, temp_dir):
        self.test_file = Path(temp_dir) / "blob_chain_corrupt.ibd"
        
        with open(self.test_file, 'wb') as f:
            # Write BLOB page
            page = bytearray(PAGE_SIZE)
            
            # FIL Header
            struct.pack_into('<I', page, 0, 0xFFFFFFFF)
            struct.pack_into('<I', page, 4, 0xFFFFFFFF)
            struct.pack_into('<Q', page, 8, 1)
            struct.pack_into('<H', page, 16, 9972)  # FIL_PAGE_TYPE_BLOB
            struct.pack_into('<I', page, 16, 0)
            struct.pack_into('<H', page, 22, 0)
            
            # BLOB Header (at offset 38)
            struct.pack_into('<I', page, 38, 100)  # next_bpage
            struct.pack_into('<I', page, 42, 0)  # prev_bpage
            struct.pack_into('<I', page, 46, 1000)  # data_len
            
            f.write(page)


class MixedCorruptionTest(CorruptionTest):
    """Test: Mixed corruption scenarios"""
    def __init__(self):
        super().__init__(
            "mixed_corruption",
            "Test recovery with multiple corruption types"
        )
    
    def setup(self, temp_dir):
        self.test_file = Path(temp_dir) / "mixed_corrupt.ibd"
        
        with open(self.test_file, 'wb') as f:
            # Write 10 pages with various corruptions
            for i in range(10):
                page = bytearray(PAGE_SIZE)
                
                # FIL Header
                struct.pack_into('<I', page, 0, 0xFFFFFFFF)
                struct.pack_into('<I', page, 4, 0xFFFFFFFF)
                struct.pack_into('<Q', page, 8, 1)
                struct.pack_into('<H', page, 16, 9969)
                struct.pack_into('<I', page, 16, 0)
                struct.pack_into('<H', page, 22, i)
                
                # Page Header
                struct.pack_into('<H', page, 38, 2)
                struct.pack_into('<H', page, 40, 75)
                struct.pack_into('<H', page, 42, 2 | 0x8000)
                struct.pack_into('<H', page, 44, 54)
                
                # Corrupt some pages
                if i == 0:
                    struct.pack_into('<H', page, 16, 0xFFFF)  # Invalid page type
                elif i == 1:
                    struct.pack_into('<I', page, 24, 0xDEADBEEF)  # Bad checksum
                elif i == 5:
                    page[54] = 0x05  # Corrupted INFIMUM
                
                f.write(page)


class EmptyFileTest(CorruptionTest):
    """Test: Empty/zero-filled file"""
    def __init__(self):
        super().__init__(
            "empty_file",
            "Test recovery of empty/zero-filled file"
        )
    
    def setup(self, temp_dir):
        self.test_file = Path(temp_dir) / "empty.ibd"
        
        with open(self.test_file, 'wb') as f:
            f.write(b'\x00' * PAGE_SIZE)


class RandomDataTest(CorruptionTest):
    """Test: Random data file"""
    def __init__(self):
        super().__init__(
            "random_data",
            "Test recovery of random data file"
        )
    
    def setup(self, temp_dir):
        self.test_file = Path(temp_dir) / "random.ibd"
        
        with open(self.test_file, 'wb') as f:
            random.seed(42)
            f.write(bytes(random.randint(0, 255) for _ in range(PAGE_SIZE * 2)))


class CircularReferenceTest(CorruptionTest):
    """Test: Circular reference in page chain"""
    def __init__(self):
        super().__init__(
            "circular_reference",
            "Test recovery with circular page chain reference"
        )
    
    def setup(self, temp_dir):
        self.test_file = Path(temp_dir) / "circular.ibd"
        
        with open(self.test_file, 'wb') as f:
            # Create pages that reference each other circularly
            for i in range(5):
                page = bytearray(PAGE_SIZE)
                
                # Circular reference: page i points to page (i+1) % 5
                struct.pack_into('<I', page, 0, 0xFFFFFFFF)
                struct.pack_into('<I', page, 4, (i + 1) % 5)
                struct.pack_into('<Q', page, 8, 1)
                struct.pack_into('<H', page, 16, 9969)
                struct.pack_into('<I', page, 16, 0)
                struct.pack_into('<H', page, 22, i)
                
                struct.pack_into('<H', page, 38, 2)
                struct.pack_into('<H', page, 40, 75)
                struct.pack_into('<H', page, 42, 2 | 0x8000)
                struct.pack_into('<H', page, 44, 54)
                
                f.write(page)


class LargePageTest(CorruptionTest):
    """Test: Large page size (64KB)"""
    def __init__(self):
        super().__init__(
            "large_page",
            "Test recovery with 64KB page size"
        )
    
    def setup(self, temp_dir):
        self.test_file = Path(temp_dir) / "large_page.ibd"
        
        with open(self.test_file, 'wb') as f:
            page = bytearray(65536)
            
            struct.pack_into('<I', page, 0, 0xFFFFFFFF)
            struct.pack_into('<I', page, 4, 0xFFFFFFFF)
            struct.pack_into('<Q', page, 8, 1)
            struct.pack_into('<H', page, 16, 9969)
            struct.pack_into('<I', page, 16, 0)
            struct.pack_into('<H', page, 22, 0)
            
            struct.pack_into('<H', page, 38, 2)
            struct.pack_into('<H', page, 40, 75)
            struct.pack_into('<H', page, 42, 2 | 0x8000)
            struct.pack_into('<H', page, 44, 54)
            
            f.write(page)


def run_all_tests():
    """Run all extreme corruption tests"""
    print("=" * 80)
    print("MySQL Recovery Pro - Extreme Scenario Data Corruption Tests")
    print("=" * 80)
    print()
    
    tests = [
        TruncationTest(),
        HeaderCorruptionTest(),
        ChecksumMismatchTest(),
        PageChainBrokenTest(),
        RecordChainCorruptionTest(),
        BLOBChainCorruptionTest(),
        MixedCorruptionTest(),
        EmptyFileTest(),
        RandomDataTest(),
        CircularReferenceTest(),
        LargePageTest(),
    ]
    
    results = []
    
    for i, test in enumerate(tests, 1):
        print(f"[{i}/{len(tests)}] Running: {test.name}")
        print(f"    Description: {test.description}")
        
        result = test.execute()
        results.append(result)
        
        # Print summary
        analysis = result.get("analysis", {})
        warnings = analysis.get("warnings", [])
        errors = analysis.get("errors", [])
        
        print(f"    Warnings: {len(warnings)}")
        print(f"    Errors: {len(errors)}")
        
        if warnings:
            print(f"    Sample warnings:")
            for w in warnings[:3]:
                print(f"      - {w[:100]}...")
        
        if errors:
            print(f"    Sample errors:")
            for e in errors[:3]:
                print(f"      - {e[:100]}...")
        
        print()
    
    # Generate summary report
    print("=" * 80)
    print("TEST SUMMARY")
    print("=" * 80)
    
    total_tests = len(results)
    tests_with_warnings = sum(1 for r in results if r.get("analysis", {}).get("warnings"))
    tests_with_errors = sum(1 for r in results if r.get("analysis", {}).get("errors"))
    
    print(f"Total tests: {total_tests}")
    print(f"Tests with warnings: {tests_with_warnings}")
    print(f"Tests with errors: {tests_with_errors}")
    print()
    
    # Save results to JSON
    output_file = TEST_DIR / "extreme_corruption_results.json"
    with open(output_file, 'w') as f:
        json.dump(results, f, indent=2)
    
    print(f"Results saved to: {output_file}")
    
    return results


if __name__ == "__main__":
    results = run_all_tests()
    
    # Exit with error if any test failed
    if any(r.get("analysis", {}).get("errors") for r in results):
        sys.exit(1)
    sys.exit(0)
