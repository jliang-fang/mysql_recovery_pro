#!/usr/bin/env python3
"""
真实灾难恢复验证测试
模拟真实灾难场景：先执行灾难操作，再尝试恢复
- 不使用备份文件
- 测试磁盘雕刻、TRUNCATE检测、DROP TABLE恢复
"""

import os
import sys
import time
import shutil
import subprocess
import json
from pathlib import Path
from datetime import datetime

# 容器配置
CONTAINERS = {
    "mysql56": {"name": "mysql56_test", "port": 3306, "has_frm": True},
    "mysql57": {"name": "mysql57_test", "port": 3307, "has_frm": True},
    "mysql80": {"name": "mysql80_test", "port": 3308, "has_frm": False},
    "mysql84": {"name": "mysql84_test", "port": 3309, "has_frm": False},
    "mysql90": {"name": "mysql90_test", "port": 3310, "has_frm": False},
}

RECOVERY_TOOL = Path("/app/mysql_recovery_pro/build/mysql_recovery_pro")
TEST_DIR = Path("/app/mysql_recovery_test/real_disaster_test")

def log(msg, level="INFO"):
    timestamp = datetime.now().strftime("%H:%M:%S")
    color = {"INFO": "\033[92m", "WARN": "\033[93m", "ERROR": "\033[91m", "PASS": "\033[92m", "FAIL": "\033[91m"}.get(level, "")
    reset = "\033[0m"
    print(f"{color}[{timestamp}] [{level}]{reset} {msg}")

def run_cmd(cmd, timeout=60, input_data=None):
    """执行命令"""
    try:
        result = subprocess.run(
            cmd,
            input=input_data,
            capture_output=True,
            text=True,
            timeout=timeout
        )
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return False, "", "Timeout"
    except Exception as e:
        return False, "", str(e)

def get_volume_path(container_name):
    """获取容器数据目录路径"""
    success, stdout, _ = run_cmd(
        ["podman", "inspect", container_name, "--format", "{{range .Mounts}}{{.Source}}{{end}}"]
    )
    if success and stdout.strip():
        return Path(stdout.strip())
    return None

def execute_mysql(container_name, sql, database="disaster_real"):
    """执行MySQL命令"""
    if database:
        cmd = ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123", database, "-e", sql]
    else:
        cmd = ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123", "-e", sql]
    success, stdout, stderr = run_cmd(cmd, timeout=120)
    return success, stdout + stderr

def create_test_table(container_name):
    """创建全字段类型测试表"""
    create_sql = """
    CREATE TABLE IF NOT EXISTS disaster_test_table (
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
        decimal_col DECIMAL(20,5),
        float_col FLOAT,
        double_col DOUBLE
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=DYNAMIC;
    """
    return execute_mysql(container_name, create_sql, "disaster_real")

