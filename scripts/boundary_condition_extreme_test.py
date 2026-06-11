#!/usr/bin/env python3
"""
Boundary Condition Extreme Test Generator
为边界条件生成极端测试场景
"""

import os
import sys
import random
import struct
import hashlib
import argparse
from typing import List, Tuple, Dict
from pathlib import Path


class BoundaryConditionExtremeTestGenerator:
    """边界条件极端测试生成器"""
    
    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.scenarios: List[Dict] = []
    
    def generate_empty_file_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        空文件极端测试 - 文件大小为0
        """
        # 返回空数据
        corrupted = bytearray(0)
        
        scenario = {
            "name": "空文件极端测试",
            "boundary_type": "empty_file",
            "corruption_type": "文件为空",
            "original_size": len(data),
            "corrupted_size": 0
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_minimal_file_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        最小文件极端测试 - 仅1个字节
        """
        # 返回1字节
        corrupted = bytearray(1)
        corrupted[0] = 0xFF
        
        scenario = {
            "name": "最小文件极端测试",
            "boundary_type": "minimal_file",
            "corruption_type": "文件仅1字节",
            "original_size": len(data),
            "corrupted_size": 1
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_single_page_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        单页文件极端测试 - 仅1个页
        """
        # 仅保留1个页
        corrupted = bytearray(data[:self.page_size])
        
        # 损坏这个页
        if len(corrupted) >= 52:
            struct.pack_into('<H', corrupted, 16, 0xFFFF)
            struct.pack_into('<I', corrupted, 24, 0xDEADBEEF)
            struct.pack_into('<I', corrupted, 28, 0xDEADBEEF)
        
        scenario = {
            "name": "单页文件极端测试",
            "boundary_type": "single_page",
            "corruption_type": "仅1个页且损坏",
            "original_size": len(data),
            "corrupted_size": len(corrupted),
            "pages": 1
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_large_file_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        大文件极端测试 - 扩展到10倍大小
        """
        # 扩展文件
        corrupted = bytearray(data)
        for _ in range(9):
            corrupted.extend(data)
        
        # 损坏随机页
        for _ in range(100):
            page_no = random.randint(0, len(corrupted) // self.page_size - 1)
            offset = page_no * self.page_size
            
            if offset + 32 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "大文件极端测试",
            "boundary_type": "large_file",
            "corruption_type": "扩展10倍+随机损坏",
            "original_size": len(data),
            "corrupted_size": len(corrupted),
            "pages": len(corrupted) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_odd_page_size_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        奇数页大小极端测试 - 使用非标准页大小
        """
        # 保持数据但标记为不同页大小
        corrupted = bytearray(data)
        
        # 损坏页头
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "奇数页大小极端测试",
            "boundary_type": "odd_page_size",
            "corruption_type": "非标准页大小标记",
            "original_size": len(data),
            "corrupted_size": len(corrupted),
            "pages": len(data) // self.page_size
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_truncated_file_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        截断文件极端测试 - 截断到50%
        """
        # 截断到50%
        corrupted = bytearray(data[:len(data) // 2])
        
        scenario = {
            "name": "截断文件极端测试",
            "boundary_type": "truncated_file",
            "corruption_type": "截断到50%",
            "original_size": len(data),
            "corrupted_size": len(corrupted),
            "truncated_percent": 50
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_fragmented_file_extreme(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        碎片化文件极端测试 - 创建多个小片段
        """
        # 创建碎片化文件
        corrupted = bytearray()
        
        # 分割成多个片段
        fragment_size = len(data) // 10
        
        for i in range(10):
            start = i * fragment_size
            end = start + fragment_size
            
            if end <= len(data):
                fragment = data[start:end]
                
                # 损坏每个片段
                if len(fragment) >= 52:
                    struct.pack_into('<H', fragment, 16, 0xFFFF)
                    struct.pack_into('<I', fragment, 24, 0xDEADBEEF)
                    struct.pack_into('<I', fragment, 28, 0xDEADBEEF)
                
                corrupted.extend(fragment)
        
        scenario = {
            "name": "碎片化文件极端测试",
            "boundary_type": "fragmented_file",
            "corruption_type": "分割成10个片段+损坏",
            "original_size": len(data),
            "corrupted_size": len(corrupted),
            "fragments": 10
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_all_boundary_scenarios(self, data: bytearray) -> List[Tuple[bytearray, Dict]]:
        """生成所有边界条件极端场景"""
        results = []
        
        scenarios = [
            self.generate_empty_file_extreme,
            self.generate_minimal_file_extreme,
            self.generate_single_page_extreme,
            self.generate_large_file_extreme,
            self.generate_odd_page_size_extreme,
            self.generate_truncated_file_extreme,
            self.generate_fragmented_file_extreme,
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
        description='Boundary Condition Extreme Test Generator'
    )
    parser.add_argument('--input', '-i', required=True, help='Input .ibd file')
    parser.add_argument('--output-dir', '-o', required=True, help='Output directory')
    parser.add_argument('--scenario', '-s',
                       choices=['empty', 'minimal', 'single_page', 'large', 'odd_page_size',
                               'truncated', 'fragmented', 'all'],
                       help='Specific boundary condition scenario')
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
    generator = BoundaryConditionExtremeTestGenerator(page_size=16384)
    
    # Ensure output directory exists
    os.makedirs(args.output_dir, exist_ok=True)
    
    # Generate scenarios
    if args.scenario and args.scenario != 'all':
        scenario_map = {
            'empty': generator.generate_empty_file_extreme,
            'minimal': generator.generate_minimal_file_extreme,
            'single_page': generator.generate_single_page_extreme,
            'large': generator.generate_large_file_extreme,
            'odd_page_size': generator.generate_odd_page_size_extreme,
            'truncated': generator.generate_truncated_file_extreme,
            'fragmented': generator.generate_fragmented_file_extreme,
        }
        
        func = scenario_map[args.scenario]
        corrupted, info = func(data)
        
        output_file = os.path.join(args.output_dir, f"test_boundary_{args.scenario}_extreme.ibd")
        with open(output_file, 'wb') as f:
            f.write(corrupted)
        
        if args.verbose:
            print(f"Boundary Type: {args.scenario}")
            print(f"Scenario: {info['name']}")
            print(f"  Type: {info['corruption_type']}")
            print(f"  Original: {info['original_size']} bytes")
            print(f"  Corrupted: {info['corrupted_size']} bytes")
            print(f"  Input hash: {generate_hash(data)}")
            print(f"  Output hash: {generate_hash(corrupted)}")
            print(f"  Output: {output_file}")
        else:
            print(f"BOUNDARY_TYPE:{args.scenario}")
            print(f"SCENARIO:{info['name']}")
            print(f"CORRUPTION:{info['corruption_type']}")
            print(f"ORIGINAL_SIZE:{info['original_size']}")
            print(f"CORRUPTED_SIZE:{info['corrupted_size']}")
            print(f"HASH_INPUT:{generate_hash(data)}")
            print(f"HASH_OUTPUT:{generate_hash(corrupted)}")
            print(f"OUTPUT:{output_file}")
    else:
        # Generate all scenarios
        results = generator.generate_all_boundary_scenarios(data)
        
        for i, (corrupted, info) in enumerate(results, 1):
            output_file = os.path.join(args.output_dir, f"test_boundary_{info['boundary_type']}_extreme.ibd")
            with open(output_file, 'wb') as f:
                f.write(corrupted)
            
            if args.verbose:
                print(f"Scenario {i}: {info['name']}")
                print(f"  Boundary Type: {info['boundary_type']}")
                print(f"  Type: {info['corruption_type']}")
                print(f"  Original: {info['original_size']} bytes")
                print(f"  Corrupted: {info['corrupted_size']} bytes")
                print(f"  Output: {output_file}")
                print()
        
        print(f"Generated {len(results)} boundary condition extreme scenarios in {args.output_dir}")


if __name__ == '__main__':
    main()
