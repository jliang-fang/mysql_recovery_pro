#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
Automated Recovery Evaluation Platform v2.0
Author: jliang.fang (Database Kernel Forensics Expert)

Purpose: 全自动化恢复评估平台

功能:
  1. 容器化MySQL 5.6-9.0环境管理
  2. 自动数据注入与基线建立
  3. 破坏执行与恢复调用
  4. 恢复结果验证与比对
  5. 生成详细评估报告

评估流程:
  数据注入 → 建立基线 → 施加破坏 → 调用恢复 → 导入验证 → 比对哈希
==============================================================================
"""

import os
import sys
import json
import time
import hashlib
import shutil
import argparse
import subprocess
from pathlib import Path
from datetime import datetime
from typing import List, Dict, Optional, Tuple, Any
from dataclasses import dataclass, field
from enum import Enum
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed

try:
    import mysql.connector
    from mysql.connector import Error
except ImportError:
    subprocess.run([sys.executable, "-m", "pip", "install", "mysql-connector-python"], check=True)
    import mysql.connector


class MySQLVersion(Enum):
    """MySQL版本枚举"""
    MYSQL_56 = "5.6"
    MYSQL_57 = "5.7"
    MYSQL_80 = "8.0"
    MYSQL_84 = "8.4"
    MYSQL_90 = "9.0"


@dataclass
class MySQLContainer:
    """MySQL容器配置"""
    version: MySQLVersion
    container_name: str
    port: int
    image: str
    status: str = "unknown"
    data_dir: str = ""

    def get_connection_config(self, password: str = "root") -> Dict[str, Any]:
        return {
            "host": "127.0.0.1",
            "port": self.port,
            "user": "root",
            "password": password,
            "charset": "utf8mb4"
        }


@dataclass
class TestData:
    """测试数据"""
    table_name: str
    schema_sql: str
    insert_sqls: List[str]
    row_count: int
    expected_hashes: Dict[str, str]  # column_name -> expected_hash
    data_file: str = ""


@dataclass
class BaselineRecord:
    """基线记录"""
    table_name: str
    row_count: int
    sha256_hash: str
    column_hashes: Dict[str, str]
    created_at: datetime
    ibd_path: str
    frm_path: str = ""


@dataclass
class RecoveryResult:
    """恢复结果"""
    success: bool
    recovered_rows: int
    expected_rows: int
    recovery_rate: float
    output_file: str
    error_file: str = ""
    recovery_time: float = 0.0
    error_message: str = ""


@dataclass
class EvaluationResult:
    """评估结果"""
    mysql_version: MySQLVersion
    corruption_type: str
    baseline: BaselineRecord
    recovery_result: RecoveryResult
    validation_passed: bool
    details: Dict[str, Any] = field(default_factory=dict)


class AutomatedEvaluationPlatform:
    """自动化评估平台"""

    # MySQL容器配置
    CONTAINER_CONFIGS = {
        MySQLVersion.MYSQL_56: MySQLContainer(
            version=MySQLVersion.MYSQL_56,
            container_name="mysql56_test",
            port=3306,
            image="docker.m.daocloud.io/mysql:5.6"
        ),
        MySQLVersion.MYSQL_57: MySQLContainer(
            version=MySQLVersion.MYSQL_57,
            container_name="mysql57_test",
            port=3307,
            image="docker.m.daocloud.io/mysql:5.7"
        ),
        MySQLVersion.MYSQL_80: MySQLContainer(
            version=MySQLVersion.MYSQL_80,
            container_name="mysql80_test",
            port=3308,
            image="docker.m.daocloud.io/mysql:8.0"
        ),
        MySQLVersion.MYSQL_84: MySQLContainer(
            version=MySQLVersion.MYSQL_84,
            container_name="mysql84_test",
            port=3309,
            image="docker.m.daocloud.io/mysql:8.4"
        ),
        MySQLVersion.MYSQL_90: MySQLContainer(
            version=MySQLVersion.MYSQL_90,
            container_name="mysql90_test",
            port=3310,
            image="docker.m.daocloud.io/mysql:9.0"
        ),
    }

    def __init__(self, recovery_tool_path: str, output_base_dir: str = "./evaluation_results"):
        self.recovery_tool_path = recovery_tool_path
        self.output_base_dir = output_base_dir
        self.baselines: Dict[str, BaselineRecord] = {}
        self.results: List[EvaluationResult] = []

        os.makedirs(output_base_dir, exist_ok=True)

    # =========================================================================
    # 容器管理
    # =========================================================================

    def check_containers_status(self) -> Dict[MySQLVersion, str]:
        """检查所有MySQL容器状态"""
        status = {}

        for version, config in self.CONTAINER_CONFIGS.items():
            try:
                result = subprocess.run(
                    ["podman", "ps", "-a", "--format", "{{.Status}}",
                     "--filter", f"name={config.container_name}"],
                    capture_output=True, text=True, timeout=10
                )
                if result.returncode == 0 and result.stdout.strip():
                    status_str = result.stdout.strip()
                    if "Up" in status_str:
                        status[version] = "running"
                        config.status = "running"
                    else:
                        status[version] = "stopped"
                        config.status = "stopped"
                else:
                    # 尝试docker
                    result = subprocess.run(
                        ["docker", "ps", "-a", "--format", "{{.Status}}",
                         "--filter", f"name={config.container_name}"],
                        capture_output=True, text=True, timeout=10
                    )
                    if result.returncode == 0 and result.stdout.strip():
                        status_str = result.stdout.strip()
                        if "Up" in status_str:
                            status[version] = "running"
                            config.status = "running"
                        else:
                            status[version] = "stopped"
                            config.status = "stopped"
                    else:
                        status[version] = "not_found"
                        config.status = "not_found"
            except Exception as e:
                status[version] = f"error: {str(e)}"

        return status

    def get_container_data_dir(self, container_name: str) -> str:
        """获取容器数据目录挂载点"""
        try:
            result = subprocess.run(
                ["podman", "inspect", "--format",
                 "{{range .Mounts}}{{if eq .Destination \"/var/lib/mysql\"}}{{.Source}}{{end}}{{end}}",
                 container_name],
                capture_output=True, text=True, timeout=10
            )
            if result.returncode == 0 and result.stdout.strip():
                return result.stdout.strip()

            # 尝试docker
            result = subprocess.run(
                ["docker", "inspect", "--format",
                 "{{range .Mounts}}{{if eq .Destination \"/var/lib/mysql\"}}{{.Source}}{{end}}{{end}}",
                 container_name],
                capture_output=True, text=True, timeout=10
            )
            if result.returncode == 0 and result.stdout.strip():
                return result.stdout.strip()
        except Exception as e:
            print(f"获取数据目录失败: {e}")

        return ""

    # =========================================================================
    # 数据注入
    # =========================================================================

    def inject_test_data(self, version: MySQLVersion, test_data: TestData,
                        database: str = "test_recovery") -> bool:
        """注入测试数据"""
        config = self.CONTAINER_CONFIGS[version]
        conn_config = config.get_connection_config()

        try:
            conn = mysql.connector.connect(**conn_config)
            cursor = conn.cursor()

            # 创建数据库
            cursor.execute(f"CREATE DATABASE IF NOT EXISTS {database}")
            cursor.execute(f"USE {database}")

            # 创建表
            cursor.execute(test_data.schema_sql)

            # 批量插入数据
            batch_size = 1000
            for i in range(0, len(test_data.insert_sqls), batch_size):
                batch = test_data.insert_sqls[i:i+batch_size]
                for sql in batch:
                    cursor.execute(sql)
                conn.commit()

                if (i + batch_size) % 10000 == 0:
                    print(f"  已插入 {i + batch_size} 行...")

            cursor.close()
            conn.close()

            print(f"✓ 数据注入完成: {test_data.table_name} ({test_data.row_count} 行)")
            return True

        except Error as e:
            print(f"✗ 数据注入失败: {e}")
            return False

    def generate_comprehensive_test_data(self, row_count: int = 10000) -> TestData:
        """生成综合性测试数据"""
        # 创建包含多种数据类型的表
        schema_sql = """
        CREATE TABLE comprehensive_test (
            id INT PRIMARY KEY AUTO_INCREMENT,
            name VARCHAR(100) NOT NULL,
            email VARCHAR(200),
            score INT,
            price DECIMAL(10, 2),
            rating FLOAT,
            created_at DATETIME,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            status ENUM('active', 'inactive', 'pending'),
            tags SET('vip', 'premium', 'verified', 'banned'),
            metadata JSON,
            description TEXT,
            INDEX idx_name (name),
            INDEX idx_score (score)
        ) ENGINE=InnoDB ROW_FORMAT=DYNAMIC
        """

        insert_sqls = []
        statuses = ['active', 'inactive', 'pending']
        tag_combos = ['vip', 'premium', 'vip,premium', 'verified', 'vip,verified', 'premium,verified']

        for i in range(1, row_count + 1):
            name = f"user_{i}_{random.randint(1000, 9999)}"
            email = f"{name}@example.com"
            score = random.randint(0, 1000)
            price = round(random.uniform(0, 10000), 2)
            rating = round(random.uniform(0, 5), 2)
            status = random.choice(statuses)
            tags = random.choice(tag_combos)
            metadata = json.dumps({
                "level": random.randint(1, 10),
                "region": random.choice(["east", "west", "north", "south"]),
                "login_count": random.randint(0, 1000)
            })
            description = f"Description for user {i} " * random.randint(5, 20)

            sql = f"""INSERT INTO comprehensive_test
                (name, email, score, price, rating, created_at, status, tags, metadata, description)
                VALUES ('{name}', '{email}', {score}, {price}, {rating},
                        DATE_ADD('2024-01-01', INTERVAL {random.randint(0, 365)} DAY),
                        '{status}', '{tags}', '{metadata}', '{description}')"""
            insert_sqls.append(sql)

        return TestData(
            table_name="comprehensive_test",
            schema_sql=schema_sql,
            insert_sqls=insert_sqls,
            row_count=row_count,
            expected_hashes={}
        )

    # =========================================================================
    # 基线建立
    # =========================================================================

    def establish_baseline(self, version: MySQLVersion, table_name: str,
                          database: str = "test_recovery") -> Optional[BaselineRecord]:
        """建立数据基线"""
        config = self.CONTAINER_CONFIGS[version]
        conn_config = config.get_connection_config()
        conn_config["database"] = database

        try:
            conn = mysql.connector.connect(**conn_config)
            cursor = conn.cursor()

            # 获取行数
            cursor.execute(f"SELECT COUNT(*) FROM {table_name}")
            row_count = cursor.fetchone()[0]

            # 获取所有数据并计算哈希
            cursor.execute(f"SELECT * FROM {table_name} ORDER BY id")

            # 计算整表SHA-256
            sha256 = hashlib.sha256()
            column_hashes = {}

            # 获取列名
            columns = [desc[0] for desc in cursor.description]

            # 为每列初始化哈希
            for col in columns:
                column_hashes[col] = hashlib.sha256()

            rows = cursor.fetchall()
            for row in rows:
                row_str = "|".join(str(v) for v in row)
                sha256.update(row_str.encode())

                for i, col in enumerate(columns):
                    if row[i] is not None:
                        column_hashes[col].update(str(row[i]).encode())

            # 获取ibd文件路径
            data_dir = self.get_container_data_dir(config.container_name)
            ibd_path = os.path.join(data_dir, database, f"{table_name}.ibd")
            frm_path = ""
            if version in [MySQLVersion.MYSQL_56, MySQLVersion.MYSQL_57]:
                frm_path = os.path.join(data_dir, database, f"{table_name}.frm")

            baseline = BaselineRecord(
                table_name=table_name,
                row_count=row_count,
                sha256_hash=sha256.hexdigest(),
                column_hashes={k: v.hexdigest() for k, v in column_hashes.items()},
                created_at=datetime.now(),
                ibd_path=ibd_path,
                frm_path=frm_path
            )

            cursor.close()
            conn.close()

            self.baselines[f"{version.value}_{table_name}"] = baseline
            print(f"✓ 基线建立完成: {row_count} 行, SHA256: {baseline.sha256_hash[:16]}...")

            return baseline

        except Error as e:
            print(f"✗ 基线建立失败: {e}")
            return None

    # =========================================================================
    # 恢复执行
    # =========================================================================

    def execute_recovery(self, ibd_path: str, output_dir: str,
                        options: Dict[str, Any] = None) -> RecoveryResult:
        """执行恢复"""
        start_time = time.time()

        os.makedirs(output_dir, exist_ok=True)

        # 构建命令
        cmd = [self.recovery_tool_path, "--ibd", ibd_path, "--output", output_dir]

        if options:
            if options.get("rmrf_recovery"):
                cmd.append("--rmrf-recovery")
            if options.get("truncate_recovery"):
                cmd.append("--truncate-recovery")
            if options.get("frm"):
                cmd.extend(["--frm", options["frm"]])
            if options.get("verbose"):
                cmd.append("--verbose")
            if options.get("format"):
                cmd.extend(["--format", options["format"]])

        try:
            result = subprocess.run(
                cmd, capture_output=True, text=True, timeout=300
            )

            recovery_time = time.time() - start_time

            # 检查输出文件
            output_file = os.path.join(output_dir, "recovered.sql")
            if not os.path.exists(output_file):
                output_file = os.path.join(output_dir, f"{os.path.basename(ibd_path).replace('.ibd', '')}.sql")

            # 统计恢复行数
            recovered_rows = 0
            if os.path.exists(output_file):
                with open(output_file, 'r') as f:
                    content = f.read()
                    recovered_rows = content.count("INSERT INTO")

            return RecoveryResult(
                success=result.returncode == 0,
                recovered_rows=recovered_rows,
                expected_rows=0,
                recovery_rate=0.0,
                output_file=output_file,
                error_file="",
                recovery_time=recovery_time,
                error_message=result.stderr if result.returncode != 0 else ""
            )

        except subprocess.TimeoutExpired:
            return RecoveryResult(
                success=False,
                recovered_rows=0,
                expected_rows=0,
                recovery_rate=0.0,
                output_file="",
                error_message="Recovery timeout (>300s)"
            )
        except Exception as e:
            return RecoveryResult(
                success=False,
                recovered_rows=0,
                expected_rows=0,
                recovery_rate=0.0,
                output_file="",
                error_message=str(e)
            )

    # =========================================================================
    # 验证恢复结果
    # =========================================================================

    def validate_recovery(self, baseline: BaselineRecord,
                         recovery_result: RecoveryResult,
                         validation_db: str = "validation_temp") -> Tuple[bool, Dict]:
        """验证恢复结果"""
        if not recovery_result.success or not os.path.exists(recovery_result.output_file):
            return False, {"error": "Recovery failed or no output file"}

        # 使用MySQL 8.0容器进行验证
        config = self.CONTAINER_CONFIGS[MySQLVersion.MYSQL_80]
        conn_config = config.get_connection_config()

        try:
            conn = mysql.connector.connect(**conn_config)
            cursor = conn.cursor()

            # 创建临时验证数据库
            cursor.execute(f"CREATE DATABASE IF NOT EXISTS {validation_db}")
            cursor.execute(f"USE {validation_db}")

            # 导入恢复的SQL
            with open(recovery_result.output_file, 'r') as f:
                sql_content = f.read()

            # 执行SQL
            for statement in sql_content.split(';'):
                statement = statement.strip()
                if statement and not statement.startswith('--'):
                    try:
                        cursor.execute(statement)
                    except Error as e:
                        # 忽略重复键等非致命错误
                        if "Duplicate" not in str(e):
                            print(f"SQL执行警告: {e}")

            conn.commit()

            # 获取恢复的行数
            cursor.execute(f"SELECT COUNT(*) FROM {baseline.table_name}")
            recovered_count = cursor.fetchone()[0]

            # 计算恢复率
            recovery_rate = recovered_count / baseline.row_count if baseline.row_count > 0 else 0

            # 比对数据
            cursor.execute(f"SELECT * FROM {baseline.table_name} ORDER BY id")
            recovered_sha256 = hashlib.sha256()

            for row in cursor.fetchall():
                row_str = "|".join(str(v) for v in row)
                recovered_sha256.update(row_str.encode())

            recovered_hash = recovered_sha256.hexdigest()
            hash_match = recovered_hash == baseline.sha256_hash

            validation_details = {
                "expected_rows": baseline.row_count,
                "recovered_rows": recovered_count,
                "recovery_rate": recovery_rate,
                "baseline_hash": baseline.sha256_hash,
                "recovered_hash": recovered_hash,
                "hash_match": hash_match
            }

            # 清理临时数据库
            cursor.execute(f"DROP DATABASE IF EXISTS {validation_db}")
            conn.commit()

            cursor.close()
            conn.close()

            validation_passed = recovery_rate >= 0.999 and hash_match

            return validation_passed, validation_details

        except Error as e:
            return False, {"error": str(e)}

    # =========================================================================
    # 完整评估流程
    # =========================================================================

    def run_full_evaluation(self, versions: List[MySQLVersion] = None,
                           row_counts: List[int] = None,
                           corruption_types: List[str] = None) -> List[EvaluationResult]:
        """运行完整评估"""
        if versions is None:
            versions = list(MySQLVersion)

        if row_counts is None:
            row_counts = [10000]

        if corruption_types is None:
            corruption_types = ["rm_rf_single", "drop_table", "truncate_table",
                              "header_erasure", "random_page"]

        # 检查容器状态
        print("\n" + "="*70)
        print("检查MySQL容器状态...")
        print("="*70)

        container_status = self.check_containers_status()
        for version, status in container_status.items():
            print(f"  {version.value}: {status}")

        # 创建破坏矩阵
        from extreme_destruction_matrix import ExtremeDestructionMatrix, CorruptionType
        destruction_matrix = ExtremeDestructionMatrix()

        results = []

        for version in versions:
            if container_status.get(version) != "running":
                print(f"\n跳过 {version.value} - 容器未运行")
                continue

            print(f"\n{'='*70}")
            print(f"评估 MySQL {version.value}")
            print(f"{'='*70}")

            for row_count in row_counts:
                print(f"\n--- 测试数据量: {row_count} 行 ---")

                # 1. 生成测试数据
                print("\n[1/6] 生成测试数据...")
                test_data = self.generate_comprehensive_test_data(row_count)

                # 2. 注入数据
                print("\n[2/6] 注入测试数据...")
                if not self.inject_test_data(version, test_data):
                    continue

                # 3. 建立基线
                print("\n[3/6] 建立基线...")
                baseline = self.establish_baseline(version, test_data.table_name)
                if not baseline:
                    continue

                # 4. 施加破坏
                print("\n[4/6] 施加破坏...")
                for corruption_type in corruption_types:
                    print(f"\n  >>> 破坏类型: {corruption_type}")

                    # 根据破坏类型执行
                    corrupted_file = None
                    recovery_options = {}

                    if corruption_type == "rm_rf_single":
                        # 备份后删除
                        backup_dir = os.path.join(self.output_base_dir, "backups")
                        os.makedirs(backup_dir, exist_ok=True)
                        backup_path = os.path.join(backup_dir, f"{test_data.table_name}_before.ibd")
                        shutil.copy(baseline.ibd_path, backup_path)

                        # 模拟删除
                        corrupted_file = backup_path
                        recovery_options["rmrf_recovery"] = True

                    elif corruption_type == "drop_table":
                        # DROP TABLE
                        backup_dir = os.path.join(self.output_base_dir, "backups")
                        os.makedirs(backup_dir, exist_ok=True)
                        backup_path = os.path.join(backup_dir, f"{test_data.table_name}_drop.ibd")
                        shutil.copy(baseline.ibd_path, backup_path)

                        config = self.CONTAINER_CONFIGS[version]
                        conn = mysql.connector.connect(**config.get_connection_config())
                        cursor = conn.cursor()
                        cursor.execute(f"DROP TABLE test_recovery.{test_data.table_name}")
                        conn.commit()
                        cursor.close()
                        conn.close()

                        corrupted_file = backup_path

                    elif corruption_type == "truncate_table":
                        # TRUNCATE TABLE
                        backup_dir = os.path.join(self.output_base_dir, "backups")
                        os.makedirs(backup_dir, exist_ok=True)
                        backup_path = os.path.join(backup_dir, f"{test_data.table_name}_truncate.ibd")
                        shutil.copy(baseline.ibd_path, backup_path)

                        config = self.CONTAINER_CONFIGS[version]
                        conn = mysql.connector.connect(**config.get_connection_config())
                        cursor = conn.cursor()
                        cursor.execute(f"TRUNCATE TABLE test_recovery.{test_data.table_name}")
                        conn.commit()
                        cursor.close()
                        conn.close()

                        corrupted_file = backup_path
                        recovery_options["truncate_recovery"] = True

                    elif corruption_type == "header_erasure":
                        corrupted_file = destruction_matrix.corrupt_header_erasure(baseline.ibd_path)

                    elif corruption_type == "random_page":
                        corrupted_file = destruction_matrix.corrupt_random_pages(baseline.ibd_path, 0.1)

                    if not corrupted_file or not os.path.exists(corrupted_file):
                        print(f"    ✗ 破坏失败")
                        continue

                    # 5. 执行恢复
                    print(f"\n[5/6] 执行恢复...")
                    output_dir = os.path.join(self.output_base_dir,
                                             f"recovery_{version.value}_{corruption_type}")
                    recovery_result = self.execute_recovery(
                        corrupted_file, output_dir, recovery_options
                    )

                    print(f"    恢复状态: {'成功' if recovery_result.success else '失败'}")
                    print(f"    恢复行数: {recovery_result.recovered_rows}")
                    print(f"    恢复时间: {recovery_result.recovery_time:.2f}s")

                    # 6. 验证恢复结果
                    print(f"\n[6/6] 验证恢复结果...")
                    validation_passed, validation_details = self.validate_recovery(
                        baseline, recovery_result
                    )

                    print(f"    验证状态: {'通过' if validation_passed else '失败'}")
                    print(f"    恢复率: {validation_details.get('recovery_rate', 0)*100:.2f}%")

                    # 记录结果
                    result = EvaluationResult(
                        mysql_version=version,
                        corruption_type=corruption_type,
                        baseline=baseline,
                        recovery_result=recovery_result,
                        validation_passed=validation_passed,
                        details=validation_details
                    )
                    results.append(result)

        self.results = results
        return results

    # =========================================================================
    # 报告生成
    # =========================================================================

    def generate_matrix_report(self, output_path: str = None) -> str:
        """生成恢复率矩阵报告"""
        report = []
        report.append("=" * 100)
        report.append("MySQL Recovery Pro - 恢复率矩阵报告")
        report.append(f"生成时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        report.append("=" * 100)
        report.append("")

        # 按版本和破坏类型组织结果
        matrix = {}
        for result in self.results:
            version = result.mysql_version.value
            corruption = result.corruption_type

            if version not in matrix:
                matrix[version] = {}

            recovery_rate = result.details.get("recovery_rate", 0) * 100
            recovery_time = result.recovery_result.recovery_time

            matrix[version][corruption] = {
                "rate": recovery_rate,
                "time": recovery_time,
                "passed": result.validation_passed
            }

        # 打印矩阵表头
        report.append("恢复率矩阵 (恢复率% / 耗时秒数)")
        report.append("-" * 100)

        # 获取所有破坏类型
        all_corruptions = set()
        for version_data in matrix.values():
            all_corruptions.update(version_data.keys())

        # 表头
        header = f"{'MySQL版本':<15}"
        for corruption in sorted(all_corruptions):
            header += f"{corruption:<25}"
        report.append(header)
        report.append("-" * 100)

        # 数据行
        for version in sorted(matrix.keys()):
            row = f"{version:<15}"
            for corruption in sorted(all_corruptions):
                if corruption in matrix[version]:
                    data = matrix[version][corruption]
                    status = "✓" if data["passed"] else "✗"
                    row += f"{status}{data['rate']:.1f}%/{data['time']:.1f}s".ljust(25)
                else:
                    row += "N/A".ljust(25)
            report.append(row)

        report.append("")
        report.append("=" * 100)

        # 统计摘要
        total_tests = len(self.results)
        passed_tests = sum(1 for r in self.results if r.validation_passed)
        avg_rate = sum(r.details.get("recovery_rate", 0) for r in self.results) / total_tests if total_tests > 0 else 0

        report.append("")
        report.append("统计摘要")
        report.append("-" * 50)
        report.append(f"总测试数: {total_tests}")
        report.append(f"通过数: {passed_tests}")
        report.append(f"通过率: {passed_tests/total_tests*100:.1f}%" if total_tests > 0 else "通过率: N/A")
        report.append(f"平均恢复率: {avg_rate*100:.2f}%")

        report_text = "\n".join(report)

        if output_path:
            with open(output_path, 'w', encoding='utf-8') as f:
                f.write(report_text)

        return report_text


def main():
    parser = argparse.ArgumentParser(description="自动化恢复评估平台 v2.0")
    parser.add_argument("--recovery-tool", required=True, help="恢复工具路径")
    parser.add_argument("--output-dir", default="./evaluation_results", help="输出目录")
    parser.add_argument("--versions", nargs='+',
                       choices=['5.6', '5.7', '8.0', '8.4', '9.0'],
                       help="要测试的MySQL版本")
    parser.add_argument("--row-counts", nargs='+', type=int, default=[10000],
                       help="测试数据量")
    parser.add_argument("--corruptions", nargs='+',
                       default=['rm_rf_single', 'drop_table', 'truncate_table'],
                       help="破坏类型")
    parser.add_argument("--report", help="报告输出路径")

    args = parser.parse_args()

    # 初始化平台
    platform = AutomatedEvaluationPlatform(
        recovery_tool_path=args.recovery_tool,
        output_base_dir=args.output_dir
    )

    # 转换版本枚举
    version_map = {
        '5.6': MySQLVersion.MYSQL_56,
        '5.7': MySQLVersion.MYSQL_57,
        '8.0': MySQLVersion.MYSQL_80,
        '8.4': MySQLVersion.MYSQL_84,
        '9.0': MySQLVersion.MYSQL_90
    }

    versions = [version_map[v] for v in args.versions] if args.versions else list(MySQLVersion)

    # 运行评估
    print("\n" + "="*70)
    print("自动化恢复评估平台 v2.0")
    print("="*70)
    print(f"恢复工具: {args.recovery_tool}")
    print(f"输出目录: {args.output_dir}")
    print(f"测试版本: {[v.value for v in versions]}")
    print(f"数据量: {args.row_counts}")
    print(f"破坏类型: {args.corruptions}")
    print("="*70 + "\n")

    results = platform.run_full_evaluation(
        versions=versions,
        row_counts=args.row_counts,
        corruption_types=args.corruptions
    )

    # 生成报告
    report = platform.generate_matrix_report(args.report)
    print("\n" + report)


if __name__ == "__main__":
    main()
