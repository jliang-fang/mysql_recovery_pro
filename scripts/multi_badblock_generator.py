#!/usr/bin/env python3
"""
Multi-Page Bad Block Combination Test Generator
生成多页坏块组合的极端测试场景
"""

import os
import sys
import random
import struct
import hashlib
import argparse
from typing import List, Tuple, Dict
from pathlib import Path


class MultiPageBadBlockGenerator:
    """多页坏块组合生成器"""
    
    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.scenarios: List[Dict] = []
    
    def generate_scenario_1(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景1: 连续10个页的页头完全损坏
        模拟连续坏块场景
        """
        corrupted = bytearray(data)
        start_page = 5
        
        for page_no in range(start_page, start_page + 10):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                # 完全损坏页头
                struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)   # prev_page
                struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)   # next_page
                struct.pack_into('<Q', corrupted, offset + 8, 0)             # LSN
                struct.pack_into('<H', corrupted, offset + 16, 0xFFFF)       # page_type
                struct.pack_into('<I', corrupted, offset + 20, 0xDEADBEEF)   # page_no
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)   # checksum
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)   # checksum
                struct.pack_into('<H', corrupted, offset + 36, 0xFFFF)       # arch_log_no
        
        scenario = {
            "name": "连续10个页头完全损坏",
            "type": "连续坏块",
            "start_page": start_page,
            "num_pages": 10,
            "corruption_type": "页头完全损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_2(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景2: 交替坏块（每2个页损坏1个）
        模拟分散的坏块
        """
        corrupted = bytearray(data)
        start_page = 3
        interval = 2
        
        for page_no in range(start_page, len(data) // self.page_size, interval):
            offset = page_no * self.page_size
            
            if offset + 32 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "交替坏块（每2个页损坏1个）",
            "type": "分散坏块",
            "start_page": start_page,
            "interval": interval,
            "corruption_type": "校验和损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_3(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景3: 页头损坏 + 页链损坏 + 校验和损坏
        模拟多重损坏
        """
        corrupted = bytearray(data)
        start_page = 5
        
        for page_no in range(start_page, start_page + 8):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                # 页头损坏
                struct.pack_into('<H', corrupted, offset + 16, 0xFFFF)
                # 页链损坏
                struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)
                # 校验和损坏
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "页头+页链+校验和多重损坏",
            "type": "多重损坏",
            "start_page": start_page,
            "num_pages": 8,
            "corruption_type": "页头损坏, 页链损坏, 校验和损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_4(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景4: B+Tree 根页损坏 + 叶子页损坏
        模拟索引结构损坏
        """
        corrupted = bytearray(data)
        
        # 根页（page 0）损坏
        if self.page_size <= len(corrupted):
            struct.pack_into('<H', corrupted, 16, 0xFFFF)  # page_type
            struct.pack_into('<I', corrupted, 24, 0xDEADBEEF)  # checksum
        
        # 叶子页损坏（每5个叶子页损坏1个）
        for page_no in range(1, len(data) // self.page_size, 5):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)  # next_page
        
        scenario = {
            "name": "B+Tree根页+叶子页损坏",
            "type": "索引损坏",
            "root_page": 0,
            "leaf_pages_interval": 5,
            "corruption_type": "页类型损坏, 页链损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_5(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景5: 页头损坏 + 记录链损坏 + 堆损坏
        模拟页内部结构损坏
        """
        corrupted = bytearray(data)
        start_page = 5
        
        for page_no in range(start_page, start_page + 6):
            # 页头损坏
            offset = page_no * self.page_size
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
            
            # 记录链损坏
            page_offset = page_no * self.page_size + 38
            if page_offset + 20 <= len(corrupted):
                struct.pack_into('<H', corrupted, page_offset + 2, 0xFFFF)   # heap_top
                struct.pack_into('<H', corrupted, page_offset + 6, 0xFFFF)   # free_offset
        
        scenario = {
            "name": "页头+记录链+堆损坏",
            "type": "页内部损坏",
            "start_page": start_page,
            "num_pages": 6,
            "corruption_type": "页头损坏, heap_top损坏, free_offset损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_6(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景6: SDI页损坏 + 所有索引页损坏
        模拟元数据完全丢失
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
        
        # 所有索引页损坏
        for page_no in range(1, len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                page_type = struct.unpack('<H', corrupted[offset + 16:offset + 18])[0]
                if page_type == 0x26F1:  # Index
                    struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)  # prev_page
                    struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)  # next_page
                    struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)  # checksum
                    struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)  # checksum
        
        scenario = {
            "name": "SDI页+所有索引页损坏",
            "type": "元数据丢失",
            "corruption_type": "SDI损坏, 索引页链损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_7(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景7: Undo页损坏 + 所有Undo页损坏
        模拟事务日志完全丢失
        """
        corrupted = bytearray(data)
        
        # 所有Undo页损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                page_type = struct.unpack('<H', corrupted[offset + 16:offset + 18])[0]
                if page_type == 0x4572:  # Undo
                    struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                    struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "所有Undo页损坏",
            "type": "事务日志丢失",
            "corruption_type": "Undo页校验和损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_8(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景8: BLOB页损坏 + 所有BLOB页损坏
        模拟大对象数据完全丢失
        """
        corrupted = bytearray(data)
        
        # 所有BLOB页损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                page_type = struct.unpack('<H', corrupted[offset + 16:offset + 18])[0]
                if page_type == 0x4574:  # BLOB
                    struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                    struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "所有BLOB页损坏",
            "type": "大对象数据丢失",
            "corruption_type": "BLOB页校验和损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_9(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景9: 页头损坏 + 位翻转 + 随机字节
        模拟复合型损坏
        """
        corrupted = bytearray(data)
        
        # 页头损坏
        for page_no in range(5, 15):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<H', corrupted, offset + 16, 0xFFFF)
        
        # 位翻转
        for _ in range(100):
            pos = random.randint(0, len(corrupted) - 1)
            bit_pos = random.randint(0, 7)
            corrupted[pos] ^= (1 << bit_pos)
        
        # 随机字节
        for _ in range(50):
            pos = random.randint(0, len(corrupted) - 1)
            corrupted[pos] = random.randint(0, 255)
        
        scenario = {
            "name": "页头损坏+位翻转+随机字节",
            "type": "复合型损坏",
            "start_page": 5,
            "num_pages": 10,
            "bit_flips": 100,
            "random_bytes": 50,
            "corruption_type": "页头损坏, 位翻转, 随机字节"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_10(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景10: 文件中间截断 + 末尾损坏
        模拟文件部分丢失
        """
        # 截断中间部分
        start_truncate = 10 * self.page_size
        end_truncate = 15 * self.page_size
        
        data_before = data[:start_truncate]
        data_after = data[end_truncate:]
        truncated = data_before + data_after
        
        # 末尾损坏
        corrupted = bytearray(truncated)
        end_offset = len(corrupted) - self.page_size
        
        if end_offset >= 0:
            struct.pack_into('<I', corrupted, end_offset + 24, 0xDEADBEEF)
            struct.pack_into('<I', corrupted, end_offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "文件中间截断+末尾损坏",
            "type": "文件截断",
            "truncate_start": 10,
            "truncate_end": 15,
            "corruption_type": "中间截断, 末尾校验和损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_all_scenarios(self, data: bytearray) -> List[Tuple[bytearray, Dict]]:
        """生成所有场景"""
        results = []
        
        scenarios = [
            self.generate_scenario_1,
            self.generate_scenario_2,
            self.generate_scenario_3,
            self.generate_scenario_4,
            self.generate_scenario_5,
            self.generate_scenario_6,
            self.generate_scenario_7,
            self.generate_scenario_8,
            self.generate_scenario_9,
            self.generate_scenario_10,
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
        description='Multi-Page Bad Block Combination Test Generator'
    )
    parser.add_argument('--input', '-i', required=True, help='Input .ibd file')
    parser.add_argument('--output-dir', '-o', required=True, help='Output directory')
    parser.add_argument('--scenario', '-s', type=int, 
                       choices=range(1, 11),
                       help='Specific scenario number (1-10)')
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
    generator = MultiPageBadBlockGenerator(page_size=16384)
    
    # Ensure output directory exists
    os.makedirs(args.output_dir, exist_ok=True)
    
    # Generate scenarios
    if args.scenario:
        # Generate specific scenario
        scenario_funcs = [
            None,
            generator.generate_scenario_1,
            generator.generate_scenario_2,
            generator.generate_scenario_3,
            generator.generate_scenario_4,
            generator.generate_scenario_5,
            generator.generate_scenario_6,
            generator.generate_scenario_7,
            generator.generate_scenario_8,
            generator.generate_scenario_9,
            generator.generate_scenario_10,
        ]
        
        func = scenario_funcs[args.scenario]
        corrupted, info = func(bytearray(data))
        
        output_file = os.path.join(args.output_dir, f"test_multi_badblock_scenario{args.scenario}.ibd")
        with open(output_file, 'wb') as f:
            f.write(corrupted)
        
        if args.verbose:
            print(f"Scenario {args.scenario}: {info['name']}")
            print(f"  Type: {info['type']}")
            print(f"  Corruption: {info['corruption_type']}")
            print(f"  Input hash: {generate_hash(data)}")
            print(f"  Output hash: {generate_hash(corrupted)}")
            print(f"  Output: {output_file}")
        else:
            print(f"SCENARIO:{args.scenario}")
            print(f"NAME:{info['name']}")
            print(f"TYPE:{info['type']}")
            print(f"CORRUPTION:{info['corruption_type']}")
            print(f"HASH_INPUT:{generate_hash(data)}")
            print(f"HASH_OUTPUT:{generate_hash(corrupted)}")
            print(f"OUTPUT:{output_file}")
    else:
        # Generate all scenarios
        results = generator.generate_all_scenarios(bytearray(data))
        
        for i, (corrupted, info) in enumerate(results, 1):
            output_file = os.path.join(args.output_dir, f"test_multi_badblock_scenario{i}.ibd")
            with open(output_file, 'wb') as f:
                f.write(corrupted)
            
            if args.verbose:
                print(f"Scenario {i}: {info['name']}")
                print(f"  Type: {info['type']}")
                print(f"  Corruption: {info['corruption_type']}")
                print(f"  Output: {output_file}")
                print()
        
        print(f"Generated {len(results)} scenarios in {args.output_dir}")


if __name__ == '__main__':
    main()
