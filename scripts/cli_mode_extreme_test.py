#!/usr/bin/env python3
"""
CLI Mode Extreme Test Generator
为所有CLI模式生成极端测试场景
"""

import os
import sys
import random
import struct
import hashlib
import argparse
from typing import List, Tuple, Dict
from pathlib import Path


class CLIModeExtremeTestGenerator:
    """CLI模式极端测试生成器"""
    
    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.scenarios: List[Dict] = []
    
    def generate_scan_mode_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        扫描模式极端测试 - 所有页类型损坏
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
            "name": "扫描模式 - 所有页头损坏",
            "cli_mode": "scan",
            "corruption_type": "所有页头损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_search_mode_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        搜索模式极端测试 - 页链完全破坏
        """
        corrupted = bytearray(data)
        
        # 所有页链破坏
        for page_no in range(1, len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)  # prev_page
                struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)  # next_page
        
        scenario = {
            "name": "搜索模式 - 页链完全破坏",
            "cli_mode": "search",
            "corruption_type": "页链完全破坏",
            "pages_corrupted": len(data) // self.page_size - 1
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_bulk_mode_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        批量恢复模式极端测试 - 所有页类型和校验和损坏
        """
        corrupted = bytearray(data)
        
        # 所有页类型和校验和损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<H', corrupted, offset + 16, 0xFFFF)  # page_type
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)  # checksum
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)  # checksum
        
        scenario = {
            "name": "批量恢复模式 - 所有页类型和校验和损坏",
            "cli_mode": "bulk",
            "corruption_type": "页类型+校验和全部损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_header_check_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        页头检查模式极端测试 - 所有页头字段损坏
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
            "name": "页头检查模式 - 所有页头字段损坏",
            "cli_mode": "check-headers",
            "corruption_type": "所有页头字段损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_corruption_check_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        坏块检查模式极端测试 - 随机位翻转 + 校验和损坏
        """
        corrupted = bytearray(data)
        
        # 随机位翻转
        for _ in range(500):
            pos = random.randint(0, len(corrupted) - 1)
            bit_pos = random.randint(0, 7)
            corrupted[pos] ^= (1 << bit_pos)
        
        # 校验和损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 32 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "坏块检查模式 - 随机位翻转+校验和损坏",
            "cli_mode": "check-corruption",
            "corruption_type": "随机位翻转+校验和损坏",
            "bit_flips": 500,
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_repair_mode_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        修复模式极端测试 - 所有关键字段损坏
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
            "name": "修复模式 - 所有关键字段损坏",
            "cli_mode": "repair",
            "corruption_type": "所有关键字段损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_extract_sdi_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        SDI提取模式极端测试 - SDI页损坏
        """
        corrupted = bytearray(data)
        
        # SDI页损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                page_type = struct.unpack('<H', corrupted[offset + 16:offset + 18])[0]
                if page_type == 0x4573:  # SDI
                    struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                    struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "SDI提取模式 - SDI页损坏",
            "cli_mode": "extract-sdi",
            "corruption_type": "SDI页损坏",
            "sdi_pages_corrupted": 0  # Will be set after scan
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_rebuild_chain_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        页链重连模式极端测试 - 页链和记录链同时破坏
        """
        corrupted = bytearray(data)
        
        # 页链破坏
        for page_no in range(1, len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)  # prev_page
                struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)  # next_page
        
        # 记录链破坏
        for page_no in range(1, len(data) // self.page_size):
            page_offset = page_no * self.page_size + 38
            
            if page_offset + 20 <= len(corrupted):
                struct.pack_into('<H', corrupted, page_offset + 2, 0xFFFF)   # heap_top
                struct.pack_into('<H', corrupted, page_offset + 6, 0xFFFF)   # free_offset
        
        scenario = {
            "name": "页链重连模式 - 页链+记录链同时破坏",
            "cli_mode": "rebuild-chain",
            "corruption_type": "页链+记录链同时破坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_parse_page_extreme(self, data: bytearray, parse_page: int = 5) -> Tuple[bytearray, Dict]:
        """
        页解析模式极端测试 - 特定页完全损坏
        """
        corrupted = bytearray(data)
        
        # 特定页完全损坏
        offset = parse_page * self.page_size
        
        if offset + 52 <= len(corrupted):
            struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)   # prev_page
            struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)   # next_page
            struct.pack_into('<Q', corrupted, offset + 8, 0)             # LSN
            struct.pack_into('<H', corrupted, offset + 16, 0xFFFF)       # page_type
            struct.pack_into('<I', corrupted, offset + 20, 0xDEADBEEF)   # page_no
            struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)   # checksum
            struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)   # checksum
        
        scenario = {
            "name": f"页解析模式 - 页{parse_page}完全损坏",
            "cli_mode": "parse-page",
            "parse_page": parse_page,
            "corruption_type": "特定页完全损坏",
            "pages_corrupted": 1
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_all_cli_scenarios(self, data: bytearray) -> List[Tuple[bytearray, Dict]]:
        """生成所有CLI模式极端场景"""
        results = []
        
        scenarios = [
            self.generate_scan_mode_extreme,
            self.generate_search_mode_extreme,
            self.generate_bulk_mode_extreme,
            self.generate_header_check_extreme,
            self.generate_corruption_check_extreme,
            self.generate_repair_mode_extreme,
            self.generate_extract_sdi_extreme,
            self.generate_rebuild_chain_extreme,
            self.generate_parse_page_extreme,
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
        description='CLI Mode Extreme Test Generator'
    )
    parser.add_argument('--input', '-i', required=True, help='Input .ibd file')
    parser.add_argument('--output-dir', '-o', required=True, help='Output directory')
    parser.add_argument('--scenario', '-s',
                       choices=['scan', 'search', 'bulk', 'check-headers', 'check-corruption',
                               'repair', 'extract-sdi', 'rebuild-chain', 'parse-page', 'all'],
                       help='Specific CLI mode scenario')
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
    generator = CLIModeExtremeTestGenerator(page_size=16384)
    
    # Ensure output directory exists
    os.makedirs(args.output_dir, exist_ok=True)
    
    # Generate scenarios
    if args.scenario and args.scenario != 'all':
        scenario_map = {
            'scan': generator.generate_scan_mode_extreme,
            'search': generator.generate_search_mode_extreme,
            'bulk': generator.generate_bulk_mode_extreme,
            'check-headers': generator.generate_header_check_extreme,
            'check-corruption': generator.generate_corruption_check_extreme,
            'repair': generator.generate_repair_mode_extreme,
            'extract-sdi': generator.generate_extract_sdi_extreme,
            'rebuild-chain': generator.generate_rebuild_chain_extreme,
            'parse-page': generator.generate_parse_page_extreme,
        }
        
        func = scenario_map[args.scenario]
        corrupted, info = func(bytearray(data))
        
        output_file = os.path.join(args.output_dir, f"test_cli_{args.scenario}_extreme.ibd")
        with open(output_file, 'wb') as f:
            f.write(corrupted)
        
        if args.verbose:
            print(f"CLI Mode: {args.scenario}")
            print(f"Scenario: {info['name']}")
            print(f"  Type: {info['corruption_type']}")
            if 'pages_corrupted' in info:
                print(f"  Pages: {info['pages_corrupted']}")
            print(f"  Input hash: {generate_hash(data)}")
            print(f"  Output hash: {generate_hash(corrupted)}")
            print(f"  Output: {output_file}")
        else:
            print(f"CLI_MODE:{args.scenario}")
            print(f"SCENARIO:{info['name']}")
            print(f"CORRUPTION:{info['corruption_type']}")
            if 'pages_corrupted' in info:
                print(f"PAGES:{info['pages_corrupted']}")
            print(f"HASH_INPUT:{generate_hash(data)}")
            print(f"HASH_OUTPUT:{generate_hash(corrupted)}")
            print(f"OUTPUT:{output_file}")
    else:
        # Generate all scenarios
        results = generator.generate_all_cli_scenarios(bytearray(data))
        
        for i, (corrupted, info) in enumerate(results, 1):
            mode = info['cli_mode'].replace('-', '_')
            output_file = os.path.join(args.output_dir, f"test_cli_{mode}_extreme.ibd")
            with open(output_file, 'wb') as f:
                f.write(corrupted)
            
            if args.verbose:
                print(f"Scenario {i}: {info['name']}")
                print(f"  CLI Mode: {info['cli_mode']}")
                print(f"  Type: {info['corruption_type']}")
                print(f"  Pages: {info['pages_corrupted']}")
                print(f"  Output: {output_file}")
                print()
        
        print(f"Generated {len(results)} CLI mode extreme scenarios in {args.output_dir}")


if __name__ == '__main__':
    main()
