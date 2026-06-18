#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
Quick Recovery Matrix Test
Author: jliang.fang

Purpose: 使用现有测试文件快速生成恢复率矩阵
==============================================================================
"""

import os
import sys
import json
import time
import shutil
import subprocess
from pathlib import Path
from datetime import datetime
import random


class QuickRecoveryMatrixTest:
    """快速恢复矩阵测试"""

    def __init__(self, project_root: str = "/app/mysql_recovery_pro"):
        self.project_root = project_root
        self.recovery_tool = os.path.join(project_root, "build", "mysql_recovery_pro")
        self.test_files_dir = "/app/mysql_recovery_test/disaster_recovery_test/backups"
        self.output_dir = os.path.join(project_root, "quick_matrix_results")
        os.makedirs(self.output_dir, exist_ok=True)

    def test_recovery(self, ibd_path: str, output_subdir: str,
                     options: list = None) -> dict:
        """执行恢复测试"""
        output_path = os.path.join(self.output_dir, output_subdir)
        os.makedirs(output_path, exist_ok=True)

        # 构建命令
        cmd = [self.recovery_tool, "--ibd", ibd_path, "--output", output_path]

        if options:
            cmd.extend(options)

        cmd.append("--verbose")

        start_time = time.time()

        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
            elapsed = time.time() - start_time

            # 查找输出文件
            sql_file = None
            for f in os.listdir(output_path):
                if f.endswith('.sql'):
                    sql_file = os.path.join(output_path, f)
                    break

            # 统计恢复行数
            recovered_rows = 0
            if sql_file and os.path.exists(sql_file):
                # 检查是SQL还是CSV
                if sql_file.endswith('.sql'):
                    with open(sql_file, 'r') as f:
                        content = f.read()
                        recovered_rows = content.count("INSERT INTO")
                elif sql_file.endswith('.csv'):
                    with open(sql_file, 'r') as f:
                        # CSV行数减去标题行
                        recovered_rows = sum(1 for _ in f) - 1

            # 如果没找到输出文件，尝试查找CSV
            if recovered_rows == 0:
                for f in os.listdir(output_path):
                    if f.endswith('.csv'):
                        csv_file = os.path.join(output_path, f)
                        with open(csv_file, 'r') as cf:
                            recovered_rows = sum(1 for _ in cf) - 1
                        break

            return {
                "success": result.returncode == 0,
                "recovered_rows": recovered_rows,
                "elapsed": elapsed,
                "sql_file": sql_file,
                "stderr": result.stderr[:500] if result.stderr else ""
            }

        except subprocess.TimeoutExpired:
            return {"success": False, "error": "Timeout"}
        except Exception as e:
            return {"success": False, "error": str(e)}

    def apply_corruption(self, ibd_path: str, corruption_type: str) -> str:
        """应用损坏"""
        corrupted_path = ibd_path.replace('.ibd', f'_{corruption_type}.ibd')
        shutil.copy(ibd_path, corrupted_path)

        file_size = os.path.getsize(corrupted_path)
        page_size = 16384
        total_pages = file_size // page_size

        if corruption_type == "header_erasure":
            with open(corrupted_path, 'r+b') as f:
                f.write(b'\xFF' * 38)
            print(f"  ✓ 页头抹除: 前38字节")

        elif corruption_type == "random_page_10pct":
            pages_to_corrupt = random.sample(range(total_pages), max(1, total_pages // 10))
            with open(corrupted_path, 'r+b') as f:
                for page_no in pages_to_corrupt:
                    offset = page_no * page_size
                    f.seek(offset)
                    f.write(bytes([random.randint(0, 255) for _ in range(64)]))
            print(f"  ✓ 随机页损坏: {len(pages_to_corrupt)}/{total_pages} 页 (10%)")

        elif corruption_type == "random_page_30pct":
            pages_to_corrupt = random.sample(range(total_pages), max(1, total_pages * 3 // 10))
            with open(corrupted_path, 'r+b') as f:
                for page_no in pages_to_corrupt:
                    offset = page_no * page_size
                    f.seek(offset)
                    f.write(bytes([random.randint(0, 255) for _ in range(64)]))
            print(f"  ✓ 随机页损坏: {len(pages_to_corrupt)}/{total_pages} 页 (30%)")

        elif corruption_type == "torn_page":
            with open(corrupted_path, 'r+b') as f:
                for page_no in range(0, total_pages, 10):
                    offset = page_no * page_size + page_size // 2
                    f.seek(offset)
                    f.write(b'\x00' * 64)
            print(f"  ✓ 页撕裂: 每10页一个撕裂点")

        elif corruption_type == "zero_fill_20pct":
            bytes_to_fill = file_size // 5
            start_pos = file_size // 4
            with open(corrupted_path, 'r+b') as f:
                f.seek(start_pos)
                f.write(b'\x00' * bytes_to_fill)
            print(f"  ✓ 零值填充: {bytes_to_fill/1024/1024:.2f}MB (20%)")

        return corrupted_path

    def run_matrix_test(self) -> dict:
        """运行矩阵测试"""
        print("\n" + "="*70)
        print("MySQL Recovery Pro - 快速恢复率矩阵测试")
        print("="*70)
        print(f"恢复工具: {self.recovery_tool}")
        print(f"测试文件目录: {self.test_files_dir}")
        print(f"输出目录: {self.output_dir}")
        print("="*70)

        # 测试场景
        test_scenarios = {
            "rmrf_test_before.ibd": {
                "expected_rows": 100000,
                "recovery_options": ["--rmrf-recovery"],
                "corruptions": ["clean", "header_erasure", "random_page_10pct"]
            },
            "drop_test_before.ibd": {
                "expected_rows": 100000,
                "recovery_options": [],
                "corruptions": ["clean", "header_erasure"]
            },
            "truncate_test_before.ibd": {
                "expected_rows": 100000,
                "recovery_options": ["--truncate-recovery"],
                "corruptions": ["clean", "header_erasure"]
            }
        }

        results = {}

        for test_file, config in test_scenarios.items():
            original_path = os.path.join(self.test_files_dir, test_file)

            if not os.path.exists(original_path):
                print(f"\n✗ 跳过: {test_file} (文件不存在)")
                continue

            print(f"\n{'='*70}")
            print(f"测试文件: {test_file}")
            print(f"预期行数: {config['expected_rows']}")
            print(f"{'='*70}")

            file_results = {}

            for corruption in config["corruptions"]:
                print(f"\n--- 破坏类型: {corruption} ---")

                # 准备测试文件
                if corruption == "clean":
                    test_path = original_path
                else:
                    test_path = self.apply_corruption(original_path, corruption)

                # 执行恢复
                print(f"执行恢复...")
                result = self.test_recovery(
                    test_path,
                    f"{test_file.replace('.ibd', '')}_{corruption}",
                    config["recovery_options"]
                )

                # 计算恢复率
                if result["success"]:
                    recovery_rate = result["recovered_rows"] / config["expected_rows"]
                    print(f"  恢复状态: 成功")
                    print(f"  恢复行数: {result['recovered_rows']}/{config['expected_rows']}")
                    print(f"  恢复率: {recovery_rate*100:.2f}%")
                    print(f"  耗时: {result['elapsed']:.2f}s")
                else:
                    recovery_rate = 0
                    print(f"  恢复状态: 失败 - {result.get('error', result.get('stderr', 'unknown'))}")

                file_results[corruption] = {
                    "success": result["success"],
                    "recovered_rows": result["recovered_rows"],
                    "expected_rows": config["expected_rows"],
                    "recovery_rate": recovery_rate,
                    "elapsed": result.get("elapsed", 0)
                }

                # 清理损坏文件
                if corruption != "clean" and test_path != original_path:
                    os.remove(test_path)

            results[test_file] = file_results

        return results

    def generate_report(self, results: dict) -> str:
        """生成报告"""
        report = []
        report.append("=" * 100)
        report.append("MySQL Recovery Pro - 恢复率矩阵报告")
        report.append(f"生成时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        report.append("=" * 100)

        # 恢复率矩阵表
        report.append("\n## 恢复率矩阵\n")
        report.append("-" * 100)

        # 收集所有破坏类型
        all_corruptions = set()
        for file_results in results.values():
            all_corruptions.update(file_results.keys())

        # 表头
        header = f"{'测试文件':<30}"
        for corruption in sorted(all_corruptions):
            header += f"{corruption:<25}"
        report.append(header)
        report.append("-" * 100)

        # 数据行
        for test_file, file_results in results.items():
            row = f"{test_file:<30}"
            for corruption in sorted(all_corruptions):
                if corruption in file_results:
                    data = file_results[corruption]
                    rate = data["recovery_rate"] * 100
                    elapsed = data["elapsed"]
                    status = "✓" if rate >= 99.9 else "✗"
                    row += f"{status}{rate:.1f}%/{elapsed:.1f}s".ljust(25)
                else:
                    row += "N/A".ljust(25)
            report.append(row)

        # 统计摘要
        report.append("\n\n## 统计摘要\n")

        total_tests = sum(len(v) for v in results.values())
        passed_tests = sum(
            1 for v in results.values()
            for c in v.values() if c["recovery_rate"] >= 0.999
        )
        avg_rate = sum(
            c["recovery_rate"] for v in results.values() for c in v.values()
        ) / total_tests if total_tests > 0 else 0

        report.append(f"- 总测试数: {total_tests}")
        report.append(f"- 通过数 (≥99.9%): {passed_tests}")
        report.append(f"- 通过率: {passed_tests/total_tests*100:.1f}%" if total_tests > 0 else "- 通过率: N/A")
        report.append(f"- 平均恢复率: {avg_rate*100:.2f}%")

        # 详细结果
        report.append("\n\n## 详细测试结果\n")

        for test_file, file_results in results.items():
            report.append(f"\n### {test_file}\n")
            for corruption, data in file_results.items():
                status = "✓ PASS" if data["recovery_rate"] >= 0.999 else "✗ FAIL"
                report.append(f"- {corruption}: {status}")
                report.append(f"  - 恢复率: {data['recovery_rate']*100:.2f}%")
                report.append(f"  - 恢复行数: {data['recovered_rows']}/{data['expected_rows']}")
                report.append(f"  - 耗时: {data['elapsed']:.2f}s")

        report.append("\n" + "=" * 100)
        report.append("测试完成")
        report.append("=" * 100)

        report_text = "\n".join(report)

        # 保存报告
        report_path = os.path.join(
            self.output_dir,
            f"RECOVERY_MATRIX_REPORT_{datetime.now().strftime('%Y%m%d_%H%M%S')}.md"
        )
        with open(report_path, 'w', encoding='utf-8') as f:
            f.write(report_text)

        print(f"\n✓ 报告已保存: {report_path}")

        return report_text


def main():
    test = QuickRecoveryMatrixTest()
    results = test.run_matrix_test()
    report = test.generate_report(results)
    print("\n" + report)


if __name__ == "__main__":
    main()
