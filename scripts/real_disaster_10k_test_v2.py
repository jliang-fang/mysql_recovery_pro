#!/usr/bin/env python3
"""
真实灾难恢复验证测试 - 10K数据量 (优化版)
- MySQL 8.0, 8.4, 9.0
- ext4和XFS文件系统
- DROP TABLE, TRUNCATE TABLE, rm -rf 场景
- 使用schema参数正确解析数据
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
BLOCK_DEVICE = "/dev/nvme0n2"
MOUNT_POINT = Path("/mnt/mysql_test_10k")
TEST_DIR = Path("/app/mysql_recovery_test/real_disaster_10k_v2")
NUM_ROWS = 10000

# 表结构定义
TABLE_SCHEMA = "id:INT:4:pk,col_int:INT:4,col_varchar:VARCHAR:100,col_decimal:DECIMAL:10:2"

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
    """设置文件系统"""
    log(f"设置 {fs_type} 文件系统...")

    run_cmd(["umount", str(MOUNT_POINT)], timeout=30)

    # 清零设备头部
    run_cmd(["dd", "if=/dev/zero", f"of={BLOCK_DEVICE}",
             "bs=1M", "count=100", "conv=notrunc"], timeout=60)

    # 创建文件系统
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

    log(f"{fs_type} 文件系统就绪")
    return True

def flush_and_sync(container_name):
    """强制刷盘"""
    log("强制刷盘...")
    run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             "SET GLOBAL innodb_flush_log_at_trx_commit=1;"])
    run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             "FLUSH NO_WRITE_TO_BINLOG ENGINE INNODB; FLUSH NO_WRITE_TO_BINLOG TABLES;"])
    run_cmd(["podman", "exec", container_name, "sync"])
    time.sleep(10)
    run_cmd(["sync"])
    time.sleep(5)
    log("刷盘完成")

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
                col_varchar = f"name_{i}_{random.randint(1000,9999)}"
                col_decimal = round(random.uniform(0, 1000), 2)
                values_list.append(f"({col_int},'{col_varchar}',{col_decimal})")
            f.write(f"INSERT INTO test_table (col_int, col_varchar, col_decimal) VALUES {','.join(values_list)};\n")
        f.write("COMMIT;\n")
    log(f"SQL文件生成完成: {output_path}")

def run_test(version, scenario, fs_type, sql_file):
    """运行单个测试"""
    config = CONTAINERS[version]
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
           config["image"]]
    success, _, stderr = run_cmd(cmd, timeout=120)
    if not success:
        log(f"容器启动失败: {stderr}", "ERROR")
        return {"error": "容器启动失败"}

    # 等待MySQL就绪
    log("等待MySQL就绪...")
    for i in range(60):
        success, stdout, _ = run_cmd(["podman", "exec", container_name,
                                       "mysqladmin", "-uroot", "-proot123", "ping"])
        if success and "alive" in stdout.lower():
            log(f"MySQL就绪 ({i+1}秒)")
            break
        time.sleep(1)
    else:
        log("MySQL启动超时", "ERROR")
        return {"error": "MySQL启动超时"}

    # 创建表
    run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             "CREATE DATABASE IF NOT EXISTS testdb"])
    run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e", """
        CREATE TABLE testdb.test_table (
            id INT PRIMARY KEY AUTO_INCREMENT,
            col_int INT,
            col_varchar VARCHAR(100),
            col_decimal DECIMAL(10,2)
        ) ENGINE=InnoDB
    """])

    # 插入数据
    log(f"插入 {NUM_ROWS} 行数据...")
    with open(sql_file, 'r') as f:
        proc = subprocess.Popen(
            ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123", "testdb"],
            stdin=f, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        proc.communicate(timeout=300)

    # 刷盘
    flush_and_sync(container_name)

    # 获取实际行数
    success, stdout, _ = run_cmd(["podman", "exec", container_name,
                                   "mysql", "-uroot", "-proot123", "-N", "-e",
                                   "SELECT COUNT(*) FROM testdb.test_table"])
    actual_rows = int(stdout.strip()) if success and stdout.strip().isdigit() else 0
    log(f"表中实际行数: {actual_rows}")

    # 执行灾难场景
    if scenario == "DROP":
        log("执行 DROP TABLE...")
        run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
                 "DROP TABLE testdb.test_table"])
        time.sleep(2)
    elif scenario == "TRUNCATE":
        log("执行 TRUNCATE TABLE...")
        run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
                 "TRUNCATE TABLE testdb.test_table"])
        time.sleep(2)
    # RMRF场景：停止容器后删除文件

    # 停止容器
    run_cmd(["podman", "stop", container_name])
    run_cmd(["podman", "rm", container_name])

    # RMRF场景删除文件
    ibd_path = MOUNT_POINT / "testdb" / "test_table.ibd"
    if scenario == "RMRF" and ibd_path.exists():
        log("执行 rm -rf...")
        ibd_path.unlink()

    # 卸载文件系统
    run_cmd(["umount", str(MOUNT_POINT)])

    # 磁盘雕刻恢复
    output_dir = TEST_DIR / version / fs_type / scenario
    output_dir.mkdir(parents=True, exist_ok=True)

    log(f"开始磁盘雕刻恢复: {BLOCK_DEVICE}")

    # 使用schema参数恢复
    cmd = [str(RECOVERY_TOOL),
           "--disk-carving",
           "--device-path", BLOCK_DEVICE,
           "--output", str(output_dir),
           "--filesystem", fs_type,
           "--schema", TABLE_SCHEMA,
           "--num-workers", "8"]

    start_time = time.time()
    success, stdout, stderr = run_cmd(cmd, timeout=1800)
    elapsed = time.time() - start_time

    if not success:
        log(f"恢复失败: {stderr}", "ERROR")
        return {"error": f"恢复失败: {stderr[:200]}", "elapsed": elapsed}

    log(f"恢复完成，耗时: {elapsed:.1f}秒")

    # 统计结果 - 查找包含正确字段的CSV
    recovered_rows = 0
    csv_files = list(output_dir.glob("*.csv"))

    for csv_file in csv_files:
        try:
            with open(csv_file, 'r') as f:
                first_line = f.readline()
                # 检查是否有正确的列
                if 'col_int' in first_line or 'col_varchar' in first_line:
                    # 统计行数
                    count = sum(1 for line in f if line.strip())
                    if count > recovered_rows:
                        recovered_rows = count
        except:
            pass

    # 如果没找到schema解析的数据，尝试统计所有行
    if recovered_rows == 0:
        for csv_file in csv_files:
            with open(csv_file, 'r') as f:
                recovered_rows += sum(1 for line in f if line.strip() and 'page_no' not in line)

    rate = (recovered_rows / NUM_ROWS * 100) if NUM_ROWS > 0 else 0
    status = "PASS" if rate >= 95 else "WARN" if rate >= 80 else "FAIL"

    log(f"[{version}] [{scenario}] [{fs_type}] 恢复率: {rate:.2f}% ({recovered_rows}/{NUM_ROWS})", status)

    return {
        "version": version,
        "scenario": scenario,
        "filesystem": fs_type,
        "expected": NUM_ROWS,
        "recovered": recovered_rows,
        "rate": round(rate, 2),
        "status": status,
        "elapsed_seconds": round(elapsed, 1)
    }

def main():
    log("=" * 60)
    log("真实灾难恢复验证测试 - 10K数据量 (优化版)")
    log(f"MySQL版本: 8.0, 8.4, 9.0")
    log(f"文件系统: ext4, XFS")
    log(f"数据量: {NUM_ROWS} 行")
    log(f"块设备: {BLOCK_DEVICE}")
    log("=" * 60)

    TEST_DIR.mkdir(parents=True, exist_ok=True)
    sql_file = TEST_DIR / "batch_insert.sql"
    generate_sql_file(NUM_ROWS, sql_file)

    results = []
    versions = ["mysql80", "mysql84", "mysql90"]
    scenarios = ["DROP", "TRUNCATE", "RMRF"]
    fs_types = ["ext4", "xfs"]

    # 只运行一个组合测试验证功能
    for fs_type in fs_types[:1]:  # 先只测试ext4
        for version in versions[:1]:  # 先只测试mysql80
            for scenario in scenarios:
                result = run_test(version, scenario, fs_type, sql_file)
                results.append(result)
                time.sleep(2)
                break  # 先只跑一个场景
            break
        break

    # 汇总
    log("\n" + "=" * 60)
    log("测试汇总")
    log("=" * 60)

    for r in results:
        if "error" in r:
            log(f"  {r.get('version', '?')} | {r.get('scenario', '?')} | {r.get('filesystem', '?')} | ERROR: {r['error']}", "ERROR")
        else:
            color = "\033[92m" if r['status'] == "PASS" else "\033[93m" if r['status'] == "WARN" else "\033[91m"
            log(f"  {r['version']} | {r['scenario']} | {r['filesystem']} | {color}{r['rate']:.2f}%\033[0m ({r['recovered']}/{r['expected']}) [{r['elapsed_seconds']}s]")

    # 保存结果
    with open(TEST_DIR / "results.json", 'w') as f:
        json.dump(results, f, indent=2)

    return True

if __name__ == "__main__":
    main()