def insert_test_data(container_name, num_rows=500):
    """插入测试数据"""
    log(f"插入 {num_rows} 行测试数据...")

    values_list = []
    for i in range(1, num_rows + 1):
        name = f"User_{i:06d}"
        age = 20 + (i % 60)
        height = round(1.5 + (i % 50) / 100, 2)
        birth_date = f"1990-{(i % 12) + 1:02d}-{(i % 28) + 1:02d}"
        created_at = f"2026-06-{(i % 28) + 1:02d} {(i % 24):02d}:00:00"
        json_col = json.dumps({"id": i, "name": name, "tags": [i % 100, (i+1) % 100, (i+2) % 100]})
        enum_col = ['active', 'inactive', 'pending'][i % 3]
        set_col = ['a', 'b', 'c', 'a,b', 'b,c', 'a,b,c'][i % 6]
        blob_hex = ''.join([f'{(i + j) % 256:02x}' for j in range(20)])
        text_col = f"Text content for record {i} with some random data"
        decimal_val = round(1000.0 + i * 10.5, 5)
        float_val = round(100.0 + i * 0.5, 4)
        double_val = round(1000.0 + i * 1.5, 8)

        values = f"('{name}',{age},{height},'{birth_date}','{created_at}','{json_col}','{enum_col}','{set_col}',UNHEX('{blob_hex}'),'{text_col}',{decimal_val},{float_val},{double_val})"
        values_list.append(values)

        if len(values_list) >= 50:
            insert_sql = f"INSERT INTO disaster_test_table (name,age,height,birth_date,created_at,json_col,enum_col,set_col,blob_col,text_col,decimal_col,float_col,double_col) VALUES {','.join(values_list)};"
            execute_mysql(container_name, insert_sql, "disaster_real")
            values_list = []

    if values_list:
        insert_sql = f"INSERT INTO disaster_test_table (name,age,height,birth_date,created_at,json_col,enum_col,set_col,blob_col,text_col,decimal_col,float_col,double_col) VALUES {','.join(values_list)};"
        execute_mysql(container_name, insert_sql, "disaster_real")

    # 强制flush数据到磁盘
    log("执行 FLUSH TABLES 强制写入磁盘...")
    execute_mysql(container_name, "FLUSH TABLES;", "disaster_real")
    time.sleep(2)

    # 验证插入
    success, output = execute_mysql(container_name, "SELECT COUNT(*) FROM disaster_test_table;", "disaster_real")
    if success:
        lines = [l.strip() for l in output.split('\n') if l.strip() and not l.startswith('mysql:')]
        if lines:
            try:
                count = int(lines[-1])
                log(f"实际插入行数: {count}")
                return count
            except:
                pass
    return 0

def test_rmrf_recovery(container_name, volume_path, expected_rows):
    """
    测试 rm -rf 灾难恢复
    流程：
    1. 停止容器
    2. 删除IBD文件
    3. 尝试从残留数据恢复（磁盘雕刻）
    """
    log("\n" + "="*60)
    log("场景: rm -rf 灾难恢复", "WARN")
    log("="*60)

    ibd_path = volume_path / "disaster_real" / "disaster_test_table.ibd"
    frm_path = volume_path / "disaster_real" / "disaster_test_table.frm"

    # 记录原始文件大小
    original_size = ibd_path.stat().st_size if ibd_path.exists() else 0
    log(f"原始文件大小: {original_size} bytes")

    # 1. 停止容器并删除文件
    log("步骤1: 停止容器...")
    run_cmd(["podman", "stop", container_name])
    time.sleep(3)

    log("步骤2: 删除IBD文件...")
    if ibd_path.exists():
        # 在删除前，尝试从内存/缓存中恢复（如果有的话）
        log(f"删除文件: {ibd_path}")
        ibd_path.unlink()
    if frm_path.exists():
        frm_path.unlink()

    # 2. 启动容器
    log("步骤3: 启动容器...")
    run_cmd(["podman", "start", container_name])
    time.sleep(10)

    # 3. 检查文件是否存在
    if ibd_path.exists():
        log(f"文件仍然存在（可能被重建）: {ibd_path}", "WARN")
    else:
        log("文件已被删除", "WARN")

    # 4. 尝试恢复 - 使用磁盘雕刻模式
    log("步骤4: 尝试恢复...")

    # 检查是否有残留的原始数据
    # 在真实场景中，我们需要访问原始磁盘设备
    # 这里我们检查数据目录是否有残留数据

    output_dir = TEST_DIR / "recovery" / container_name.replace("_test", "") / "rmrf"
    output_dir.mkdir(parents=True, exist_ok=True)

    # 由于文件已删除，需要使用磁盘雕刻
    # 检查系统表空间是否有undo log
    system_tablespace = volume_path / "ibdata1"

    if system_tablespace.exists():
        log(f"尝试从系统表空间恢复: {system_tablespace}")
        cmd = [
            str(RECOVERY_TOOL),
            "--disk-carving",
            "--device-path", str(system_tablespace),
            "--output", str(output_dir),
            "--verbose"
        ]
        success, stdout, stderr = run_cmd(cmd, timeout=300)

        # 统计恢复行数
        recovered = count_csv_rows(output_dir)
        rate = recovered / expected_rows if expected_rows > 0 else 0
        status = "PASS" if rate >= 0.8 else "FAIL"

        log(f"恢复结果: {recovered}/{expected_rows} ({rate*100:.1f}%)", status)
        return {
            "recovered": recovered,
            "expected": expected_rows,
            "rate": rate,
            "status": status,
            "note": "从系统表空间尝试恢复"
        }

    log("无法恢复: 文件已删除且无系统表空间", "ERROR")
    return {
        "recovered": 0,
        "expected": expected_rows,
        "rate": 0,
        "status": "FAIL",
        "note": "rm -rf场景需要原始磁盘设备进行磁盘雕刻"
    }

