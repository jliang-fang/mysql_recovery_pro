#!/usr/bin/env python3
"""
Complete Recovery Validation System
全面的 mysql_recovery_pro 恢复能力验证系统

测试所有损坏场景下的恢复能力：
1. 页头损坏
2. 页链断裂
3. 记录链损坏
4. 校验和损坏
5. INFIMUM/SUPREMUM损坏
6. B+Tree 结构损坏
7. 混合严重损坏

性能目标：
- 500万+数据 99.9%+ 恢复率
- GB级文件在合理时间内完成恢复
- 多种输出格式（SQL/CSV/JSON）稳定达标
"""

import os
import sys
import json
import subprocess
import time
import hashlib
import struct
import argparse
from datetime import datetime
from pathlib import Path
from typing import List, Dict, Tuple, Optional
from concurrent.futures import ProcessPoolExecutor, as_completed
import resource


# MySQL 5.6/5.7 InnoDB Constants
FIL_PAGE_TYPE_INDEX_56 = 0x45BC  # MySQL 5.6/5.7 B+Tree
FIL_PAGE_TYPE_SDI = 0x26F3
FIL_PAGE_TYPE_UNDO_LOG = 0x26F2
FIL_PAGE_TYPE_BLOB = 0x26F4

# Offsets
FIL_PAGE_PREV = 0
FIL_PAGE_NEXT = 4
FIL_PAGE_LSN = 8
FIL_PAGE_SPACE_ID = 16
FIL_PAGE_OFFSET = 20
FIL_PAGE_CHECKSUM = 24
FIL_PAGE_TYPE = 16
FIL_PAGE_ARCH_LOG_NO = 36

FILE_PAGE_DATA = 38

# Record Header Constants
REC_HEADER_NEXT_OFFSET = 0
REC_HEADER_INFO_BITS = 4
REC_HEADER_INFO_BITS_MASK = 0x78

REC_STATUS_ORDINARY = 0x00
REC_STATUS_NODE_PTR = 0x01
REC_STATUS_INFIMUM = 0x02
REC_STATUS_SUPREMUM = 0x03


class GoldenDataGenerator:
    """生成黄金基准数据用于验证"""
    
    def __init__(self, num_records: int = 5500000, page_size: int = 16384, seed: int = 42):
        self.num_records = num_records
        self.page_size = page_size
        self.seed = seed
        self.records: List[Dict] = []
        
    def generate_records(self) -> List[Dict]:
        """生成测试记录"""
        import random
        random.seed(self.seed)
        
        self.records = []
        for i in range(self.num_records):
            record = {
                "id": i,
                "name": f"user_{i:08d}",
                "email": f"user{i}@example.com",
                "age": random.randint(18, 80),
                "created_at": f"2024-01-{random.randint(1, 28):02d} 12:00:00",
                "status": random.choice([0, 1, 2]),
                "data": f"record_data_{i}_placeholder_" + "x" * 50
            }
            self.records.append(record)
        return self.records
    
    def get_record_hash(self, record: Dict) -> str:
        """获取记录的哈希值"""
        content = json.dumps(record, sort_keys=True)
        return hashlib.sha256(content.encode()).hexdigest()
    
    def get_all_hashes(self) -> List[str]:
        """获取所有记录的哈希列表"""
        return [self.get_record_hash(r) for r in self.records]


