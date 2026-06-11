#!/usr/bin/env python3
"""
Extreme Chaos Monkey - Advanced File System Level Disaster Simulator
模拟更极端的数据库灾难场景
"""

import os
import sys
import random
import struct
import hashlib
import argparse
from typing import List, Tuple, Dict, Optional
from pathlib import Path
import mmap


class ExtremeChaosMonkey:
    """极端混沌工程模拟器 - 文件系统级灾难"""
    
    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.corruption_log: List[Dict] = []
    
    def create_file_hole(self, data: bytearray, start_page: int, num_pages: int) -> bytearray:
        """在文件中间创建空洞（模拟文件系统损坏）"""
        if start_page * self.page_size >= len(data):
            return data
        
        end_page = min(start_page + num_pages, len(data) // self.page_size)
        
        # 将空洞后的数据向前移动
        hole_start = start_page * self.page_size
        hole_end = end_page * self.page_size
        data_after_hole = data[hole_end:]
        
        # 填充空洞为零
        data = data[:hole_start] + bytearray(len(data_after_hole))
        data.extend(data_after_hole)
        
        self.corruption_log.append({
            "type": "file_hole",
            "start_page": start_page,
            "num_pages": num_pages,
            "hole_size": hole_end - hole_start
        })
        
        return data
    
    def truncate_file_middle(self, data: bytearray, start_page: int, num_pages: int) -> bytearray:
        """截断文件中间部分（模拟部分数据丢失）"""
        if start_page * self.page_size >= len(data):
            return data
        
        end_page = min(start_page + num_pages, len(data) // self.page_size)
        
        # 保留前面和后面的数据
        data_before = data[:start_page * self.page_size]
        data_after = data[end_page * self.page_size:]
        
        # 合并
        data = data_before + data_after
        
        self.corruption_log.append({
            "type": "truncate_middle",
            "start_page": start_page,
            "num_pages": num_pages,
            "lost_size": (end_page - start_page) * self.page_size
        })
        
        return data
    
    def duplicate_pages(self, data: bytearray, source_page: int, num_copies: int) -> bytearray:
        """复制某些页到文件末尾（模拟重复数据）"""
        if source_page * self.page_size >= len(data):
            return data
        
        source_data = data[source_page * self.page_size:(source_page + 1) * self.page_size]
        
        for _ in range(num_copies):
            data.extend(source_data)
        
        self.corruption_log.append({
            "type": "duplicate_pages",
            "source_page": source_page,
            "num_copies": num_copies
        })
        
        return data
    
    def scramble_page_order(self, data: bytearray, num_groups: int = 4) -> Tuple[bytearray, List[int]]:
        """打乱页的顺序（模拟页表损坏）"""
        total_pages = len(data) // self.page_size
        if total_pages < num_groups:
            return data, []
        
        pages = []
        for i in range(total_pages):
            pages.append(data[i * self.page_size:(i + 1) * self.page_size])
        
        # 分组并打乱
        group_size = max(1, total_pages // num_groups)
        groups = []
        for i in range(0, total_pages, group_size):
            groups.append(pages[i:i + group_size])
        
        # 打乱组顺序
        random.shuffle(groups)
        
        # 重新组合
        scrambled = bytearray()
        for group in groups:
            for page in group:
                scrambled.extend(page)
        
        self.corruption_log.append({
            "type": "scramble_order",
            "num_groups": num_groups
        })
        
        return scrambled, list(range(total_pages))
    
    def inject_random_bytes(self, data: bytearray, num_bytes: int = 1000) -> bytearray:
        """在随机位置注入随机字节（模拟位翻转）"""
        for _ in range(num_bytes):
            pos = random.randint(0, len(data) - 1)
            data[pos] = random.randint(0, 255)
        
        self.corruption_log.append({
            "type": "random_bytes",
            "num_bytes": num_bytes
        })
        
        return data
    
    def overwrite_with_pattern(self, data: bytearray, pattern: bytes, 
                               interval: int = 1000) -> bytearray:
        """用特定模式覆盖数据"""
        for i in range(0, len(data), interval):
            end = min(i + len(pattern), len(data))
            data[i:end] = pattern[:end - i]
        
        self.corruption_log.append({
            "type": "pattern_overwrite",
            "pattern": pattern.hex(),
            "interval": interval
        })
        
        return data
    
    def create_sparse_file(self, data: bytearray, num_holes: int = 5) -> Tuple[bytearray, List[Tuple[int, int]]]:
        """创建稀疏文件（多个空洞）"""
        holes = []
        total_pages = len(data) // self.page_size
        
        for _ in range(num_holes):
            start_page = random.randint(1, total_pages - 5)
            num_pages = random.randint(1, 3)
            
            hole_start = start_page * self.page_size
            hole_end = (start_page + num_pages) * self.page_size
            
            # 移除这部分数据
            data = data[:hole_start] + data[hole_end:]
            
            holes.append((start_page, num_pages))
        
        self.corruption_log.append({
            "type": "sparse_file",
            "num_holes": num_holes,
            "holes": holes
        })
        
        return data, holes
    
    def bit_flip(self, data: bytearray, num_bits: int = 100) -> bytearray:
        """翻转随机位（模拟硬件故障）"""
        for _ in range(num_bits):
            pos = random.randint(0, len(data) - 1)
            bit_pos = random.randint(0, 7)
            data[pos] ^= (1 << bit_pos)
        
        self.corruption_log.append({
            "type": "bit_flip",
            "num_bits": num_bits
        })
        
        return data
    
    def corrupt_page_headers_sequential(self, data: bytearray, start_page: int, 
                                        num_pages: int) -> bytearray:
        """连续损坏多个页的页头"""
        for page_no in range(start_page, min(start_page + num_pages, len(data) // self.page_size)):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(data):
                # Corrupt page type
                struct.pack_into('<H', data, offset + 16, 0xFFFF)
                # Corrupt checksum
                struct.pack_into('<I', data, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', data, offset + 28, 0xDEADBEEF)
        
        self.corruption_log.append({
            "type": "sequential_headers",
            "start_page": start_page,
            "num_pages": num_pages
        })
        
        return data
    
    def corrupt_all_checksums(self, data: bytearray) -> bytearray:
        """损坏所有页的校验和"""
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 32 <= len(data):
                struct.pack_into('<I', data, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', data, offset + 28, 0xDEADBEEF)
        
        self.corruption_log.append({
            "type": "all_checksums",
            "num_pages": len(data) // self.page_size
        })
        
        return data
    
    def corrupt_record_headers(self, data: bytearray, num_records: int = 10) -> bytearray:
        """损坏记录头（模拟记录层损坏）"""
        for _ in range(num_records):
            # 随机选择一个页
            page_no = random.randint(1, len(data) // self.page_size - 1)
            offset = page_no * self.page_size + 38  # Page header
            
            if offset + 20 <= len(data):
                # Corrupt heap_top
                struct.pack_into('<H', data, offset + 2, 0xFFFF)
                # Corrupt free_offset
                struct.pack_into('<H', data, offset + 6, 0xFFFF)
        
        self.corruption_log.append({
            "type": "record_headers",
            "num_records": num_records
        })
        
        return data
    
    def corrupt_btree_internal_nodes(self, data: bytearray, num_nodes: int = 5) -> bytearray:
        """损坏 B+Tree 内部节点"""
        for _ in range(num_nodes):
            page_no = random.randint(1, len(data) // self.page_size - 1)
            offset = page_no * self.page_size
            
            if offset + 52 <= len(data):
                # Corrupt node pointer (next_page)
                struct.pack_into('<I', data, offset + 4, 0xDEADBEEF)
                # Corrupt prev_page
                struct.pack_into('<I', data, offset + 0, 0xDEADBEEF)
        
        self.corruption_log.append({
            "type": "btree_internal",
            "num_nodes": num_nodes
        })
        
        return data
    
    def corrupt_sdi_pages(self, data: bytearray) -> Tuple[bytearray, List[int]]:
        """损坏所有 SDI 页"""
        corrupted_pages = []
        
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(data):
                page_type = struct.unpack('<H', data[offset + 16:offset + 18])[0]
                if page_type == 0x4573:  # SDI page type
                    struct.pack_into('<I', data, offset + 24, 0xDEADBEEF)
                    struct.pack_into('<I', data, offset + 28, 0xDEADBEEF)
                    corrupted_pages.append(page_no)
        
        self.corruption_log.append({
            "type": "sdi_pages",
            "num_pages": len(corrupted_pages)
        })
        
        return data, corrupted_pages
    
    def corrupt_undo_pages(self, data: bytearray) -> Tuple[bytearray, List[int]]:
        """损坏所有 undo log 页"""
        corrupted_pages = []
        
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(data):
                page_type = struct.unpack('<H', data[offset + 16:offset + 18])[0]
                if page_type == 0x4572:  # Undo log page type
                    struct.pack_into('<I', data, offset + 24, 0xDEADBEEF)
                    struct.pack_into('<I', data, offset + 28, 0xDEADBEEF)
                    corrupted_pages.append(page_no)
        
        self.corruption_log.append({
            "type": "undo_pages",
            "num_pages": len(corrupted_pages)
        })
        
        return data, corrupted_pages
    
    def corrupt_blob_pages(self, data: bytearray) -> Tuple[bytearray, List[int]]:
        """损坏所有 BLOB 页"""
        corrupted_pages = []
        
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(data):
                page_type = struct.unpack('<H', data[offset + 16:offset + 18])[0]
                if page_type == 0x4574:  # BLOB page type
                    struct.pack_into('<I', data, offset + 24, 0xDEADBEEF)
                    struct.pack_into('<I', data, offset + 28, 0xDEADBEEF)
                    corrupted_pages.append(page_no)
        
        self.corruption_log.append({
            "type": "blob_pages",
            "num_pages": len(corrupted_pages)
        })
        
        return data, corrupted_pages
    
    def corrupt_all_page_types(self, data: bytearray) -> Dict[str, List[int]]:
        """同时损坏所有类型的页"""
        result = {
            "index": [],
            "sdi": [],
            "undo": [],
            "blob": []
        }
        
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(data):
                page_type = struct.unpack('<H', data[offset + 16:offset + 18])[0]
                
                if page_type == 0x26F1:  # Index
                    result["index"].append(page_no)
                elif page_type == 0x26F3:  # SDI
                    result["sdi"].append(page_no)
                elif page_type == 0x26F2:  # Undo
                    result["undo"].append(page_no)
                elif page_type == 0x26F4:  # BLOB
                    result["blob"].append(page_no)
        
        # Corrupt all
        for page_type, pages in result.items():
            for page_no in pages:
                offset = page_no * self.page_size
                if offset + 32 <= len(data):
                    struct.pack_into('<I', data, offset + 24, 0xDEADBEEF)
                    struct.pack_into('<I', data, offset + 28, 0xDEADBEEF)
        
        self.corruption_log.append({
            "type": "all_page_types",
            "corrupted": result
        })
        
        return result
    
    def create_file_fragmentation(self, data: bytearray, num_fragments: int = 10) -> Tuple[bytearray, List[Tuple[int, int]]]:
        """创建文件碎片（模拟文件系统碎片）"""
        fragments = []
        total_pages = len(data) // self.page_size
        
        if total_pages < num_fragments * 2:
            return data, fragments
        
        # 分割成多个片段
        fragment_size = total_pages // num_fragments
        
        # 收集所有片段
        all_pages = []
        for i in range(num_fragments):
            start = i * fragment_size
            end = start + fragment_size
            if end <= total_pages:
                fragment = data[start * self.page_size:end * self.page_size]
                all_pages.append(fragment)
                fragments.append((start, fragment_size))
        
        # 打乱片段顺序
        random.shuffle(all_pages)
        
        # 重新组合
        fragmented = bytearray()
        for fragment in all_pages:
            fragmented.extend(fragment)
        
        self.corruption_log.append({
            "type": "fragmentation",
            "num_fragments": num_fragments,
            "fragments": fragments
        })
        
        return fragmented, fragments
    
    def corrupt_checksum_and_type(self, data: bytearray, num_pages: int = 10) -> bytearray:
        """同时损坏校验和和页类型"""
        for _ in range(num_pages):
            page_no = random.randint(1, len(data) // self.page_size - 1)
            offset = page_no * self.page_size
            
            if offset + 52 <= len(data):
                # Corrupt checksum
                struct.pack_into('<I', data, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', data, offset + 28, 0xDEADBEEF)
                # Corrupt page type
                struct.pack_into('<H', data, offset + 16, 0xFFFF)
        
        self.corruption_log.append({
            "type": "checksum_and_type",
            "num_pages": num_pages
        })
        
        return data
    
    def corrupt_chain_and_checksum(self, data: bytearray, num_pages: int = 10) -> bytearray:
        """同时损坏页链和校验和"""
        for _ in range(num_pages):
            page_no = random.randint(1, len(data) // self.page_size - 1)
            offset = page_no * self.page_size
            
            if offset + 52 <= len(data):
                # Corrupt chain
                struct.pack_into('<I', data, offset + 0, 0xDEADBEEF)  # prev_page
                struct.pack_into('<I', data, offset + 4, 0xDEADBEEF)  # next_page
                # Corrupt checksum
                struct.pack_into('<I', data, offset + 24, 0xDEADBEEF)
                struct.pack_into('<I', data, offset + 28, 0xDEADBEEF)
        
        self.corruption_log.append({
            "type": "chain_and_checksum",
            "num_pages": num_pages
        })
        
        return data
    
    def corrupt_all_headers_complete(self, data: bytearray) -> bytearray:
        """完全损坏所有页头（最极端的情况）"""
        for page_no in range(len(data) // self.page_size):
            offset = page_no * self.page_size
            
            if offset + 52 <= len(data):
                # Corrupt all header fields
                struct.pack_into('<I', data, offset + 0, 0xDEADBEEF)   # prev_page
                struct.pack_into('<I', data, offset + 4, 0xDEADBEEF)   # next_page
                struct.pack_into('<Q', data, offset + 8, 0)             # LSN
                struct.pack_into('<H', data, offset + 16, 0xFFFF)       # page_type
                struct.pack_into('<I', data, offset + 20, 0xDEADBEEF)   # page_no
                struct.pack_into('<I', data, offset + 24, 0xDEADBEEF)   # checksum
                struct.pack_into('<I', data, offset + 28, 0xDEADBEEF)   # checksum
                struct.pack_into('<H', data, offset + 36, 0xFFFF)       # arch_log_no
        
        self.corruption_log.append({
            "type": "all_headers_complete",
            "num_pages": len(data) // self.page_size
        })
        
        return data
    
    def corrupt_record_chain_and_heap(self, data: bytearray, num_pages: int = 10) -> bytearray:
        """同时损坏记录链和堆"""
        for _ in range(num_pages):
            page_no = random.randint(1, len(data) // self.page_size - 1)
            offset = page_no * self.page_size + 38
            
            if offset + 20 <= len(data):
                # Corrupt record chain
                struct.pack_into('<H', data, offset + 2, 0xFFFF)   # heap_top
                struct.pack_into('<H', data, offset + 6, 0xFFFF)   # free_offset
                struct.pack_into('<H', data, offset + 8, 0xFFFF)   # garbage_end
                # Corrupt checksum
                struct.pack_into('<I', data, offset - 14, 0xDEADBEEF)
                struct.pack_into('<I', data, offset - 10, 0xDEADBEEF)
        
        self.corruption_log.append({
            "type": "record_chain_and_heap",
            "num_pages": num_pages
        })
        
        return data
    
    def corrupt_multiple_scenarios(self, data: bytearray, scenarios: List[str]) -> Tuple[bytearray, List[Dict]]:
        """应用多个损坏场景"""
        corrupted_data = bytearray(data)
        all_corruptions = []
        
        for scenario in scenarios:
            if scenario == "random_bytes":
                corrupted_data = self.inject_random_bytes(corrupted_data, 500)
                all_corruptions.append({"type": "random_bytes", "count": 500})
            
            elif scenario == "bit_flip":
                corrupted_data = self.bit_flip(corrupted_data, 200)
                all_corruptions.append({"type": "bit_flip", "count": 200})
            
            elif scenario == "all_checksums":
                corrupted_data = self.corrupt_all_checksums(corrupted_data)
                all_corruptions.append({"type": "all_checksums", 
                                       "count": len(data) // self.page_size})
            
            elif scenario == "all_headers":
                corrupted_data = self.corrupt_all_headers_complete(corrupted_data)
                all_corruptions.append({"type": "all_headers", 
                                       "count": len(data) // self.page_size})
            
            elif scenario == "file_hole":
                corrupted_data, _ = self.create_sparse_file(corrupted_data, 3)
                all_corruptions.append({"type": "file_hole", "count": 3})
            
            elif scenario == "scramble":
                corrupted_data, _ = self.scramble_page_order(corrupted_data, 4)
                all_corruptions.append({"type": "scramble", "count": 4})
            
            elif scenario == "chain":
                corrupted_data = self.corrupt_chain_and_checksum(corrupted_data, 15)
                all_corruptions.append({"type": "chain", "count": 15})
            
            elif scenario == "record":
                corrupted_data = self.corrupt_record_chain_and_heap(corrupted_data, 10)
                all_corruptions.append({"type": "record", "count": 10})
        
        self.corruption_log.extend(all_corruptions)
        return corrupted_data, all_corruptions


def generate_hash(data: bytes) -> str:
    """Generate SHA-256 hash"""
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(
        description='Extreme Chaos Monkey - Advanced File System Level Disaster Simulator'
    )
    parser.add_argument('--input', '-i', required=True, help='Input .ibd file')
    parser.add_argument('--output', '-o', required=True, help='Output corrupted .ibd file')
    parser.add_argument('--scenario', '-s', required=True,
                       choices=[
                           'file_hole', 'truncate_middle', 'scramble_order',
                           'random_bytes', 'bit_flip', 'all_checksums',
                           'sequential_headers', 'sparse_file', 'duplicate_pages',
                           'pattern_overwrite', 'corrupt_btree', 'corrupt_sdi',
                           'corrupt_undo', 'corrupt_blob', 'all_page_types',
                           'fragmentation', 'checksum_and_type', 'chain_and_checksum',
                           'all_headers', 'record_chain', 'multi_scenario'
                       ],
                       help='Corruption scenario')
    parser.add_argument('--scenarios', nargs='+',
                       help='Multiple scenarios for multi_scenario mode')
    parser.add_argument('--seed', type=int, default=None, help='Random seed')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    
    args = parser.parse_args()
    
    # Set random seed
    if args.seed:
        random.seed(args.seed)
    
    # Read input file
    with open(args.input, 'rb') as f:
        data = bytearray(f.read())
    
    # Create monkey
    monkey = ExtremeChaosMonkey(page_size=16384)
    
    # Apply corruption
    if args.scenario == 'multi_scenario':
        if not args.scenarios:
            print("Error: --scenarios required for multi_scenario mode")
            sys.exit(1)
        corrupted_data, corruptions = monkey.corrupt_multiple_scenarios(data, args.scenarios)
    else:
        if args.scenario == 'file_hole':
            corrupted_data = monkey.create_file_hole(data, 5, 3)
        elif args.scenario == 'truncate_middle':
            corrupted_data = monkey.truncate_file_middle(data, 5, 3)
        elif args.scenario == 'scramble_order':
            corrupted_data, _ = monkey.scramble_page_order(data, 4)
        elif args.scenario == 'random_bytes':
            corrupted_data = monkey.inject_random_bytes(data, 500)
        elif args.scenario == 'bit_flip':
            corrupted_data = monkey.bit_flip(data, 200)
        elif args.scenario == 'all_checksums':
            corrupted_data = monkey.corrupt_all_checksums(data)
        elif args.scenario == 'sequential_headers':
            corrupted_data = monkey.corrupt_page_headers_sequential(data, 5, 10)
        elif args.scenario == 'sparse_file':
            corrupted_data, _ = monkey.create_sparse_file(data, 3)
        elif args.scenario == 'duplicate_pages':
            corrupted_data = monkey.duplicate_pages(data, 10, 2)
        elif args.scenario == 'pattern_overwrite':
            corrupted_data = monkey.overwrite_with_pattern(data, b'\xFF\xFF\xFF\xFF', 1000)
        elif args.scenario == 'corrupt_btree':
            corrupted_data = monkey.corrupt_btree_internal_nodes(data, 5)
        elif args.scenario == 'corrupt_sdi':
            corrupted_data, _ = monkey.corrupt_sdi_pages(data)
        elif args.scenario == 'corrupt_undo':
            corrupted_data, _ = monkey.corrupt_undo_pages(data)
        elif args.scenario == 'corrupt_blob':
            corrupted_data, _ = monkey.corrupt_blob_pages(data)
        elif args.scenario == 'all_page_types':
            monkey.corrupt_all_page_types(data)
            corrupted_data = data
        elif args.scenario == 'fragmentation':
            corrupted_data, _ = monkey.create_file_fragmentation(data, 10)
        elif args.scenario == 'checksum_and_type':
            corrupted_data = monkey.corrupt_checksum_and_type(data, 10)
        elif args.scenario == 'chain_and_checksum':
            corrupted_data = monkey.corrupt_chain_and_checksum(data, 10)
        elif args.scenario == 'all_headers':
            corrupted_data = monkey.corrupt_all_headers_complete(data)
        elif args.scenario == 'record_chain':
            corrupted_data = monkey.corrupt_record_chain_and_heap(data, 10)
        else:
            corrupted_data = data
    
    # Write output file
    with open(args.output, 'wb') as f:
        f.write(corrupted_data)
    
    # Print results
    if args.verbose:
        print(f"Input file: {args.input}")
        print(f"Output file: {args.output}")
        print(f"File size: {len(data)} bytes -> {len(corrupted_data)} bytes")
        print(f"Page size: {monkey.page_size} bytes")
        print(f"Total pages: {len(data) // monkey.page_size} -> {len(corrupted_data) // monkey.page_size}")
        print()
        print("Corruption log:")
        for corruption in monkey.corruption_log:
            print(f"  {corruption}")
        print()
        print(f"Input hash: {generate_hash(data)}")
        print(f"Output hash: {generate_hash(corrupted_data)}")
    else:
        print(f"SCENARIO:{args.scenario}")
        print(f"PAGES_INPUT:{len(data) // monkey.page_size}")
        print(f"PAGES_OUTPUT:{len(corrupted_data) // monkey.page_size}")
        print(f"HASH_INPUT:{generate_hash(data)}")
        print(f"HASH_OUTPUT:{generate_hash(corrupted_data)}")
        print(f"CORRUPTIONS:{len(monkey.corruption_log)}")


if __name__ == '__main__':
    main()
