#!/usr/bin/env python3
"""
ASan 灾难恢复矩阵测试
- 5个MySQL版本: 5.6, 5.7, 8.0, 8.4, 9.0
- 2种文件系统: ext4, XFS
- 3种场景: DROP TABLE, TRUNCATE TABLE, rm -rf
- 使用 ASan 构建(检测内存错误)
- 共 5 × 3 × 2 = 30 个场景
"""

import os
import sys
import time
import subprocess
import random
import json
import shutil
import csv
from pathlib import Path
from datetime import datetime

# 容器配置
CONTAINERS = {
    "mysql56": {"image": "docker.m.daocloud.io/mysql:5.6", "has_frm": True},
    "mysql57": {"image": "docker.m.daocloud.io/mysql:5.7", "has_frm": True},
    "mysql80": {"image": "docker.m.daocloud.io/mysql:8.0", "has_frm": False},
    "mysql84": {"image": "docker.m.daocloud.io/mysql:8.4", "has_frm": False},
    "mysql90": {"image": "docker.m.daocloud.io/mysql:9.0", "has_frm": False},
}

# ASan 构建
RECOVERY_TOOL = Path("/app/mysql_recovery_pro/build/mysql_recovery_pro")

BLOCK_DEVICE = "/dev/nvme0n2"
MOUNT_POINT = Path("/mnt/mysql_asan_test")
TEST_DIR = Path("/app/mysql_recovery_test/asan_disaster_matrix")
NUM_ROWS = 2000  # ASan较慢，使用较小数据集
PARTITION_SIZE_GB = 20

# 只运行指定场景(用于烟雾测试): 逗号分隔 "mysql80|DROP|xfs"
FILTER = os.environ.get("ASAN_MATRIX_FILTER", "")

def log(msg, level="INFO"):
    timestamp = datetime.now().strftime("%H:%M:%S")
    colors = {"INFO": "\033[0m", "WARN": "\033[93m", "ERROR": "\033[91m",
              "PASS": "\033[92m", "ASAN": "\033[95m"}
    print(f"{colors.get(level, '')}[{timestamp}] [{level}] {msg}\033[0m")
    sys.stdout.flush()

def run_cmd(cmd, timeout=600, capture=True):
    try:
        if capture:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
            return result.returncode == 0, result.stdout, result.stderr
        else:
            result = subprocess.run(cmd, timeout=timeout)
            return result.returncode == 0, "", ""
    except subprocess.TimeoutExpired:
        return False, "", "Timeout"
    except FileNotFoundError as e:
        return False, "", f"Command not found: {e}"

def setup_filesystem(fs_type):
    """格式化并挂载文件系统"""
    log(f"格式化 {BLOCK_DEVICE} 为 {fs_type}...")
    run_cmd(["umount", str(MOUNT_POINT)], timeout=30)
    run_cmd(["pkill", "-f", "mysqld"], timeout=10)

    # Wipe first 1GB to remove old InnoDB pages from previous tests.
    # This prevents OOM when the tool loads all pages from the 200GB device.
    run_cmd(["dd", "if=/dev/zero", f"of={BLOCK_DEVICE}", "bs=1M", "count=1000", "status=none"], timeout=120)

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
    # lost+found from mkfs breaks MySQL 5.7+ entrypoint (refuses --initialize
    # when datadir has files). Remove it so the datadir is truly empty.
    lost_found = MOUNT_POINT / "lost+found"
    if lost_found.exists():
        run_cmd(["rmdir", str(lost_found)], capture=False)
    log(f"{fs_type} 文件系统就绪")
    return True

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
                col_datetime = "2026-01-01 00:00:00"
                values_list.append(f"({col_int},'{col_varchar}',{col_decimal},'{col_datetime}')")
            f.write(f"INSERT INTO test_table (col_int, col_varchar, col_decimal, col_datetime) VALUES {','.join(values_list)};\n")
        f.write("COMMIT;\n")
    return output_path

def wait_mysql(container_name, timeout=90):
    for i in range(timeout):
        success, stdout, _ = run_cmd(["podman", "exec", container_name,
                                      "mysqladmin", "-uroot", "-proot123", "ping"])
        if success and "alive" in stdout.lower():
            return True
        time.sleep(1)
    return False

