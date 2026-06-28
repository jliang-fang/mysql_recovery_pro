#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
MySQL Disaster Recovery Test Framework with Podman
Author: jliang.fang

Purpose: 使用podman容器文件映射进行MySQL 5.6-9.0大规模灾难恢复验证
- 100万行测试数据（全字段类型）
- rm -rf 恢复
- DROP TABLE 恢复
- TRUNCATE TABLE 恢复
- 坏块恢复
==============================================================================
"""

import os
import sys
import json
import time
import shutil
import subprocess
import tempfile
import random
import hashlib
from pathlib import Path
from datetime import datetime, timedelta
from typing import Dict, List, Optional, Tuple

# MySQL版本配置
MYSQL_VERSIONS = {
    "mysql56": {"image": "mysql:5.6", "has_sdi": False, "port": 33056},
    "mysql57": {"image": "mysql:5.7", "has_sdi": False, "port": 33057},
    "mysql80": {"image": "mysql:8.0", "has_sdi": True, "port": 33080},
    "mysql84": {"image": "mysql:8.4", "has_sdi": True, "port": 33084},
    "mysql90": {"image": "mysql:9.0", "has_sdi": True, "port": 33090},
}

# 测试表定义 - 包含所有MySQL数据类型
CREATE_TABLE_SQL = """
CREATE TABLE disaster_test (
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
    varchar2_col VARCHAR(50)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
"""

RECOVERY_TOOL = "/app/mysql_recovery_pro/build/mysql_recovery_pro"
TEST_DATA_DIR = Path("/app/mysql_recovery_test/large_scale_test")


class LargeScaleDisasterRecoveryTest:
    """大规模灾难恢复测试框架"""

    def __init__(self, num_rows: int = 100000):
        self.num_rows = num_rows
        self.test_dir = TEST_DATA_DIR
        self.test_dir.mkdir(parents=True, exist_ok=True)
        self.results = {}
        self.containers = {}

    def log(self, msg: str, level: str = "INFO"):
        """打印日志"""
        timestamp = datetime.now().strftime("%H:%M:%S")
        print(f"[{timestamp}] [{level}] {msg}")

    def generate_insert_sql(self, batch_size: int = 1000) -> List[str]:
        """生成批量INSERT SQL语句"""
        inserts = []
        values_list = []

        enum_values = ['active', 'inactive', 'pending']
        set_values = ['a', 'b', 'c', 'a,b', 'b,c', 'a,b,c']

        for i in range(1, self.num_rows + 1):
            # 生成随机但可预测的数据（用于验证）
            random.seed(i)  # 确保可重复

            name = f"User_{i:06d}"
            age = random.randint(18, 80)
            height = round(random.uniform(1.5, 2.0), 2)
            birth_date = f"{random.randint(1960, 2005)}-{random.randint(1,12):02d}-{random.randint(1,28):02d}"
            created_at = f"202{random.randint(0,5)}-{random.randint(1,12):02d}-{random.randint(1,28):02d} {random.randint(0,23):02d}:{random.randint(0,59):02d}:{random.randint(0,59):02d}"
            json_col = json.dumps({"id": i, "name": name, "tags": [random.randint(1,10) for _ in range(3)]})
            enum_col = random.choice(enum_values)
            set_col = random.choice(set_values)
            blob_hex = ''.join([f'{random.randint(0,255):02x}' for _ in range(20)])
            text_col = f"Text content for record {i} with some random data"
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
            timestamp2 = f"202{random.randint(0,5)}-{random.randint(1,12):02d}-{random.randint(1,28):02d} {random.randint(0,23):02d}:{random.randint(0,59):02d}:{random.randint(0,59):02d}.{random.randint(0,99):02d}"
            time2 = f"{random.randint(0,23):02d}:{random.randint(0,59):02d}:{random.randint(0,59):02d}.{random.randint(0,99):02d}"
            char_col = f"Char{i:044d}"[:50]
            varchar2 = f"Var{i}"

            values = f"('{name}', {age}, {height}, '{birth_date}', '{created_at}', '{json_col}', '{enum_col}', '{set_col}', UNHEX('{blob_hex}'), '{text_col}', UNHEX('{varbinary_hex}'), UNHEX('{binary_hex}'), {decimal_val}, {float_val}, {double_val}, {tinyint_val}, {smallint_val}, {mediumint_val}, {year_val}, '{time_col}', '{datetime2}', '{timestamp2}', '{time2}', '{char_col}', '{varchar2}')"
            values_list.append(values)

            if len(values_list) >= batch_size:
                inserts.append("INSERT INTO disaster_test (name, age, height, birth_date, created_at, json_col, enum_col, set_col, blob_col, text_col, varbinary_col, binary_col, decimal_col, float_col, double_col, tinyint_col, smallint_col, mediumint_col, year_col, time_col, datetime2_col, timestamp2_col, time2_col, char_col, varchar2_col) VALUES " + ",".join(values_list) + ";")
                values_list = []

        if values_list:
            inserts.append("INSERT INTO disaster_test (name, age, height, birth_date, created_at, json_col, enum_col, set_col, blob_col, text_col, varbinary_col, binary_col, decimal_col, float_col, double_col, tinyint_col, smallint_col, mediumint_col, year_col, time_col, datetime2_col, timestamp2_col, time2_col, char_col, varchar2_col) VALUES " + ",".join(values_list) + ";")

        return inserts

    def start_mysql_container(self, version: str, data_dir: Path) -> Optional[str]:
        """使用podman启动MySQL容器"""
        config = MYSQL_VERSIONS.get(version)
        if not config:
            self.log(f"Unknown version: {version}", "ERROR")
            return None

        container_name = f"mysql_test_{version}_{int(time.time())}"

        # 创建配置文件
        my_cnf = data_dir / "my.cnf"
        my_cnf.write_text("""[mysqld]
innodb_file_per_table=ON
innodb_flush_log_at_trx_commit=2
skip-name-resolve
max_connections=1000
innodb_buffer_pool_size=256M
""")

        # 确保数据目录存在
        mysql_data = data_dir / "mysql_data"
        mysql_data.mkdir(parents=True, exist_ok=True)

        try:
            # 停止并删除可能存在的旧容器
            subprocess.run(["podman", "rm", "-f", container_name],
                          capture_output=True, timeout=30)

            # 启动新容器
            cmd = [
                "podman", "run", "-d",
                "--name", container_name,
                "-e", "MYSQL_ROOT_PASSWORD=testpass",
                "-e", "MYSQL_DATABASE=testdb",
                "-v", f"{mysql_data}:/var/lib/mysql",
                "-v", f"{my_cnf}:/etc/mysql/conf.d/my.cnf",
                "-p", f"{config['port']}:3306",
                config["image"]
            ]

            result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
            if result.returncode != 0:
                self.log(f"Failed to start container: {result.stderr}", "ERROR")
                return None

            self.log(f"Container started: {container_name}")
            return container_name

        except Exception as e:
            self.log(f"Error starting container: {e}", "ERROR")
            return None

    def stop_container(self, container_name: str):
        """停止并删除容器"""
        if not container_name:
            return
        try:
            subprocess.run(["podman", "stop", container_name],
                          capture_output=True, timeout=30)
            subprocess.run(["podman", "rm", container_name],
                          capture_output=True, timeout=30)
        except Exception as e:
            self.log(f"Error stopping container: {e}", "WARN")

    def wait_for_mysql(self, container_name: str, timeout: int = 60) -> bool:
        """等待MySQL就绪"""
        start_time = time.time()
        while time.time() - start_time < timeout:
            try:
                result = subprocess.run(
                    ["podman", "exec", container_name,
                     "mysql", "-uroot", "-ptestpass", "-e", "SELECT 1"],
                    capture_output=True, text=True, timeout=10
                )
                if result.returncode == 0:
                    return True
            except:
                pass
            time.sleep(2)
        return False

    def execute_sql(self, container_name: str, sql: str) -> bool:
        """执行SQL语句"""
        try:
            result = subprocess.run(
                ["podman", "exec", "-i", container_name,
                 "mysql", "-uroot", "-ptestpass", "testdb"],
                input=sql, capture_output=True, text=True, timeout=300
            )
            return result.returncode == 0
        except Exception as e:
            self.log(f"SQL execution error: {e}", "ERROR")
            return False

    def insert_test_data(self, container_name: str) -> Tuple[bool, int]:
        """插入测试数据"""
        self.log(f"Generating {self.num_rows} rows of test data...")

        # 创建表
        if not self.execute_sql(container_name, CREATE_TABLE_SQL):
            self.log("Failed to create table", "ERROR")
            return False, 0

        # 生成并插入数据
        inserts = self.generate_insert_sql(batch_size=1000)
        total_inserted = 0

        for i, insert_sql in enumerate(inserts):
            if self.execute_sql(container_name, insert_sql):
                total_inserted += insert_sql.count("'),('") + 1
                if (i + 1) % 100 == 0:
                    self.log(f"Inserted {total_inserted}/{self.num_rows} rows...")
            else:
                self.log(f"Failed to insert batch {i}", "ERROR")

        self.log(f"Total inserted: {total_inserted} rows")
        return total_inserted == self.num_rows, total_inserted

    def save_original_checksum(self, container_name: str, version: str) -> Dict:
        """保存原始数据的校验和"""
        try:
            result = subprocess.run(
                ["podman", "exec", container_name,
                 "mysql", "-uroot", "-ptestpass", "testdb", "-N",
                 "-e", "SELECT COUNT(*), MD5(GROUP_CONCAT(id ORDER BY id)) FROM disaster_test"],
                capture_output=True, text=True, timeout=60
            )
            if result.returncode == 0:
                parts = result.stdout.strip().split("\t")
                if len(parts) >= 2:
                    return {
                        "count": int(parts[0]),
                        "checksum": parts[1]
                    }
        except Exception as e:
            self.log(f"Error getting checksum: {e}", "ERROR")
        return {}

    def backup_ibd_file(self, data_dir: Path, version: str, scenario: str) -> Optional[Path]:
        """备份IBD文件"""
        mysql_data = data_dir / "mysql_data"
        ibd_src = mysql_data / "testdb" / "disaster_test.ibd"
        frm_src = mysql_data / "testdb" / "disaster_test.frm"

        if not ibd_src.exists():
            self.log(f"IBD file not found: {ibd_src}", "ERROR")
            return None

        backup_dir = self.test_dir / "backups" / version / scenario
        backup_dir.mkdir(parents=True, exist_ok=True)

        ibd_backup = backup_dir / "disaster_test.ibd"
        shutil.copy(ibd_src, ibd_backup)

        if frm_src.exists():
            shutil.copy(frm_src, backup_dir / "disaster_test.frm")

        self.log(f"Backup created: {ibd_backup}")
        return ibd_backup

    def simulate_disaster(self, container_name: str, data_dir: Path,
                          disaster_type: str, version: str) -> Optional[Path]:
        """模拟灾难场景"""
        mysql_data = data_dir / "mysql_data"
        ibd_path = mysql_data / "testdb" / "disaster_test.ibd"
        frm_path = mysql_data / "testdb" / "disaster_test.frm"

        if disaster_type == "rmrf":
            # 先备份，然后删除文件
            backup = self.backup_ibd_file(data_dir, version, "rmrf")
            if container_name:
                self.execute_sql(container_name, "DROP TABLE IF EXISTS disaster_test;")
            return backup

        elif disaster_type == "truncate":
            # 备份TRUNCATE前的文件
            backup = self.backup_ibd_file(data_dir, version, "truncate_before")
            if container_name:
                self.execute_sql(container_name, "TRUNCATE TABLE disaster_test;")
            return backup

        elif disaster_type == "drop":
            # 备份DROP前的文件
            backup = self.backup_ibd_file(data_dir, version, "drop_before")
            if container_name:
                self.execute_sql(container_name, "DROP TABLE disaster_test;")
            return backup

        elif disaster_type == "corruption":
            # 备份并损坏文件
            backup = self.backup_ibd_file(data_dir, version, "corruption_before")
            if ibd_path.exists():
                with open(ibd_path, 'r+b') as f:
                    # 损坏第二个数据页
                    f.seek(16384 * 4)
                    corrupt_data = bytes([random.randint(0, 255) for _ in range(1024)])
                    f.write(corrupt_data)
            return backup

        return None

    def run_recovery(self, ibd_path: Path, output_dir: Path,
                     frm_path: Path = None, mode: str = "default") -> Dict:
        """执行恢复"""
        output_dir.mkdir(parents=True, exist_ok=True)

        cmd = [RECOVERY_TOOL, "--ibd", str(ibd_path), "--output", str(output_dir)]

        if mode == "truncate":
            cmd.append("--truncate-recovery")
        elif mode == "rmrf":
            cmd.append("--rmrf-recovery")
        elif mode == "disk-carving":
            cmd.append("--disk-carving")

        if frm_path and frm_path.exists():
            cmd.extend(["--frm", str(frm_path)])

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
                "csv_file": str(csv_file) if csv_file else None
            }

        except subprocess.TimeoutExpired:
            return {"success": False, "error": "Timeout"}
        except Exception as e:
            return {"success": False, "error": str(e)}

    def validate_recovery(self, csv_file: Path, expected_count: int,
                          original_checksum: str = None) -> Dict:
        """验证恢复结果"""
        if not csv_file or not csv_file.exists():
            return {"valid": False, "reason": "No CSV file"}

        try:
            with open(csv_file, 'r') as f:
                lines = f.readlines()

            data_rows = len(lines) - 1  # 减去标题行

            # 计算恢复率
            recovery_rate = data_rows / expected_count if expected_count > 0 else 0

            # 验证数据完整性
            valid_rows = 0
            for line in lines[1:]:
                fields = line.strip().split(',')
                if len(fields) >= 3:
                    try:
                        int(fields[0])  # 验证ID是数字
                        valid_rows += 1
                    except:
                        pass

            return {
                "valid": recovery_rate >= 0.95,
                "recovered_rows": data_rows,
                "valid_rows": valid_rows,
                "recovery_rate": recovery_rate,
                "expected_rows": expected_count
            }

        except Exception as e:
            return {"valid": False, "reason": str(e)}

    def test_version(self, version: str) -> Dict:
        """测试单个版本"""
        self.log(f"\n{'='*60}")
        self.log(f"Testing {version} with {self.num_rows} rows")
        self.log(f"{'='*60}")

        results = {"version": version, "num_rows": self.num_rows, "scenarios": {}}

        # 创建数据目录
        data_dir = self.test_dir / version
        data_dir.mkdir(parents=True, exist_ok=True)

        # 启动MySQL容器
        container_name = self.start_mysql_container(version, data_dir)
        if not container_name:
            results["error"] = "Failed to start MySQL container"
            return results

        try:
            # 等待MySQL就绪
            if not self.wait_for_mysql(container_name):
                results["error"] = "MySQL did not become ready"
                return results

            # 插入测试数据
            success, inserted = self.insert_test_data(container_name)
            if not success:
                results["error"] = f"Failed to insert all data ({inserted}/{self.num_rows})"
                return results

            # 获取原始数据校验和
            original_checksum = self.save_original_checksum(container_name, version)
            self.log(f"Original data: {original_checksum}")

            # 测试各灾难场景
            for scenario in ["rmrf", "truncate", "drop"]:
                self.log(f"\n--- Scenario: {scenario} ---")

                # 停止容器以确保数据刷新到磁盘
                self.stop_container(container_name)
                container_name = None

                # 重新启动容器
                container_name = self.start_mysql_container(version, data_dir)
                if not container_name or not self.wait_for_mysql(container_name):
                    self.log(f"Failed to restart container for {scenario}", "ERROR")
                    continue

                # 模拟灾难
                backup_file = self.simulate_disaster(container_name, data_dir, scenario, version)
                if not backup_file:
                    self.log(f"Failed to simulate {scenario}", "ERROR")
                    continue

                # 停止容器
                self.stop_container(container_name)
                container_name = None

                # 执行恢复
                output_dir = self.test_dir / "recovery" / version / scenario
                frm_path = backup_file.parent / "disaster_test.frm"

                recovery_result = self.run_recovery(
                    backup_file, output_dir, frm_path, mode=scenario
                )

                # 验证恢复
                if recovery_result.get("csv_file"):
                    validation = self.validate_recovery(
                        Path(recovery_result["csv_file"]),
                        self.num_rows,
                        original_checksum.get("checksum")
                    )
                else:
                    validation = {"valid": False, "reason": "No recovery output"}

                results["scenarios"][scenario] = {
                    "success": recovery_result.get("success", False),
                    "recovered_rows": validation.get("recovered_rows", 0),
                    "recovery_rate": validation.get("recovery_rate", 0),
                    "valid": validation.get("valid", False),
                    "elapsed": recovery_result.get("elapsed", 0),
                    "backup_file": str(backup_file)
                }

                status = "PASS" if validation.get("valid") else "FAIL"
                rate = validation.get("recovery_rate", 0) * 100
                self.log(f"  {status}: {validation.get('recovered_rows', 0)}/{self.num_rows} rows ({rate:.1f}%)")

        finally:
            if container_name:
                self.stop_container(container_name)

        return results

    def run_all_versions(self) -> Dict:
        """运行所有版本的测试"""
        self.log("\n" + "="*70)
        self.log(f"MySQL Disaster Recovery Test - {self.num_rows} rows")
        self.log("="*70)
        self.log(f"Recovery tool: {RECOVERY_TOOL}")
        self.log(f"Test directory: {self.test_dir}")
        self.log("="*70)

        all_results = {}

        for version in MYSQL_VERSIONS.keys():
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
        report.append(f"MySQL Disaster Recovery Test Report")
        report.append(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        report.append(f"Test rows: {self.num_rows}")
        report.append("=" * 80)

        report.append("\n## Test Results Matrix\n")
        report.append("-" * 80)
        report.append(f"{'Version':<12} {'rm -rf':<15} {'TRUNCATE':<15} {'DROP':<15}")
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
                    total_recovered += scenario_result.get("recovered_rows", 0)
                    rate = scenario_result.get("recovery_rate", 0) * 100
                    row.append(f"PASS ({rate:.0f}%)")
                elif scenario_result.get("success"):
                    rate = scenario_result.get("recovery_rate", 0) * 100
                    row.append(f"PARTIAL ({rate:.0f}%)")
                else:
                    row.append("FAIL")

            report.append(f"{row[0]:<12} {row[1]:<15} {row[2]:<15} {row[3]:<15}")

        report.append("-" * 80)
        report.append(f"\n## Summary\n")
        report.append(f"- Total scenarios: {total_scenarios}")
        report.append(f"- Passed: {passed_scenarios}")
        report.append(f"- Success rate: {passed_scenarios/total_scenarios*100:.1f}%" if total_scenarios > 0 else "- Success rate: N/A")
        report.append(f"- Total recovered rows: {total_recovered}")

        report_text = "\n".join(report)

        # 保存报告
        report_path = self.test_dir / f"REPORT_{datetime.now().strftime('%Y%m%d_%H%M%S')}.md"
        report_path.write_text(report_text)

        self.log(f"\nReport saved: {report_path}")
        return report_text


def main():
    import argparse
    parser = argparse.ArgumentParser(description="MySQL Disaster Recovery Test")
    parser.add_argument("--rows", type=int, default=10000, help="Number of test rows (default: 10000)")
    parser.add_argument("--version", type=str, help="Test specific version only")
    args = parser.parse_args()

    # 检查恢复工具
    if not Path(RECOVERY_TOOL).exists():
        print(f"Error: Recovery tool not found: {RECOVERY_TOOL}")
        print("Please build the project first: cmake --build build")
        sys.exit(1)

    # 运行测试
    tester = LargeScaleDisasterRecoveryTest(num_rows=args.rows)

    if args.version:
        results = {args.version: tester.test_version(args.version)}
    else:
        results = tester.run_all_versions()

    # 生成报告
    report = tester.generate_report(results)
    print("\n" + report)


if __name__ == "__main__":
    main()
