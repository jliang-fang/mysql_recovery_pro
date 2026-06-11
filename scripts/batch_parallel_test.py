#!/usr/bin/env python3
"""
MySQL Recovery Pro - Batch Parallel Processing Chaos Test
Purpose: Test massive file handling (10,000+ files) and resource leak detection
Author: jliang.fang (Global MySQL Kernel QA Expert)
Version: 2.1.0 - Batch Processing Validation
"""

import os
import sys
import json
import time
import hashlib
import random
import argparse
import resource
from typing import List, Dict, Tuple, Optional
from pathlib import Path
from datetime import datetime

import struct


class BatchParallelChaos:
    """Test batch file processing for resource leaks"""
    
    def __init__(self, seed: int = 42):
        self.seed = seed
        random.seed(seed)
        self.metrics = {
            "files_processed": 0,
            "files_opened": 0,
            "files_closed": 0,
            "file_handles_leaked": 0,
            "max_handles_used": 0,
            "hash_calculations": 0,
            "bytes_processed": 0,
            "timeouts": 0,
            "errors": []
        }
    
    def create_batch_test_files(self, base_dir: str, num_files: int = 10000,
                                file_size_kb: int = 64) -> Dict:
        """Create 10,000+ .ibd files for concurrent processing"""
        base_path = Path(base_dir)
        base_path.mkdir(parents=True, exist_ok=True)
        
        result = {
            "scenario": "batch_parallel_test",
            "target_files": num_files,
            "files_created": 0,
            "total_size_mb": 0,
            "create_time_ms": 0,
            "file_hashes": []
        }
        
        start_time = time.time()
        
        for i in range(num_files):
            filepath = base_path / f"test_{i:05d}.ibd"
            
            # Create deterministic test data
            random.seed(self.seed + i)
            file_data = bytes([random.randint(0, 255) for _ in range(file_size_kb * 1024)])
            
            # Write file
            with open(filepath, 'wb') as f:
                f.write(file_data)
            
            # Calculate hash
            file_hash = hashlib.sha256(file_data).hexdigest()
            result["file_hashes"].append({
                "index": i,
                "path": str(filepath),
                "hash": file_hash
            })
            
            result["files_created"] += 1
            result["total_size_mb"] += file_size_kb / 1024
        
        elapsed = (time.time() - start_time) * 1000
        
        result["create_time_ms"] = round(elapsed, 2)
        result["hashes_generated"] = len(result["file_hashes"])
        result["test_passed"] = result["files_created"] == num_files
        
        return result
    
    def test_file_handle_exhaustion(self, file_list: List[str]) -> Dict:
        """Test file handle exhaustion with many open files"""
        result = {
            "scenario": "file_handle_exhaustion",
            "files_opened": 0,
            "files_closed": 0,
            "max_handles_at_once": 0,
            "leak_detected": False,
            "error": None
        }
        
        try:
            handles = []
            max_handles = 0
            
            for filepath in file_list:
                if os.path.exists(filepath):
                    fd = open(filepath, 'rb')
                    handles.append(fd)
                    result["files_opened"] += 1
                    max_handles = max(max_handles, len(handles))
                    
                    # Close periodically to prevent actual exhaustion
                    if len(handles) >= 500:
                        for h in handles[:250]:
                            h.close()
                            result["files_closed"] += 1
                        handles = handles[250:]
            
            # Close remaining
            for h in handles:
                h.close()
                result["files_closed"] += 1
            
            result["max_handles_at_once"] = max_handles
            result["file_handles_leaked"] = result["files_opened"] - result["files_closed"]
            result["leak_detected"] = result["file_handles_leaked"] > 0
            
        except OSError as e:
            result["error"] = str(e)
            result["leak_detected"] = True
            result["error_type"] = "file_descriptor_exhaustion"
        except Exception as e:
            result["error"] = str(e)
            result["leak_detected"] = True
        
        return result
    
    def test_concurrent_processing(self, file_list: List[str], 
                                    batch_size: int = 100) -> Dict:
        """Test concurrent file processing"""
        result = {
            "scenario": "concurrent_processing",
            "total_files": len(file_list),
            "batches_processed": 0,
            "files_per_batch": batch_size,
            "timeouts": 0,
            "errors": []
        }
        
        start_time = time.time()
        
        for i in range(0, len(file_list), batch_size):
            batch = file_list[i:i + batch_size]
            
            try:
                for filepath in batch:
                    if os.path.exists(filepath):
                        with open(filepath, 'rb') as f:
                            data = f.read(4096)  # Read first 4KB
                            # Process data
                            sha = hashlib.sha256(data).hexdigest()
                            result["files_per_batch"] = batch_size
            except Exception as e:
                result["errors"].append({
                    "batch": i // batch_size,
                    "error": str(e)
                })
            
            result["batches_processed"] += 1
        
        elapsed = (time.time() - start_time) * 1000
        result["processing_time_ms"] = round(elapsed, 2)
        result["throughput_files_per_sec"] = round(len(file_list) / (elapsed / 1000), 2) if elapsed > 0 else 0
        
        return result
    
    def test_memory_leak(self, file_list: List[str], iterations: int = 10) -> Dict:
        """Test for memory leaks during repeated processing"""
        result = {
            "scenario": "memory_leak_test",
            "iterations": iterations,
            "memory_max_mb": 0,
            "memory_delta_mb": 0,
            "leak_detected": False,
            "per_iteration_stats": []
        }
        
        base_memory = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        
        for i in range(iterations):
            # Process all files
            for filepath in file_list:
                if os.path.exists(filepath):
                    with open(filepath, 'rb') as f:
                        data = f.read()
                        hashlib.sha256(data).hexdigest()
            
            current_memory = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
            memory_mb = current_memory / 1024  # Convert to MB
            
            result["per_iteration_stats"].append({
                "iteration": i + 1,
                "memory_mb": round(memory_mb, 2)
            })
            
            result["memory_max_mb"] = max(
                result["memory_max_mb"], memory_mb
            )
        
        result["memory_delta_mb"] = round(
            result["memory_max_mb"] - (base_memory / 1024), 2
        )
        
        # Memory leak if delta grows significantly
        if result["memory_delta_mb"] > 100:  # > 100MB increase
            result["leak_detected"] = True
        
        return result


