#!/usr/bin/env python3
"""
10K 灾难恢复验证测试 - 简化版
- MySQL 8.0, 8.4, 9.0
- 10K行全字段表
- DROP TABLE, TRUNCATE TABLE, rm -rf 三种灾难场景
"""

import os
import sys
import time
import subprocess
import random
import json
import shutil
from pathlib import Path
from datetime import datetime

# 容器配置
CONTAINERS = {
    "mysql80": {"image": "docker.m.daocloud.io/mysql:8.0"},
    "mysql84": {"image": "docker.m.daocloud.io/mysql:8.4"},
    "mysql90": {"image": "docker.m.daocloud.io/mysql:9.0"},
}

RECOVERY_TOOL = Path("/app/mysql_recovery_pro/build/mysql_recovery_pro")
TEST_DIR = Path("/app/mysql_recovery_test/validation_10k_v2")
NUM_ROWS = 10000

def log(msg, level="INFO"):
    timestamp = datetime.now().strftime("%H:%M:%S")
    colors = {"INFO": "\033[0m", "WARN": "\033[93m", "ERROR": "\033[91m", "PASS": "\033[92m"}
    print(f"{colors.get(level, '')}[{timestamp}] [{level}] {msg}\033[0m")
    sys.stdout.flush()

def run_cmd(cmd, timeout=300):
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return False, "", "Timeout"

def execute_sql(container_name, sql):
    cmd = ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123", "-e", sql]
    success, stdout, stderr = run_cmd(cmd)
    return success, stdout + stderr

def generate_sql_file(num_rows, output_path):
    log(f"生成SQL文件: {num_rows} 行...")
    with open(output_path, 'w') as f:
        f.write("SET autocommit=0;\nSET unique_checks=0;\nSET foreign_key_checks=0;\n")
        batch_size = 500
        for batch_start in range(0, num_rows, batch_size):
            batch_end = min(batch_start + batch_size, num_rows)
            values_list = []
            for i in range(batch_start, batch_end):
                col_int = random.randint(0, 1000000)
                col_varchar = f"name_{i}"
                col_decimal = round(random.uniform(0, 1000), 2)
                values_list.append(f"({col_int},'{col_varchar}',{col_decimal})")
            f.write(f"INSERT INTO test_table (col_int, col_varchar, col_decimal) VALUES {','.join(values_list)};\n")
        f.write("COMMIT;\n")
    log(f"SQL文件生成完成: {output_path}")