def test_drop_table_recovery(container_name, volume_path, expected_rows):
    """
    测试 DROP TABLE 灾难恢复
    流程：
    1. 执行 DROP TABLE
    2. 检查文件是否还存在
    3. 尝试从当前文件/系统表空间恢复
    """
    log("\n" + "="*60)
    log("场景: DROP TABLE 灾难恢复", "WARN")
    log("="*60)

    ibd_path = volume_path / "disaster_real" / "disaster_test_table.ibd"
    frm_path = volume_path / "disaster_real" / "disaster_test_table.frm"

    # 记录原始文件
    original_size = ibd_path.stat().st_size if ibd_path.exists() else 0
    log(f"原始文件大小: {original_size} bytes")

    # 1. 执行 DROP TABLE
    log("步骤1: 执行 DROP TABLE...")
    execute_mysql(container_name, "DROP TABLE IF EXISTS disaster_test_table;", "disaster_real")
    time.sleep(2)

    # 2. 检查文件状态
    log("步骤2: 检查文件状态...")
    if ibd_path.exists():
        new_size = ibd_path.stat().st_size
        log(f"文件仍存在，大小: {new_size} bytes")

        # 文件存在，尝试恢复
        output_dir = TEST_DIR / "recovery" / container_name.replace("_test", "") / "drop"
        output_dir.mkdir(parents=True, exist_ok=True)

        cmd = [str(RECOVERY_TOOL), "--ibd", str(ibd_path), "--output", str(output_dir)]
        success, stdout, stderr = run_cmd(cmd, timeout=120)

        recovered = count_csv_rows(output_dir)
        rate = recovered / expected_rows if expected_rows > 0 else 0
        status = "PASS" if rate >= 0.8 else "FAIL"

        log(f"恢复结果: {recovered}/{expected_rows} ({rate*100:.1f}%)", status)
        return {
            "recovered": recovered,
            "expected": expected_rows,
            "rate": rate,
            "status": status
        }
    else:
        log("文件已被删除", "WARN")

        # 尝试从系统表空间恢复
        system_tablespace = volume_path / "ibdata1"
        output_dir = TEST_DIR / "recovery" / container_name.replace("_test", "") / "drop"
        output_dir.mkdir(parents=True, exist_ok=True)

        if system_tablespace.exists():
            log(f"尝试从系统表空间恢复...")
            cmd = [
                str(RECOVERY_TOOL),
                "--drop-table-recovery",
                "--system-tablespace", str(system_tablespace),
                "--output", str(output_dir)
            ]
            success, stdout, stderr = run_cmd(cmd, timeout=300)

            recovered = count_csv_rows(output_dir)
            rate = recovered / expected_rows if expected_rows > 0 else 0
            status = "PASS" if rate >= 0.8 else "FAIL"

            log(f"恢复结果: {recovered}/{expected_rows} ({rate*100:.1f}%)", status)
            return {
                "recovered": recovered,
                "expected": expected_rows,
                "rate": rate,
                "status": status,
                "note": "从系统表空间恢复"
            }

        return {
            "recovered": 0,
            "expected": expected_rows,
            "rate": 0,
            "status": "FAIL",
            "note": "文件已删除，需要磁盘雕刻"
        }