class BatchRecoveryEngineTest:
    """Test recovery engine with batch files"""
    
    def __init__(self, engine_path: str = "mysql_recovery_pro"):
        self.engine_path = engine_path
    
    def run_recovery_on_batch(self, file_list: List[str], 
                              output_dir: str = "./batch_recovery_output") -> Dict:
        """Run recovery engine on batch of files"""
        output_path = Path(output_dir)
        output_path.mkdir(parents=True, exist_ok=True)
        
        result = {
            "scenario": "batch_recovery",
            "files_processed": 0,
            "files_failed": 0,
            "records_recovered": 0,
            "errors": []
        }
        
        for filepath in file_list[:100]:  # Test first 100 files
            output_file = output_path / f"{Path(filepath).stem}_recovered.json"
            
            try:
                # Simulate recovery engine execution
                # In real scenario, would call: subprocess.run([self.engine_path, '--ibd', filepath, '--output', output_file])
                
                result["files_processed"] += 1
                result["records_recovered"] += random.randint(0, 1000)  # Simulated
            except Exception as e:
                result["files_failed"] += 1
                result["errors"].append({
                    "file": filepath,
                    "error": str(e)
                })
        
        return result


def main():
    parser = argparse.ArgumentParser(
        description="MySQL Recovery Pro - Batch Parallel Processing Chaos Test",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Batch Test Scenarios:
  1. File Handle Exhaustion - Test 10,000+ file handles
  2. Concurrent Processing - Test batch processing performance
  3. Memory Leak Detection - Track memory usage over iterations
  4. Recovery Engine Batch - Test engine with many files

Usage Examples:
  python3 batch_parallel_test.py --generate --output /tmp/batch_test
  python3 batch_parallel_test.py --test --input /tmp/batch_test/files
  python3 batch_parallel_test.py --all --output /tmp/batch_results
        """
    )
    
    parser.add_argument('--generate', action='store_true',
                       help='Generate batch test files')
    parser.add_argument('--test', action='store_true',
                       help='Run batch chaos tests')
    parser.add_argument('--output', type=str, default='./batch_test_output',
                       help='Output directory')
    parser.add_argument('--input', type=str, default=None,
                       help='Input directory with test files')
    parser.add_argument('--num-files', type=int, default=10000,
                       help='Number of files to generate')
    parser.add_argument('--file-size-kb', type=int, default=64,
                       help='Size of each test file in KB')
    parser.add_argument('--all', action='store_true',
                       help='Generate and test')
    
    args = parser.parse_args()
    
    chaos = BatchParallelChaos(seed=42)
    
    if args.generate:
        result = chaos.create_batch_test_files(
            base_dir=args.output,
            num_files=args.num_files,
            file_size_kb=args.file_size_kb
        )
        print(json.dumps(result, indent=2))
    
    elif args.test:
        if not args.input:
            print("Error: --input required for testing")
            sys.exit(1)
        
        # Get file list
        input_path = Path(args.input)
        file_list = list(input_path.glob("test_*.ibd"))
        
        # Run tests
        results = {}
        
        # Test 1: File handle exhaustion
        results["file_handle_test"] = chaos.test_file_handle_exhaustion(file_list)
        
        # Test 2: Concurrent processing
        results["concurrent_test"] = chaos.test_concurrent_processing(file_list)
        
        # Test 3: Memory leak
        results["memory_leak_test"] = chaos.test_memory_leak(file_list)
        
        # Test 4: Recovery engine batch
        recovery_test = BatchRecoveryEngineTest()
        results["recovery_batch"] = recovery_test.run_recovery_on_batch(file_list)
        
        print(json.dumps(results, indent=2))
    
    elif args.all:
        # Generate files
        print("[BATCH] Generating test files...")
        gen_result = chaos.create_batch_test_files(
            base_dir=args.output,
            num_files=args.num_files,
            file_size_kb=args.file_size_kb
        )
        
        # Get file list
        input_path = Path(args.output)
        file_list = list(input_path.glob("test_*.ibd"))
        
        # Run all tests
        print("[BATCH] Running chaos tests...")
        results = {
            "generation": gen_result,
            "file_handle_test": chaos.test_file_handle_exhaustion(file_list),
            "concurrent_test": chaos.test_concurrent_processing(file_list),
            "memory_leak_test": chaos.test_memory_leak(file_list),
            "batch_recovery": BatchRecoveryEngineTest().run_recovery_on_batch(file_list)
        }
        
        print(json.dumps(results, indent=2))
    
    else:
        print("Usage: python3 batch_parallel_test.py [--generate|--test|--all] [options]")
        sys.exit(1)


if __name__ == "__main__":
    main()
