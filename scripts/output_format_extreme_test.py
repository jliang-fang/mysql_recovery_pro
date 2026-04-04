#!/usr/bin/env python3
"""
Output Format Extreme Test Generator
为所有输出格式生成极端测试场景
"""

import os
import sys
import random
import struct
import hashlib
import argparse
import json
from typing import List, Tuple, Dict
from pathlib import Path


class OutputFormatExtremeTestGenerator:
    """输出格式极端测试生成器"""
    
    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.scenarios: List[Dict] = []
    
    def generate_csv_format_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        CSV格式极端测试 - 所有页头损坏
        """
        corrupted = bytearray(data)
        
        # 所有页头损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<H', corrupted, offset + 16, 0xFFFF)
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "CSV格式 - 所有页头损坏",
            "output_format": "csv",
            "corruption_type": "所有页头损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_sql_format_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        SQL格式极端测试 - 页链完全破坏
        """
        corrupted = bytearray(data)
        
        # 页链完全破坏
        for page_no in range(1, len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)  # prev_page
                struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)  # next_page
        
        scenario = {
            "name": "SQL格式 - 页链完全破坏",
            "output_format": "sql",
            "corruption_type": "页链完全破坏",
            "pages_corrupted": len(data) // self.page_size - 1
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_json_format_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        JSON格式极端测试 - 所有校验和损坏
        """
        corrupted = bytearray(data)
        
        # 所有校验和损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 32 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "JSON格式 - 所有校验和损坏",
            "output_format": "json",
            "corruption_type": "所有校验和损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_ibd_format_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        IBD格式极端测试 - 所有关键字段损坏
        """
        corrupted = bytearray(data)
        
        # 所有关键字段损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)   # prev_page
                struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)   # next_page
                struct.pack_into('<Q', corrupted, offset + 8, 0)             # LSN
                struct.pack_into('<I', corrupted, offset + 16, 0xDEADBEEF)   # space_id
                struct.pack_into('<I', corrupted, offset + 20, 0xDEADBEEF)   # page_no
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)   # checksum
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)   # checksum
        
        scenario = {
            "name": "IBD格式 - 所有关键字段损坏",
            "output_format": "ibd",
            "corruption_type": "所有关键字段损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_all_format_scenarios(self, data: bytearray) -> List[Tuple[bytearray, Dict]]:
        """生成所有输出格式极端场景"""
        results = []
        
        scenarios = [
            self.generate_csv_format_extreme,
            self.generate_sql_format_extreme,
            self.generate_json_format_extreme,
            self.generate_ibd_format_extreme,
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
        description='Output Format Extreme Test Generator'
    )
    parser.add_argument('--input', '-i', required=True, help='Input .ibd file')
    parser.add_argument('--output-dir', '-o', required=True, help='Output directory')
    parser.add_argument('--scenario', '-s',
                       choices=['csv', 'sql', 'json', 'ibd', 'all'],
                       help='Specific output format scenario')
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
    generator = OutputFormatExtremeTestGenerator(page_size=16384)
    
    # Ensure output directory exists
    os.makedirs(args.output_dir, exist_ok=True)
    
    # Generate scenarios
    if args.scenario and args.scenario != 'all':
        scenario_map = {
            'csv': generator.generate_csv_format_extreme,
            'sql': generator.generate_sql_format_extreme,
            'json': generator.generate_json_format_extreme,
            'ibd': generator.generate_ibd_format_extreme,
        }
        
        func = scenario_map[args.scenario]
        corrupted, info = func(bytearray(data))
        
        output_file = os.path.join(args.output_dir, f"test_format_{args.scenario}_extreme.ibd")
        with open(output_file, 'wb') as f:
            f.write(corrupted)
        
        if args.verbose:
            print(f"Output Format: {args.scenario}")
            print(f"Scenario: {info['name']}")
            print(f"  Type: {info['corruption_type']}")
            print(f"  Pages: {info['pages_corrupted']}")
            print(f"  Input hash: {generate_hash(data)}")
            print(f"  Output hash: {generate_hash(corrupted)}")
            print(f"  Output: {output_file}")
        else:
            print(f"FORMAT:{args.scenario}")
            print(f"SCENARIO:{info['name']}")
            print(f"CORRUPTION:{info['corruption_type']}")
            print(f"PAGES:{info['pages_corrupted']}")
            print(f"HASH_INPUT:{generate_hash(data)}")
            print(f"HASH_OUTPUT:{generate_hash(corrupted)}")
            print(f"OUTPUT:{output_file}")
    else:
        # Generate all scenarios
        results = generator.generate_all_format_scenarios(bytearray(data))
        
        for i, (corrupted, info) in enumerate(results, 1):
            output_file = os.path.join(args.output_dir, f"test_format_{info['output_format']}_extreme.ibd")
            with open(output_file, 'wb') as f:
                f.write(corrupted)
            
            if args.verbose:
                print(f"Scenario {i}: {info['name']}")
                print(f"  Format: {info['output_format']}")
                print(f"  Type: {info['corruption_type']}")
                print(f"  Pages: {info['pages_corrupted']}")
                print(f"  Output: {output_file}")
                print()
        
        print(f"Generated {len(results)} output format extreme scenarios in {args.output_dir}")


if __name__ == '__main__':
    main()
