#!/usr/bin/env python3
"""
Exception Input Extreme Test Generator
为异常输入生成极端测试场景
"""

import os
import sys
import random
import struct
import hashlib
import argparse
from typing import List, Tuple, Dict
from pathlib import Path


class ExceptionInputExtremeTestGenerator:
    """异常输入极端测试生成器"""
    
    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.scenarios: List[Dict] = []
    
    def generate_corrupted_header_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        异常输入1: 页头完全损坏
        """
        corrupted = bytearray(data)
        
        # 页头完全损坏
        if len(corrupted) >= 52:
            struct.pack_into('<H', corrupted, 16, 0xFFFF)
            struct.pack_into('<I', corrupted, 24, 0xDEADBEEF)
            struct.pack_into('<I', corrupted, 28, 0xDEADBEEF)
        
        scenario = {
            "name": "异常输入1 - 页头完全损坏",
            "exception_type": "corrupted_header",
            "corruption_type": "页头完全损坏",
            "pages_corrupted": 1
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_invalid_checksum_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        异常输入2: 所有校验和无效
        """
        corrupted = bytearray(data)
        
        # 所有校验和无效
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 32 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 24, 0x00000000)  # Zero checksum
                struct.pack_into('<I', corrupted, offset + 28, 0xFFFFFFFF)  # Max checksum
        
        scenario = {
            "name": "异常输入2 - 所有校验和无效",
            "exception_type": "invalid_checksum",
            "corruption_type": "所有校验和无效",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_invalid_page_type_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        异常输入3: 页类型无效
        """
        corrupted = bytearray(data)
        
        # 页类型无效
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<H', corrupted, offset + 16, 0x9999)  # Invalid page type
        
        scenario = {
            "name": "异常输入3 - 页类型无效",
            "exception_type": "invalid_page_type",
            "corruption_type": "所有页类型无效",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_invalid_page_no_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        异常输入4: 页号全部错误
        """
        corrupted = bytearray(data)
        
        # 页号全部错误
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 20, 0xFFFFFFFF)  # Invalid page no
        
        scenario = {
            "name": "异常输入4 - 页号全部错误",
            "exception_type": "invalid_page_no",
            "corruption_type": "所有页号无效",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_invalid_lsn_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        异常输入5: LSN全部为0
        """
        corrupted = bytearray(data)
        
        # LSN全部为0
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<Q', corrupted, offset + 8, 0)
        
        scenario = {
            "name": "异常输入5 - LSN全部为0",
            "exception_type": "invalid_lsn",
            "corruption_type": "所有LSN为0",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_invalid_space_id_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        异常输入6: 空间ID错误
        """
        corrupted = bytearray(data)
        
        # 空间ID错误
        if self.page_size <= len(corrupted):
            struct.pack_into('<I', corrupted, 16, 0xDEADBEEF)
        
        scenario = {
            "name": "异常输入6 - 空间ID错误",
            "exception_type": "invalid_space_id",
            "corruption_type": "空间ID错误",
            "pages_corrupted": 1
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_invalid_prev_next_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        异常输入7: prev_page和next_page全部错误
        """
        corrupted = bytearray(data)
        
        # prev_page和next_page全部错误
        for page_no in range(1, len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 0, 0xFFFFFFFF)  # Invalid prev_page
                struct.pack_into('<I', corrupted, offset + 4, 0xFFFFFFFF)  # Invalid next_page
        
        scenario = {
            "name": "异常输入7 - prev_page和next_page全部错误",
            "exception_type": "invalid_prev_next",
            "corruption_type": "页链指针全部错误",
            "pages_corrupted": len(data) // self.page_size - 1
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_invalid_heap_top_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        异常输入8: heap_top全部错误
        """
        corrupted = bytearray(data)
        
        # heap_top全部错误
        for page_no in range(1, len(data) // self.page_size):
            page_offset = page_no * self.page_size + 38
            
            if page_offset + 20 <= len(corrupted):
                struct.pack_into('<H', corrupted, page_offset + 2, 0xFFFF)   # Invalid heap_top
                struct.pack_into('<H', corrupted, page_offset + 6, 0xFFFF)   # Invalid free_offset
        
        scenario = {
            "name": "异常输入8 - heap_top全部错误",
            "exception_type": "invalid_heap_top",
            "corruption_type": "heap_top全部错误",
            "pages_corrupted": len(data) // self.page_size - 1
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_all_exception_inputs(self, data: bytearray) -> List[Tuple[bytearray, Dict]]:
        """生成所有异常输入极端场景"""
        results = []
        
        scenarios = [
            self.generate_corrupted_header_extreme,
            self.generate_invalid_checksum_extreme,
            self.generate_invalid_page_type_extreme,
            self.generate_invalid_page_no_extreme,
            self.generate_invalid_lsn_extreme,
            self.generate_invalid_space_id_extreme,
            self.generate_invalid_prev_next_extreme,
            self.generate_invalid_heap_top_extreme,
        ]
        
        for scenario_func in scenarios:
            corrupted, info = scenario_func(bytearray(data))
            results.append((corrupted, info))
        
        return results


def generate_hash(data: bytes) -> str:
    """Generate SHA-256 hash"""
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(
        description='Exception Input Extreme Test Generator'
    )
    parser.add_argument('--input', '-i', required=True, help='Input .ibd file')
    parser.add_argument('--output-dir', '-o', required=True, help='Output directory')
    parser.add_argument('--exception', '-e', type=int,
                       choices=range(1, 9),
                       help='Specific exception input number (1-8)')
    parser.add_argument('--seed', type=int, default=None, help='Random seed')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    
    args = parser.parse_args()
    
    # Set random seed
    if args.seed:
        random.seed(args.seed)
    
    # Read input file
    with open(args.input, 'rb') as f:
        data = f.read()
    
    # Create generator
    generator = ExceptionInputExtremeTestGenerator(page_size=16384)
    
    # Ensure output directory exists
    os.makedirs(args.output_dir, exist_ok=True)
    
    # Generate scenarios
    if args.exception:
        scenario_map = {
            1: generator.generate_corrupted_header_extreme,
            2: generator.generate_invalid_checksum_extreme,
            3: generator.generate_invalid_page_type_extreme,
            4: generator.generate_invalid_page_no_extreme,
            5: generator.generate_invalid_lsn_extreme,
            6: generator.generate_invalid_space_id_extreme,
            7: generator.generate_invalid_prev_next_extreme,
            8: generator.generate_invalid_heap_top_extreme,
        }
        
        func = scenario_map[args.exception]
        corrupted, info = func(bytearray(data))
        
        output_file = os.path.join(args.output_dir, f"test_exception_{args.exception}_extreme.ibd")
        with open(output_file, 'wb') as f:
            f.write(corrupted)
        
        if args.verbose:
            print(f"Exception Input: {args.exception}")
            print(f"Scenario: {info['name']}")
            print(f"  Type: {info['corruption_type']}")
            print(f"  Pages: {info['pages_corrupted']}")
            print(f"  Input hash: {generate_hash(data)}")
            print(f"  Output hash: {generate_hash(corrupted)}")
            print(f"  Output: {output_file}")
        else:
            print(f"EXCEPTION_INPUT:{args.exception}")
            print(f"SCENARIO:{info['name']}")
            print(f"CORRUPTION:{info['corruption_type']}")
            print(f"PAGES:{info['pages_corrupted']}")
            print(f"HASH_INPUT:{generate_hash(data)}")
            print(f"HASH_OUTPUT:{generate_hash(corrupted)}")
            print(f"OUTPUT:{output_file}")
    else:
        # Generate all scenarios
        results = generator.generate_all_exception_inputs(bytearray(data))
        
        for i, (corrupted, info) in enumerate(results, 1):
            output_file = os.path.join(args.output_dir, f"test_exception_{i}_extreme.ibd")
            with open(output_file, 'wb') as f:
                f.write(corrupted)
            
            if args.verbose:
                print(f"Exception Input {i}: {info['name']}")
                print(f"  Type: {info['corruption_type']}")
                print(f"  Pages: {info['pages_corrupted']}")
                print(f"  Output: {output_file}")
                print()
        
        print(f"Generated {len(results)} exception input extreme scenarios in {args.output_dir}")


if __name__ == '__main__':
    main()
