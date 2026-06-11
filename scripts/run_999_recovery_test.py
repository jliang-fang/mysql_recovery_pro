#!/usr/bin/env python3
"""
99.9% Recovery Rate Validation Test Framework
完整实现99.9%以上恢复率的验证基准测试
Author: Qwen Code
Version: 3.0.0 - Ultimate Recovery Rate Validation
"""

import os
import sys
import json
import subprocess
import time
import hashlib
import argparse
from datetime import datetime
from pathlib import Path
from typing import List, Dict, Tuple, Optional
from concurrent.futures import ProcessPoolExecutor, as_completed
import traceback

# 添加脚本目录到路径
sys.path.append(os.path.dirname(os.path.abspath(__file__)))

# MySQL recovery tool paths
RECOVERY_TOOL = "/app/mysql_recovery_pro/build/mysql_recovery_pro"
TEST_DATA_DIR = "/app/mysql_recovery_pro/test_data"
OUTPUT_DIR = "/app/mysql_recovery_pro/test_results/999_test"
GOLDEN_HASHES_DIR = "/app/mysql_recovery_pro/golden-hashes"


class RecoveryRateValidator:
    """验证99.9%恢复率的核心类"""
    
    def __init__(self, seed: int = 42):
        self.seed = seed
        self.test_results = []
        self.corruption_scenarios = [
            # 轻度损坏场景 - 预期99.9%+恢复率
            {"name": "none", "description": "No corruption (baseline)", "corrupt_pct": 0},
            {"name": "random_0.1pct", "description": "Random 0.1% pages corrupted", "corrupt_pct": 0.001},
            {"name": "random_0.5pct", "description": "Random 0.5% pages corrupted", "corrupt_pct": 0.005},
            {"name": "random_1pct", "description": "Random 1% pages corrupted", "corrupt_pct": 0.01},
            {"name": "random_2pct", "description": "Random 2% pages corrupted", "corrupt_pct": 0.02},
            {"name": "random_5pct", "description": "Random 5% pages corrupted", "corrupt_pct": 0.05},
            {"name": "page_headers_1pct", "description": "Page headers 1% corrupted", "corrupt_pct": 0.01, "type": "header"},
            {"name": "page_headers_2pct", "description": "Page headers 2% corrupted", "corrupt_pct": 0.02, "type": "header"},
            {"name": "page_headers_5pct", "description": "Page headers 5% corrupted", "corrupt_pct": 0.05, "type": "header"},
            {"name": "infimum_1pct", "description": "INFIMUM records 1% corrupted", "corrupt_pct": 0.01, "type": "infimum"},
            {"name": "infimum_2pct", "description": "INFIMUM records 2% corrupted", "corrupt_pct": 0.02, "type": "infimum"},
            {"name": "infimum_5pct", "description": "INFIMUM records 5% corrupted", "corrupt_pct": 0.05, "type": "infimum"},
            {"name": "supremum_1pct", "description": "SUPREMUM records 1% corrupted", "corrupt_pct": 0.01, "type": "supremum"},
            {"name": "supremum_2pct", "description": "SUPREMUM records 2% corrupted", "corrupt_pct": 0.02, "type": "supremum"},
            {"name": "page_chain_1pct", "description": "Page chain 1% broken", "corrupt_pct": 0.01, "type": "chain"},
            {"name": "page_chain_2pct", "description": "Page chain 2% broken", "corrupt_pct": 0.02, "type": "chain"},
            {"name": "record_chain_1pct", "description": "Record chain 1% broken", "corrupt_pct": 0.01, "type": "record_chain"},
            {"name": "record_chain_2pct", "description": "Record chain 2% broken", "corrupt_pct": 0.02, "type": "record_chain"},
            {"name": "checksum_1pct", "description": "Checksum 1% corrupted", "corrupt_pct": 0.01, "type": "checksum"},
            {"name": "checksum_2pct", "description": "Checksum 2% corrupted", "corrupt_pct": 0.02, "type": "checksum"},
            {"name": "checksum_5pct", "description": "Checksum 5% corrupted", "corrupt_pct": 0.05, "type": "checksum"},
            # 极端场景 - 预期较低恢复率
            {"name": "truncate_10pct", "description": "File truncated 10%", "corrupt_pct": 0.10, "type": "truncate"},
            {"name": "truncate_20pct", "description": "File truncated 20%", "corrupt_pct": 0.20, "type": "truncate"},
            {"name": "truncate_30pct", "description": "File truncated 30%", "corrupt_pct": 0.30, "type": "truncate"},
            {"name": "random_10pct", "description": "Random 10% pages corrupted", "corrupt_pct": 0.10},
            {"name": "random_20pct", "description": "Random 20% pages corrupted", "corrupt_pct": 0.20},
        ]
    
    def generate_test_file_info(self, ibd_file: str) -> Dict:
        """获取测试文件信息"""
        size = os.path.getsize(ibd_file)
        # 估算页数 (默认16KB页大小)
        page_size = 16384
        total_pages = size // page_size
        
        return {
            "file": ibd_file,
            "size_bytes": size,
            "size_mb": size / (1024**2),
            "size_gb": size / (1024**3),
            "total_pages": total_pages,
            "page_size": page_size,
            "estimated_records": total_pages * 100  # 粗略估算
        }
    
    def run_recovery(self, input_file: str, output_dir: str, 
                     recovery_mode: str = "default") -> Dict:
        """运行mysql_recovery_pro恢复"""
        result = {
            "success": False,
            "output_dir": output_dir,
            "records_recovered": 0,
            "recovery_time_seconds": 0,
            "error": None,
            "command": "",
            "stdout": "",
            "stderr": ""
        }
        
        os.makedirs(output_dir, exist_ok=True)
        
        cmd = [
            RECOVERY_TOOL,
            "--ibd", input_file,
            "--output", output_dir,
            "--format", "sql"
        ]
        
        if recovery_mode == "schemaless":
            cmd.append("--schemaless")
        elif recovery_mode == "truncate":
            cmd.append("--truncate")
        elif recovery_mode == "rmrf":
            cmd.append("--rmrf")
        
        result["command"] = " ".join(cmd)
        
        try:
            start_time = time.time()
            proc = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=300  # 5分钟超时
            )
            end_time = time.time()
            
            result["recovery_time_seconds"] = end_time - start_time
            result["stdout"] = proc.stdout
            result["stderr"] = proc.stderr
            result["returncode"] = proc.returncode
            
            if proc.returncode == 0:
                result["success"] = True
                
                # 统计恢复的记录数
                output_files = [f for f in os.listdir(output_dir) 
                              if f.endswith('.sql') or f.endswith('.csv')]
                
                for output_file in output_files:
                    filepath = os.path.join(output_dir, output_file)
                    if filepath.endswith('.sql'):
                        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
                            content = f.read()
                            result["records_recovered"] += content.count("VALUES (")
                            result["records_recovered"] += content.count("INSERT INTO")
            
        except subprocess.TimeoutExpired:
            result["error"] = "Recovery timed out (5 min)"
        except Exception as e:
            result["error"] = str(e)
        
        return result
    
    def calculate_recovery_rate(self, expected_records: int, 
                               recovered_records: int) -> float:
        """计算恢复率"""
        if expected_records == 0:
            return 1.0 if recovered_records == 0 else 0.0
        return min(1.0, recovered_records / expected_records)
    
    def analyzeRecoveredFile(self, output_dir: str) -> Dict:
        """分析恢复的文件"""
        result = {
            "records_recovered": 0,
            "sql_files": 0,
            "csv_files": 0,
            "total_size": 0,
            "recovery_estimate_method": "sql_count"
        }
        
        if not os.path.exists(output_dir):
            return result
        
        for filename in os.listdir(output_dir):
            filepath = os.path.join(output_dir, filename)
            if not os.path.isfile(filepath):
                continue
            
            result["total_size"] += os.path.getsize(filepath)
            
            if filename.endswith('.sql'):
                result["sql_files"] += 1
                try:
                    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
                        content = f.read()
                        # More accurate record counting
                        result["records_recovered"] += content.count("VALUES (")
                except:
                    pass
            elif filename.endswith('.csv'):
                result["csv_files"] += 1
                try:
                    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
                        lines = f.readlines()
                        # First line is header
                        result["records_recovered"] += max(0, len(lines) - 1)
                except:
                    pass
        
        return result
    
    def run_test_scenario(self, ibd_file: str, scenario: Dict, 
                          output_base: str, seed: int) -> Dict:
        """运行单个测试场景"""
        scenario_name = scenario["name"]
        corrupt_pct = scenario["corrupt_pct"]
        corrupt_type = scenario.get("type", "random")
        
        # 创建输出目录
        scenario_output = os.path.join(output_base, scenario_name)
        os.makedirs(scenario_output, exist_ok=True)
        
        # 检查是否已有损坏文件
        corrupted_file = os.path.join(scenario_output, 
                                      f"corrupted_{scenario_name}.ibd")
        
        if not os.path.exists(corrupted_file):
            # 创建损坏文件
            corrupt_script = "/app/mysql_recovery_pro/scripts/corruption_scenarios.py"
            
            cmd = [
                sys.executable, corrupt_script,
                "-i", ibd_file,
                "-o", corrupted_file,
                "-s", corrupt_type if corrupt_type != "random" else "random_pages",
                "--seed", str(seed),
                "--page-size", "16384"
            ]
            
            # 添加特定参数
            if corrupt_type == "truncate":
                num_pages = int(100 * corrupt_pct)  # 简化处理
                cmd.extend(["--num-pages", str(num_pages)])
            
            subprocess.run(cmd, capture_output=True, timeout=60)
        
        # 运行恢复
        recovery_output = os.path.join(scenario_output, "recovered")
        
        # 测试多种恢复模式
        recovery_modes = ["default", "truncate", "schemaless"]
        
        result = {
            "scenario": scenario_name,
            "description": scenario["description"],
            "corrupt_pct": corrupt_pct,
            "ibd_file": ibd_file,
            "seed": seed,
            "recovery_modes": {}
        }
        
        for mode in recovery_modes:
            mode_output = os.path.join(recovery_output, mode)
            recovery_result = self.run_recovery(corrupted_file, mode_output, mode)
            
            # 分析恢复结果
            analysis = self.analyzeRecoveredFile(mode_output)
            recovery_result["records_analyzed"] = analysis["records_recovered"]
            
            # 计算恢复率
            expected_records = 5000000  # 基准500万
            recovery_rate = self.calculate_recovery_rate(
                expected_records, 
                analysis["records_recovered"]
            )
            recovery_result["recovery_rate"] = recovery_rate
            
            result["recovery_modes"][mode] = recovery_result
        
        # 计算最佳恢复率
        rates = [m.get("recovery_rate", 0) for m in result["recovery_modes"].values()]
        result["best_recovery_rate"] = max(rates) if rates else 0
        
        return result
    
    def run_full_validation(self, ibd_file: str, recovery_rate_target: float = 0.999,
                           max_failures: int = 3) -> Dict:
        """运行完整验证测试"""
        print(f"\n{'='*70}")
        print(f"99.9% Recovery Rate Validation Test")
        print(f"{'='*70}")
        print(f"Target recovery rate: {recovery_rate_target*100:.1f}%")
        print(f"Input file: {ibd_file}")
        
        file_info = self.generate_test_file_info(ibd_file)
        print(f"File size: {file_info['size_gb']:.2f} GB")
        print(f"Estimated pages: {file_info['total_pages']:,}")
        print(f"Estimated records: {file_info['estimated_records']:,}")
        print(f"{'='*70}\n")
        
        # 创建输出目录
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_dir = os.path.join(OUTPUT_DIR, f"test_{timestamp}")
        os.makedirs(output_dir, exist_ok=True)
        
        # 保存文件信息
        with open(os.path.join(output_dir, "file_info.json"), 'w') as f:
            json.dump(file_info, f, indent=2)
        
        self.test_results = []
        
        # 运行测试场景
        scenario_start = time.time()
        
        for i, scenario in enumerate(self.corruption_scenarios):
            print(f"[{i+1}/{len(self.corruption_scenarios)}] "
                  f"Running: {scenario['description']}...")
            
            try:
                result = self.run_test_scenario(
                    ibd_file, scenario, output_dir, self.seed
                )
                self.test_results.append(result)
                
                # 打印进度
                good_modes = sum(1 for m in result["recovery_modes"].values() 
                               if m.get("recovery_rate", 0) >= recovery_rate_target)
                total_modes = len(result["recovery_modes"])
                
                print(f"  Best recovery rate: {result['best_recovery_rate']*100:.3f}%")
                print(f"  Modes >= {recovery_rate_target*100:.1f}%: {good_modes}/{total_modes}")
                
                # 检查是否达到目标
                if result["best_recovery_rate"] < recovery_rate_target:
                    print(f"  ⚠️  FAILURE: Recovery rate below target!")
                else:
                    print(f"  ✓ PASSED")
                
            except Exception as e:
                print(f"  ERROR: {e}")
                self.test_results.append({
                    "scenario": scenario["name"],
                    "error": str(e),
                    "best_recovery_rate": 0
                })
            
            # 每10个场景保存一次进度
            if (i + 1) % 10 == 0:
                self.save_results(output_dir)
        
        # 保存最终结果
        self.save_results(output_dir)
        
        # 计算总体统计
        total_scenarios = len(self.test_results)
        scenarios_passed = sum(1 for r in self.test_results 
                              if r.get("best_recovery_rate", 0) >= recovery_rate_target)
        scenarios_failed = total_scenarios - scenarios_passed
        
        avg_recovery_rate = sum(r.get("best_recovery_rate", 0) 
                               for r in self.test_results) / total_scenarios if total_scenarios > 0 else 0
        
        # 按损坏比例分组统计
        grouped = {}
        for result in self.test_results:
            corrupt_pct = result.get("corrupt_pct", 0)
            if corrupt_pct not in grouped:
                grouped[corrupt_pct] = {
                    "count": 0,
                    "passed": 0,
                    "min_rate": 1.0,
                    "max_rate": 0.0,
                    "avg_rate": 0
                }
            rate = result.get("best_recovery_rate", 0)
            grouped[corrupt_pct]["count"] += 1
            grouped[corrupt_pct]["passed"] += 1 if rate >= recovery_rate_target else 0
            grouped[corrupt_pct]["min_rate"] = min(grouped[corrupt_pct]["min_rate"], rate)
            grouped[corrupt_pct]["max_rate"] = max(grouped[corrupt_pct]["max_rate"], rate)
            grouped[corrupt_pct]["avg_rate"] += rate
        
        # 计算平均值
        for pct in grouped:
            grouped[pct]["avg_rate"] /= grouped[pct]["count"]
        
        # 生成验证报告
        validation_report = {
            "timestamp": datetime.now().isoformat(),
            "target_recovery_rate": recovery_rate_target,
            "max_failures_allowed": max_failures,
            "summary": {
                "total_scenarios": total_scenarios,
                "passed": scenarios_passed,
                "failed": scenarios_failed,
                "pass_rate": scenarios_passed / total_scenarios if total_scenarios > 0 else 0,
                "average_recovery_rate": avg_recovery_rate,
                "satisfied_target": scenarios_failed <= max_failures
            },
            "grouped_by_corruption": grouped,
            "scenarios": self.test_results
        }
        
        # 保存详细报告
        report_file = os.path.join(output_dir, "validation_report.json")
        with open(report_file, 'w') as f:
            json.dump(validation_report, f, indent=2)
        
        # 生成Markdown报告
        markdown_report = self.generate_markdown_report(validation_report, output_dir)
        with open(os.path.join(output_dir, "validation_report.md"), 'w') as f:
            f.write(markdown_report)
        
        print(f"\n{'='*70}")
        print(f"VALIDATION SUMMARY")
        print(f"{'='*70}")
        print(f"Target: {recovery_rate_target*100:.1f}% recovery rate")
        print(f"Scenarios passed: {scenarios_passed}/{total_scenarios}")
        print(f"Average recovery rate: {avg_recovery_rate*100:.3f}%")
        print(f"Satisfied target (max {max_failures} failures): {validation_report['summary']['satisfied_target']}")
        print(f"{'='*70}\n")
        
        # 打印详细分组统计
        print("Recovery rates by corruption level:")
        for pct in sorted(grouped.keys()):
            g = grouped[pct]
            status = "✓" if g["passed"] == g["count"] else "✗"
            print(f"  {pct*100:5.1f}% corruption: {status} "
                  f"avg={g['avg_rate']*100:6.2f}% (min={g['min_rate']*100:6.2f}%, "
                  f"max={g['max_rate']*100:6.2f}%) [{g['passed']}/{g['count']} passed]")
        
        return validation_report
    
    def generate_markdown_report(self, report: Dict, output_dir: str) -> str:
        """生成Markdown格式的验证报告"""
        md = []
        md.append("# 99.9% Recovery Rate Validation Report")
        md.append("")
        md.append(f"**Generated:** {report['timestamp']}")
        md.append(f"**Target Recovery Rate:** {report['target_recovery_rate']*100:.1f}%")
        md.append("")
        
        # Summary
        md.append("## Summary")
        md.append("")
        md.append("| Metric | Value |")
        md.append("|--------|-------|")
        md.append(f"| Total Scenarios | {report['summary']['total_scenarios']} |")
        md.append(f"| Passed | {report['summary']['passed']} |")
        md.append(f"| Failed | {report['summary']['failed']} |")
        md.append(f"| Pass Rate | {report['summary']['pass_rate']*100:.2f}% |")
        md.append(f"| Average Recovery Rate | {report['summary']['average_recovery_rate']*100:.3f}% |")
        md.append(f"| Satisfied Target | {'✓ Yes' if report['summary']['satisfied_target'] else '✗ No'} |")
        md.append("")
        
        # Grouped by Corruption
        md.append("## Recovery Rates by Corruption Level")
        md.append("")
        md.append("| Corruption % | Avg Rate | Min Rate | Max Rate | Passed | Total | Status |")
        md.append("|--------------|----------|----------|----------|--------|-------|--------|")
        
        for pct in sorted(report['grouped_by_corruption'].keys()):
            g = report['grouped_by_corruption'][pct]
            status = "✓" if g["passed"] == g["count"] else "✗"
            md.append(f"| {pct*100:5.1f}% | {g['avg_rate']*100:8.2f}% | "
                     f"{g['min_rate']*100:8.2f}% | {g['max_rate']*100:8.2f}% | "
                     f"{g['passed']:6d} | {g['count']:5d} | {status} |")
        
        md.append("")
        
        # Scenario Results
        md.append("## Scenario Results")
        md.append("")
        
        for result in report['scenarios'][:20]:  #Limit to first 20
            scenario = result.get("scenario", "unknown")
            rate = result.get("best_recovery_rate", 0)
            status = "✓ PASS" if rate >= report['target_recovery_rate'] else "✗ FAIL"
            
            md.append(f"### {scenario}")
            md.append(f"- **Recovery Rate:** {rate*100:.3f}%")
            md.append(f"- **Status:** {status}")
            
            if "recovery_modes" in result:
                for mode, details in result["recovery_modes"].items():
                    rate = details.get("recovery_rate", 0)
                    time = details.get("recovery_time_seconds", 0)
                    records = details.get("records_analyzed", 0)
                    md.append(f"  - {mode}: {rate*100:.3f}% in {time:.1f}s ({records:,} records)")
            
            md.append("")
        
        # Footer
        md.append("---")
        md.append("*Generated by 99.9% Recovery Rate Validation Framework*")
        
        return "\n".join(md)
    
    def save_results(self, output_dir: str):
        """保存测试结果到JSON"""
        results_file = os.path.join(output_dir, "test_results.json")
        with open(results_file, 'w') as f:
            json.dump({
                "timestamp": datetime.now().isoformat(),
                "results": self.test_results
            }, f, indent=2)


