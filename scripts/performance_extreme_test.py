#!/usr/bin/env python3
"""
Performance极限测试生成器
为性能极限生成极端测试场景
"""

import os
import sys
import random
import struct
import hashlib
import argparse
import time
from typing import List, Tuple, Dict
from pathlib import Path


class PerformanceExtremeTestGenerator:
    """性能极限测试生成器"""
    
    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.scenarios: List[Dict] = []
    
    def generate_performance_test_1(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        性能测试1: 最小页大小(4KB)
        """
        # 生成4KB文件
        corrupted = bytearray(4096)
        
        # 填充基本页结构
        if len(corrupted) >= 52:
            struct.pack_into('<I', corrupted, 0, 0)          # prev_page
            struct.pack_into('<I', corrupted, 4, 0xFFFFFFFF) # next_page
            struct.pack_into('<Q', corrupted, 8, 1)          # LSN
            struct.pack_into('<I', corrupted, 16, 0x26F1)    # page_type (index)
            struct.pack_into('<I', corrupted, 20, 0)          # page_no
            struct.pack_into('<I', corrupted, 24, 0)          # checksum
            struct.pack_into('<I', corrupted, 28, 0)          # checksum
        
        scenario = {
            "name": "性能测试1 - 最小页大小(4KB)",
            "performance_test": 1,
            "corruption_type": "最小页大小",
            "file_size": 4096,
            "page_size": 4096,
            "pages": 1
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_performance_test_2(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        性能测试2: 最大页大小(64KB)
        """
        # 生成64KB文件
        corrupted = bytearray(65536)
        
        # 填充基本页结构
        if len(corrupted) >= 52:
            struct.pack_into('<I', corrupted, 0, 0)          # prev_page
            struct.pack_into('<I', corrupted, 4, 0xFFFFFFFF) # next_page
            struct.pack_into('<Q', corrupted, 8, 1)          # LSN
            struct.pack_into('<I', corrupted, 16, 0x26F1)    # page_type (index)
            struct.pack_into('<I', corrupted, 20, 0)          # page_no
            struct.pack_into('<I', corrupted, 24, 0)          # checksum
            struct.pack_into('<I', corrupted, 28, 0)          # checksum
        
        scenario = {
            "name": "性能测试2 - 最大页大小(64KB)",
            "performance_test": 2,
            "corruption_type": "最大页大小",
            "file_size": 65536,
            "page_size": 65536,
            "pages": 1
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_performance_test_3(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        性能测试3: 超大文件(1GB+)
        """
        # 生成1GB文件
        file_size = 1024 * 1024 * 1024  # 1GB
        corrupted = bytearray(file_size)
        
        # 填充基本页结构
        for page_no in range(file_size // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 0, 0)          # prev_page
                struct.pack_into('<I', corrupted, offset + 4, 0xFFFFFFFF) # next_page
                struct.pack_into('<Q', corrupted, offset + 8, 1)          # LSN
                struct.pack_into('<I', corrupted, offset + 16, 0x26F1)    # page_type
                struct.pack_into('<I', corrupted, offset + 20, page_no)   # page_no
                struct.pack_into('<I', corrupted, offset + 24, 0)          # checksum
                struct.pack_into('<I', corrupted, offset + 28, 0)          # checksum
        
        scenario = {
            "name": "性能测试3 - 超大文件(1GB+)",
            "performance_test": 3,
            "corruption_type": "超大文件",
            "file_size": file_size,
            "page_size": self.page_size,
            "pages": file_size // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_performance_test_4(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        性能测试4: 高密度记录页
        """
        corrupted = bytearray(data)
        
        # 创建高密度记录页
        for page_no in range(1, min(10, len(data) // self.page_size)):
            page_offset = page_no * self.page_size + 38
            
            if page_offset + 20 <= len(corrupted):
                # 设置最大heap_top
                struct.pack_into('<H', corrupted, page_offset + 2, 0xFFFE)  # Max heap_top
                struct.pack_into('<H', corrupted, page_offset + 6, 0xFFFE)  # Max free_offset
        
        scenario = {
            "name": "性能测试4 - 高密度记录页",
            "performance_test": 4,
            "corruption_type": "高密度记录页",
            "pages_corrupted": min(10, len(data) // self.page_size - 1)
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_performance_test_5(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        性能测试5: 长页链
        """
        corrupted = bytearray(data)
        
        # 创建长页链
        total_pages = len(data) // self.page_size
        
        for page_no in range(1, total_pages):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                # 设置正确的页链
                struct.pack_into('<I', corrupted, offset + 0, (page_no - 1) * self.page_size // self.page_size)
                struct.pack_into('<I', corrupted, offset + 4, (page_no + 1) * self.page_size // self.page_size)
        
        scenario = {
            "name": "性能测试5 - 长页链",
            "performance_test": 5,
            "corruption_type": "长页链",
            "total_pages": total_pages
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_performance_test_6(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        性能测试6: 并发访问模拟
        """
        corrupted = bytearray(data)
        
        # 模拟并发访问 - 随机损坏多个页
        num_pages = min(100, len(data) // self.page_size)
        
        for _ in range(num_pages):
            page_no = random.randint(1, len(data) // self.page_size - 1)
            offset = page_no * self.page_size
            
            if offset + 32 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "性能测试6 - 并发访问模拟",
            "performance_test": 6,
            "corruption_type": "并发访问模拟",
            "pages_corrupted": num_pages
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_performance_test_7(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        性能测试7: 高频LSN更新
        """
        corrupted = bytearray(data)
        
        # LSN全部设置为最大值
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<Q', corrupted, offset + 8, 0xFFFFFFFFFFFFFFFF)  # Max LSN
        
        scenario = {
            "name": "性能测试7 - 高频LSN更新",
            "performance_test": 7,
            "corruption_type": "高频LSN更新",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_performance_test_8(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        性能测试8: 大量SDI页
        """
        corrupted = bytearray(data)
        
        # 创建多个SDI页
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<H', corrupted, offset + 16, 0x4573)  # SDI page type
        
        scenario = {
            "name": "性能测试8 - 大量SDI页",
            "performance_test": 8,
            "corruption_type": "大量SDI页",
            "pages_corrupted": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_all_performance_tests(self, data: bytearray) -> List[Tuple[bytearray, Dict]]:
        """生成所有性能极限测试场景"""
        results = []
        
        scenarios = [
            self.generate_performance_test_1,
            self.generate_performance_test_2,
            self.generate_performance_test_3,
            self.generate_performance_test_4,
            self.generate_performance_test_5,
            self.generate_performance_test_6,
            self.generate_performance_test_7,
            self.generate_performance_test_8,
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
        description='Performance Extreme Test Generator'
    )
    parser.add_argument('--input', '-i', required=True, help='Input .ibd file')
    parser.add_argument('--output-dir', '-o', required=True, help='Output directory')
    parser.add_argument('--test', '-t', type=int,
                       choices=range(1, 9),
                       help='Specific performance test number (1-8)')
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
    generator = PerformanceExtremeTestGenerator(page_size=16384)
    
    # Ensure output directory exists
    os.makedirs(args.output_dir, exist_ok=True)
    
    # Generate scenarios
    if args.test:
        scenario_map = {
            1: generator.generate_performance_test_1,
            2: generator.generate_performance_test_2,
            3: generator.generate_performance_test_3,
            4: generator.generate_performance_test_4,
            5: generator.generate_performance_test_5,
            6: generator.generate_performance_test_6,
            7: generator.generate_performance_test_7,
            8: generator.generate_performance_test_8,
        }
        
        func = scenario_map[args.test]
        corrupted, info = func(data)
        
        output_file = os.path.join(args.output_dir, f"test_performance_{args.test}_extreme.ibd")
        with open(output_file, 'wb') as f:
            f.write(corrupted)
        
        if args.verbose:
            print(f"Performance Test: {args.test}")
            print(f"Scenario: {info['name']}")
            print(f"  Type: {info['corruption_type']}")
            if 'file_size' in info:
                print(f"  File Size: {info['file_size']} bytes")
            if 'pages' in info:
                print(f"  Pages: {info['pages']}")
            print(f"  Input hash: {generate_hash(data)}")
            print(f"  Output hash: {generate_hash(corrupted)}")
            print(f"  Output: {output_file}")
        else:
            print(f"PERFORMANCE_TEST:{args.test}")
            print(f"SCENARIO:{info['name']}")
            print(f"CORRUPTION:{info['corruption_type']}")
            if 'file_size' in info:
                print(f"FILE_SIZE:{info['file_size']}")
            if 'pages' in info:
                print(f"PAGES:{info['pages']}")
            print(f"HASH_INPUT:{generate_hash(data)}")
            print(f"HASH_OUTPUT:{generate_hash(corrupted)}")
            print(f"OUTPUT:{output_file}")
    else:
        # Generate all scenarios
        results = generator.generate_all_performance_tests(data)
        
        for i, (corrupted, info) in enumerate(results, 1):
            output_file = os.path.join(args.output_dir, f"test_performance_{i}_extreme.ibd")
            with open(output_file, 'wb') as f:
                f.write(corrupted)
            
            if args.verbose:
                print(f"Performance Test {i}: {info['name']}")
                print(f"  Type: {info['corruption_type']}")
                if 'file_size' in info:
                    print(f"  File Size: {info['file_size']} bytes")
                if 'pages' in info:
                    print(f"  Pages: {info['pages']}")
                print(f"  Output: {output_file}")
                print()
        
        print(f"Generated {len(results)} performance extreme scenarios in {args.output_dir}")


if __name__ == '__main__':
    main()
