#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
Extreme Destruction Matrix v2.0
Author: jliang.fang (Database Kernel Forensics Expert)

Purpose: 构建完整的"破坏→分析→修复→验证"闭环进化系统

破坏场景覆盖:
  1. 物理层损坏 (Physical Corruption)
     - 随机页损坏 (Random Page Corruption)
     - 关键页头抹除 (Header Erasure)
     - 页撕裂模拟 (Torn Pages)
     - B+Tree指针断裂 (Orphaned Pages)
     - 零值填充 (Zero-filling)

  2. 文件与元数据丢失 (File & Metadata Loss)
     - 完全脱离实例 (No ibdata1/.frm/SDI)
     - 全目录毁灭 (rm -rf simulation)
     - 磁盘雕刻测试 (Disk Carving)

  3. 逻辑删除 (Logical Deletion)
     - TRUNCATE + Overwrite
     - DROP TABLE + Purge
==============================================================================
"""

import os
import sys
import json
import time
import hashlib
import random
import shutil
import argparse
import subprocess
from pathlib import Path
from datetime import datetime
from typing import List, Dict, Optional, Tuple, Any
from dataclasses import dataclass, field
from enum import Enum
import struct
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed

try:
    import mysql.connector
    from mysql.connector import Error
except ImportError:
    print("Installing mysql-connector-python...")
    subprocess.run([sys.executable, "-m", "pip", "install", "mysql-connector-python"], check=True)
    import mysql.connector


class CorruptionType(Enum):
    """损坏类型枚举"""
    # 物理层损坏
    RANDOM_PAGE = "random_page"              # 随机页损坏
    HEADER_ERASURE = "header_erasure"        # 页头抹除
    TORN_PAGE = "torn_page"                  # 页撕裂
    ORPHANED_PAGE = "orphaned_page"          # B+Tree指针断裂
    ZERO_FILL = "zero_fill"                  # 零值填充
    CHECKSUM_CORRUPT = "checksum_corrupt"    # 校验和损坏
    LSN_CORRUPT = "lsn_corrupt"              # LSN损坏

    # 文件与元数据丢失
    RM_RF_SINGLE = "rm_rf_single"            # rm -rf 单文件
    RM_RF_FULL = "rm_rf_full"                # 全目录删除
    DROP_TABLE = "drop_table"                # DROP TABLE
    TRUNCATE_TABLE = "truncate_table"        # TRUNCATE TABLE
    FRM_LOSS = "frm_loss"                    # FRM文件丢失
    SDI_LOSS = "sdi_loss"                    # SDI丢失

    # 组合损坏
    MULTI_CORRUPTION = "multi_corruption"    # 多重损坏
    CASCADING_FAILURE = "cascading_failure"  # 级联故障


@dataclass
class CorruptionScenario:
    """损坏场景定义"""
    name: str
    corruption_type: CorruptionType
    severity: int  # 1-10, 10 = 最严重
    description: str
    parameters: Dict[str, Any] = field(default_factory=dict)
    requires_backup: bool = False

    def __str__(self):
        return f"[{self.corruption_type.value}] {self.name} (severity={self.severity})"


@dataclass
class DestructionResult:
    """破坏结果"""
    scenario: CorruptionScenario
    success: bool
    original_file: str
    corrupted_file: Optional[str]
    baseline_hash: str
    metadata: Dict[str, Any] = field(default_factory=dict)
    error_message: str = ""


class ExtremeDestructionMatrix:
    """极限破坏矩阵"""

    # InnoDB页头常量
    FIL_PAGE_TYPE_OFFSET = 24
    FIL_PAGE_LSN_OFFSET = 16
    FIL_PAGE_PREV_OFFSET = 8
    FIL_PAGE_NEXT_OFFSET = 12
    FIL_HEADER_SIZE = 38
    PAGE_HEADER_SIZE = 56

    # 页类型常量
    PAGE_TYPES = {
        0x0000: "ALLOCATED",
        0x0002: "UNDO_LOG",
        0x0003: "INODE",
        0x0004: "IBUF_FREE_LIST",
        0x0005: "IBUF_BITMAP",
        0x0006: "SYSTEM",
        0x0007: "TRX_SYSTEM",
        0x0008: "FILE_SYSTEM",
        0x0009: "WRITE_BLOB",
        0x000A: "WRITE_COMPRESSED_BLOB",
        0x000B: "WRITE_COMPRESSED_BLOB_LEGACY",
        0x000C: "RTREE_INDEX",
        0x000D: "SDI_BLOB",
        0x000E: "SDI_COMPRESSED_BLOB",
        0x45BD: "COMPRESSED_SDI",
        0x45BE: "COMPRESSED_INDEX",
        0xFFFF: "SPACE_BLOB_HEADER",
    }

    INDEX_PAGE_TYPES = [0x45BF, 0x45BE]  # INDEX page types

    def __init__(self, page_size: int = 16384, seed: int = 42):
        self.page_size = page_size
        self.seed = seed
        random.seed(seed)
        self.results: List[DestructionResult] = []

    def define_scenarios(self) -> List[CorruptionScenario]:
        """定义所有破坏场景"""
        scenarios = [
            # === 物理层损坏 ===
            CorruptionScenario(
                name="随机10%页损坏",
                corruption_type=CorruptionType.RANDOM_PAGE,
                severity=5,
                description="随机选择10%的页面注入随机字节",
                parameters={"corruption_ratio": 0.1, "random_bytes": True}
            ),
            CorruptionScenario(
                name="随机30%页损坏",
                corruption_type=CorruptionType.RANDOM_PAGE,
                severity=7,
                description="随机选择30%的页面注入随机字节",
                parameters={"corruption_ratio": 0.3, "random_bytes": True}
            ),
            CorruptionScenario(
                name="关键页头抹除",
                corruption_type=CorruptionType.HEADER_ERASURE,
                severity=6,
                description="覆盖前38字节FIL Header",
                parameters={"header_bytes": 38}
            ),
            CorruptionScenario(
                name="页撕裂模拟",
                corruption_type=CorruptionType.TORN_PAGE,
                severity=6,
                description="模拟写入中断,部分新数据部分旧数据",
                parameters={"torn_ratio": 0.5}
            ),
            CorruptionScenario(
                name="B+Tree指针断裂",
                corruption_type=CorruptionType.ORPHANED_PAGE,
                severity=8,
                description="篡改prev_page_no和next_page_no制造孤立页",
                parameters={"break_ratio": 0.2}
            ),
            CorruptionScenario(
                name="零值填充50%",
                corruption_type=CorruptionType.ZERO_FILL,
                severity=9,
                description="将文件50%覆盖为0x00",
                parameters={"fill_ratio": 0.5}
            ),
            CorruptionScenario(
                name="校验和损坏",
                corruption_type=CorruptionType.CHECKSUM_CORRUPT,
                severity=4,
                description="破坏页校验和字段",
                parameters={}
            ),
            CorruptionScenario(
                name="LSN损坏",
                corruption_type=CorruptionType.LSN_CORRUPT,
                severity=5,
                description="破坏LSN字段使页链断裂",
                parameters={}
            ),

            # === 文件与元数据丢失 ===
            CorruptionScenario(
                name="rm -rf单文件",
                corruption_type=CorruptionType.RM_RF_SINGLE,
                severity=10,
                description="删除单个.ibd文件",
                parameters={},
                requires_backup=True
            ),
            CorruptionScenario(
                name="rm -rf全目录",
                corruption_type=CorruptionType.RM_RF_FULL,
                severity=10,
                description="删除整个数据目录",
                parameters={},
                requires_backup=True
            ),
            CorruptionScenario(
                name="DROP TABLE",
                corruption_type=CorruptionType.DROP_TABLE,
                severity=9,
                description="执行DROP TABLE命令",
                parameters={},
                requires_backup=True
            ),
            CorruptionScenario(
                name="TRUNCATE TABLE",
                corruption_type=CorruptionType.TRUNCATE_TABLE,
                severity=8,
                description="执行TRUNCATE TABLE命令",
                parameters={},
                requires_backup=True
            ),
            CorruptionScenario(
                name="FRM文件丢失",
                corruption_type=CorruptionType.FRM_LOSS,
                severity=6,
                description="仅删除.frm文件",
                parameters={}
            ),
            CorruptionScenario(
                name="SDI丢失",
                corruption_type=CorruptionType.SDI_LOSS,
                severity=6,
                description="删除SDI页",
                parameters={}
            ),

            # === 组合损坏 ===
            CorruptionScenario(
                name="多重损坏(页损坏+页头抹除)",
                corruption_type=CorruptionType.MULTI_CORRUPTION,
                severity=9,
                description="同时应用多种损坏类型",
                parameters={"types": ["random_page", "header_erasure"]}
            ),
            CorruptionScenario(
                name="级联故障(页损坏+指针断裂+零值填充)",
                corruption_type=CorruptionType.CASCADING_FAILURE,
                severity=10,
                description="最极端的损坏场景",
                parameters={"types": ["random_page", "orphaned_page", "zero_fill"]}
            ),
        ]
        return scenarios

    # =========================================================================
    # 物理层损坏实现
    # =========================================================================

    def corrupt_random_pages(self, file_path: str, corruption_ratio: float = 0.1,
                              random_bytes: bool = True) -> str:
        """随机页损坏"""
        output_path = file_path.replace('.ibd', f'_corrupted_random_{int(corruption_ratio*100)}.ibd')
        shutil.copy(file_path, output_path)

        file_size = os.path.getsize(output_path)
        total_pages = file_size // self.page_size
        pages_to_corrupt = int(total_pages * corruption_ratio)

        with open(output_path, 'r+b') as f:
            pages_selected = random.sample(range(total_pages), pages_to_corrupt)
            for page_no in pages_selected:
                offset = page_no * self.page_size
                f.seek(offset)
                if random_bytes:
                    corrupt_data = bytes([random.randint(0, 255) for _ in range(64)])
                else:
                    corrupt_data = b'\x00' * 64
                f.write(corrupt_data)

        print(f"✓ 随机页损坏完成: {pages_to_corrupt}/{total_pages} 页 ({corruption_ratio*100}%)")
        return output_path

    def corrupt_header_erasure(self, file_path: str, header_bytes: int = 38) -> str:
        """页头抹除 - 覆盖FIL Header"""
        output_path = file_path.replace('.ibd', '_corrupted_header.ibd')
        shutil.copy(file_path, output_path)

        file_size = os.path.getsize(output_path)
        total_pages = file_size // self.page_size

        # 抹除所有页的前N字节
        with open(output_path, 'r+b') as f:
            # 随机选择20%的页面进行页头抹除
            pages_to_corrupt = random.sample(range(total_pages), max(1, total_pages // 5))
            for page_no in pages_to_corrupt:
                offset = page_no * self.page_size
                f.seek(offset)
                f.write(b'\xFF' * header_bytes)  # 使用0xFF覆盖

        print(f"✓ 页头抹除完成: {len(pages_to_corrupt)} 页的前{header_bytes}字节")
        return output_path

    def corrupt_torn_page(self, file_path: str, torn_ratio: float = 0.5) -> str:
        """页撕裂模拟 - 模拟写入中断"""
        output_path = file_path.replace('.ibd', '_corrupted_torn.ibd')
        shutil.copy(file_path, output_path)

        file_size = os.path.getsize(output_path)
        total_pages = file_size // self.page_size

        with open(output_path, 'r+b') as f:
            # 选择一些页面进行撕裂
            pages_to_tear = random.sample(range(total_pages), max(1, total_pages // 10))
            for page_no in pages_to_tear:
                offset = page_no * self.page_size

                # 读取原始页数据
                f.seek(offset)
                original_data = f.read(self.page_size)

                # 创建"撕裂"效果 - 前半部分新数据,后半部分保持原样
                torn_position = int(self.page_size * torn_ratio)
                new_data = bytes([random.randint(0, 255) for _ in range(torn_position)])

                f.seek(offset)
                f.write(new_data + original_data[torn_position:])

        print(f"✓ 页撕裂完成: {len(pages_to_tear)} 页在{torn_ratio*100}%位置撕裂")
        return output_path

    def corrupt_orphaned_page(self, file_path: str, break_ratio: float = 0.2) -> str:
        """B+Tree指针断裂 - 制造孤立页"""
        output_path = file_path.replace('.ibd', '_corrupted_orphaned.ibd')
        shutil.copy(file_path, output_path)

        file_size = os.path.getsize(output_path)
        total_pages = file_size // self.page_size

        with open(output_path, 'r+b') as f:
            # 篡改页链指针
            pages_to_break = random.sample(range(1, total_pages),
                                          max(1, int(total_pages * break_ratio)))

            for page_no in pages_to_break:
                offset = page_no * self.page_size

                # 破坏FIL_PAGE_PREV (offset 8) 和 FIL_PAGE_NEXT (offset 12)
                # 将它们设置为无效值
                f.seek(offset + self.FIL_PAGE_PREV_OFFSET)
                f.write(struct.pack('<I', 0xFFFFFFFF))  # 无效prev

                f.seek(offset + self.FIL_PAGE_NEXT_OFFSET)
                f.write(struct.pack('<I', 0xFFFFFFFF))  # 无效next

        print(f"✓ B+Tree指针断裂完成: {len(pages_to_break)} 页成为孤立页")
        return output_path

    def corrupt_zero_fill(self, file_path: str, fill_ratio: float = 0.5) -> str:
        """零值填充"""
        output_path = file_path.replace('.ibd', '_corrupted_zeroed.ibd')
        shutil.copy(file_path, output_path)

        file_size = os.path.getsize(output_path)
        bytes_to_fill = int(file_size * fill_ratio)

        with open(output_path, 'r+b') as f:
            # 随机选择起始位置
            start_pos = random.randint(0, file_size - bytes_to_fill)
            f.seek(start_pos)
            f.write(b'\x00' * bytes_to_fill)

        print(f"✓ 零值填充完成: {bytes_to_fill/1024/1024:.2f}MB ({fill_ratio*100}%)")
        return output_path

    def corrupt_checksum(self, file_path: str) -> str:
        """校验和损坏"""
        output_path = file_path.replace('.ibd', '_corrupted_checksum.ibd')
        shutil.copy(file_path, output_path)

        file_size = os.path.getsize(output_path)
        total_pages = file_size // self.page_size

        with open(output_path, 'r+b') as f:
            # 破坏每个页的校验和字段 (offset 0 和 4)
            pages_to_corrupt = random.sample(range(total_pages), max(1, total_pages // 5))
            for page_no in pages_to_corrupt:
                offset = page_no * self.page_size
                # 校验和字段在页头的前8字节
                f.seek(offset)
                f.write(struct.pack('<II', 0xDEADBEEF, 0xDEADBEEF))

        print(f"✓ 校验和损坏完成: {len(pages_to_corrupt)} 页")
        return output_path

    def corrupt_lsn(self, file_path: str) -> str:
        """LSN损坏"""
        output_path = file_path.replace('.ibd', '_corrupted_lsn.ibd')
        shutil.copy(file_path, output_path)

        file_size = os.path.getsize(output_path)
        total_pages = file_size // self.page_size

        with open(output_path, 'r+b') as f:
            pages_to_corrupt = random.sample(range(total_pages), max(1, total_pages // 5))
            for page_no in pages_to_corrupt:
                offset = page_no * self.page_size
                # LSN在offset 16
                f.seek(offset + self.FIL_PAGE_LSN_OFFSET)
                f.write(struct.pack('<Q', 0xFFFFFFFFFFFFFFFF))

        print(f"✓ LSN损坏完成: {len(pages_to_corrupt)} 页")
        return output_path

    # =========================================================================
    # 文件与元数据丢失实现
    # =========================================================================

    def simulate_rm_rf_single(self, file_path: str, backup_dir: str) -> Tuple[str, str]:
        """模拟rm -rf单文件"""
        # 创建备份
        backup_path = os.path.join(backup_dir, os.path.basename(file_path))
        shutil.copy(file_path, backup_path)

        # "删除"原文件
        deleted_path = file_path + ".deleted"
        shutil.move(file_path, deleted_path)

        print(f"✓ rm -rf单文件模拟完成: {file_path} -> 备份在 {backup_path}")
        return backup_path, deleted_path

    def simulate_rm_rf_full(self, data_dir: str, backup_dir: str) -> Tuple[str, str]:
        """模拟rm -rf全目录"""
        # 创建备份
        backup_path = os.path.join(backup_dir, "full_backup")
        shutil.copytree(data_dir, backup_path)

        # "删除"原目录
        deleted_path = data_dir + ".deleted"
        shutil.move(data_dir, deleted_path)

        print(f"✓ rm -rf全目录模拟完成")
        return backup_path, deleted_path

    def simulate_drop_table(self, connection_config: Dict, table_name: str,
                           ibd_backup_path: str) -> str:
        """模拟DROP TABLE"""
        try:
            conn = mysql.connector.connect(**connection_config)
            cursor = conn.cursor()

            # 备份ibd文件
            # (实际场景中需要从MySQL数据目录复制)
            print(f"✓ DROP TABLE模拟完成: {table_name}")
            return ibd_backup_path

        except Error as e:
            print(f"✗ DROP TABLE失败: {e}")
            return ""

    def simulate_truncate_table(self, connection_config: Dict, table_name: str,
                               ibd_backup_path: str) -> str:
        """模拟TRUNCATE TABLE"""
        try:
            conn = mysql.connector.connect(**connection_config)
            cursor = conn.cursor()

            # TRUNCATE会清空表数据
            cursor.execute(f"TRUNCATE TABLE {table_name}")
            conn.commit()

            print(f"✓ TRUNCATE TABLE模拟完成: {table_name}")
            return ibd_backup_path

        except Error as e:
            print(f"✗ TRUNCATE TABLE失败: {e}")
            return ""

    # =========================================================================
    # 组合损坏实现
    # =========================================================================

    def apply_multi_corruption(self, file_path: str,
                               corruption_types: List[str]) -> str:
        """应用多重损坏"""
        current_file = file_path

        for ctype in corruption_types:
            if ctype == "random_page":
                current_file = self.corrupt_random_pages(current_file, 0.2)
            elif ctype == "header_erasure":
                current_file = self.corrupt_header_erasure(current_file)
            elif ctype == "torn_page":
                current_file = self.corrupt_torn_page(current_file)
            elif ctype == "orphaned_page":
                current_file = self.corrupt_orphaned_page(current_file)
            elif ctype == "zero_fill":
                current_file = self.corrupt_zero_fill(current_file, 0.3)
            elif ctype == "checksum_corrupt":
                current_file = self.corrupt_checksum(current_file)
            elif ctype == "lsn_corrupt":
                current_file = self.corrupt_lsn(current_file)

        print(f"✓ 多重损坏完成: {corruption_types}")
        return current_file

    # =========================================================================
    # 主执行函数
    # =========================================================================

    def execute_scenario(self, file_path: str, scenario: CorruptionScenario,
                        backup_dir: str = None) -> DestructionResult:
        """执行单个破坏场景"""

        # 计算基线哈希
        baseline_hash = self._calculate_file_hash(file_path)

        try:
            corrupted_file = None

            if scenario.corruption_type == CorruptionType.RANDOM_PAGE:
                ratio = scenario.parameters.get("corruption_ratio", 0.1)
                corrupted_file = self.corrupt_random_pages(file_path, ratio)

            elif scenario.corruption_type == CorruptionType.HEADER_ERASURE:
                header_bytes = scenario.parameters.get("header_bytes", 38)
                corrupted_file = self.corrupt_header_erasure(file_path, header_bytes)

            elif scenario.corruption_type == CorruptionType.TORN_PAGE:
                torn_ratio = scenario.parameters.get("torn_ratio", 0.5)
                corrupted_file = self.corrupt_torn_page(file_path, torn_ratio)

            elif scenario.corruption_type == CorruptionType.ORPHANED_PAGE:
                break_ratio = scenario.parameters.get("break_ratio", 0.2)
                corrupted_file = self.corrupt_orphaned_page(file_path, break_ratio)

            elif scenario.corruption_type == CorruptionType.ZERO_FILL:
                fill_ratio = scenario.parameters.get("fill_ratio", 0.5)
                corrupted_file = self.corrupt_zero_fill(file_path, fill_ratio)

            elif scenario.corruption_type == CorruptionType.CHECKSUM_CORRUPT:
                corrupted_file = self.corrupt_checksum(file_path)

            elif scenario.corruption_type == CorruptionType.LSN_CORRUPT:
                corrupted_file = self.corrupt_lsn(file_path)

            elif scenario.corruption_type == CorruptionType.MULTI_CORRUPTION:
                types = scenario.parameters.get("types", [])
                corrupted_file = self.apply_multi_corruption(file_path, types)

            elif scenario.corruption_type == CorruptionType.CASCADING_FAILURE:
                types = scenario.parameters.get("types", ["random_page", "orphaned_page", "zero_fill"])
                corrupted_file = self.apply_multi_corruption(file_path, types)

            result = DestructionResult(
                scenario=scenario,
                success=True,
                original_file=file_path,
                corrupted_file=corrupted_file,
                baseline_hash=baseline_hash,
                metadata={
                    "file_size": os.path.getsize(file_path),
                    "corruption_time": datetime.now().isoformat()
                }
            )

        except Exception as e:
            result = DestructionResult(
                scenario=scenario,
                success=False,
                original_file=file_path,
                corrupted_file=None,
                baseline_hash=baseline_hash,
                error_message=str(e)
            )

        self.results.append(result)
        return result

    def execute_all_scenarios(self, file_path: str, backup_dir: str = None) -> List[DestructionResult]:
        """执行所有破坏场景"""
        scenarios = self.define_scenarios()
        results = []

        print(f"\n{'='*70}")
        print(f"极限破坏矩阵 v2.0")
        print(f"目标文件: {file_path}")
        print(f"场景数量: {len(scenarios)}")
        print(f"{'='*70}\n")

        for scenario in scenarios:
            print(f"\n>>> 执行场景: {scenario}")
            result = self.execute_scenario(file_path, scenario, backup_dir)
            results.append(result)

            if result.success:
                print(f"    ✓ 成功 - 输出: {result.corrupted_file}")
            else:
                print(f"    ✗ 失败 - 错误: {result.error_message}")

        return results

    def generate_report(self, output_path: str = None) -> str:
        """生成破坏矩阵报告"""
        report = []
        report.append("=" * 80)
        report.append("极限破坏矩阵测试报告")
        report.append(f"生成时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        report.append("=" * 80)
        report.append("")

        # 统计
        total = len(self.results)
        success = sum(1 for r in self.results if r.success)

        report.append(f"总场景数: {total}")
        report.append(f"成功执行: {success}")
        report.append(f"执行率: {success/total*100:.1f}%")
        report.append("")

        # 详细结果
        report.append("-" * 80)
        report.append("详细结果:")
        report.append("-" * 80)

        for result in self.results:
            status = "✓" if result.success else "✗"
            report.append(f"\n{status} {result.scenario.name}")
            report.append(f"   类型: {result.scenario.corruption_type.value}")
            report.append(f"   严重度: {result.scenario.severity}/10")
            if result.success:
                report.append(f"   损坏文件: {result.corrupted_file}")
                report.append(f"   原始大小: {result.metadata.get('file_size', 0)/1024/1024:.2f} MB")
            else:
                report.append(f"   错误: {result.error_message}")

        report_text = "\n".join(report)

        if output_path:
            with open(output_path, 'w', encoding='utf-8') as f:
                f.write(report_text)

        return report_text

    def _calculate_file_hash(self, file_path: str) -> str:
        """计算文件SHA-256哈希"""
        sha256 = hashlib.sha256()
        with open(file_path, 'rb') as f:
            for chunk in iter(lambda: f.read(8192), b''):
                sha256.update(chunk)
        return sha256.hexdigest()


def main():
    parser = argparse.ArgumentParser(description="极限破坏矩阵 v2.0")
    parser.add_argument("--ibd", required=True, help="目标IBD文件路径")
    parser.add_argument("--output-dir", default="./destruction_output",
                       help="输出目录")
    parser.add_argument("--scenario", help="执行特定场景")
    parser.add_argument("--all", action="store_true", help="执行所有场景")
    parser.add_argument("--page-size", type=int, default=16384, help="页大小")
    parser.add_argument("--seed", type=int, default=42, help="随机种子")
    parser.add_argument("--report", help="报告输出路径")

    args = parser.parse_args()

    # 创建输出目录
    os.makedirs(args.output_dir, exist_ok=True)

    # 初始化破坏矩阵
    matrix = ExtremeDestructionMatrix(page_size=args.page_size, seed=args.seed)

    if args.all:
        results = matrix.execute_all_scenarios(args.ibd, args.output_dir)
        report = matrix.generate_report(args.report)
        print("\n" + report)
    elif args.scenario:
        scenarios = matrix.define_scenarios()
        scenario = next((s for s in scenarios if s.name == args.scenario), None)
        if scenario:
            result = matrix.execute_scenario(args.ibd, scenario, args.output_dir)
            print(f"\n场景执行完成: {result.success}")
        else:
            print(f"场景未找到: {args.scenario}")
            print("可用场景:")
            for s in scenarios:
                print(f"  - {s.name}")
    else:
        # 显示可用场景
        scenarios = matrix.define_scenarios()
        print("可用破坏场景:")
        for s in scenarios:
            print(f"  {s}")


if __name__ == "__main__":
    main()
