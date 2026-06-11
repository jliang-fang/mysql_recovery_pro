#!/usr/bin/env python3
"""
Metadata Loss Scenario Generator
模拟元数据完全丢失的极端场景
"""

import os
import sys
import random
import struct
import hashlib
import argparse
from typing import List, Tuple, Dict
from pathlib import Path


class MetadataLossGenerator:
    """元数据丢失生成器"""
    
    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.scenarios: List[Dict] = []
    
    def generate_scenario_1(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景1: SDI页完全损坏 + 所有页类型损坏
        模拟无架构恢复
        """
        corrupted = bytearray(data)
        
        # 找到并损坏SDI页
        sdi_pages = []
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                page_type = struct.unpack('<H', corrupted[offset + 16:offset + 18])[0]
                
                if page_type == 0x4573:  # SDI
                    sdi_pages.append(page_no)
                    # 完全损坏SDI页
                    struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)
                    struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)
                    struct.pack_into('<Q', corrupted, offset + 8, 0)
                    struct.pack_into('<H', corrupted, offset + 16, 0xFFFF)
                    struct.pack_into('<I', corrupted, offset + 20, 0xDEADBEEF)
                    struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                    struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        # 所有页类型损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<H', corrupted, offset + 16, 0xFFFF)
        
        scenario = {
            "name": "SDI页完全损坏+所有页类型损坏",
            "type": "无架构恢复",
            "sdi_pages": sdi_pages,
            "corruption_type": "SDI损坏, 页类型全部损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_2(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景2: 所有页头完全损坏（最极端）
        模拟文件头完全丢失
        """
        corrupted = bytearray(data)
        
        # 所有页头完全损坏
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
            "name": "所有页头完全损坏",
            "type": "完全损坏",
            "num_pages": len(data) // self.page_size,
            "corruption_type": "所有页头字段损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_3(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景3: 页0（文件头）损坏 + SDI页损坏
        模拟文件头和元数据同时丢失
        """
        corrupted = bytearray(data)
        
        # 页0损坏
        if self.page_size <= len(corrupted):
            struct.pack_into('<I', corrupted, 0, 0xDEADBEEF)   # space_id
            struct.pack_into('<I', corrupted, 4, 0xDEADBEEF)   # page_size
            struct.pack_into('<Q', corrupted, 8, 0)             # lsn
            struct.pack_into('<I', corrupted, 16, 0xFFFF)       # page_type
            struct.pack_into('<I', corrupted, 24, 0xDEADBEEF)   # checksum
            struct.pack_into('<I', corrupted, 28, 0xDEADBEEF)   # checksum
        
        # SDI页损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                page_type = struct.unpack('<H', corrupted[offset + 16:offset + 18])[0]
                if page_type == 0x4573:  # SDI
                    struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                    struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "页0(文件头)+SDI页损坏",
            "type": "文件头+元数据丢失",
            "file_header_page": 0,
            "corruption_type": "文件头损坏, SDI损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_4(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景4: Undo页全部损坏 + Redo日志丢失
        模拟事务日志完全丢失
        """
        corrupted = bytearray(data)
        
        # Undo页损坏
        undo_pages = []
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                page_type = struct.unpack('<H', corrupted[offset + 16:offset + 18])[0]
                if page_type == 0x4572:  # Undo
                    undo_pages.append(page_no)
                    struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                    struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "Undo页全部损坏",
            "type": "事务日志丢失",
            "undo_pages": undo_pages,
            "corruption_type": "Undo页校验和损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_5(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景5: BLOB页全部损坏 + 大对象数据丢失
        模拟BLOB数据完全丢失
        """
        corrupted = bytearray(data)
        
        # BLOB页损坏
        blob_pages = []
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                page_type = struct.unpack('<H', corrupted[offset + 16:offset + 18])[0]
                if page_type == 0x4574:  # BLOB
                    blob_pages.append(page_no)
                    struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)
                    struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)
        
        scenario = {
            "name": "BLOB页全部损坏",
            "type": "大对象数据丢失",
            "blob_pages": blob_pages,
            "corruption_type": "BLOB页校验和损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_6(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景6: 页链完全破坏 + 页头损坏
        模拟页链表完全丢失
        """
        corrupted = bytearray(data)
        
        # 所有页链破坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 0, 0xDEADBEEF)  # prev_page
                struct.pack_into('<I', corrupted, offset + 4, 0xDEADBEEF)  # next_page
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)  # checksum
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)  # checksum
        
        scenario = {
            "name": "页链完全破坏",
            "type": "页链丢失",
            "corruption_type": "prev_page和next_page全部损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_7(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景7: 页号全部错误 + 页头损坏
        模拟页号映射完全错误
        """
        corrupted = bytearray(data)
        
        # 所有页号损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 20, 0xDEADBEEF)  # page_no
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)  # checksum
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)  # checksum
        
        scenario = {
            "name": "页号全部错误",
            "type": "页号映射错误",
            "corruption_type": "所有页号字段损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_8(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景8: LSN全部清零 + 页头损坏
        模拟LSN日志完全丢失
        """
        corrupted = bytearray(data)
        
        # 所有LSN清零
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<Q', corrupted, offset + 8, 0)             # LSN
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)   # checksum
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)   # checksum
        
        scenario = {
            "name": "LSN全部清零",
            "type": "LSN日志丢失",
            "corruption_type": "所有LSN字段清零"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_9(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景9: 空间ID损坏 + 页号损坏 + 校验和损坏
        模拟空间元数据完全丢失
        """
        corrupted = bytearray(data)
        
        # 空间ID损坏
        if self.page_size <= len(corrupted):
            struct.pack_into('<I', corrupted, 16, 0xDEADBEEF)  # space_id
        
        # 所有页号和校验和损坏
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(corrupted):
                struct.pack_into('<I', corrupted, offset + 20, 0xDEADBEEF)   # page_no
                struct.pack_into('<I', corrupted, offset + 24, 0xDEADBEEF)   # checksum
                struct.pack_into('<I', corrupted, offset + 28, 0xDEADBEEF)   # checksum
        
        scenario = {
            "name": "空间ID+页号+校验和损坏",
            "type": "空间元数据丢失",
            "corruption_type": "space_id损坏, page_no损坏, checksum损坏"
        }
        self.scenarios.append(scenario)
        
        return corrupted, scenario
    
    def generate_scenario_10(self, data: bytearray) -> Tuple[bytearray, Dict]:
        """
        场景10: 所有关键字段全部损坏（最极端）
        模拟文件完全无法识别
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
                struct.pack_into('<H', corrupted, offset + 36, 0xFFFF)       # arch_log_no (max valid value)
        
        scenario = {
            "name": "所有关键字段全部损坏",
            "type": "完全无法识别",
            "num_pages": len(data) // self.page_size,
            "corruption_type": "所有关键字段损坏"
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
        description='Metadata Loss Scenario Generator'
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
    generator = MetadataLossGenerator(page_size=16384)
    
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
        
        output_file = os.path.join(args.output_dir, f"test_metadata_loss_scenario{args.scenario}.ibd")
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
            output_file = os.path.join(args.output_dir, f"test_metadata_loss_scenario{i}.ibd")
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