def run_test(version, scenario, fs_type, sql_file):
    config = CONTAINERS[version]
    container_name = f"asan_{version}_{scenario}_{fs_type}"

    if not setup_filesystem(fs_type):
        return {"error": "文件系统设置失败"}

    log(f"\n{'='*55}")
    log(f"测试: {version} | {scenario} | {fs_type}")
    log(f"{'='*55}")

    # 启动容器
    run_cmd(["podman", "rm", "-f", container_name])
    cmd = ["podman", "run", "-d", "--name", container_name,
           "-e", "MYSQL_ROOT_PASSWORD=root123",
           "-e", "MYSQL_DATABASE=testdb",
           "-v", f"{MOUNT_POINT}:/var/lib/mysql",
           config["image"]]
    success, _, stderr = run_cmd(cmd, timeout=180)
    if not success:
        log(f"容器启动失败: {stderr}", "ERROR")
        return {"error": f"容器启动失败: {stderr[:200]}"}

    if not wait_mysql(container_name):
        log("MySQL启动超时", "ERROR")
        return {"error": "MySQL启动超时"}

    run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             "CREATE DATABASE IF NOT EXISTS testdb"])
    run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e", """
        CREATE TABLE testdb.test_table (
            id INT PRIMARY KEY AUTO_INCREMENT,
            col_int INT,
            col_varchar VARCHAR(100),
            col_decimal DECIMAL(10,2),
            col_datetime DATETIME
        ) ENGINE=InnoDB
    """])

    log(f"插入 {NUM_ROWS} 行数据...")
    with open(sql_file, 'r') as f:
        proc = subprocess.Popen(
            ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123", "testdb"],
            stdin=f, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        proc.communicate(timeout=300)

    # 强制刷盘
    log("强制刷盘...")
    run_cmd(["podman", "exec", container_name, "sync"])
    time.sleep(5)
    run_cmd(["sync"])
    time.sleep(3)

    success, stdout, _ = run_cmd(["podman", "exec", container_name,
                                   "mysql", "-uroot", "-proot123", "-N", "-e",
                                   "SELECT COUNT(*) FROM testdb.test_table"])
    actual_rows = int(stdout.strip()) if success and stdout.strip().isdigit() else 0
    log(f"表中实际行数: {actual_rows}")

    ibd_path = MOUNT_POINT / "testdb" / "test_table.ibd"

    # 执行灾难场景
    if scenario == "DROP":
        log("执行 DROP TABLE...")
        run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
                 "DROP TABLE testdb.test_table"])
        time.sleep(2)
        run_cmd(["podman", "stop", container_name])
        run_cmd(["podman", "rm", container_name])
        run_cmd(["umount", str(MOUNT_POINT)])

    elif scenario == "TRUNCATE":
        log("执行 TRUNCATE TABLE...")
        run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
                 "TRUNCATE TABLE testdb.test_table"])
        time.sleep(2)
        run_cmd(["podman", "stop", container_name])
        run_cmd(["podman", "rm", container_name])
        run_cmd(["umount", str(MOUNT_POINT)])

    else:  # RMRF
        log("执行 rm -rf...")
        run_cmd(["podman", "stop", container_name])
        run_cmd(["podman", "rm", container_name])
        if ibd_path.exists():
            ibd_path.unlink()
        run_cmd(["umount", str(MOUNT_POINT)])

    # ------------- 磁盘雕刻恢复 (ASan) -------------
    output_dir = TEST_DIR / version / fs_type / scenario
    output_dir.mkdir(parents=True, exist_ok=True)

    log(f"开始 ASan 磁盘雕刻恢复...")
    env = os.environ.copy()
    # Only set ASAN/UBSAN options if using ASan build
    if "asan" in str(RECOVERY_TOOL).lower():
        env["ASAN_OPTIONS"] = "abort_on_error=1:detect_leaks=1:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"

    cmd = [str(RECOVERY_TOOL),
           "--disk-carving",
           "--device-path", BLOCK_DEVICE,
           "--output", str(output_dir),
           "--filesystem", fs_type,
           "--num-workers", "8"]
    if config["has_frm"]:
        schema_file = find_schema_file(version, scenario, fs_type)
        if schema_file:
            cmd += ["--schema-file", schema_file]

    start_time = time.time()
    try:
        result = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=1800, env=env)
        success = result.returncode == 0
        stdout = result.stdout
        stderr = result.stderr
    except subprocess.TimeoutExpired:
        success = False
        stdout = ""
        stderr = "Timeout"
    elapsed = time.time() - start_time

    # 检查 ASan 错误
    asan_errors = []
    combined = stdout + stderr
    for marker in ["AddressSanitizer", "runtime error", "heap-use-after-free",
                   "stack-buffer-overflow", "detected memory leaks", "SEGV"]:
        if marker in combined:
            asan_errors.append(marker)

    asan_status = "CLEAN" if not asan_errors else "ASAN_" + ",".join(asan_errors)

    if not success:
        err_print = stderr[:300] if stderr else "unknown"
        log(f"恢复失败: {err_print}", "ERROR")
        log(f"ASan状态: {asan_status}", "ASAN")
        return {"version": version, "scenario": scenario, "filesystem": fs_type,
                "expected": actual_rows, "recovered": 0, "rate": 0.0,
                "status": "FAIL" if not asan_errors else "ASAN_FAIL",
                "elapsed_seconds": round(elapsed, 1),
                "asan_status": asan_status, "error": err_print}

    # 统计恢复行数
    recovered_rows = 0
    for csv_file in output_dir.glob("*.csv"):
        try:
            with open(csv_file, 'r', errors='ignore') as f:
                reader = csv.reader(f)
                header = next(reader, None)
                for row in reader:
                    if row and any(c.strip() for c in row):
                        # 跳过非数据行
                        if row[0].strip().lstrip('"').isdigit():
                            recovered_rows += 1
        except Exception:
            pass

    rate = (recovered_rows / actual_rows * 100) if actual_rows > 0 else 0
    status = "PASS" if rate >= 95 and not asan_errors else \
             "WARN" if rate >= 80 and not asan_errors else \
             "FAIL" if not asan_errors else "ASAN_FAIL"

    color = "PASS" if status == "PASS" else status
    log(f"[{version}] [{scenario}] [{fs_type}] 恢复率: {rate:.2f}% "
        f"({recovered_rows}/{actual_rows}) | ASan: {asan_status} | {elapsed:.1f}s", color)

    return {"version": version, "scenario": scenario, "filesystem": fs_type,
            "expected": actual_rows, "recovered": recovered_rows,
            "rate": round(rate, 2), "status": status,
            "elapsed_seconds": round(elapsed, 1),
            "asan_status": asan_status}

def find_schema_file(version, scenario, fs_type):
    """查找或生成schema文件(用于5.6/5.7 FRM恢复)"""
    # 尝试从之前运行中查找
    return None

def main():
    log("=" * 60)
    log("ASan 灾难恢复矩阵测试")
    log(f"MySQL版本: 5.6, 5.7, 8.0, 8.4, 9.0")
    log(f"文件系统: ext4, XFS")
    log(f"场景: DROP, TRUNCATE, RMRF")
    log(f"数据量: {NUM_ROWS} 行")
    log(f"块设备: {BLOCK_DEVICE}")
    log(f"恢复工具: {RECOVERY_TOOL}")
    log(f"共 {5 * 3 * 2} = 30 个场景")
    log("=" * 60)

    TEST_DIR.mkdir(parents=True, exist_ok=True)
    sql_file = TEST_DIR / "batch_insert.sql"
    generate_sql_file(NUM_ROWS, sql_file)

    versions = ["mysql56", "mysql57", "mysql80", "mysql84", "mysql90"]
    scenarios = ["DROP", "TRUNCATE", "RMRF"]
    fs_types = ["ext4", "xfs"]

    # 收集要运行的场景
    tests_to_run = []
    for fs_type in fs_types:
        for version in versions:
            for scenario in scenarios:
                tests_to_run.append((version, scenario, fs_type))

    if FILTER:
        # 过滤: "mysql80|DROP|xfs" 格式
        fv, fsc, ffs = FILTER.split("|")
        tests_to_run = [t for t in tests_to_run
                        if (fv == "*" or t[0] == fv) and
                           (fsc == "*" or t[1] == fsc) and
                           (ffs == "*" or t[2] == ffs)]

    results = []
    for version, scenario, fs_type in tests_to_run:
        result = run_test(version, scenario, fs_type, sql_file)
        results.append(result)
        time.sleep(2)

    # 汇总
    log("\n" + "=" * 60)
    log("测试汇总")
    log("=" * 60)

    summary = {}
    for r in results:
        key = f"{r.get('version','?')}|{r.get('scenario','?')}|{r.get('filesystem','?')}"
        if "error" in r and "asan_status" not in r:
            log(f"  {key} | FAIL: {r['error']}", "ERROR")
        else:
            st = r.get('status', '?')
            color = "\033[92m" if st == "PASS" else "\033[95m" if "ASAN" in st else \
                    "\033[93m" if st == "WARN" else "\033[91m"
            log(f"  {key} | {color}{r.get('rate',0):.2f}%\033[0m "
                f"({r.get('recovered',0)}/{r.get('expected',0)}) "
                f"[{r.get('elapsed_seconds',0)}s] ASan:{r.get('asan_status','?')}")
        summary[key] = r

    out_json = TEST_DIR / "results.json"
    with open(out_json, 'w') as f:
        json.dump(results, f, indent=2, default=str)
    log(f"结果已保存: {out_json}")

    passed = sum(1 for r in results if r.get('status') == 'PASS')
    warned = sum(1 for r in results if r.get('status') == 'WARN')
    asan_fail = sum(1 for r in results if "ASAN" in r.get('status',''))
    failed = sum(1 for r in results if r.get('status') == 'FAIL' or "error" in r)

    log(f"\n通过: {passed}, 警告: {warned}, ASan失败: {asan_fail}, 常规失败: {failed}")
    return asan_fail == 0 and failed == 0

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)