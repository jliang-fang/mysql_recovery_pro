#!/usr/bin/env python3
"""
快速验证测试 - 100K数据量
验证DECIMAL和其他字段类型解析修复效果
"""

import os
import sys
import time
import subprocess
import random
import json
from pathlib import Path
from datetime import datetime

# 容器配置
CONTAINERS = {
    "mysql80": {"image": "docker.m.daocloud.io/mysql:8.0"},
}

RECOVERY_TOOL = Path("/app/mysql_recovery_pro/build/mysql_recovery_pro")
BLOCK_DEVICE = "/dev/nvme0n2"
MOUNT_POINT = Path("/mnt/mysql_quick_test")
TEST_DIR = Path("/app/mysql_recovery_test/quick_validation_100k")
NUM_ROWS = 100000

# 包含核心字段类型的表
CREATE_TABLE_SQL = """
CREATE TABLE test_table (
    id INT PRIMARY KEY AUTO_INCREMENT,
    col_int INT,
    col_bigint BIGINT,
    col_float FLOAT,
    col_double DOUBLE,
    col_decimal DECIMAL(12,4),
    col_varchar VARCHAR(100),
    col_text TEXT,
    col_date DATE,
    col_datetime DATETIME,
    col_enum ENUM('active','inactive','pending'),
    col_json JSON
) ENGINE=InnoDB
"""

# Schema参数
SCHEMA_PARAM = (
    "id:INT:4:pk,"
    "col_int:INT:4,"
    "col_bigint:BIGINT:8,"
    "col_float:FLOAT:4,"
    "col_double:DOUBLE:8,"
    "col_decimal:DECIMAL:12:4,"
    "col_varchar:VARCHAR:100,"
    "col_text:TEXT:0,"
    "col_date:DATE:3,"
    "col_datetime:DATETIME:5,"
    "col_enum:ENUM:0,"
    "col_json:JSON:0"
)

def log(msg, level="INFO"):
    timestamp = datetime.now().strftime("%H:%M:%S")
    colors = {"INFO": "\033[0m", "WARN": "\033[93m", "ERROR": "\033[91m", "PASS": "\033[92m"}
    print(f"{colors.get(level, '')}[{timestamp}] [{level}] {msg}\033[0m")
    sys.stdout.flush()

def run_cmd(cmd, timeout=600):
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return False, "", "Timeout"

def setup_filesystem(fs_type):
    run_cmd(["umount", str(MOUNT_POINT)], timeout=30)
    run_cmd(["dd", "if=/dev/zero", f"of={BLOCK_DEVICE}", "bs=1M", "count=50", "conv=notrunc"], timeout=60)

    if fs_type == "xfs":
        success, _, stderr = run_cmd(["mkfs.xfs", "-f", BLOCK_DEVICE], timeout=120)
    else:
        success, _, stderr = run_cmd(["mkfs.ext4", "-F", BLOCK_DEVICE], timeout=120)

    if not success:
        log(f"创建文件系统失败: {stderr}", "ERROR")
        return False

    MOUNT_POINT.mkdir(parents=True, exist_ok=True)
    success, _, stderr = run_cmd(["mount", BLOCK_DEVICE, str(MOUNT_POINT)], timeout=30)
    if not success:
        log(f"挂载失败: {stderr}", "ERROR")
        return False

    return True

def flush_and_sync(container_name):
    run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             "SET GLOBAL innodb_flush_log_at_trx_commit=1;"])
    run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             "FLUSH NO_WRITE_TO_BINLOG ENGINE INNODB; FLUSH NO_WRITE_TO_BINLOG TABLES;"])
    run_cmd(["podman", "exec", container_name, "sync"])
    time.sleep(5)
    run_cmd(["sync"])
    time.sleep(3)

def generate_sql_file(num_rows, output_path):
    log(f"生成SQL文件: {num_rows} 行...")
    with open(output_path, 'w') as f:
        f.write("SET autocommit=0;\nSET unique_checks=0;\nSET foreign_key_checks=0;\n")

        batch_size = 200
        enum_values = ['active', 'inactive', 'pending']

        for batch_start in range(0, num_rows, batch_size):
            batch_end = min(batch_start + batch_size, num_rows)
            values_list = []

            for i in range(batch_start, batch_end):
                col_int = random.randint(0, 1000000)
                col_bigint = random.randint(0, 1000000000)
                col_float = round(random.uniform(0, 1000), 4)
                col_double = round(random.uniform(0, 100000), 8)
                col_decimal = round(random.uniform(0, 10000), 4)
                col_varchar = f"name_{i}_{random.randint(1000,9999)}"
                col_text = f"content_{i}_" + "x" * random.randint(5, 30)
                col_date = f"'2026-07-{random.randint(1,28):02d}'"
                col_datetime = f"'2026-07-{random.randint(1,28):02d} {random.randint(0,23):02d}:{random.randint(0,59):02d}:00'"
                col_enum = f"'{random.choice(enum_values)}'"
                col_json = f"'{{\"id\":{i},\"value\":{random.randint(1,100)}}}'"

                values = f"({col_int},{col_bigint},{col_float},{col_double},{col_decimal}," \
                         f"'{col_varchar}','{col_text}',{col_date},{col_datetime},{col_enum},{col_json})"
                values_list.append(values)

            f.write(f"INSERT INTO test_table (col_int,col_bigint,col_float,col_double,col_decimal," \
                   f"col_varchar,col_text,col_date,col_datetime,col_enum,col_json) " \
                   f"VALUES {','.join(values_list)};\n")

        f.write("COMMIT;\n")