def test_truncate_recovery(container_name, volume_path, expected_rows):
    """
    测试 TRUNCATE TABLE 灾难恢复
    流程：
    1. 执行 TRUNCATE TABLE
    2. 检查文件状态（文件应该还存在但数据被清空）
    3. 尝试恢复残留数据
    """
    log("\n" + "="*60)
    log("场景: TRUNCATE TABLE 灾难恢复", "WARN")
    log("="*60)

    ibd_path = volume_path / "disaster_real" / "disaster_test_table.ibd"
    frm_path = volume_path / "disaster_real" / "disaster_test_table.frm"

    # 记录原始文件
    original_size = ibd_path.stat().st_size if ibd_path.exists() else 0
    log(f"原始文件大小: {original_size} bytes")

    # 1. 执行 TRUNCATE TABLE
    log("步骤1: 执行 TRUNCATE TABLE...")
    execute_mysql(container_name, "TRUNCATE TABLE disaster_test_table;", "disaster_real")
    time.sleep(2)

    # 2. 检查文件状态
    log("步骤2: 检查文件状态...")
    if ibd_path.exists():
        new_size = ibd_path.stat().st_size
        log(f"文件仍存在，大小: {new_size} bytes (原始: {original_size} bytes)")

        # MySQL 8.0+ TRUNCATE 会重建文件，文件大小会变化
        # MySQL 5.7 TRUNCATE 使用 DELETE，文件大小不变

        # 3. 尝试恢复
        output_dir = TEST_DIR / "recovery" / container_name.replace("_test", "") / "truncate"
        output_dir.mkdir(parents=True, exist_ok=True)

        # 使用 truncate-recovery 模式
        cmd = [
            str(RECOVERY_TOOL),
            "--truncate-recovery",
            "--ibd", str(ibd_path),
            "--output", str(output_dir)
        ]

        if frm_path.exists():
            cmd.extend(["--frm", str(frm_path)])

        log("步骤3: 执行TRUNCATE恢复...")
        success, stdout, stderr = run_cmd(cmd, timeout=120)

        recovered = count_csv_rows(output_dir)
        rate = recovered / expected_rows if expected_rows > 0 else 0

        # TRUNCATE恢复可能只能恢复部分或无法恢复
        # MySQL 8.0+ TRUNCATE = DROP + CREATE，数据完全丢失
        # MySQL 5.7 TRUNCATE = DELETE，可从undo log恢复

        if rate >= 0.8:
            status = "PASS"
        elif rate > 0:
            status = "PARTIAL"
        else:
            status = "FAIL"

        log(f"恢复结果: {recovered}/{expected_rows} ({rate*100:.1f}%)", status)
        return {
            "recovered": recovered,
            "expected": expected_rows,
            "rate": rate,
            "status": status,
            "file_size_before": original_size,
            "file_size_after": new_size
        }
    else:
        log("文件不存在（可能被DROP）", "ERROR")
        return {
            "recovered": 0,
            "expected": expected_rows,
            "rate": 0,
            "status": "FAIL",
            "note": "文件不存在"
        }

def count_csv_rows(output_dir):
    """统计CSV文件行数"""
    for f in output_dir.iterdir():
        if f.suffix == '.csv':
            with open(f, 'r') as csv:
                return sum(1 for _ in csv) - 1  # 减去标题行
    return 0