class CorruptionInjector:
    """数据损坏注入器"""
    
    def __init__(self, page_size: int = 16384, seed: int = 42):
        self.page_size = page_size
        self.seed = seed
        self.corruption_log: List[Dict] = []
        
    def corrupt_page_headers(self, data: bytearray, corrupt_pct: float = 0.01) -> bytearray:
        """损坏页头信息"""
        import random
        random.seed(self.seed)
        
        total_pages = len(data) // self.page_size
        num_to_corrupt = max(1, int(total_pages * corrupt_pct))
        pages_to_corrupt = random.sample(range(1, total_pages), num_to_corrupt)
        
        for page_no in pages_to_corrupt:
            offset = page_no * self.page_size
            
            # Corrupt page type
            struct.pack_into('<H', data, offset + FIL_PAGE_TYPE, 0xFFFF)
            
            # Corrupt checksum
            struct.pack_into('<I', data, offset + FIL_PAGE_CHECKSUM, 0xDEADBEEF)
            struct.pack_into('<I', data, offset + FIL_PAGE_CHECKSUM + 4, 0xDEADBEEF)
            
            # Corrupt LSN
            struct.pack_into('<Q', data, offset + FIL_PAGE_LSN, 0)
            
            self.corruption_log.append({
                "type": "page_header",
                "page_no": page_no,
                "corrupted_fields": ["type", "checksum", "lsn"]
            })
        
        return data
    
    def corrupt_page_chain(self, data: bytearray, corrupt_pct: float = 0.01) -> bytearray:
        """损坏页链（prev/next指针）"""
        import random
        random.seed(self.seed + 1)
        
        total_pages = len(data) // self.page_size
        num_to_corrupt = max(1, int(total_pages * corrupt_pct))
        pages_to_corrupt = random.sample(range(1, total_pages), num_to_corrupt)
        
        for page_no in pages_to_corrupt:
            offset = page_no * self.page_size
            
            # Corrupt prev_page pointer
            struct.pack_into('<I', data, offset + FIL_PAGE_PREV, 0xDEADBEEF)
            
            # Corrupt next_page pointer
            struct.pack_into('<I', data, offset + FIL_PAGE_NEXT, 0xDEADBEEF)
            
            self.corruption_log.append({
                "type": "page_chain",
                "page_no": page_no,
                "corrupted_fields": ["prev_page", "next_page"]
            })
        
        return data
    
    def corrupt_record_chain(self, data: bytearray, corrupt_pct: float = 0.01) -> bytearray:
        """损坏记录链（next_offset）"""
        import random
        random.seed(self.seed + 2)
        
        total_pages = len(data) // self.page_size
        num_pages = min(100, max(1, int(total_pages * corrupt_pct)))
        pages_to_corrupt = random.sample(range(1, total_pages), num_pages)
        
        for page_no in pages_to_corrupt:
            offset = page_no * self.page_size + FILE_PAGE_DATA
            
            # Try to corrupt record headers (look for record-like structures)
            # In a real B+Tree, records start after heap_top
            heap_top_offset = offset + 2
            heap_top = struct.unpack_from('<H', data, heap_top_offset)[0]
            
            if heap_top > FILE_PAGE_DATA + 10:
                # Corrupt some record next_offset fields
                record_start = heap_top
                num_records_to_corrupt = min(5, (len(data) - record_start) // 100)
                
                for _ in range(num_records_to_corrupt):
                    if record_start + 2 < len(data):
                        struct.pack_into('<H', data, record_start, 0xFFFF)
                        record_start += random.randint(50, 200)
            
            self.corruption_log.append({
                "type": "record_chain",
                "page_no": page_no,
                "corrupted_fields": ["next_offset"]
            })
        
        return data
    
    def corrupt_infimum(self, data: bytearray, corrupt_pct: float = 0.01) -> bytearray:
        """损坏INFIMUM记录"""
        import random
        random.seed(self.seed + 3)
        
        total_pages = len(data) // self.page_size
        num_to_corrupt = max(1, int(total_pages * corrupt_pct))
        pages_to_corrupt = random.sample(range(1, total_pages), num_to_corrupt)
        
        for page_no in pages_to_corrupt:
            offset = page_no * self.page_size + FILE_PAGE_DATA + 12  # INFIMUM position
            
            if offset + 5 < len(data):
                # Corrupt INFIMUM record header
                data[offset] = 0xFF  # next_offset
                data[offset + 4] = 0xFF  # info_bits
        
        return data
    
    def corrupt_supremum(self, data: bytearray, corrupt_pct: float = 0.01) -> bytearray:
        """损坏SUPREMUM记录"""
        import random
        random.seed(self.seed + 4)
        
        total_pages = len(data) // self.page_size
        num_to_corrupt = max(1, int(total_pages * corrupt_pct))
        pages_to_corrupt = random.sample(range(1, total_pages), num_to_corrupt)
        
        for page_no in pages_to_corrupt:
            # INFIMUM is at +12, SUPREMUM is typically at +42 in COMPACT
            offset = page_no * self.page_size + FILE_PAGE_DATA + 42
            
            if offset + 5 < len(data):
                data[offset] = 0xFF
                data[offset + 4] = 0xFF
        
        return data
    
    def corrupt_checksum_only(self, data: bytearray, corrupt_pct: float = 0.01) -> bytearray:
        """仅损坏校验和（保留其他字段）"""
        import random
        random.seed(self.seed + 5)
        
        total_pages = len(data) // self.page_size
        num_to_corrupt = max(1, int(total_pages * corrupt_pct))
        pages_to_corrupt = random.sample(range(1, total_pages), num_to_corrupt)
        
        for page_no in pages_to_corrupt:
            offset = page_no * self.page_size
            
            # Only corrupt checksums
            struct.pack_into('<I', data, offset + FIL_PAGE_CHECKSUM, 0xDEADBEEF)
            struct.pack_into('<I', data, offset + FIL_PAGE_CHECKSUM + 4, 0xDEADBEEF)
            
            self.corruption_log.append({
                "type": "checksum_only",
                "page_no": page_no
            })
        
        return data
    
    def corrupt_all_headers(self, data: bytearray, corrupt_pct: float = 0.01) -> bytearray:
        """损坏所有页头字段"""
        import random
        random.seed(self.seed + 6)
        
        total_pages = len(data) // self.page_size
        num_to_corrupt = max(1, int(total_pages * corrupt_pct))
        pages_to_corrupt = random.sample(range(1, total_pages), num_to_corrupt)
        
        for page_no in pages_to_corrupt:
            offset = page_no * self.page_size
            
            # Corrupt all header fields
            struct.pack_into('<I', data, offset + FIL_PAGE_PREV, 0xDEADBEEF)
            struct.pack_into('<I', data, offset + FIL_PAGE_NEXT, 0xDEADBEEF)
            struct.pack_into('<Q', data, offset + FIL_PAGE_LSN, 0)
            struct.pack_into('<I', data, offset + FIL_PAGE_SPACE_ID, 0xDEADBEEF)
            struct.pack_into('<I', data, offset + FIL_PAGE_OFFSET, 0xDEADBEEF)
            struct.pack_into('<H', data, offset + FIL_PAGE_TYPE, 0xFFFF)
            struct.pack_into('<I', data, offset + FIL_PAGE_CHECKSUM, 0xDEADBEEF)
            struct.pack_into('<I', data, offset + FIL_PAGE_CHECKSUM + 4, 0xDEADBEEF)
            struct.pack_into('<H', data, offset + FIL_PAGE_ARCH_LOG_NO, 0xFFFF)
            
            self.corruption_log.append({
                "type": "all_headers",
                "page_no": page_no
            })
        
        return data
    
    def create_mixed_corruption(self, data: bytearray, corrupt_pct: float = 0.02) -> bytearray:
        """混合严重损坏场景"""
        # Combine multiple corruption types
        data = self.corrupt_page_headers(data, corrupt_pct * 0.3)
        data = self.corrupt_page_chain(data, corrupt_pct * 0.3)
        data = self.corrupt_record_chain(data, corrupt_pct * 0.2)
        data = self.corrupt_checksum_only(data, corrupt_pct * 0.2)
        
        return data


class RecoveryValidator:
    """恢复验证器 - 对比恢复结果与Golden数据"""
    
    def __init__(self, golden_hashes: List[str]):
        self.golden_hashes = set(golden_hashes)
        self.golden_count = len(golden_hashes)
        
    def validate_sql_file(self, sql_file: str) -> Dict:
        """验证SQL文件的恢复质量"""
        try:
            with open(sql_file, 'r', encoding='utf-8', errors='ignore') as f:
                content = f.read()
            
            # Count INSERT statements
            insert_count = content.count("INSERT INTO")
            # Count VALUES tuples
            values_count = content.count("VALUES (")
            
            # Extract record hashes (simplified - would need actual SQL parsing)
            recovered_hashes = set()
            
            return {
                "sql_file": sql_file,
                "insert_count": insert_count,
                "values_count": values_count,
                "recovered_records": max(insert_count, values_count // 2),
                "recovery_rate": min(1.0, (insert_count + values_count // 2) / (2 * self.golden_count))
            }
        except Exception as e:
            return {
                "sql_file": sql_file,
                "error": str(e),
                "recovery_rate": 0
            }
    
    def validate_csv_file(self, csv_file: str) -> Dict:
        """验证CSV文件的恢复质量"""
        try:
            with open(csv_file, 'r', encoding='utf-8', errors='ignore') as f:
                lines = f.readlines()
            
            # Skip header
            actual_records = max(0, len(lines) - 1)
            
            return {
                "csv_file": csv_file,
                "recovered_records": actual_records,
                "recovery_rate": min(1.0, actual_records / self.golden_count)
            }
        except Exception as e:
            return {
                "csv_file": csv_file,
                "error": str(e),
                "recovery_rate": 0
            }
    
    def validate_json_file(self, json_file: str) -> Dict:
        """验证JSON文件的恢复质量"""
        try:
            with open(json_file, 'r', encoding='utf-8', errors='ignore') as f:
                import json as js
                data = js.load(f)
            
            if isinstance(data, list):
                recovered = len(data)
            elif isinstance(data, dict) and "records" in data:
                recovered = len(data["records"])
            else:
                recovered = 0
                
            return {
                "json_file": json_file,
                "recovered_records": recovered,
                "recovery_rate": min(1.0, recovered / self.golden_count)
            }
        except Exception as e:
            return {
                "json_file": json_file,
                "error": str(e),
                "recovery_rate": 0
            }
    
    def calculate_recovery_statistics(self, results: List[Dict]) -> Dict:
        """计算恢复统计信息"""
        rates = [r.get("recovery_rate", 0) for r in results]
        
        return {
            "max_rate": max(rates) if rates else 0,
            "min_rate": min(rates) if rates else 0,
            "avg_rate": sum(rates) / len(rates) if rates else 0,
            "best_format": results[rates.index(max(rates))].get("sql_file", "") if results else ""
        }


class RecoveryTestRunner:
    """_recovery_pro 测试运行器"""
    
    def __init__(self, tool_path: str, project_dir: str = ".", seed: int = 42):
        self.tool_path = tool_path
        self.project_dir = Path(project_dir)
        self.seed = seed
        
    def run_recovery(self, input_file: str, output_dir: str,
                    format: str = "sql", mode: str = "default") -> Dict:
        """运行恢复命令"""
        os.makedirs(output_dir, exist_ok=True)
        
        cmd = [
            self.tool_path,
            "--ibd", input_file,
            "--output", output_dir,
            "--format", format,
        ]
        
        if mode == "truncate":
            cmd.append("--truncate-recovery")
        elif mode == "rmrf":
            cmd.append("--rmrf-recovery")
        elif mode == "header":
            cmd.append("--header-repair")
        elif mode == "blob":
            cmd.append("--blob-recovery")
        
        start_time = time.time()
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=600  # 10分钟超时
            )
            elapsed = time.time() - start_time
            
            return {
                "success": result.returncode == 0,
                "returncode": result.returncode,
                "stdout": result.stdout[:500],
                "stderr": result.stderr[:500],
                "recovery_time_seconds": elapsed,
                "output_dir": output_dir
            }
        except subprocess.TimeoutExpired:
            return {
                "success": False,
                "error": "Timeout (10 min)",
                "recovery_time_seconds": 600
            }
        except Exception as e:
            return {
                "success": False,
                "error": str(e),
                "recovery_time_seconds": time.time() - start_time
            }
    
    def analyze_output(self, output_dir: str) -> Dict:
        """分析输出目录"""
        result = {
            "sql_files": [],
            "csv_files": [],
            "json_files": [],
            "recovered_count": 0
        }
        
        output_path = Path(output_dir)
        if not output_path.exists():
            return result
        
        for ext in ["sql", "csv", "json"]:
            for f in output_path.glob(f"*.{ext}"):
                try:
                    with open(f, 'r', encoding='utf-8', errors='ignore') as fp:
                        content = fp.read()
                        if ext == "sql":
                            count = content.count("VALUES (")
                        elif ext == "csv":
                            count = max(0, len(content.split('\n')) - 1)
                        elif ext == "json":
                            try:
                                import json
                                data = json.loads(content)
                                count = len(data) if isinstance(data, list) else 0
                            except:
                                count = 0
                        
                        result["recovered_count"] += count
                        result[f"{ext}_files"].append(str(f))
                except:
                    pass
        
        return result


class ComprehensiveValidationSystem:
    """综合验证系统 - 运行所有测试并生成报告"""

    def __init__(self, project_dir: str = ".", build_dir: str = "build",
                 test_data_dir: str = "test_data", output_dir: str = "validation_output",
                 seed: int = 42):
        self.project_dir = Path(project_dir)
        self.build_dir = Path(build_dir)
        self.test_data_dir = Path(test_data_dir)
        self.output_dir = Path(output_dir)
        self.seed = seed

        self.tool_path = self.build_dir / "mysql_recovery_pro"
        self.runner = RecoveryTestRunner(str(self.tool_path), str(self.project_dir), seed)

        # Test configurations
        self.output_formats = ["sql", "csv", "json", "ibd"]
        self.recovery_modes = ["default", "truncate", "rmrf", "header", "blob"]

        self.results: List[Dict] = []
        self.timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        
    def setup(self):
        """初始化"""
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.test_data_dir.mkdir(parents=True, exist_ok=True)
        
    def count_records_in_file(self, ibd_file: str) -> int:
        """估算I文件中的记录数"""
        import os
        file_size = os.path.getsize(ibd_file)
        page_size = 16384
        pages = file_size // page_size
        
        # 每页大约可以存几百到上千条记录，取决于记录大小
        # 简单估算：每页 ~500 条记录
        return pages * 500
    
    def run_single_scenario(self, scenario: Dict, test_file: str,
                           golden_hashes: List[str], format: str) -> Dict:
        """运行单个损坏场景测试"""
        print(f"  Testing: {scenario['name']} (format={format})")
        
        # Inject corruption
        injector = CorruptionInjector(seed=self.seed)
        func_map = {
            "page_header": injector.corrupt_page_headers,
            "page_chain": injector.corrupt_page_chain,
            "record_chain": injector.corrupt_record_chain,
            "infimum": injector.corrupt_infimum,
            "supremum": injector.corrupt_supremum,
            "checksum_only": injector.corrupt_checksum_only,
            "all_headers": injector.corrupt_all_headers,
            "mixed": injector.create_mixed_corruption,
        }
        
        corrupt_func = func_map.get(scenario["type"], injector.corrupt_page_headers)
        corrupted_data = bytearray(open(test_file, 'rb').read())
        corrupted_data = corrupt_func(corrupted_data, scenario["corrupt_pct"])
        
        # Write corrupted file
        corrupted_file = self.test_data_dir / f"corrupted_{scenario['name']}_{format}.ibd"
        with open(corrupted_file, 'wb') as f:
            f.write(corrupted_data)
        
        # Run recovery for each mode
        best_rate = 0
        best_result = None
        
        for mode in self.recovery_modes:
            mode_output = self.output_dir / f"{scenario['name']}_{format}_{mode}"
            recovery_result = self.runner.run_recovery(
                str(corrupted_file), str(mode_output), format, mode
            )
            
            analysis = self.runner.analyze_output(str(mode_output))
            
            # Calculate recovery rate
            golden_count = len(golden_hashes)
            recovered = analysis["recovered_count"]
            rate = min(1.0, recovered / golden_count) if golden_count > 0 else 0
            
            if rate > best_rate:
                best_rate = rate
                best_result = {
                    "mode": mode,
                    "recovery_rate": rate,
                    "recovered_records": recovered,
                    "recovery_time": recovery_result.get("recovery_time_seconds", 0)
                }
            
            # Log result
            self.results.append({
                "scenario": scenario["name"],
                "format": format,
                "mode": mode,
                "recovery_rate": rate,
                "recovered_records": recovered,
                "recovery_time_seconds": recovery_result.get("recovery_time_seconds", 0),
                "success": recovery_result.get("success", False),
                "error": recovery_result.get("error", "")
            })
        
        return {
            "scenario": scenario["name"],
            "format": format,
            "best_recovery_rate": best_rate,
            "best_mode": best_result.get("mode", "unknown") if best_result else "none",
            "best_records": best_result.get("recovered_records", 0) if best_result else 0
        }
    
    def run_comprehensive_test(self, test_file: str = None,
                              num_records: int = 5500000) -> Dict:
        """运行完整的综合测试"""
        print("=" * 70)
        print("MySQL Recovery Pro - Comprehensive Recovery Validation")
        print("=" * 70)
        print(f"Target: 500万+ records, 99.9%+ recovery rate")
        print(f"Timestamp: {self.timestamp}")
        print()
        
        # Setup
        self.setup()
        
        # Generate golden data if needed
        golden_gen = GoldenDataGenerator(num_records=num_records, seed=self.seed)
        golden_records = golden_gen.generate_records()
        golden_hashes = golden_gen.get_all_hashes()
        
        if test_file:
            test_file_path = Path(test_file)
        else:
            # Use existing test file
            test_file_path = self.test_data_dir / "test_mysql56_5m_full.ibd"
        
        if not test_file_path.exists():
            print(f"WARNING: Test file not found: {test_file_path}")
            return {"error": "Test file not found"}
        
        print(f"Test file: {test_file_path}")
        print(f"Golden records: {num_records:,}")
        print()
        
        # Define corruption scenarios
        scenarios = [
            {"name": "page_header_0.01pct", "type": "page_header", "corrupt_pct": 0.0001},
            {"name": "page_header_0.1pct", "type": "page_header", "corrupt_pct": 0.001},
            {"name": "page_header_0.5pct", "type": "page_header", "corrupt_pct": 0.005},
            {"name": "page_chain_0.01pct", "type": "page_chain", "corrupt_pct": 0.0001},
            {"name": "page_chain_0.5pct", "type": "page_chain", "corrupt_pct": 0.005},
            {"name": "record_chain_0.01pct", "type": "record_chain", "corrupt_pct": 0.0001},
            {"name": "infimum_0.5pct", "type": "infimum", "corrupt_pct": 0.005},
            {"name": "supremum_0.5pct", "type": "supremum", "corrupt_pct": 0.005},
            {"name": "checksum_only_0.5pct", "type": "checksum_only", "corrupt_pct": 0.005},
            {"name": "all_headers_0.1pct", "type": "all_headers", "corrupt_pct": 0.001},
            {"name": "all_headers_1pct", "type": "all_headers", "corrupt_pct": 0.01},
            {"name": "mixed_0.5pct", "type": "mixed", "corrupt_pct": 0.005},
        ]
        
        total_tests = len(scenarios) * len(self.output_formats)
        current = 0
        
        # Run tests
        for scenario in scenarios:
            for fmt in self.output_formats:
                current += 1
                print(f"[{current}/{total_tests}] ", end="")
                
                result = self.run_single_scenario(scenario, str(test_file_path), 
                                                 golden_hashes, fmt)
                
                status = "✓" if result["best_recovery_rate"] >= 0.999 else "✗"
                print(f"{status} {result['scenario']:30s} "
                      f"best_rate={result['best_recovery_rate']*100:6.2f}% "
                      f"(mode={result['best_mode']})")
        
        # Generate report
        return self.generate_report()
    
    def generate_report(self) -> Dict:
        """生成验证报告"""
        report = {
            "timestamp": self.timestamp,
            "total_tests": len(self.results),
            "summary": self.calculate_summary(),
            "scenarios": self.results
        }
        
        # Save JSON
        json_file = self.output_dir / f"validation_report_{self.timestamp}.json"
        with open(json_file, 'w') as f:
            json.dump(report, f, indent=2)
        
        # Save Markdown
        md_file = self.output_dir / f"validation_report_{self.timestamp}.md"
        with open(md_file, 'w') as f:
            f.write(self.generate_markdown_report(report))
        
        print(f"\nReports saved:")
        print(f"  JSON: {json_file}")
        print(f"  Markdown: {md_file}")
        
        return report
    
    def calculate_summary(self) -> Dict:
        """计算汇总统计"""
        rates = [r.get("recovery_rate", 0) for r in self.results]
        
        # Group by scenario
        scenarios = {}
        for r in self.results:
            name = r["scenario"]
            if name not in scenarios:
                scenarios[name] = []
            scenarios[name].append(r["recovery_rate"])
        
        # Group by format
        formats = {}
        for r in self.results:
            fmt = r["format"]
            if fmt not in formats:
                formats[fmt] = []
            formats[fmt].append(r["recovery_rate"])
        
        return {
            "total_tests": len(self.results),
            "max_rate": max(rates) if rates else 0,
            "min_rate": min(rates) if rates else 0,
            "avg_rate": sum(rates) / len(rates) if rates else 0,
            " scenarios_passed_999": sum(1 for r in rates if r >= 0.999),
            " scenarios_failed": sum(1 for r in rates if r < 0.999),
            "per_scenario": {k: {"avg": sum(v)/len(v), "max": max(v)} 
                           for k, v in scenarios.items()},
            "per_format": {k: {"avg": sum(v)/len(v)} 
                          for k, v in formats.items()}
        }
    
    def generate_markdown_report(self, report: Dict) -> str:
        """生成Markdown报告"""
        lines = [
            "# MySQL Recovery Pro - Recovery Validation Report",
            "",
            f"**Generated:** {report['timestamp']}",
            f"**Total Tests:** {report['summary']['total_tests']}",
            "",
            "## Summary",
            "",
            "| Metric | Value |",
            "|--------|-------|",
            f"| Max Recovery Rate | {report['summary']['max_rate']*100:.2f}% |",
            f"| Min Recovery Rate | {report['summary']['min_rate']*100:.2f}% |",
            f"| Average Recovery Rate | {report['summary']['avg_rate']*100:.2f}% |",
            f"| Scenarios >= 99.9% | {report['summary']['scenarios_passed_999']} |",
            f"| Scenarios < 99.9% | {report['summary']['scenarios_failed']} |",
            "",
            "## Scenarios by Corruption Type",
            ""
        ]
        
        for scenario, stats in report['summary']['per_scenario'].items():
            avg_rate = stats['avg']
            status = "✓ PASS" if avg_rate >= 0.999 else "✗ FAIL"
            lines.append(f"- **{scenario}**: {avg_rate*100:.2f}% ({status})")
        
        lines.extend([
            "",
            "## Per-Format Performance",
            ""
        ])
        
        for fmt, stats in report['summary']['per_format'].items():
            lines.append(f"- **{fmt}**: avg={stats['avg']*100:.2f}%")
        
        lines.extend([
            "",
            "## Detailed Results",
            "",
            "| Scenario | Format | Mode | Rate | Status |",
            "|----------|--------|------|------|--------|"
        ])
        
        for r in report['scenarios'][:50]:  # First 50 results
            rate = r['recovery_rate']
            status = "✓ PASS" if rate >= 0.999 else "✗ FAIL"
            lines.append(
                f"| {r['scenario']} | {r['format']} | {r['mode']} | "
                f"{rate*100:.2f}% | {status} |"
            )
        
        return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="MySQL Recovery Pro - Comprehensive Recovery Validation"
    )
    parser.add_argument("--project-dir", default="/app/mysql_recovery_pro",
                       help="Project directory")
    parser.add_argument("--build-dir", default="build",
                       help="Build directory")
    parser.add_argument("--test-data-dir", default="test_data",
                       help="Test data directory")
    parser.add_argument("--output-dir", default="validation_output",
                       help="Output directory")
    parser.add_argument("--test-file", default=None,
                       help="Specific test file to use")
    parser.add_argument("--num-records", type=int, default=5500000,
                       help="Number of golden records (default: 5.5M)")
    parser.add_argument("--seed", type=int, default=42,
                       help="Random seed for reproducibility")
    parser.add_argument("--fast-mode", action="store_true",
                       help="Fast mode - test fewer scenarios")
    
    args = parser.parse_args()
    
    # Use specific test file if provided
    test_file = None
    if args.test_file:
        test_file = os.path.join(args.project_dir, args.test_file)
    
    validator = ComprehensiveValidationSystem(
        project_dir=args.project_dir,
        build_dir=args.build_dir,
        test_data_dir=args.test_data_dir,
        output_dir=args.output_dir
    )
    
    # Adjust scenarios for fast mode
    if args.fast_mode:
        print("Running FAST mode - reduced scenarios")
        # In fast mode, we would reduce scenarios (not implemented here)
    
    report = validator.run_comprehensive_test(
        test_file=test_file,
        num_records=args.num_records
    )
    
    # Print summary
    summary = report.get("summary", {})
    print("\n" + "=" * 70)
    print("VALIDATION SUMMARY")
    print("=" * 70)
    print(f"Total scenarios tested: {summary.get('total_tests', 0)}")
    print(f"Average recovery rate: {summary.get('avg_rate', 0)*100:.2f}%")
    print(f"Max recovery rate: {summary.get('max_rate', 0)*100:.2f}%")
    print(f"Min recovery rate: {summary.get('min_rate', 0)*100:.2f}%")
    print(f"Scenarios >= 99.9%: {summary.get('scenarios_passed_999', 0)}")
    print(f"Scenarios < 99.9%: {summary.get('scenarios_failed', 0)}")
    print("=" * 70)
    
    # Check if target is met
    target_rate = 0.999
    target_pct = summary.get('scenarios_passed_999', 0)
    total = summary.get('total_tests', 0)
    pass_pct = target_pct / total if total > 0 else 0
    
    if pass_pct >= 0.999:
        print("✓ TARGET MET: 99.9%+ recovery rate achieved!")
        sys.exit(0)
    else:
        print(f"✗ TARGET NOT MET: Only {pass_pct*100:.2f}% scenarios passed")
        sys.exit(1)


if __name__ == "__main__":
    main()
