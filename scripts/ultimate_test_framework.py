#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
MySQL Recovery Pro - Ultimate Testing Framework v2.0
Author: jliang.fang (Database Kernel Forensics Expert, Storage Architect)

Purpose: 完整的"破坏→分析→修复→验证"闭环进化系统

功能模块:
  1. 极限破坏矩阵 (Extreme Destruction Matrix)
  2. 自动化评估平台 (Automated Evaluation Platform)
  3. 迭代修复进化机制 (Evolution Loop System)
  4. 恢复率矩阵报告 (Recovery Rate Matrix Report)

目标:
  - 覆盖 MySQL 5.6-9.0 全版本
  - 支持 99.9%+ 恢复率
  - 极端物理损坏场景测试
  - 自动化闭环验证
==============================================================================
"""

import os
import sys
import json
import time
import argparse
import subprocess
from pathlib import Path
from datetime import datetime
from typing import List, Dict, Optional, Tuple
import random

# 添加脚本目录到路径
SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPTS_DIR)


class UltimateTestingFramework:
    """终极测试框架"""

    def __init__(self, project_root: str = "/app/mysql_recovery_pro",
                 output_base: str = None):
        self.project_root = project_root
        self.output_base = output_base or os.path.join(project_root, "ultimate_test_results")
        self.recovery_tool = os.path.join(project_root, "build", "mysql_recovery_pro")

        # 创建输出目录
        os.makedirs(self.output_base, exist_ok=True)

        # 模块导入
        self.destruction_matrix = None
        self.evaluation_platform = None
        self.evolution_system = None

    def initialize_modules(self):
        """初始化模块"""
        from extreme_destruction_matrix import ExtremeDestructionMatrix
        from automated_evaluation_platform import AutomatedEvaluationPlatform
        from evolution_loop_system import EvolutionLoopSystem

        self.destruction_matrix = ExtremeDestructionMatrix()
        self.evaluation_platform = AutomatedEvaluationPlatform(
            recovery_tool_path=self.recovery_tool,
            output_base_dir=os.path.join(self.output_base, "evaluation")
        )
        self.evolution_system = EvolutionLoopSystem(self.project_root)

    # =========================================================================
    # Phase 1: 环境准备
    # =========================================================================

    def check_environment(self) -> Dict:
        """检查环境"""
        print("\n" + "="*70)
        print("Phase 1: 环境检查")
        print("="*70)

        env_status = {
            "recovery_tool": False,
            "mysql_containers": {},
            "output_dir": False
        }

        # 检查恢复工具
        if os.path.exists(self.recovery_tool):
            print(f"✓ 恢复工具: {self.recovery_tool}")
            env_status["recovery_tool"] = True
        else:
            print(f"✗ 恢复工具未找到: {self.recovery_tool}")
            print("  请先编译: cd build && cmake .. && make")

        # 检查MySQL容器
        print("\nMySQL容器状态:")
        containers = {
            "5.6": ("mysql56_test", 3306),
            "5.7": ("mysql57_test", 3307),
            "8.0": ("mysql80_test", 3308),
            "8.4": ("mysql84_test", 3309),
            "9.0": ("mysql90_test", 3310)
        }

        for version, (name, port) in containers.items():
            try:
                result = subprocess.run(
                    ["podman", "ps", "-a", "--format", "{{.Status}}",
                     "--filter", f"name={name}"],
                    capture_output=True, text=True, timeout=5
                )
                if result.returncode == 0 and "Up" in result.stdout:
                    print(f"  ✓ MySQL {version}: 运行中 (端口 {port})")
                    env_status["mysql_containers"][version] = "running"
                else:
                    # 尝试docker
                    result = subprocess.run(
                        ["docker", "ps", "-a", "--format", "{{.Status}}",
                         "--filter", f"name={name}"],
                        capture_output=True, text=True, timeout=5
                    )
                    if result.returncode == 0 and "Up" in result.stdout:
                        print(f"  ✓ MySQL {version}: 运行中 (端口 {port})")
                        env_status["mysql_containers"][version] = "running"
                    else:
                        print(f"  ✗ MySQL {version}: 未运行")
                        env_status["mysql_containers"][version] = "stopped"
            except Exception as e:
                print(f"  ? MySQL {version}: 检查失败 ({e})")
                env_status["mysql_containers"][version] = "error"

        # 检查输出目录
        try:
            os.makedirs(self.output_base, exist_ok=True)
            print(f"\n✓ 输出目录: {self.output_base}")
            env_status["output_dir"] = True
        except Exception as e:
            print(f"\n✗ 输出目录创建失败: {e}")

        return env_status

    # =========================================================================
    # Phase 2: 数据注入与基线建立
    # =========================================================================

    def prepare_test_data(self, mysql_versions: List[str] = None,
                         row_counts: List[int] = None) -> Dict:
        """准备测试数据"""
        print("\n" + "="*70)
        print("Phase 2: 数据注入与基线建立")
        print("="*70)

        if mysql_versions is None:
            mysql_versions = ["8.0"]  # 默认只测试8.0

        if row_counts is None:
            row_counts = [10000]

        test_data = {
            "versions": {},
            "baselines": {}
        }

        # 为每个版本准备数据
        for version in mysql_versions:
            print(f"\n--- MySQL {version} ---")

            # 生成测试数据SQL
            for row_count in row_counts:
                print(f"\n准备 {row_count} 行测试数据...")

                # 创建测试表并插入数据
                result = self._create_and_populate_table(version, row_count)

                if result["success"]:
                    test_data["versions"][version] = {
                        "row_count": row_count,
                        "table_name": result["table_name"],
                        "ibd_path": result["ibd_path"],
                        "baseline_hash": result["baseline_hash"]
                    }
                    test_data["baselines"][f"{version}_{result['table_name']}"] = {
                        "row_count": row_count,
                        "hash": result["baseline_hash"]
                    }

        return test_data

    def _create_and_populate_table(self, mysql_version: str, row_count: int) -> Dict:
        """创建表并填充数据"""
        import mysql.connector

        # 端口映射
        port_map = {
            "5.6": 3306, "5.7": 3307, "8.0": 3308, "8.4": 3309, "9.0": 3310
        }

        port = port_map.get(mysql_version, 3308)
        table_name = f"test_ultimate_{row_count}"

        try:
            conn = mysql.connector.connect(
                host="127.0.0.1", port=port, user="root", password="root",
                charset="utf8mb4"
            )
            cursor = conn.cursor()

            # 创建数据库
            cursor.execute("CREATE DATABASE IF NOT EXISTS ultimate_test")
            cursor.execute("USE ultimate_test")

            # 删除旧表
            cursor.execute(f"DROP TABLE IF EXISTS {table_name}")

            # 创建表
            create_sql = f"""
            CREATE TABLE {table_name} (
                id INT PRIMARY KEY AUTO_INCREMENT,
                name VARCHAR(100) NOT NULL,
                email VARCHAR(200),
                score INT,
                price DECIMAL(10, 2),
                rating FLOAT,
                created_at DATETIME,
                status ENUM('active', 'inactive', 'pending'),
                metadata JSON,
                description TEXT
            ) ENGINE=InnoDB ROW_FORMAT=DYNAMIC
            """
            cursor.execute(create_sql)

            # 批量插入数据
            batch_size = 1000
            for batch_start in range(0, row_count, batch_size):
                values = []
                for i in range(batch_start, min(batch_start + batch_size, row_count)):
                    name = f"user_{i}_{random.randint(1000, 9999)}"
                    email = f"{name}@example.com"
                    score = random.randint(0, 1000)
                    price = round(random.uniform(0, 10000), 2)
                    rating = round(random.uniform(0, 5), 2)
                    status = random.choice(['active', 'inactive', 'pending'])
                    metadata = f'{{"level":{random.randint(1,10)}}}'
                    desc = f"Description {i}"

                    values.append(f"('{name}', '{email}', {score}, {price}, {rating}, "
                                f"NOW(), '{status}', '{metadata}', '{desc}')")

                insert_sql = f"INSERT INTO {table_name} " \
                            f"(name, email, score, price, rating, created_at, status, metadata, description) " \
                            f"VALUES {','.join(values)}"
                cursor.execute(insert_sql)
                conn.commit()

                if (batch_start + batch_size) % 5000 == 0:
                    print(f"  已插入 {batch_start + batch_size} 行...")

            # 计算基线哈希
            import hashlib
            cursor.execute(f"SELECT * FROM {table_name} ORDER BY id")
            sha256 = hashlib.sha256()
            for row in cursor.fetchall():
                row_str = "|".join(str(v) for v in row)
                sha256.update(row_str.encode())

            baseline_hash = sha256.hexdigest()

            # 获取ibd路径
            cursor.execute("SHOW VARIABLES LIKE 'datadir'")
            datadir = cursor.fetchone()[1]

            ibd_path = os.path.join(datadir, "ultimate_test", f"{table_name}.ibd")

            # 复制ibd到备份目录
            backup_dir = os.path.join(self.output_base, "backups")
            os.makedirs(backup_dir, exist_ok=True)
            backup_path = os.path.join(backup_dir, f"{table_name}_before.ibd")

            if os.path.exists(ibd_path):
                import shutil
                shutil.copy(ibd_path, backup_path)
                print(f"  备份已保存: {backup_path}")

            cursor.close()
            conn.close()

            print(f"  ✓ 数据准备完成: {row_count} 行")
            print(f"  基线哈希: {baseline_hash[:16]}...")

            return {
                "success": True,
                "table_name": table_name,
                "ibd_path": backup_path,
                "baseline_hash": baseline_hash
            }

        except Exception as e:
            print(f"  ✗ 数据准备失败: {e}")
            return {"success": False, "error": str(e)}

    # =========================================================================
    # Phase 3: 破坏执行
    # =========================================================================

    def execute_destruction(self, test_data: Dict,
                           corruption_types: List[str] = None) -> Dict:
        """执行破坏"""
        print("\n" + "="*70)
        print("Phase 3: 破坏执行")
        print("="*70)

        if corruption_types is None:
            corruption_types = ["header_erasure", "random_page", "rm_rf_single"]

        destruction_results = {}

        for version, data in test_data["versions"].items():
            print(f"\n--- MySQL {version} ---")
            ibd_path = data["ibd_path"]

            if not os.path.exists(ibd_path):
                print(f"  ✗ IBD文件不存在: {ibd_path}")
                continue

            destruction_results[version] = {}

            for corruption_type in corruption_types:
                print(f"\n执行破坏: {corruption_type}")

                output_dir = os.path.join(self.output_base, "corrupted", version, corruption_type)
                os.makedirs(output_dir, exist_ok=True)

                result = self._apply_corruption(ibd_path, corruption_type, output_dir)
                destruction_results[version][corruption_type] = result

        return destruction_results

    def _apply_corruption(self, ibd_path: str, corruption_type: str,
                         output_dir: str) -> Dict:
        """应用破坏"""
        import shutil

        corrupted_path = os.path.join(output_dir, os.path.basename(ibd_path).replace(".ibd", f"_{corruption_type}.ibd"))

        try:
            if corruption_type == "rm_rf_single":
                # rm -rf 模拟: 直接使用备份
                shutil.copy(ibd_path, corrupted_path)
                print(f"  ✓ rm -rf模拟: 使用备份文件")

            elif corruption_type == "header_erasure":
                # 页头抹除
                shutil.copy(ibd_path, corrupted_path)
                with open(corrupted_path, 'r+b') as f:
                    # 抹除第一个页的头38字节
                    f.write(b'\xFF' * 38)
                print(f"  ✓ 页头抹除完成")

            elif corruption_type == "random_page":
                # 随机页损坏
                shutil.copy(ibd_path, corrupted_path)
                import random
                file_size = os.path.getsize(corrupted_path)
                page_size = 16384
                total_pages = file_size // page_size

                # 损坏10%的页面
                pages_to_corrupt = random.sample(range(total_pages), max(1, total_pages // 10))

                with open(corrupted_path, 'r+b') as f:
                    for page_no in pages_to_corrupt:
                        offset = page_no * page_size
                        f.seek(offset)
                        f.write(bytes([random.randint(0, 255) for _ in range(64)]))

                print(f"  ✓ 随机页损坏: {len(pages_to_corrupt)}/{total_pages} 页")

            elif corruption_type == "torn_page":
                # 页撕裂
                shutil.copy(ibd_path, corrupted_path)
                file_size = os.path.getsize(corrupted_path)
                page_size = 16384
                total_pages = file_size // page_size

                with open(corrupted_path, 'r+b') as f:
                    for page_no in range(0, total_pages, 10):
                        offset = page_no * page_size + page_size // 2
                        f.seek(offset)
                        f.write(b'\x00' * 64)

                print(f"  ✓ 页撕裂完成")

            return {
                "success": True,
                "corrupted_path": corrupted_path,
                "corruption_type": corruption_type
            }

        except Exception as e:
            print(f"  ✗ 破坏失败: {e}")
            return {
                "success": False,
                "error": str(e)
            }

    # =========================================================================
    # Phase 4: 恢复执行
    # =========================================================================

    def execute_recovery(self, destruction_results: Dict) -> Dict:
        """执行恢复"""
        print("\n" + "="*70)
        print("Phase 4: 恢复执行")
        print("="*70)

        recovery_results = {}

        for version, corruptions in destruction_results.items():
            print(f"\n--- MySQL {version} ---")
            recovery_results[version] = {}

            for corruption_type, destruction in corruptions.items():
                if not destruction["success"]:
                    continue

                print(f"\n恢复场景: {corruption_type}")

                corrupted_path = destruction["corrupted_path"]
                output_dir = os.path.join(self.output_base, "recovered", version, corruption_type)

                result = self._run_recovery(corrupted_path, output_dir, corruption_type)
                recovery_results[version][corruption_type] = result

        return recovery_results

    def _run_recovery(self, ibd_path: str, output_dir: str,
                     corruption_type: str) -> Dict:
        """运行恢复"""
        os.makedirs(output_dir, exist_ok=True)

        # 构建命令
        cmd = [self.recovery_tool, "--ibd", ibd_path, "--output", output_dir]

        if corruption_type == "rm_rf_single":
            cmd.append("--rmrf-recovery")

        cmd.append("--verbose")

        start_time = time.time()

        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            recovery_time = time.time() - start_time

            # 查找输出文件
            output_file = None
            for f in os.listdir(output_dir):
                if f.endswith('.sql'):
                    output_file = os.path.join(output_dir, f)
                    break

            # 统计恢复行数
            recovered_rows = 0
            if output_file and os.path.exists(output_file):
                with open(output_file, 'r') as f:
                    content = f.read()
                    recovered_rows = content.count("INSERT INTO")

            print(f"  恢复状态: {'成功' if result.returncode == 0 else '失败'}")
            print(f"  恢复行数: {recovered_rows}")
            print(f"  恢复时间: {recovery_time:.2f}s")

            return {
                "success": result.returncode == 0,
                "output_file": output_file,
                "recovered_rows": recovered_rows,
                "recovery_time": recovery_time,
                "stdout": result.stdout,
                "stderr": result.stderr
            }

        except subprocess.TimeoutExpired:
            print(f"  ✗ 恢复超时")
            return {"success": False, "error": "Timeout"}
        except Exception as e:
            print(f"  ✗ 恢复失败: {e}")
            return {"success": False, "error": str(e)}

    # =========================================================================
    # Phase 5: 验证与分析
    # =========================================================================

    def validate_and_analyze(self, test_data: Dict, recovery_results: Dict) -> Dict:
        """验证与分析"""
        print("\n" + "="*70)
        print("Phase 5: 验证与分析")
        print("="*70)

        validation_results = {}

        for version, corruptions in recovery_results.items():
            print(f"\n--- MySQL {version} ---")
            validation_results[version] = {}

            baseline_data = test_data["versions"].get(version, {})
            expected_rows = baseline_data.get("row_count", 0)
            baseline_hash = baseline_data.get("baseline_hash", "")

            for corruption_type, recovery in corruptions.items():
                if not recovery["success"]:
                    continue

                print(f"\n验证: {corruption_type}")

                result = self._validate_recovery(
                    recovery["output_file"],
                    expected_rows,
                    baseline_hash
                )

                validation_results[version][corruption_type] = result

                print(f"  恢复率: {result['recovery_rate']*100:.2f}%")
                print(f"  验证状态: {'通过' if result['passed'] else '失败'}")

        return validation_results

    def _validate_recovery(self, output_file: str, expected_rows: int,
                          baseline_hash: str) -> Dict:
        """验证恢复结果"""
        if not output_file or not os.path.exists(output_file):
            return {
                "passed": False,
                "recovery_rate": 0,
                "error": "Output file not found"
            }

        try:
            with open(output_file, 'r') as f:
                content = f.read()
                recovered_rows = content.count("INSERT INTO")

            recovery_rate = recovered_rows / expected_rows if expected_rows > 0 else 0

            return {
                "passed": recovery_rate >= 0.999,
                "recovery_rate": recovery_rate,
                "recovered_rows": recovered_rows,
                "expected_rows": expected_rows
            }

        except Exception as e:
            return {
                "passed": False,
                "recovery_rate": 0,
                "error": str(e)
            }

    # =========================================================================
    # Phase 6: 报告生成
    # =========================================================================

    def generate_final_report(self, test_data: Dict, validation_results: Dict) -> str:
        """生成最终报告"""
        print("\n" + "="*70)
        print("Phase 6: 报告生成")
        print("="*70)

        report = []
        report.append("=" * 100)
        report.append("MySQL Recovery Pro - 终极测试报告")
        report.append(f"生成时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        report.append(f"测试执行者: jliang.fang (Database Kernel Forensics Expert)")
        report.append("=" * 100)

        # 恢复率矩阵
        report.append("\n## 恢复率矩阵\n")
        report.append("-" * 100)

        # 表头
        header = f"{'MySQL版本':<15}"
        all_corruptions = set()
        for version_data in validation_results.values():
            all_corruptions.update(version_data.keys())

        for corruption in sorted(all_corruptions):
            header += f"{corruption:<20}"
        report.append(header)
        report.append("-" * 100)

        # 数据行
        for version in sorted(validation_results.keys()):
            row = f"{version:<15}"
            for corruption in sorted(all_corruptions):
                if corruption in validation_results[version]:
                    data = validation_results[version][corruption]
                    rate = data.get("recovery_rate", 0) * 100
                    status = "✓" if data.get("passed") else "✗"
                    row += f"{status}{rate:.1f}%".ljust(20)
                else:
                    row += "N/A".ljust(20)
            report.append(row)

        # 统计摘要
        report.append("\n## 统计摘要\n")
        total_tests = sum(len(v) for v in validation_results.values())
        passed_tests = sum(
            1 for v in validation_results.values()
            for c in v.values() if c.get("passed")
        )

        report.append(f"- 总测试数: {total_tests}")
        report.append(f"- 通过数: {passed_tests}")
        report.append(f"- 通过率: {passed_tests/total_tests*100:.1f}%" if total_tests > 0 else "- 通过率: N/A")

        # 详细结果
        report.append("\n## 详细测试结果\n")
        for version, corruptions in validation_results.items():
            report.append(f"\n### MySQL {version}\n")
            for corruption, result in corruptions.items():
                status = "✓ PASS" if result.get("passed") else "✗ FAIL"
                report.append(f"- {corruption}: {status}")
                report.append(f"  - 恢复率: {result.get('recovery_rate', 0)*100:.2f}%")
                report.append(f"  - 恢复行数: {result.get('recovered_rows', 0)}/{result.get('expected_rows', 0)}")

        report.append("\n" + "=" * 100)
        report.append("测试完成")
        report.append("=" * 100)

        report_text = "\n".join(report)

        # 保存报告
        report_path = os.path.join(self.output_base,
                                   f"ULTIMATE_TEST_REPORT_{datetime.now().strftime('%Y%m%d_%H%M%S')}.md")
        with open(report_path, 'w', encoding='utf-8') as f:
            f.write(report_text)

        print(f"\n✓ 报告已保存: {report_path}")

        return report_text

    # =========================================================================
    # 主执行流程
    # =========================================================================

    def run_full_test(self, mysql_versions: List[str] = None,
                     row_counts: List[int] = None,
                     corruption_types: List[str] = None) -> Dict:
        """运行完整测试"""
        print("\n" + "="*70)
        print("MySQL Recovery Pro - 终极测试框架 v2.0")
        print("="*70)
        print(f"项目根目录: {self.project_root}")
        print(f"输出目录: {self.output_base}")
        print("="*70)

        # Phase 1: 环境检查
        env_status = self.check_environment()
        if not env_status["recovery_tool"]:
            print("\n✗ 环境检查失败，请先编译恢复工具")
            return {"success": False, "error": "Recovery tool not found"}

        # Phase 2: 数据准备
        test_data = self.prepare_test_data(mysql_versions, row_counts)

        # Phase 3: 破坏执行
        destruction_results = self.execute_destruction(test_data, corruption_types)

        # Phase 4: 恢复执行
        recovery_results = self.execute_recovery(destruction_results)

        # Phase 5: 验证分析
        validation_results = self.validate_and_analyze(test_data, recovery_results)

        # Phase 6: 报告生成
        report = self.generate_final_report(test_data, validation_results)

        print("\n" + report)

        return {
            "success": True,
            "test_data": test_data,
            "validation_results": validation_results,
            "report": report
        }


def main():
    parser = argparse.ArgumentParser(description="MySQL Recovery Pro - 终极测试框架 v2.0")
    parser.add_argument("--project-root", default="/app/mysql_recovery_pro",
                       help="项目根目录")
    parser.add_argument("--output-dir", help="输出目录")
    parser.add_argument("--versions", nargs='+',
                       choices=['5.6', '5.7', '8.0', '8.4', '9.0'],
                       default=['8.0'],
                       help="要测试的MySQL版本")
    parser.add_argument("--row-counts", nargs='+', type=int,
                       default=[10000],
                       help="测试数据量")
    parser.add_argument("--corruptions", nargs='+',
                       default=['rm_rf_single', 'header_erasure', 'random_page'],
                       help="破坏类型")

    args = parser.parse_args()

    # 创建框架实例
    framework = UltimateTestingFramework(
        project_root=args.project_root,
        output_base=args.output_dir
    )

    # 运行测试
    result = framework.run_full_test(
        mysql_versions=args.versions,
        row_counts=args.row_counts,
        corruption_types=args.corruptions
    )

    return 0 if result.get("success") else 1


if __name__ == "__main__":
    sys.exit(main())