def test_version(version, num_rows=500):
    """测试单个MySQL版本"""
    log(f"\n{'#'*60}")
    log(f"测试版本: {version}")
    log(f"{'#'*60}")

    config = CONTAINERS.get(version)
    if not config:
        log(f"未知版本: {version}", "ERROR")
        return None

    container_name = config["name"]
    has_frm = config["has_frm"]

    # 获取数据目录
    volume_path = get_volume_path(container_name)
    if not volume_path:
        log(f"无法获取数据目录", "ERROR")
        return None

    log(f"数据目录: {volume_path}")

    # 确保容器运行
    run_cmd(["podman", "start", container_name])
    time.sleep(10)

    # 创建数据库
    execute_mysql(container_name, "CREATE DATABASE IF NOT EXISTS disaster_real;", None)
    execute_mysql(container_name, "DROP TABLE IF EXISTS disaster_test_table;", "disaster_real")

    results = {
        "version": version,
        "scenarios": {}
    }

    # ===== 测试 TRUNCATE 场景 =====
    # TRUNCATE后文件还存在，是最容易恢复的场景
    log("\n准备 TRUNCATE 测试...")
    create_test_table(container_name)
    truncate_rows = insert_test_data(container_name, num_rows)
    results["truncate_expected"] = truncate_rows

    results["scenarios"]["truncate"] = test_truncate_recovery(
        container_name, volume_path, truncate_rows
    )

    # ===== 测试 DROP TABLE 场景 =====
    # DROP后文件被删除，需要磁盘雕刻
    log("\n准备 DROP TABLE 测试...")
    run_cmd(["podman", "start", container_name])
    time.sleep(5)
    execute_mysql(container_name, "DROP TABLE IF EXISTS disaster_test_table;", "disaster_real")
    create_test_table(container_name)
    drop_rows = insert_test_data(container_name, num_rows)
    results["drop_expected"] = drop_rows

    results["scenarios"]["drop"] = test_drop_table_recovery(
        container_name, volume_path, drop_rows
    )

    # ===== 测试 rm -rf 场景 =====
    # rm -rf后文件被删除，需要磁盘雕刻
    log("\n准备 rm -rf 测试...")
    run_cmd(["podman", "start", container_name])
    time.sleep(5)
    execute_mysql(container_name, "DROP TABLE IF EXISTS disaster_test_table;", "disaster_real")
    create_test_table(container_name)
    rmrf_rows = insert_test_data(container_name, num_rows)
    results["rmrf_expected"] = rmrf_rows

    results["scenarios"]["rmrf"] = test_rmrf_recovery(
        container_name, volume_path, rmrf_rows
    )

    return results

def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=500, help="测试行数")
    parser.add_argument("--version", type=str, default=None, help="测试特定版本")
    parser.add_argument("--all", action="store_true", help="测试所有版本")
    args = parser.parse_args()

    TEST_DIR.mkdir(parents=True, exist_ok=True)

    all_results = {}

    if args.version:
        versions = [args.version]
    elif args.all:
        versions = list(CONTAINERS.keys())
    else:
        versions = ["mysql80", "mysql56"]  # 默认测试主要版本

    for version in versions:
        try:
            result = test_version(version, args.rows)
            if result:
                all_results[version] = result
        except Exception as e:
            log(f"测试 {version} 失败: {e}", "ERROR")
            all_results[version] = {"error": str(e)}

    # 打印摘要
    print(f"\n{'='*60}")
    print("测试摘要 - 真实灾难恢复")
    print(f"{'='*60}")

    total_pass = 0
    total_tests = 0

    for version, result in all_results.items():
        if "scenarios" in result:
            print(f"\n{version}:")
            for scenario, data in result["scenarios"].items():
                total_tests += 1
                if data.get("status") == "PASS":
                    total_pass += 1
                rate = data.get("rate", 0) * 100
                status_color = "\033[92m" if data.get("status") == "PASS" else "\033[91m"
                reset = "\033[0m"
                print(f"  {scenario}: {status_color}{data.get('status', 'N/A')}{reset} - {data.get('recovered', 0)}/{data.get('expected', 0)} ({rate:.1f}%)")
                if data.get("note"):
                    print(f"    注: {data['note']}")

    print(f"\n通过率: {total_pass}/{total_tests} ({total_pass/total_tests*100:.1f}%)" if total_tests > 0 else "\n无测试结果")
    print(f"\n说明:")
    print("  - PASS: 恢复率 >= 80%")
    print("  - PARTIAL: 恢复部分数据")
    print("  - FAIL: 无法恢复或恢复率 < 80%")
    print("  - rm -rf场景需要原始磁盘设备进行磁盘雕刻")

    return 0 if total_pass == total_tests else 1

if __name__ == "__main__":
    sys.exit(main())