def run_test(version, scenario, sql_file):
    """运行单个测试"""
    config = CONTAINERS[version]
    container_name = f"test_{version}_{scenario}"
    volume_path = TEST_DIR / version / scenario

    # 清理
    run_cmd(["podman", "rm", "-f", container_name])
    if volume_path.exists():
        shutil.rmtree(volume_path)
    volume_path.mkdir(parents=True, exist_ok=True)

    log(f"\n{'='*50}")
    log(f"测试: {version} | {scenario}")
    log(f"{'='*50}")

    # 启动容器
    cmd = ["podman", "run", "-d", "--name", container_name,
           "-e", "MYSQL_ROOT_PASSWORD=root123",
           "-e", "MYSQL_DATABASE=testdb",
           "-v", f"{volume_path}:/var/lib/mysql",
           config["image"]]
    success, _, stderr = run_cmd(cmd, timeout=120)
    if not success:
        log(f"容器启动失败: {stderr}", "ERROR")
        return {"error": "容器启动失败"}

    log("等待MySQL就绪...")
    for i in range(60):
        success, _ = execute_sql(container_name, "SELECT 1")
        if success:
            log(f"MySQL就绪 ({i+1}秒)")
            break
        time.sleep(1)
    else:
        log("MySQL启动超时", "ERROR")
        return {"error": "MySQL启动超时"}

    # 创建表
    execute_sql(container_name, "CREATE DATABASE IF NOT EXISTS testdb")
    execute_sql(container_name, """
        CREATE TABLE testdb.test_table (
            id INT PRIMARY KEY AUTO_INCREMENT,
            col_int INT,
            col_varchar VARCHAR(100),
            col_decimal DECIMAL(10,2)
        ) ENGINE=InnoDB
    """)

    # 插入数据
    log(f"插入 {NUM_ROWS} 行数据...")
    with open(sql_file, 'r') as f:
        proc = subprocess.Popen(
            ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123", "testdb"],
            stdin=f, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        proc.communicate(timeout=300)

    # 强制刷盘
    execute_sql(container_name, "FLUSH TABLES testdb.test_table")
    time.sleep(2)

    # 获取IBD路径
    ibd_path = volume_path / "testdb" / "test_table.ibd"

    # 执行灾难场景
    if scenario == "DROP":
        execute_sql(container_name, "DROP TABLE testdb.test_table")
        time.sleep(1)
        run_cmd(["podman", "stop", container_name])
        run_cmd(["podman", "rm", container_name])
        # DROP恢复需要扫描整个目录
        recovery_input = str(volume_path / "testdb")
    elif scenario == "TRUNCATE":
        execute_sql(container_name, "TRUNCATE TABLE testdb.test_table")
        time.sleep(1)
        run_cmd(["podman", "stop", container_name])
        run_cmd(["podman", "rm", container_name])
        recovery_input = str(ibd_path)
    else:  # RMRF
        run_cmd(["podman", "stop", container_name])
        run_cmd(["podman", "rm", container_name])
        if ibd_path.exists():
            ibd_path.unlink()
        recovery_input = str(ibd_path)

    # 运行恢复 - 使用正确的恢复模式
    output_csv = volume_path / "recovered.csv"
    log(f"开始恢复: {recovery_input}")

    # 根据场景选择正确的恢复模式
    if scenario == "DROP":
        # DROP TABLE: 扫描目录中残留的IBD文件
        cmd = [str(RECOVERY_TOOL), "--scan-dir", recovery_input, "--output", str(output_csv)]
    elif scenario == "TRUNCATE":
        # TRUNCATE: 使用truncate-recovery模式
        cmd = [str(RECOVERY_TOOL), "--ibd", recovery_input, "--truncate-recovery", "--output", str(output_csv)]
    else:  # RMRF
        # RMRF: 使用rmrf-recovery模式
        cmd = [str(RECOVERY_TOOL), "--ibd", recovery_input, "--rmrf-recovery", "--output", str(output_csv)]

    success, stdout, stderr = run_cmd(cmd, timeout=300)

    if not success:
        log(f"恢复失败: {stderr}", "ERROR")
        return {"error": f"恢复失败: {stderr}"}

    # 统计结果
    recovered_rows = 0
    # 恢复工具可能创建目录，CSV文件在其中
    csv_path = output_csv
    if output_csv.is_dir():
        csv_path = output_csv / "recovered.csv"
    if csv_path.exists():
        with open(csv_path, 'r') as f:
            recovered_rows = sum(1 for _ in f) - 1

    rate = (recovered_rows / NUM_ROWS * 100) if NUM_ROWS > 0 else 0
    status = "PASS" if rate >= 95 else "WARN" if rate >= 80 else "FAIL"

    log(f"[{version}] [{scenario}] 恢复率: {rate:.2f}% ({recovered_rows}/{NUM_ROWS})", status)

    return {
        "version": version,
        "scenario": scenario,
        "expected": NUM_ROWS,
        "recovered": recovered_rows,
        "rate": round(rate, 2),
        "status": status
    }

def main():
    log("=" * 60)
    log("10K 灾难恢复验证测试")
    log(f"MySQL版本: 8.0, 8.4, 9.0")
    log(f"数据量: {NUM_ROWS} 行")
    log("=" * 60)

    TEST_DIR.mkdir(parents=True, exist_ok=True)
    sql_file = TEST_DIR / "batch_insert.sql"
    generate_sql_file(NUM_ROWS, sql_file)

    results = []
    versions = ["mysql80", "mysql84", "mysql90"]
    scenarios = ["DROP", "TRUNCATE", "RMRF"]

    for version in versions:
        for scenario in scenarios:
            result = run_test(version, scenario, sql_file)
            results.append(result)
            time.sleep(2)

    # 汇总
    log("\n" + "=" * 60)
    log("测试汇总")
    log("=" * 60)

    for r in results:
        if "error" in r:
            log(f"  {r.get('version', '?')} | {r.get('scenario', '?')} | ERROR: {r['error']}", "ERROR")
        else:
            color = "\033[92m" if r['status'] == "PASS" else "\033[93m" if r['status'] == "WARN" else "\033[91m"
            log(f"  {r['version']} | {r['scenario']} | {color}{r['rate']:.2f}%\033[0m ({r['recovered']}/{r['expected']})")

    # 保存结果
    with open(TEST_DIR / "results.json", 'w') as f:
        json.dump(results, f, indent=2)

    passed = sum(1 for r in results if r.get('status') == 'PASS')
    warned = sum(1 for r in results if r.get('status') == 'WARN')
    failed = sum(1 for r in results if 'error' in r or r.get('status') == 'FAIL')

    log(f"\n通过: {passed}, 警告: {warned}, 失败: {failed}")
    return failed == 0

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
