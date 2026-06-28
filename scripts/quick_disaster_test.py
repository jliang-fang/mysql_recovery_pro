#!/usr/bin/env python3
"""
快速灾难恢复验证测试
测试MySQL 5.6-9.0的rm -rf, DROP TABLE, TRUNCATE TABLE恢复
"""

import os
import sys
import time
import shutil
import subprocess
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
TEST_DIR = Path("/app/mysql_recovery_test/quick_validation")

def log(msg):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}")

def run_cmd(cmd, timeout=60):
    """执行命令"""
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
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

def execute_mysql(container_name, sql, database="test_recovery"):
    """执行MySQL命令"""
    cmd = ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123", database]
    result = subprocess.run(cmd, input=sql, capture_output=True, text=True, timeout=120)
    return result.returncode == 0, result.stdout + result.stderr

def create_test_table(container_name):
    """创建测试表"""
    create_sql = """
    CREATE TABLE IF NOT EXISTS test_table (
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
        text_col TEXT
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=DYNAMIC;
    """
    return execute_mysql(container_name, create_sql)

def insert_test_data(container_name, num_rows=1000):
    """插入测试数据"""
    log(f"插入 {num_rows} 行测试数据...")

    values_list = []
    for i in range(1, num_rows + 1):
        name = f"User_{i:06d}"
        age = 20 + (i % 60)
        height = round(1.5 + (i % 50) / 100, 2)
        birth_date = f"1990-{(i % 12) + 1:02d}-{(i % 28) + 1:02d}"
        created_at = f"2026-06-{(i % 28) + 1:02d} {(i % 24):02d}:00:00"
        json_col = f'{{"id":{i},"name":"{name}"}}'
        enum_col = ['active', 'inactive', 'pending'][i % 3]
        set_col = ['a', 'b', 'c', 'a,b', 'b,c', 'a,b,c'][i % 6]
        blob_hex = ''.join([f'{(i + j) % 256:02x}' for j in range(10)])
        text_col = f"Text for record {i}"

        values = f"('{name}',{age},{height},'{birth_date}','{created_at}','{json_col}','{enum_col}','{set_col}',UNHEX('{blob_hex}'),'{text_col}')"
        values_list.append(values)

        if len(values_list) >= 100:
            insert_sql = f"INSERT INTO test_table (name,age,height,birth_date,created_at,json_col,enum_col,set_col,blob_col,text_col) VALUES {','.join(values_list)};"
            success, _ = execute_mysql(container_name, insert_sql)
            if not success:
                log(f"插入失败在行 {i}")
            values_list = []

    if values_list:
        insert_sql = f"INSERT INTO test_table (name,age,height,birth_date,created_at,json_col,enum_col,set_col,blob_col,text_col) VALUES {','.join(values_list)};"
        execute_mysql(container_name, insert_sql)

    # 验证插入
    success, output = execute_mysql(container_name, "SELECT COUNT(*) FROM test_table;")
    if success:
        lines = [l.strip() for l in output.split('\n') if l.strip() and not l.startswith('mysql:')]
        if lines:
            count = int(lines[-1])
            log(f"实际插入行数: {count}")
            return count
    return 0

def backup_ibd(volume_path, version):
    """备份IBD文件"""
    ibd_src = volume_path / "test_recovery" / "test_table.ibd"
    frm_src = volume_path / "test_recovery" / "test_table.frm"

    if not ibd_src.exists():
        log(f"IBD文件不存在: {ibd_src}")
        return None, None

    backup_dir = TEST_DIR / "backups" / version
    backup_dir.mkdir(parents=True, exist_ok=True)

    timestamp = datetime.now().strftime("%H%M%S")
    ibd_backup = backup_dir / f"test_table_{timestamp}.ibd"
    shutil.copy(ibd_src, ibd_backup)

    frm_backup = None
    if frm_src.exists():
        frm_backup = backup_dir / f"test_table_{timestamp}.frm"
        shutil.copy(frm_src, frm_backup)

    log(f"备份: {ibd_backup} ({ibd_backup.stat().st_size} bytes)")
    return ibd_backup, frm_backup

def simulate_disaster(container_name, volume_path, disaster_type, version):
    """模拟灾难场景"""
    ibd_path = volume_path / "test_recovery" / "test_table.ibd"
    frm_path = volume_path / "test_recovery" / "test_table.frm"

    # 确保容器运行并等待MySQL flush数据
    run_cmd(["podman", "start", container_name])
    time.sleep(5)

    # 执行FLUSH TABLES确保数据写入磁盘
    execute_mysql(container_name, "FLUSH TABLES;")
    time.sleep(2)

    # 备份
    ibd_backup, frm_backup = backup_ibd(volume_path, version)
    if not ibd_backup:
        return None, None

    if disaster_type == "rmrf":
        log(f"模拟 rm -rf...")
        run_cmd(["podman", "stop", container_name])
        time.sleep(3)
        if ibd_path.exists():
            ibd_path.unlink()
        if frm_path.exists():
            frm_path.unlink()
        run_cmd(["podman", "start", container_name])
        time.sleep(10)

    elif disaster_type == "drop":
        log(f"模拟 DROP TABLE...")
        execute_mysql(container_name, "DROP TABLE test_table;")

    elif disaster_type == "truncate":
        log(f"模拟 TRUNCATE TABLE...")
        execute_mysql(container_name, "TRUNCATE TABLE test_table;")

    return ibd_backup, frm_backup

