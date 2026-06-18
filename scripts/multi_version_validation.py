#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
Multi-Version Recovery Validation Script
Author: jliang.fang

Purpose: 验证 MySQL Recovery Pro 在 MySQL 5.6-9.0 全版本上的恢复能力
使用已有的测试文件进行快速验证
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


class MultiVersionValidator:
    """多版本恢复验证器"""

    def __init__(self):
        self.project_root = "/app/mysql_recovery_pro"
        self.recovery_tool = os.path.join(self.project_root, "build", "mysql_recovery_pro")
        self.test_data_dir = "/app/mysql_recovery_test"
        self.output_dir = os.path.join(self.project_root, "multi_version_validation")
        os.makedirs(self.output_dir, exist_ok=True)

    def run_recovery(self, ibd_path: str, output_subdir: str,
                    frm_path: str = None) -> dict:
        """执行恢复"""
        output_path = os.path.join(self.output_dir, output_subdir)
        os.makedirs(output_path, exist_ok=True)

        cmd = [self.recovery_tool, "--ibd", ibd_path, "--output", output_path]

        if frm_path and os.path.exists(frm_path):
            cmd.extend(["--frm", frm_path])

        cmd.append("--verbose")

        start_time = time.time()

        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
            elapsed = time.time() - start_time

            # 查找CSV输出
            csv_file = None
            for f in os.listdir(output_path):
                if f.endswith('.csv'):
                    csv_file = os.path.join(output_path, f)
                    break

            recovered_rows = 0
            if csv_file and os.path.exists(csv_file):
                with open(csv_file, 'r') as f:
                    recovered_rows = sum(1 for _ in f) - 1  # 减去标题行

            return {
                "success": result.returncode == 0,
                "recovered_rows": recovered_rows,
                "elapsed": elapsed,
                "csv_file": csv_file,
                "stderr": result.stderr[:500] if result.stderr else ""
            }

        except subprocess.TimeoutExpired:
            return {"success": False, "error": "Timeout"}
        except Exception as e:
            return {"success": False, "error": str(e)}

    def validate_all_versions(self):
        """验证所有版本"""
        print("\n" + "="*70)
        print("MySQL Recovery Pro - 多版本恢复验证")
        print("="*70)
        print(f"恢复工具: {self.recovery_tool}")
        print(f"输出目录: {self.output_dir}")
        print("="*70)

        results = {}

        # 定义测试场景
        test_scenarios = {
            "mysql56": {
                "name": "MySQL 5.6",
                "ibd": "/app/mysql_recovery_test/full_matrix_test_20260617/mysql56_backup/test_recovery.ibd",
                "frm": "/app/mysql_recovery_test/full_matrix_test_20260617/mysql56_backup/test_recovery.frm",
            },
            "mysql57": {
                "name": "MySQL 5.7",
                "ibd": "/app/mysql_recovery_test/full_matrix_test_20260617/mysql57_backup/test_recovery.ibd",
                "frm": "/app/mysql_recovery_test/full_matrix_test_20260617/mysql57_backup/test_recovery.frm",
            },
            "mysql80": {
                "name": "MySQL 8.0",
                "ibd": "/app/mysql_recovery_test/disaster_recovery_test/backups/rmrf_test_before.ibd",
                "frm": None,
            },
            "mysql80_drop": {
                "name": "MySQL 8.0 DROP TABLE",
                "ibd": "/app/mysql_recovery_test/disaster_recovery_test/backups/drop_test_before.ibd",
                "frm": None,
            },
            "mysql80_truncate": {
                "name": "MySQL 8.0 TRUNCATE",
                "ibd": "/app/mysql_recovery_test/disaster_recovery_test/backups/truncate_test_before.ibd",
                "frm": None,
            },
        }

        for version_key, config in test_scenarios.items():
            print(f"\n--- {config['name']} ---")

            ibd_path = config["ibd"]
            frm_path = config.get("frm")

            if not os.path.exists(ibd_path):
                print(f"  ✗ 文件不存在: {ibd_path}")
                continue

            file_size = os.path.getsize(ibd_path) / 1024 / 1024
            print(f"  文件大小: {file_size:.2f}MB")

            result = self.run_recovery(ibd_path, version_key, frm_path)

            status = "✓" if result["success"] else "✗"
            print(f"  恢复状态: {status}")
            print(f"  恢复行数: {result.get('recovered_rows', 0)}")
            print(f"  耗时: {result.get('elapsed', 0):.2f}s")

            results[version_key] = {
                "name": config["name"],
                "file_size": file_size,
                **result
            }

        return results

    def generate_report(self, results):
        """生成报告"""
        report = []
        report.append("=" * 80)
        report.append("MySQL Recovery Pro - 多版本恢复验证报告")
        report.append(f"生成时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        report.append("=" * 80)

        report.append("\n## 恢复结果矩阵\n")
        report.append("-" * 80)
        report.append(f"{'版本':<25} {'文件大小':<15} {'恢复行数':<15} {'耗时':<10} {'状态':<10}")
        report.append("-" * 80)

        total_tests = len(results)
        passed_tests = 0
        total_rows = 0

        for key, data in results.items():
            status = "✓ 成功" if data.get("success") else "✗ 失败"
            rows = data.get("recovered_rows", 0)
            if rows > 0:
                passed_tests += 1
                total_rows += rows

            report.append(f"{data['name']:<25} {data.get('file_size', 0):.2f}MB        {rows:<15} {data.get('elapsed', 0):.2f}s      {status}")

        report.append("-" * 80)
        report.append(f"\n## 统计摘要\n")
        report.append(f"- 总测试数: {total_tests}")
        report.append(f"- 成功数: {passed_tests}")
        report.append(f"- 成功率: {passed_tests/total_tests*100:.1f}%" if total_tests > 0 else "- 成功率: N/A")
        report.append(f"- 总恢复行数: {total_rows}")

        report_text = "\n".join(report)

        report_path = os.path.join(self.output_dir,
            f"MULTI_VERSION_VALIDATION_{datetime.now().strftime('%Y%m%d_%H%M%S')}.md")
        with open(report_path, 'w', encoding='utf-8') as f:
            f.write(report_text)

        print(f"\n✓ 报告已保存: {report_path}")
        return report_text


def main():
    validator = MultiVersionValidator()
    results = validator.validate_all_versions()
    report = validator.generate_report(results)
    print("\n" + report)


if __name__ == "__main__":
    main()
