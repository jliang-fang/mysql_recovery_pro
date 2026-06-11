#!/usr/bin/env python3
"""
Stress Test Suite Generator
压力测试套件生成器
"""

import os
import sys
import random
import struct
import hashlib
import argparse
from typing import List, Tuple, Dict
from pathlib import Path


class StressTestSuiteGenerator:
    """压力测试套件生成器"""
    
    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.scenarios: List[Dict] = []
    
    def generate_stress_test_1(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        压力测试1: 高强度随机损坏
        """
        corrupted = bytearray(data)
        
        # 随机损坏大量数据
        num_corruptions = min(1000, len(data))
        
        for _ in range(num_corruptions):
            pos = random.randint(0, len(corrupted) - 1)
            
            if random.random() < 0.3:
                # 位翻转
                bit_pos = random.randint(0, 7)
                corrupted[pos] ^= (1 << bit_pos)
            else:
                # 字节覆盖
                corrupted[pos] = random.randint(0, 255)
        
        scenario = {
            "name": "压力测试1 - 高强度随机损坏",
            "stress_test": 1,
            "corruption_type": "高强度随机损坏",
            "corruptions": num_corruptions,
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_stress_test_2(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        压力测试2: 连续坏块链
        """
        corrupted = bytearray(data)
        
        # 创建连续坏块链
        start_page = 5
        num_pages = min(50, len(data) // self.page_size - 10)
        
        for page_no in range(start_page, start_page + num_pages):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<H', corrupted, offset + 16, 0xFFFF)
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "压力测试2 - 连续坏块链",
            "stress_test": 2,
            "corruption_type": "连续坏块链",
            "start_page": start_page,
            "num_pages": num_pages,
            "pages_corrupted": num_pages
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_stress_test_3(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        压力测试3: 所有页头+所有校验和+所有页链
        """
        corrupted = bytearray(data)
        
        # 所有页头损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<H', corrupted, offset + 16, 0xFFFF)
        
        # 所有校验和损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 32 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        # 所有页链损坏
        for page_no in range(1, len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)
        
        scenario = {
            "name": "压力测试3 - 全面损坏",
            "stress_test": 3,
            "corruption_type": "页头+校验和+页链全部损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_stress_test_4(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        压力测试4: 页号循环引用
        """
        corrupted = bytearray(data)
        
        # 创建页号循环引用
        total_pages = len(data) // self.page_size
        
        for page_no in range(1, total_pages):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                # 设置循环引用
                next_page = (page_no % total_pages) * self.page_size // self.page_size
                struct.pack_into('<I', corrupted, offset + 4, next_page)
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "压力测试4 - 页号循环引用",
            "stress_test": 4,
            "corruption_type": "页号循环引用",
            "total_pages": total_pages,
            "pages_corrupted": total_pages - 1
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_stress_test_5(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        压力测试5: LSN全部清零+所有校验和损坏
        """
        corrupted = bytearray(data)
        
        # LSN全部清零
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<Q', corrupted, offset + 8, 0)
        
        # 所有校验和损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 32 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "压力测试5 - LSN清零+校验和损坏",
            "stress_test": 5,
            "corruption_type": "LSN清零+校验和损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_stress_test_6(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        压力测试6: 记录链全部损坏
        """
        corrupted = bytearray(data)
        
        # 记录链全部损坏
        for page_no in range(1, len(data) // self.page_size):
            page_offset = page_no * self.page_size + 38
            
            if page_offset + 20 <= len(corrupted):
                struct.pack_into('<H', corrupted, page_offset + 2, 0xFFFF)   # heap_top
                struct.pack_into('<H', corrupted, page_offset + 6, 0xFFFF)   # free_offset
                struct.pack_into('<H', corrupted, page_offset + 8, 0xFFFF)   # garbage_end
        
        scenario = {
            "name": "压力测试6 - 记录链全部损坏",
            "stress_test": 6,
            "corruption_type": "记录链全部损坏",
            "pages_corrupted": len(data) // self.page_size - 1
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_stress_test_7(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        压力测试7: 空间ID损坏+页号全部错误
        """
        corrupted = bytearray(data)
        
        # 空间ID损坏
        if self.page_size <= len(corrupted):
            struct.pack_into('<I', corrupted, 16, 0xDEADBEEF)
        
        # 页号全部错误
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 20, 0xDEADBEEF)
        
        scenario = {
            "name": "压力测试7 - 空间ID+页号损坏",
            "stress_test": 7,
            "corruption_type": "空间ID+页号损坏",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_stress_test_8(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        压力测试8: 混合所有损坏类型
        """
        corrupted = bytearray(data)
        
        # 所有类型混合损坏
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
                struct.pack_into('<H', corrupted, offset + 36, 0xFFFF)       # arch_log_no
        
        scenario = {
            "name": "压力测试8 - 混合所有损坏类型",
            "stress_test": 8,
            "corruption_type": "混合所有损坏类型",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_all_stress_tests(self, data: bytearray) -> List[Tuple[bytearray, Dict]]:
        """生成所有压力测试场景"""
        results = []
        
        scenarios = [
            self.generate_stress_test_1,
            self.generate_stress_test_2,
            self.generate_stress_test_3,
            self.generate_stress_test_4,
            self.generate_stress_test_5,
            self.generate_stress_test_6,
            self.generate_stress_test_7,
            self.generate_stress_test_8,
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
        description='Stress Test Suite Generator'
    )
    parser.add_argument('--input', '-i', required=True, help='Input .ibd file')
    parser.add_argument('--output-dir', '-o', required=True, help='Output directory')
    parser.add_argument('--test', '-t', type=int,
                       choices=range(1, 9),
                       help='Specific stress test number (1-8)')
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
    generator = StressTestSuiteGenerator(page_size=16384)
    
    # Ensure output directory exists
    os.makedirs(args.output_dir, exist_ok=True)
    
    # Generate scenarios
    if args.test:
        scenario_map = {
            1: generator.generate_stress_test_1,
            2: generator.generate_stress_test_2,
            3: generator.generate_stress_test_3,
            4: generator.generate_stress_test_4,
            5: generator.generate_stress_test_5,
            6: generator.generate_stress_test_6,
            7: generator.generate_stress_test_7,
            8: generator.generate_stress_test_8,
        }
        
        func = scenario_map[args.test]
        corrupted, info = func(bytearray(data))
        
        output_file = os.path.join(args.output_dir, f"test_stress_{args.test}_extreme.ibd")
        with open(output_file, 'wb') as f:
            f.write(corrupted)
        
        if args.verbose:
            print(f"Stress Test: {args.test}")
            print(f"Scenario: {info['name']}")
            print(f"  Type: {info['corruption_type']}")
            if 'corruptions' in info:
                print(f"  Corruptions: {info['corruptions']}")
            if 'num_pages' in info:
                print(f"  Pages: {info['num_pages']}")
            print(f"  Input hash: {generate_hash(data)}")
            print(f"  Output hash: {generate_hash(corrupted)}")
            print(f"  Output: {output_file}")
        else:
            print(f"STRESS_TEST:{args.test}")
            print(f"SCENARIO:{info['name']}")
            print(f"CORRUPTION:{info['corruption_type']}")
            if 'corruptions' in info:
                print(f"CORRUPTIONS:{info['corruptions']}")
            if 'num_pages' in info:
                print(f"PAGES:{info['num_pages']}")
            print(f"HASH_INPUT:{generate_hash(data)}")
            print(f"HASH_OUTPUT:{generate_hash(corrupted)}")
            print(f"OUTPUT:{output_file}")
    else:
        # Generate all scenarios
        results = generator.generate_all_stress_tests(bytearray(data))
        
        for i, (corrupted, info) in enumerate(results, 1):
            output_file = os.path.join(args.output_dir, f"test_stress_{i}_extreme.ibd")
            with open(output_file, 'wb') as f:
                f.write(corrupted)
            
            if args.verbose:
                print(f"Stress Test {i}: {info['name']}")
                print(f"  Type: {info['corruption_type']}")
                if 'corruptions' in info:
                    print(f"  Corruptions: {info['corruptions']}")
                if 'num_pages' in info:
                    print(f"  Pages: {info['num_pages']}")
                print(f"  Output: {output_file}")
                print()
        
        print(f"Generated {len(results)} stress test scenarios in {args.output_dir}")


if __name__ == '__main__':
    main()