def analyze_csv_quality(output_dir):
    """分析恢复数据的质量"""
    import csv

    total_rows = 0
    valid_decimal = 0
    valid_enum = 0
    valid_json = 0
    valid_varchar = 0
    test_data_rows = 0

    for csv_file in output_dir.glob("*.csv"):
        try:
            with open(csv_file, 'r', errors='replace') as f:
                reader = csv.reader(f)
                header = next(reader, None)
                if not header:
                    continue

                # 查找列索引
                col_idx = {}
                for i, col in enumerate(header):
                    col_idx[col] = i

                for row in reader:
                    if not row or len(row) < 3:
                        continue
                    total_rows += 1

                    # 检查DECIMAL
                    if 'col_decimal' in col_idx and col_idx['col_decimal'] < len(row):
                        try:
                            val = row[col_idx['col_decimal']]
                            if val and val != 'NULL':
                                float(val)
                                valid_decimal += 1
                        except:
                            pass

                    # 检查ENUM
                    if 'col_enum' in col_idx and col_idx['col_enum'] < len(row):
                        val = row[col_idx['col_enum']]
                        if val in ['active', 'inactive', 'pending']:
                            valid_enum += 1

                    # 检查JSON
                    if 'col_json' in col_idx and col_idx['col_json'] < len(row):
                        val = row[col_idx['col_json']]
                        if val and val != 'NULL' and ('{' in val or 'id' in val):
                            valid_json += 1

                    # 检查VARCHAR (测试数据模式)
                    if 'col_varchar' in col_idx and col_idx['col_varchar'] < len(row):
                        val = row[col_idx['col_varchar']]
                        if val and 'name_' in val:
                            valid_varchar += 1
                            test_data_rows += 1
        except:
            pass

    return {
        'total_rows': total_rows,
        'valid_decimal': valid_decimal,
        'valid_enum': valid_enum,
        'valid_json': valid_json,
        'valid_varchar': valid_varchar,
        'test_data_rows': test_data_rows
    }