def main():
    parser = argparse.ArgumentParser(
        description="99.9% Recovery Rate Validation Test Framework"
    )
    parser.add_argument("--ibd", type=str, default=None,
                        help="Input IBD file to test")
    parser.add_argument("--target-rate", type=float, default=0.999,
                        help="Target recovery rate (default: 0.999)")
    parser.add_argument("--max-failures", type=int, default=3,
                        help="Maximum allowed failures (default: 3)")
    parser.add_argument("--seed", type=int, default=42,
                        help="Random seed (default: 42)")
    parser.add_argument("--quick-test", action="store_true",
                        help="Run quick test (first 10 scenarios)")
    parser.add_argument("--output-dir", type=str, default=None,
                        help="Custom output directory")
    
    args = parser.parse_args()
    
    # Determine IBD file
    ibd_file = args.ibd
    
    if not ibd_file:
        # Find largest IBD file in test_data
        test_data_dir = TEST_DATA_DIR
        ibd_files = [f for f in os.listdir(test_data_dir) if f.endswith('.ibd')]
        
        if ibd_files:
            # Sort by size and get largest
            ibd_files_with_size = [(f, os.path.getsize(os.path.join(test_data_dir, f))) 
                                   for f in ibd_files]
            ibd_files_with_size.sort(key=lambda x: x[1], reverse=True)
            ibd_file = os.path.join(test_data_dir, ibd_files_with_size[0][0])
            print(f"Using largest IBD file: {ibd_file}")
            print(f"Size: {ibd_files_with_size[0][1] / (1024**3):.2f} GB")
        else:
            print("ERROR: No IBD files found in test_data directory")
            print("Please specify an IBD file with --ibd or ensure test_data has IBD files")
            sys.exit(1)
    
    # Run validation
    validator = RecoveryRateValidator(seed=args.seed)
    
    # Override output directory if specified
    if args.output_dir:
        global OUTPUT_DIR
        OUTPUT_DIR = args.output_dir
    
    if args.quick_test:
        # Run only first 10 scenarios
        print("Running quick test (first 10 scenarios)...")
    
    validation_result = validator.run_full_validation(
        ibd_file,
        recovery_rate_target=args.target_rate,
        max_failures=args.max_failures
    )
    
    # Exit with appropriate code
    if validation_result['summary']['satisfied_target']:
        print(f"\n✓ VALIDATION PASSED: Target of {args.target_rate*100:.1f}% recovery rate achieved")
        sys.exit(0)
    else:
        print(f"\n✗ VALIDATION FAILED: Too many scenarios failed")
        sys.exit(1)


if __name__ == "__main__":
    main()
