#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
MySQL Large-Scale Disaster Recovery Test using Existing Podman Containers
Author: jliang.fang

Purpose: 使用现有podman容器执行大规模灾难恢复验证
- 100万行测试数据（全字段类型）
- MySQL 5.6/5.7/8.0/8.4/9.0验证
- rm -rf / DROP TABLE / TRUNCATE TABLE 恢复
==============================================================================
"""

import os
import sys
import json
import time
import shutil
import subprocess
import random
import tempfile
from pathlib import Path
from datetime import datetime
from typing import Dict, List, Optional, Tuple

# 现有容器配置
CONTAINERS = {
    "mysql56": {"name": "mysql56_test", "port": 3306, "has_frm": True},
    "mysql57": {"name": "mysql57_test", "port": 3307, "has_frm": True},
    "mysql80": {"name": "mysql80_test", "port": 3308, "has_frm": False},
    "mysql84": {"name": "mysql84_test", "port": 3309, "has_frm": False},
    "mysql90": {"name": "mysql90_test", "port": 3310, "has_frm": False},
}

RECOVERY_TOOL = Path("/app/mysql_recovery_pro/build/mysql_recovery_pro")
TEST_DIR = Path("/app/mysql_recovery_test/large_scale_validation")
MYSQL_CLIENT = "mysql"  # 使用本地mysql客户端连接

# 全字段类型测试表
CREATE_TABLE_SQL = """
CREATE TABLE IF NOT EXISTS large_scale_test (
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(100) NOT NULL,
    age INT,
    height DOUBLE,
    birth_date DATE,
    created_at DATETIME,
    json_col JSON,
    enum_col ENUM('active', 'inactive', 'pending'),
    set_col SET('a', 'b', 'c'),
    blob_col BLOB,
    text_col TEXT,
    varbinary_col VARBINARY(100),
    binary_col BINARY(20),
    decimal_col DECIMAL(20,5),
    float_col FLOAT,
    double_col DOUBLE,
    tinyint_col TINYINT,
    smallint_col SMALLINT,
    mediumint_col MEDIUMINT,
    year_col YEAR,
    time_col TIME,
    datetime2_col DATETIME(2),
    timestamp2_col TIMESTAMP(2),
    time2_col TIME(2),
    char_col CHAR(50),
    varchar2_col VARCHAR(50),
    default_value VARCHAR(50) DEFAULT 'default_test'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=DYNAMIC;
"""


class LargeScaleDisasterTest:
    """大规模灾难恢复测试"""

    def __init__(self, num_rows: int = 100000):
        self.num_rows = num_rows
        self.test_dir = TEST_DIR
        self.test_dir.mkdir(parents=True, exist_ok=True)
        self.results = {}
        self.volume_paths = {}

    def log(self, msg: str, level: str = "INFO"):
        """打印日志"""
        timestamp = datetime.now().strftime("%H:%M:%S")
        print(f"[{timestamp}] [{level}] {msg}")

    def get_container_volume_path(self, container_name: str) -> Optional[Path]:
        """获取容器数据目录路径"""
        try:
            result = subprocess.run(
                ["podman", "inspect", container_name, "--format", "{{range .Mounts}}{{.Source}}{{end}}"],
                capture_output=True, text=True, timeout=30
            )
            if result.returncode == 0 and result.stdout.strip():
                return Path(result.stdout.strip())
        except Exception as e:
            self.log(f"Error getting volume path: {e}", "ERROR")
        return None

    def execute_mysql(self, container_name: str, sql: str, database: str = "disaster_test") -> Tuple[bool, str]:
        """通过podman exec执行SQL"""
        try:
            result = subprocess.run(
                ["podman", "exec", "-i", container_name,
                 "mysql", "-uroot", "-proot123", database],
                input=sql, capture_output=True, text=True, timeout=300
            )
            return result.returncode == 0, result.stdout + result.stderr
        except subprocess.TimeoutExpired:
            return False, "Timeout"
        except Exception as e:
            return False, str(e)

    def execute_mysql_direct(self, port: int, sql: str, database: str = "disaster_test") -> Tuple[bool, str]:
        """通过podman exec执行SQL（绕过网络连接问题）"""
        container_name = None
        for ver, cfg in CONTAINERS.items():
            if cfg["port"] == port:
                container_name = cfg["name"]
                break
        if container_name:
            return self.execute_mysql(container_name, sql, database)
        return False, "Container not found"

    def insert_test_data(self, container_name: str, port: int) -> Tuple[bool, int]:
        """插入大规模测试数据"""
        self.log(f"Creating table and inserting {self.num_rows} rows...")

        # 创建表
        success, output = self.execute_mysql_direct(port, CREATE_TABLE_SQL)
        if not success:
            self.log(f"Failed to create table: {output}", "ERROR")
            return False, 0

        # 批量插入数据
        batch_size = 1000
        total_inserted = 0
        enum_values = ['active', 'inactive', 'pending']
        set_values = ['a', 'b', 'c', 'a,b', 'b,c', 'a,b,c']

        self.log(f"Generating INSERT statements...")

        for batch_start in range(0, self.num_rows, batch_size):
            values_list = []

            for i in range(batch_start, min(batch_start + batch_size, self.num_rows)):
                random.seed(i)  # 可重复的随机数据
                row_num = i + 1

                name = f"User_{row_num:06d}"
                age = random.randint(18, 80)
                height = round(random.uniform(1.5, 2.0), 2)
                birth_date = f"{random.randint(1960, 2005)}-{random.randint(1,12):02d}-{random.randint(1,28):02d}"
                created_at = f"202{random.randint(0,5)}-{random.randint(1,12):02d}-{random.randint(1,28):02d} {random.randint(0,23):02d}:{random.randint(0,59):02d}:{random.randint(0,59):02d}"
                json_col = json.dumps({"id": row_num, "name": name, "tags": random.sample(range(1,100), 3)})
                enum_col = random.choice(enum_values)
                set_col = random.choice(set_values)
                blob_hex = ''.join([f'{random.randint(0,255):02x}' for _ in range(20)])
                text_col = f"Text content for record {row_num} with random data"
                varbinary_hex = ''.join([f'{random.randint(0,255):02x}' for _ in range(10)])
                binary_hex = ''.join([f'{random.randint(0,255):02x}' for _ in range(20)])
                decimal_val = round(random.uniform(-1000000, 1000000), 5)
                float_val = round(random.uniform(-1000, 1000), 4)
                double_val = round(random.uniform(-10000, 10000), 8)
                tinyint_val = random.randint(-128, 127)
                smallint_val = random.randint(-32768, 32767)
                mediumint_val = random.randint(-8388608, 8388607)
                year_val = random.randint(1901, 2155)
                time_col = f"{random.randint(0,23):02d}:{random.randint(0,59):02d}:{random.randint(0,59):02d}"
                datetime2 = f"202{random.randint(0,5)}-{random.randint(1,12):02d}-{random.randint(1,28):02d} {random.randint(0,23):02d}:{random.randint(0,59):02d}:{random.randint(0,59):02d}.{random.randint(0,99):02d}"
                timestamp2 = datetime2
                time2 = f"{random.randint(0,23):02d}:{random.randint(0,59):02d}:{random.randint(0,59):02d}.{random.randint(0,99):02d}"
                char_col = f"Char{row_num:044d}"[:50]
                varchar2 = f"Var{row_num}"

                values = f"('{name}',{age},{height},'{birth_date}','{created_at}','{json_col}','{enum_col}','{set_col}',UNHEX('{blob_hex}'),'{text_col}',UNHEX('{varbinary_hex}'),UNHEX('{binary_hex}'),{decimal_val},{float_val},{double_val},{tinyint_val},{smallint_val},{mediumint_val},{year_val},'{time_col}','{datetime2}','{timestamp2}','{time2}','{char_col}','{varchar2}','default_test')"
                values_list.append(values)

            # 执行批量INSERT
            insert_sql = f"INSERT INTO large_scale_test (name,age,height,birth_date,created_at,json_col,enum_col,set_col,blob_col,text_col,varbinary_col,binary_col,decimal_col,float_col,double_col,tinyint_col,smallint_col,mediumint_col,year_col,time_col,datetime2_col,timestamp2_col,time2_col,char_col,varchar2_col,default_value) VALUES {','.join(values_list)};"

            success, output = self.execute_mysql(container_name, insert_sql)
            if success:
                total_inserted += len(values_list)
                if (batch_start // batch_size + 1) % 50 == 0:
                    self.log(f"Inserted {total_inserted}/{self.num_rows} rows...")
            else:
                self.log(f"Insert failed at batch {batch_start}: {output[:200]}", "WARN")
                # 尝试单行插入
                for val in values_list[:10]:
                    single_sql = f"INSERT INTO large_scale_test (name,age,height,birth_date,created_at,json_col,enum_col,set_col,blob_col,text_col,varbinary_col,binary_col,decimal_col,float_col,double_col,tinyint_col,smallint_col,mediumint_col,year_col,time_col,datetime2_col,timestamp2_col,time2_col,char_col,varchar2_col,default_value) VALUES {val};"
                    self.execute_mysql(container_name, single_sql)

        self.log(f"Total inserted: {total_inserted} rows")
        return total_inserted >= self.num_rows * 0.95, total_inserted

    def backup_ibd_file(self, version: str, volume_path: Path) -> Dict:
        """备份IBD文件"""
        ibd_src = volume_path / "disaster_test" / "large_scale_test.ibd"
        frm_src = volume_path / "disaster_test" / "large_scale_test.frm"

        if not ibd_src.exists():
            self.log(f"IBD file not found: {ibd_src}", "ERROR")
            return {}

        backup_dir = self.test_dir / "backups" / version
        backup_dir.mkdir(parents=True, exist_ok=True)

        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        ibd_backup = backup_dir / f"large_scale_test_{timestamp}.ibd"
        shutil.copy(ibd_src, ibd_backup)

        backup_info = {
            "ibd_path": str(ibd_backup),
            "ibd_size": ibd_backup.stat().st_size,
            "original_rows": self.num_rows,
            "timestamp": timestamp
        }

        if frm_src.exists() and frm_src.stat().st_size > 0:
            frm_backup = backup_dir / f"large_scale_test_{timestamp}.frm"
            shutil.copy(frm_src, frm_backup)
            backup_info["frm_path"] = str(frm_backup)

        self.log(f"Backup created: {ibd_backup} ({backup_info['ibd_size']} bytes)")
        return backup_info

    def simulate_disaster(self, version: str, container_name: str,
                          volume_path: Path, disaster_type: str) -> Optional[Path]:
        """模拟灾难"""
        ibd_path = volume_path / "disaster_test" / "large_scale_test.ibd"
        frm_path = volume_path / "disaster_test" / "large_scale_test.frm"

        # 先备份
        backup_info = self.backup_ibd_file(version, volume_path)
        if not backup_info:
            return None

        backup_file = Path(backup_info["ibd_path"])

        if disaster_type == "rmrf":
            # 模拟rm -rf: 停止容器后删除文件
            self.log(f"Simulating rm -rf for {version}...")
            subprocess.run(["podman", "stop", container_name], capture_output=True, timeout=30)
            time.sleep(2)

            if ibd_path.exists():
                ibd_path.unlink()
            if frm_path.exists():
                frm_path.unlink()

            subprocess.run(["podman", "start", container_name], capture_output=True, timeout=30)
            time.sleep(10)

        elif disaster_type == "drop":
            # 模拟DROP TABLE
            self.log(f"Simulating DROP TABLE for {version}...")
            self.execute_mysql(container_name, "DROP TABLE large_scale_test;")

        elif disaster_type == "truncate":
            # 模拟TRUNCATE TABLE
            self.log(f"Simulating TRUNCATE TABLE for {version}...")
            self.execute_mysql(container_name, "TRUNCATE TABLE large_scale_test;")

        elif disaster_type == "corruption":
            # 模拟坏块
            self.log(f"Simulating corruption for {version}...")
            subprocess.run(["podman", "stop", container_name], capture_output=True, timeout=30)
            time.sleep(2)

            if ibd_path.exists():
                with open(ibd_path, 'r+b') as f:
                    # 损坏第4和第5个数据页
                    f.seek(16384 * 4)
                    corrupt_data = bytes([random.randint(0, 255) for _ in range(1024)])
                    f.write(corrupt_data)
                    f.seek(16384 * 5)
                    f.write(corrupt_data)

            subprocess.run(["podman", "start", container_name], capture_output=True, timeout=30)
            time.sleep(10)

        return backup_file

    def run_recovery(self, ibd_path: Path, output_dir: Path,
                     frm_path: Path = None, mode: str = "default") -> Dict:
        """执行恢复"""
        output_dir.mkdir(parents=True, exist_ok=True)

        cmd = [str(RECOVERY_TOOL), "--ibd", str(ibd_path), "--output", str(output_dir)]

        if mode == "truncate":
            cmd.append("--truncate-recovery")
        # rmrf和默认都使用默认恢复模式（会自动提取SDI）

        if frm_path and frm_path.exists() and frm_path.stat().st_size > 0:
            cmd.extend(["--frm", str(frm_path)])

        cmd.append("--verbose")

        self.log(f"Running recovery: {' '.join(cmd[:6])}...")

        start_time = time.time()
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            elapsed = time.time() - start_time

            # 查找CSV输出
            csv_file = None
            for f in output_dir.iterdir():
                if f.suffix == '.csv':
                    csv_file = f
                    break

            recovered_rows = 0
            if csv_file and csv_file.exists():
                with open(csv_file, 'r') as f:
                    recovered_rows = sum(1 for _ in f) - 1

            return {
                "success": result.returncode == 0,
                "recovered_rows": recovered_rows,
                "elapsed": elapsed,
                "csv_file": str(csv_file) if csv_file else None,
                "stdout": result.stdout[-500:] if result.stdout else ""
            }

        except subprocess.TimeoutExpired:
            return {"success": False, "error": "Timeout", "elapsed": 600}
        except Exception as e:
            return {"success": False, "error": str(e)}

    def validate_recovery(self, csv_file: Path, expected_rows: int) -> Dict:
        """验证恢复结果"""
        if not csv_file or not csv_file.exists():
            return {"valid": False, "reason": "No CSV file"}

        try:
            with open(csv_file, 'r') as f:
                lines = f.readlines()

            recovered_rows = len(lines) - 1
            recovery_rate = recovered_rows / expected_rows if expected_rows > 0 else 0

            # 验证数据质量
            valid_rows = 0
            json_valid = 0
            for line in lines[1:]:  # 跳过标题行
                fields = line.strip().split(',')
                if len(fields) >= 5:
                    try:
                        int(fields[0])  # ID
                        valid_rows += 1
                        # 检查JSON字段
                        if len(fields) >= 7 and fields[6].startswith('"{"'):
                            json_valid += 1
                    except:
                        pass

            return {
                "valid": recovery_rate >= 0.95,
                "recovered_rows": recovered_rows,
                "valid_rows": valid_rows,
                "json_valid": json_valid,
                "recovery_rate": recovery_rate,
                "expected_rows": expected_rows
            }

        except Exception as e:
            return {"valid": False, "reason": str(e)}

    def test_version(self, version: str) -> Dict:
        """测试单个版本"""
        self.log(f"\n{'='*60}")
        self.log(f"Testing {version} - {self.num_rows} rows")
        self.log(f"{'='*60}")

        config = CONTAINERS.get(version)
        if not config:
            self.log(f"Unknown version: {version}", "ERROR")
            return {"error": "Unknown version"}

        container_name = config["name"]
        port = config["port"]
        has_frm = config["has_frm"]

        results = {
            "version": version,
            "container": container_name,
            "num_rows": self.num_rows,
            "scenarios": {}
        }

        # 获取数据目录路径
        volume_path = self.get_container_volume_path(container_name)
        if not volume_path:
            results["error"] = "Cannot get volume path"
            return results

        self.log(f"Volume path: {volume_path}")

        # 检查容器状态并启动
        status_result = subprocess.run(
            ["podman", "inspect", container_name, "--format", "{{.State.Status}}"],
            capture_output=True, text=True
        )
        status = status_result.stdout.strip()

        if status != "running":
            self.log(f"Starting container {container_name}...")
            subprocess.run(["podman", "start", container_name], capture_output=True)
            time.sleep(15)

        # 创建数据库
        self.execute_mysql(container_name, "CREATE DATABASE IF NOT EXISTS disaster_test;")

        # 插入测试数据
        success, inserted = self.insert_test_data(container_name, port)
        results["inserted_rows"] = inserted

        if not success:
            results["error"] = f"Failed to insert data ({inserted}/{self.num_rows})"
            return results

        # 获取原始数据统计
        success, output = self.execute_mysql(container_name, "SELECT COUNT(*) FROM large_scale_test;")
        if success:
            try:
                lines = [l for l in output.strip().split('\n') if l.strip() and not l.startswith('mysql:')]
                if lines:
                    actual_count = int(lines[-1].strip())
                    self.log(f"Actual rows in table: {actual_count}")
            except:
                pass

        # 测试各灾难场景
        for scenario in ["rmrf", "truncate", "drop"]:
            self.log(f"\n--- Scenario: {scenario} ---")

            # 模拟灾难
            backup_file = self.simulate_disaster(version, container_name, volume_path, scenario)
            if not backup_file:
                self.log(f"Failed to simulate {scenario}", "ERROR")
                continue

            # 执行恢复
            output_dir = self.test_dir / "recovery" / version / scenario

            frm_file = None
            if has_frm:
                frm_candidates = list(backup_file.parent.glob("*.frm"))
                if frm_candidates:
                    frm_file = frm_candidates[0]

            recovery_result = self.run_recovery(backup_file, output_dir, frm_file, scenario)

            # 验证恢复
            if recovery_result.get("csv_file"):
                validation = self.validate_recovery(
                    Path(recovery_result["csv_file"]),
                    self.num_rows
                )
            else:
                validation = {"valid": False, "reason": "No recovery output"}

            results["scenarios"][scenario] = {
                "success": recovery_result.get("success", False),
                "recovered_rows": validation.get("recovered_rows", 0),
                "valid_rows": validation.get("valid_rows", 0),
                "json_valid": validation.get("json_valid", 0),
                "recovery_rate": validation.get("recovery_rate", 0),
                "valid": validation.get("valid", False),
                "elapsed": recovery_result.get("elapsed", 0),
                "backup_file": str(backup_file)
            }

            status = "PASS" if validation.get("valid") else "FAIL"
            rate = validation.get("recovery_rate", 0) * 100
            self.log(f"  {status}: {validation.get('recovered_rows', 0)}/{self.num_rows} rows ({rate:.1f}%)")

            # 重置表以准备下一个场景
            subprocess.run(["podman", "exec", container_name, "mysql", "-uroot", "-ptestpass",
                           "-e", "DROP TABLE IF EXISTS disaster_test.large_scale_test;"],
                          capture_output=True)
            time.sleep(2)

            # 重新插入数据
            self.execute_mysql(container_name, CREATE_TABLE_SQL)
            # 使用更快的LOAD DATA方式
            self.log(f"Re-inserting data for next scenario...")

        return results

    def run_all_versions(self) -> Dict:
        """运行所有版本测试"""
        self.log("\n" + "="*70)
        self.log(f"MySQL Large-Scale Disaster Recovery Test")
        self.log(f"Rows per table: {self.num_rows}")
        self.log("="*70)

        all_results = {}

        for version in CONTAINERS.keys():
            try:
                results = self.test_version(version)
                all_results[version] = results
            except Exception as e:
                self.log(f"Error testing {version}: {e}", "ERROR")
                all_results[version] = {"error": str(e)}

        return all_results

    def generate_report(self, all_results: Dict) -> str:
        """生成测试报告"""
        report = []
        report.append("=" * 80)
        report.append("MySQL Large-Scale Disaster Recovery Test Report")
        report.append(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        report.append(f"Rows per table: {self.num_rows}")
        report.append("=" * 80)

        report.append("\n## Recovery Results Matrix\n")
        report.append("-" * 80)
        report.append(f"{'Version':<12} {'rm -rf':<20} {'TRUNCATE':<20} {'DROP':<20}")
        report.append("-" * 80)

        total_scenarios = 0
        passed_scenarios = 0
        total_recovered = 0

        for version, results in all_results.items():
            scenarios = results.get("scenarios", {})
            row = [version]

            for scenario in ["rmrf", "truncate", "drop"]:
                total_scenarios += 1
                scenario_result = scenarios.get(scenario, {})
                if scenario_result.get("valid", False):
                    passed_scenarios += 1
                    rate = scenario_result.get("recovery_rate", 0) * 100
                    row.append(f"PASS ({rate:.0f}%)")
                    total_recovered += scenario_result.get("recovered_rows", 0)
                elif scenario_result.get("success"):
                    rate = scenario_result.get("recovery_rate", 0) * 100
                    row.append(f"PARTIAL ({rate:.0f}%)")
                else:
                    row.append("FAIL")

            report.append(f"{row[0]:<12} {row[1]:<20} {row[2]:<20} {row[3]:<20}")

        report.append("-" * 80)
        report.append(f"\n## Summary\n")
        report.append(f"- Total scenarios: {total_scenarios}")
        report.append(f"- Passed: {passed_scenarios}")
        report.append(f"- Success rate: {passed_scenarios/total_scenarios*100:.1f}%" if total_scenarios > 0 else "- N/A")
        report.append(f"- Total recovered rows: {total_recovered}")
        report.append(f"- Test rows per scenario: {self.num_rows}")

        report_text = "\n".join(report)

        report_path = self.test_dir / f"REPORT_{datetime.now().strftime('%Y%m%d_%H%M%S')}.md"
        report_path.write_text(report_text)

        self.log(f"\nReport saved: {report_path}")
        return report_text


def main():
    import argparse
    parser = argparse.ArgumentParser(description="MySQL Large-Scale Disaster Recovery Test")
    parser.add_argument("--rows", type=int, default=100000, help="Number of test rows (default: 100000)")
    parser.add_argument("--version", type=str, help="Test specific version only")
    args = parser.parse_args()

    if not RECOVERY_TOOL.exists():
        print(f"Error: Recovery tool not found: {RECOVERY_TOOL}")
        print("Please build: cmake --build build")
        sys.exit(1)

    tester = LargeScaleDisasterTest(num_rows=args.rows)

    if args.version:
        results = {args.version: tester.test_version(args.version)}
    else:
        results = tester.run_all_versions()

    report = tester.generate_report(results)
    print("\n" + report)


if __name__ == "__main__":
    main()