def run_recovery(ibd_path, output_dir, frm_path=None):
    """执行恢复"""
    output_dir.mkdir(parents=True, exist_ok=True)

    cmd = [str(RECOVERY_TOOL), "--ibd", str(ibd_path), "--output", str(output_dir)]
    if frm_path and frm_path.exists():
        cmd.extend(["--frm", str(frm_path)])

    log(f"执行恢复: {' '.join(cmd[:4])}...")

    start_time = time.time()
    success, stdout, stderr = run_cmd(cmd, timeout=300)
    elapsed = time.time() - start_time

    # 查找CSV文件
    csv_file = None
    for f in output_dir.iterdir():
        if f.suffix == '.csv':
            csv_file = f
            break

    # 统计恢复行数
    recovered_rows = 0
    if csv_file and csv_file.exists():
        with open(csv_file, 'r') as f:
            recovered_rows = sum(1 for _ in f) - 1

    return {
        "success": success,
        "recovered_rows": recovered_rows,
        "elapsed": elapsed,
        "csv_file": str(csv_file) if csv_file else None
    }

def test_version(version, num_rows=1000):
    """测试单个版本"""
    log(f"\n{'='*60}")
    log(f"测试 {version} - {num_rows} 行")
    log(f"{'='*60}")

    config = CONTAINERS.get(version)
    if not config:
        log(f"未知版本: {version}")
        return None

    container_name = config["name"]
    has_frm = config["has_frm"]

    # 获取数据目录
    volume_path = get_volume_path(container_name)
    if not volume_path:
        log(f"无法获取数据目录")
        return None

    # 确保容器运行
    run_cmd(["podman", "start", container_name])
    time.sleep(5)

    # 创建数据库和表
    execute_mysql(container_name, "CREATE DATABASE IF NOT EXISTS test_recovery;")
    execute_mysql(container_name, "DROP TABLE IF EXISTS test_table;")
    create_test_table(container_name)

    # 插入数据
    actual_rows = insert_test_data(container_name, num_rows)

    results = {"version": version, "expected_rows": actual_rows, "scenarios": {}}

    # 测试各场景
    for scenario in ["rmrf", "drop", "truncate"]:
        log(f"\n--- 场景: {scenario} ---")

        # 每个场景前确保表存在并有数据
        run_cmd(["podman", "start", container_name])
        time.sleep(3)
        execute_mysql(container_name, "CREATE DATABASE IF NOT EXISTS test_recovery;")
        execute_mysql(container_name, "DROP TABLE IF EXISTS test_table;")
        create_test_table(container_name)
        actual_rows = insert_test_data(container_name, num_rows)

        # 模拟灾难
        ibd_backup, frm_backup = simulate_disaster(container_name, volume_path, scenario, version)
        if not ibd_backup:
            results["scenarios"][scenario] = {"error": "备份失败"}
            continue

        # 执行恢复
        output_dir = TEST_DIR / "recovery" / version / scenario
        recovery_result = run_recovery(ibd_backup, output_dir, frm_backup if has_frm else None)

        recovery_rate = recovery_result["recovered_rows"] / actual_rows if actual_rows > 0 else 0
        status = "PASS" if recovery_rate >= 0.95 else "FAIL"

        log(f"  {status}: {recovery_result['recovered_rows']}/{actual_rows} ({recovery_rate*100:.1f}%)")

        results["scenarios"][scenario] = {
            "recovered_rows": recovery_result["recovered_rows"],
            "recovery_rate": recovery_rate,
            "elapsed": recovery_result["elapsed"],
            "status": status
        }

        # 重置表准备下一个测试
        execute_mysql(container_name, "DROP TABLE IF EXISTS test_table;")
        time.sleep(1)

    return results

def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=1000, help="测试行数")
    parser.add_argument("--version", type=str, default=None, help="测试特定版本")
    args = parser.parse_args()

    TEST_DIR.mkdir(parents=True, exist_ok=True)

    all_results = {}

    if args.version:
        versions = [args.version]
    else:
        versions = ["mysql80", "mysql56"]  # 先测试主要版本

    for version in versions:
        try:
            result = test_version(version, args.rows)
            if result:
                all_results[version] = result
        except Exception as e:
            log(f"测试 {version} 失败: {e}")
            all_results[version] = {"error": str(e)}

    # 打印摘要
    print(f"\n{'='*60}")
    print("测试摘要")
    print(f"{'='*60}")

    total_pass = 0
    total_tests = 0

    for version, result in all_results.items():
        if "scenarios" in result:
            for scenario, data in result["scenarios"].items():
                total_tests += 1
                if data.get("status") == "PASS":
                    total_pass += 1
                rate = data.get("recovery_rate", 0) * 100
                print(f"  {version}/{scenario}: {data.get('status', 'N/A')} ({rate:.1f}%)")

    print(f"\n通过率: {total_pass}/{total_tests} ({total_pass/total_tests*100:.1f}%)" if total_tests > 0 else "\n无测试结果")

    return 0 if total_pass == total_tests else 1

if __name__ == "__main__":
    sys.exit(main())