def run_test(version, scenario, fs_type, sql_file):
    container_name = f"test_{version}_{scenario}"

    if not setup_filesystem(fs_type):
        return {"error": "文件系统设置失败"}

    log(f"\n{'='*50}")
    log(f"测试: {version} | {scenario} | {fs_type}")
    log(f"{'='*50}")

    # 启动容器
    run_cmd(["podman", "rm", "-f", container_name])
    cmd = ["podman", "run", "-d", "--name", container_name,
           "-e", "MYSQL_ROOT_PASSWORD=root123",
           "-e", "MYSQL_DATABASE=testdb",
           "-v", f"{MOUNT_POINT}:/var/lib/mysql",
           CONTAINERS[version]["image"]]
    success, _, stderr = run_cmd(cmd, timeout=120)
    if not success:
        log(f"容器启动失败: {stderr}", "ERROR")
        return {"error": "容器启动失败"}

    # 等待MySQL就绪
    log("等待MySQL就绪...")
    for i in range(90):
        success, stdout, _ = run_cmd(["podman", "exec", container_name,
                                       "mysqladmin", "-uroot", "-proot123", "ping"])
        if success and "alive" in stdout.lower():
            log(f"MySQL就绪 ({i+1}秒)")
            break
        time.sleep(1)
    else:
        return {"error": "MySQL启动超时"}

    # 创建表
    run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             "CREATE DATABASE IF NOT EXISTS testdb"])
    run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             CREATE_TABLE_SQL])

    # 插入数据
    log(f"插入 {NUM_ROWS} 行数据...")
    start_time = time.time()
    with open(sql_file, 'r') as f:
        proc = subprocess.Popen(
            ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123", "testdb"],
            stdin=f, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        proc.communicate(timeout=600)
    log(f"数据插入完成，耗时: {time.time()-start_time:.1f}秒")

    # 刷盘
    flush_and_sync(container_name)

    # 获取实际行数
    success, stdout, _ = run_cmd(["podman", "exec", container_name,
                                   "mysql", "-uroot", "-proot123", "-N", "-e",
                                   "SELECT COUNT(*) FROM testdb.test_table"])
    actual_rows = int(stdout.strip()) if success and stdout.strip().isdigit() else 0
    log(f"表中实际行数: {actual_rows}")

    if actual_rows == 0:
        return {"error": "数据插入失败"}

    # 执行灾难场景
    if scenario == "DROP":
        log("执行 DROP TABLE...")
        run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
                 "DROP TABLE testdb.test_table"])

    # 停止容器
    run_cmd(["podman", "stop", container_name])
    run_cmd(["podman", "rm", container_name])

    # 卸载文件系统
    run_cmd(["umount", str(MOUNT_POINT)])

    # 磁盘雕刻恢复
    output_dir = TEST_DIR / version / fs_type / scenario
    output_dir.mkdir(parents=True, exist_ok=True)

    log(f"开始磁盘雕刻恢复...")
    cmd = [str(RECOVERY_TOOL),
           "--disk-carving",
           "--device-path", BLOCK_DEVICE,
           "--output", str(output_dir),
           "--filesystem", fs_type,
           "--schema", SCHEMA_PARAM,
           "--num-workers", "8"]

    start_time = time.time()
    success, stdout, stderr = run_cmd(cmd, timeout=1800)
    elapsed = time.time() - start_time

    if not success:
        log(f"恢复失败: {stderr[:300]}", "ERROR")
        return {"error": f"恢复失败", "elapsed": elapsed}

    log(f"恢复完成，耗时: {elapsed:.1f}秒")

    # 分析数据质量
    quality = analyze_csv_quality(output_dir)

    log(f"恢复统计:")
    log(f"  总行数: {quality['total_rows']}")
    log(f"  有效DECIMAL: {quality['valid_decimal']}")
    log(f"  有效ENUM: {quality['valid_enum']}")
    log(f"  有效JSON: {quality['valid_json']}")
    log(f"  有效VARCHAR: {quality['valid_varchar']}")
    log(f"  测试数据行: {quality['test_data_rows']}")

    # 计算恢复率
    rate = (quality['test_data_rows'] / actual_rows * 100) if actual_rows > 0 else 0
    decimal_rate = (quality['valid_decimal'] / quality['total_rows'] * 100) if quality['total_rows'] > 0 else 0
    enum_rate = (quality['valid_enum'] / quality['total_rows'] * 100) if quality['total_rows'] > 0 else 0

    status = "PASS" if rate >= 95 else "WARN" if rate >= 80 else "FAIL"

    log(f"[{version}] [{scenario}] [{fs_type}] 恢复率: {rate:.2f}%", status)
    log(f"  DECIMAL质量: {decimal_rate:.1f}%, ENUM质量: {enum_rate:.1f}%")

    return {
        "version": version,
        "scenario": scenario,
        "filesystem": fs_type,
        "expected": actual_rows,
        "recovered": quality['test_data_rows'],
        "total_rows": quality['total_rows'],
        "rate": round(rate, 2),
        "decimal_rate": round(decimal_rate, 2),
        "enum_rate": round(enum_rate, 2),
        "status": status,
        "elapsed_seconds": round(elapsed, 1)
    }

def main():
    log("=" * 60)
    log("快速验证测试 - 100K数据量")
    log(f"验证DECIMAL/ENUM/JSON字段解析修复效果")
    log("=" * 60)

    TEST_DIR.mkdir(parents=True, exist_ok=True)
    sql_file = TEST_DIR / "insert_data.sql"
    generate_sql_file(NUM_ROWS, sql_file)

    results = []

    # 测试配置
    for fs_type in ["ext4"]:
        for version in ["mysql80"]:
            for scenario in ["DROP"]:
                result = run_test(version, scenario, fs_type, sql_file)
                results.append(result)
                time.sleep(2)

    # 汇总
    log("\n" + "=" * 60)
    log("测试汇总")
    log("=" * 60)

    for r in results:
        if "error" in r:
            log(f"  ERROR: {r['error']}", "ERROR")
        else:
            color = "\033[92m" if r['status'] == "PASS" else "\033[93m" if r['status'] == "WARN" else "\033[91m"
            log(f"  {r['version']} | {r['scenario']} | {r['filesystem']} | {color}{r['rate']:.2f}%\033[0m ({r['recovered']}/{r['expected']})")
            log(f"    DECIMAL: {r['decimal_rate']:.1f}%, ENUM: {r['enum_rate']:.1f}%")

    with open(TEST_DIR / "results.json", 'w') as f:
        json.dump(results, f, indent=2)

    return all(r.get('status') != 'FAIL' for r in results if 'error' not in r)

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)