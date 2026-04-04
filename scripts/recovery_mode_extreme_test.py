#!/usr/bin/env python3
"""
Recovery Mode Extreme Test Generator
为所有恢复模式生成极端测试场景
"""

import os
import sys
import random
import struct
import hashlib
import argparse
from typing import List, Tuple, Dict
from pathlib import Path


class RecoveryModeExtremeTestGenerator:
    """恢复模式极端测试生成器"""
    
    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.scenarios: List[Dict] = []
    
    def generate_truncate_recovery_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        Truncate恢复模式极端测试 - 所有页头和记录链损坏
        """
        corrupted = bytearray(data)
        
        # 所有页头损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<H', corrupted, offset + 16, 0xFFFF)
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        # 记录链损坏
        for page_no in range(1, len(data) // self.page_size):
            page_offset = page_no * self.page_size + 38
            
            if page_offset + 20 <= len(corrupted):
                struct.pack_into('<H', corrupted, page_offset + 2, 0xFFFF)   # heap_top
                struct.pack_into('<H', corrupted, page_offset + 6, 0xFFFF)   # free_offset
        
        scenario = {
            "name": "Truncate恢复模式 - 所有页头+记录链损坏",
            "recovery_mode": "truncate-recovery",
            "corruption_type": "页头+记录链全部损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_rmrf_recovery_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        rm -rf恢复模式极端测试 - 页链完全破坏 + 校验和损坏
        """
        corrupted = bytearray(data)
        
        # 页链完全破坏
        for page_no in range(1, len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)  # prev_page
                struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)  # next_page
        
        # 校验和损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 32 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "rm -rf恢复模式 - 页链完全破坏+校验和损坏",
            "recovery_mode": "rmrf-recovery",
            "corruption_type": "页链完全破坏+校验和损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_default_recovery_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        默认恢复模式极端测试 - 所有关键字段损坏
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
            "name": "默认恢复模式 - 所有关键字段损坏",
            "recovery_mode": "default-recovery",
            "corruption_type": "所有关键字段损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_blob_recovery_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        BLOB恢复模式极端测试 - BLOB页损坏 + 记录损坏
        """
        corrupted = bytearray(data)
        
        # BLOB页损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                page_type = struct.unpack('<H', corrupted[offset + 16:offset + 18])[0]
                if page_type == 0x4574:  # BLOB
                    struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                    struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        # 记录损坏
        for page_no in range(1, len(data) // self.page_size):
            page_offset = page_no * self.page_size + 38
            
            if page_offset + 20 <= len(corrupted):
                struct.pack_into('<H', corrupted, page_offset + 2, 0xFFFF)   # heap_top
                struct.pack_into('<H', corrupted, page_offset + 6, 0xFFFF)   # free_offset
        
        scenario = {
            "name": "BLOB恢复模式 - BLOB页损坏+记录损坏",
            "recovery_mode": "blob-recovery",
            "corruption_type": "BLOB页+记录损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_header_repair_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        页头修复模式极端测试 - 所有页头字段损坏
        """
        corrupted = bytearray(data)
        
        # 所有页头字段损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)   # prev_page
                struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)   # next_page
                struct.pack_into('<Q', corrupted, offset + 8, 0)             # LSN
                struct.pack_into('<H', corrupted, offset + 16, 0xFFFF)       # page_type
                struct.pack_into('<I', corrupted, offset + 20, 0xDEADBEEF)   # page_no
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)   # checksum
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)   # checksum
                struct.pack_into('<H', corrupted, offset + 36, 0xFFFF)       # arch_log_no
        
        scenario = {
            "name": "页头修复模式 - 所有页头字段损坏",
            "recovery_mode": "header-repair",
            "corruption_type": "所有页头字段损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_all_recovery_scenarios(self, data: bytearray) -> List[Tuple[bytearray, Dict]]:
        """生成所有恢复模式极端场景"""
        results = []
        
        scenarios = [
            self.generate_truncate_recovery_extreme,
            self.generate_rmrf_recovery_extreme,
            self.generate_default_recovery_extreme,
            self.generate_blob_recovery_extreme,
            self.generate_header_repair_extreme,
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
        description='Recovery Mode Extreme Test Generator'
    )
    parser.add_argument('--input', '-i', required=True, help='Input .ibd file')
    parser.add_argument('--output-dir', '-o', required=True, help='Output directory')
    parser.add_argument('--scenario', '-s',
                       choices=['truncate-recovery', 'rmrf-recovery', 'default-recovery',
                               'blob-recovery', 'header-repair', 'all'],
                       help='Specific recovery mode scenario')
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
    generator = RecoveryModeExtremeTestGenerator(page_size=16384)
    
    # Ensure output directory exists
    os.makedirs(args.output_dir, exist_ok=True)
    
    # Generate scenarios
    if args.scenario and args.scenario != 'all':
        scenario_map = {
            'truncate-recovery': generator.generate_truncate_recovery_extreme,
            'rmrf-recovery': generator.generate_rmrf_recovery_extreme,
            'default-recovery': generator.generate_default_recovery_extreme,
            'blob-recovery': generator.generate_blob_recovery_extreme,
            'header-repair': generator.generate_header_repair_extreme,
        }
        
        func = scenario_map[args.scenario]
        corrupted, info = func(bytearray(data))
        
        output_file = os.path.join(args.output_dir, f"test_recovery_{args.scenario.replace('-', '_')}_extreme.ibd")
        with open(output_file, 'wb') as f:
            f.write(corrupted)
        
        if args.verbose:
            print(f"Recovery Mode: {args.scenario}")
            print(f"Scenario: {info['name']}")
            print(f"  Type: {info['corruption_type']}")
            print(f"  Pages: {info['pages_corrupted']}")
            print(f"  Input hash: {generate_hash(data)}")
            print(f"  Output hash: {generate_hash(corrupted)}")
            print(f"  Output: {output_file}")
        else:
            print(f"RECOVERY_MODE:{args.scenario}")
            print(f"SCENARIO:{info['name']}")
            print(f"CORRUPTION:{info['corruption_type']}")
            print(f"PAGES:{info['pages_corrupted']}")
            print(f"HASH_INPUT:{generate_hash(data)}")
            print(f"HASH_OUTPUT:{generate_hash(corrupted)}")
            print(f"OUTPUT:{output_file}")
    else:
        # Generate all scenarios
        results = generator.generate_all_recovery_scenarios(bytearray(data))
        
        for i, (corrupted, info) in enumerate(results, 1):
            mode = info['recovery_mode'].replace('-', '_')
            output_file = os.path.join(args.output_dir, f"test_recovery_{mode}_extreme.ibd")
            with open(output_file, 'wb') as f:
                f.write(corrupted)
            
            if args.verbose:
                print(f"Scenario {i}: {info['name']}")
                print(f"  Recovery Mode: {info['recovery_mode']}")
                print(f"  Type: {info['corruption_type']}")
                print(f"  Pages: {info['pages_corrupted']}")
                print(f"  Output: {output_file}")
                print()
        
        print(f"Generated {len(results)} recovery mode extreme scenarios in {args.output_dir}")


if __name__ == '__main__':
    main()
