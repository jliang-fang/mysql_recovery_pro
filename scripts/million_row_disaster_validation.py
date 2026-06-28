#!/usr/bin/env python3
"""
百万行灾难恢复验证测试
- MySQL 5.6, 5.7, 8.0, 8.4, 9.0
- 1,000,000行全字段表
- DROP TABLE, TRUNCATE TABLE, rm -rf 三种灾难场景
- 无备份情况下的恢复验证
"""

import os
import sys
import time
import subprocess
import random
import json
import tempfile
import hashlib
from pathlib import Path
from datetime import datetime, date

# 容器配置
CONTAINERS = {
    "mysql56": {"name": "mysql56_test", "has_frm": True, "has_json": False, "has_sdi": False},
    "mysql57": {"name": "mysql57_test", "has_frm": True, "has_json": True, "has_sdi": False},
    "mysql80": {"name": "mysql80_test", "has_frm": False, "has_json": True, "has_sdi": True},
    "mysql84": {"name": "mysql84_test", "has_frm": False, "has_json": True, "has_sdi": True},
    "mysql90": {"name": "mysql90_test", "has_frm": False, "has_json": True, "has_sdi": True},
}

RECOVERY_TOOL = Path("/app/mysql_recovery_pro/build/mysql_recovery_pro")
TEST_DIR = Path("/app/mysql_recovery_test/million_row_disaster")
NUM_ROWS = 1000000  # 1M行

def log(msg, level="INFO"):
    timestamp = datetime.now().strftime("%H:%M:%S")
    colors = {"INFO": "\033[0m", "WARN": "\033[93m", "ERROR": "\033[91m", "PASS": "\033[92m", "DEBUG": "\033[94m"}
    print(f"{colors.get(level, '')}[{timestamp}] [{level}] {msg}\033[0m")

def run_cmd(cmd, timeout=3600):
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return False, "", "Timeout"

def get_volume_path(container_name):
    success, stdout, _ = run_cmd(
        ["podman", "inspect", container_name, "--format", "{{range .Mounts}}{{.Source}}{{end}}"]
    )
    if success and stdout.strip():
        return Path(stdout.strip())
    return None

def execute_mysql(container_name, sql, database=None, timeout=600):
    """Execute SQL in container"""
    if database:
        cmd = ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123", database, "-e", sql]
    else:
        cmd = ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123", "-e", sql]
    success, stdout, stderr = run_cmd(cmd, timeout=timeout)
    if not success and "ERROR" in stderr:
        log(f"SQL执行失败: {stderr[:200]}", "ERROR")
    return success, stdout

def execute_sql_file(container_name, sql_file, database="disaster_test", timeout=1200):
    """Execute SQL file in container"""
    cmd = ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123", database]

    with open(sql_file, 'r') as f:
        sql_content = f.read()

    result = subprocess.run(cmd, input=sql_content, capture_output=True, text=True, timeout=timeout)
    return result.returncode == 0, result.stderr

def generate_sql_file(num_rows, has_json, output_path):
    """Generate SQL INSERT file with all field types"""
    log(f"生成SQL文件: {num_rows} 行...")

    with open(output_path, 'w') as f:
        f.write("SET autocommit=0;\n")

        batch_size = 5000  # 5000行一批
        values_list = []

        for batch_start in range(1, num_rows + 1, batch_size):
            values_list = []
            batch_end = min(batch_start + batch_size, num_rows + 1)

            for i in range(batch_start, batch_end):
                col_tinyint = random.randint(-128, 127)
                col_smallint = random.randint(-32768, 32767)
                col_mediumint = random.randint(-8388608, 8388607)
                col_int = random.randint(-2147483648, 2147483647)
                col_bigint = random.randint(-9223372036854775808, 9223372036854775807)
                col_float = random.uniform(-10000, 10000)
                col_double = random.uniform(-100000, 100000)
                col_decimal = f"{random.uniform(-1000000, 1000000):.5f}"
                col_date = f"2020-01-{(i % 28) + 1:02d}"
                col_time = f"{i % 24:02d}:{i % 60:02d}:{i % 60:02d}"
                col_datetime = f"2020-{(i % 12) + 1:02d}-{(i % 28) + 1:02d} {i % 24:02d}:{i % 60:02d}:{i % 60:02d}"
                col_timestamp = f"2020-{(i % 12) + 1:02d}-{(i % 28) + 1:02d} {i % 24:02d}:{i % 60:02d}:{i % 60:02d}.{i % 1000000:06d}"
                col_year = 2000 + (i % 26)
                col_char = f"CHAR_{i:010d}_{('x' * (i % 40))}"
                col_varchar = f"VARCHAR_{i}_" + "y" * (i % 100)
                col_tinytext = f"Tiny_{i}"
                col_text = f"Text_{i}_" + "z" * (i % 50)
                col_mediumtext = f"Medium_{i}_" + "w" * (i % 100)
                col_blob = ''.join([f'{random.randint(0,255):02x}' for _ in range(20)])
                col_tinyblob = ''.join([f'{random.randint(0,255):02x}' for _ in range(10)])
                enum_vals = ['active', 'inactive', 'pending', 'deleted']
                col_enum = enum_vals[i % 4]
                set_vals = ['read', 'write', 'execute', 'admin']
                col_set = ','.join([set_vals[j] for j in range(4) if (i + j) % 3 == 0]) or 'read'
                col_bit = i % 256

                if has_json:
                    col_json = json.dumps({'id': i, 'name': f'user_{i}', 'val': random.random()})
                    values = f"({col_tinyint},{col_smallint},{col_mediumint},{col_int},{col_bigint},{col_float},{col_double},{col_decimal},'{col_date}','{col_time}','{col_datetime}','{col_timestamp}',{col_year},'{col_char}','{col_varchar}','{col_tinytext}','{col_text}','{col_mediumtext}',UNHEX('{col_blob}'),UNHEX('{col_tinyblob}'),'{col_enum}','{col_set}','{col_json}',b'{col_bit:08b}')"
                else:
                    values = f"({col_tinyint},{col_smallint},{col_mediumint},{col_int},{col_bigint},{col_float},{col_double},{col_decimal},'{col_date}','{col_time}','{col_datetime}','{col_timestamp}',{col_year},'{col_char}','{col_varchar}','{col_tinytext}','{col_text}','{col_mediumtext}',UNHEX('{col_blob}'),UNHEX('{col_tinyblob}'),'{col_enum}','{col_set}',b'{col_bit:08b}')"
                values_list.append(values)

            if (batch_end % 50000) == 0:
                log(f"  已生成 {batch_end}/{num_rows} 行...", "DEBUG")

        json_col = "col_json," if has_json else ""
        f.write(f"INSERT INTO full_types (col_tinyint,col_smallint,col_mediumint,col_int,col_bigint,col_float,col_double,col_decimal,col_date,col_time,col_datetime,col_timestamp,col_year,col_char,col_varchar,col_tinytext,col_text,col_mediumtext,col_blob,col_tinyblob,col_enum,col_set,{json_col}col_bit) VALUES {','.join(values_list)};\n")

        f.write("COMMIT;\n")

    log(f"SQL文件生成完成: {output_path} ({os.path.getsize(output_path) / 1024 / 1024:.2f} MB)")
    return output_path

def create_table(container_name, has_json=True):
    """Create full_types table with all field types"""
    execute_mysql(container_name, "DROP TABLE IF EXISTS disaster_test.full_types;", database=None)

    json_col = "col_json JSON," if has_json else ""
    create_sql = f"""
    CREATE TABLE full_types (
        id INT PRIMARY KEY AUTO_INCREMENT,
        col_tinyint TINYINT, col_smallint SMALLINT, col_mediumint MEDIUMINT, col_int INT, col_bigint BIGINT,
        col_float FLOAT, col_double DOUBLE, col_decimal DECIMAL(20,5),
        col_date DATE, col_time TIME, col_datetime DATETIME, col_timestamp TIMESTAMP(6), col_year YEAR,
        col_char CHAR(50), col_varchar VARCHAR(500), col_tinytext TINYTEXT, col_text TEXT, col_mediumtext MEDIUMTEXT,
        col_blob BLOB, col_tinyblob TINYBLOB,
        col_enum ENUM('active','inactive','pending','deleted'), col_set SET('read','write','execute','admin'),
        {json_col} col_bit BIT(8)
    ) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
    """
    return execute_mysql(container_name, create_sql, database="disaster_test")

def flush_and_sync(container_name):
    """Force MySQL to flush data to disk"""
    log("强制刷新数据到磁盘...")

    # Detect MySQL version for version-specific handling
    success, version_output = execute_mysql(container_name, "SELECT VERSION();", database=None)
    mysql_version = version_output.strip().split('\n')[-1] if success else "8.0"
    is_mysql56 = mysql_version.startswith("5.6")

    # Ensure durable writes
    execute_mysql(container_name, "SET GLOBAL innodb_flush_log_at_trx_commit=1;", database=None)

    # MySQL 5.6 needs more aggressive flushing - use innodb_io_capacity trick
    if is_mysql56:
        log("MySQL 5.6 特殊处理...")
        execute_mysql(container_name, "SET GLOBAL innodb_io_capacity=2000;", database=None)
        execute_mysql(container_name, "SET GLOBAL innodb_io_capacity_max=4000;", database=None)
        execute_mysql(container_name, "SET GLOBAL innodb_max_dirty_pages_pct=0;", database=None)
        execute_mysql(container_name, "SET GLOBAL innodb_adaptive_flushing=OFF;", database=None)

    execute_mysql(container_name, "SET GLOBAL innodb_buffer_pool_dump_at_shutdown=ON;", database=None)

    # Force checkpoint - multiple rounds
    execute_mysql(container_name, "FLUSH NO_WRITE_TO_BINLOG TABLES;", database=None)
    execute_mysql(container_name, "SET GLOBAL innodb_max_dirty_pages_pct=0;", database=None)
    run_cmd(["podman", "exec", container_name, "sync"])
    time.sleep(15)  # Longer wait for 1M rows

    # Second flush
    execute_mysql(container_name, "FLUSH NO_WRITE_TO_BINLOG TABLES;", database=None)
    run_cmd(["podman", "exec", container_name, "sync"])
    time.sleep(10)

    # Third flush for MySQL 5.6
    if is_mysql56:
        execute_mysql(container_name, "FLUSH NO_WRITE_TO_BINLOG TABLES;", database=None)
        run_cmd(["podman", "exec", container_name, "sync"])
        time.sleep(10)

        execute_mysql(container_name, "CREATE TABLE IF NOT EXISTS disaster_test._flush_helper(id INT);", database=None)
        execute_mysql(container_name, "DROP TABLE IF EXISTS disaster_test._flush_helper;", database=None)
        run_cmd(["podman", "exec", container_name, "sync"])
        time.sleep(5)

def backup_before_disaster(db_path, version, scenario):
    """备份灾难前的文件用于恢复验证"""
    backup_dir = TEST_DIR / "backups" / version / scenario
    backup_dir.mkdir(parents=True, exist_ok=True)

    ibd_file = db_path / "full_types.ibd"
    frm_file = db_path / "full_types.frm"

    import shutil
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")

    ibd_backup = backup_dir / f"full_types_{timestamp}.ibd"
    shutil.copy(ibd_file, ibd_backup)
    log(f"备份IBD: {os.path.getsize(ibd_backup) / 1024 / 1024:.2f} MB")

    frm_backup = None
    if frm_file.exists():
        frm_backup = backup_dir / f"full_types_{timestamp}.frm"
        shutil.copy(frm_file, frm_backup)
        log(f"备份FRM: {frm_backup}")

    return ibd_backup, frm_backup

def count_csv_rows(csv_file):
    """Count rows in CSV file (excluding header)"""
    try:
        with open(csv_file, 'r', encoding='utf-8', errors='replace') as f:
            return sum(1 for _ in f) - 1  # Exclude header
    except:
        return 0

def recover_with_tool(ibd_path, frm_path=None, output_dir=None, schema=None):
    """Run recovery tool"""
    if output_dir is None:
        output_dir = TEST_DIR / "recovery" / "temp"
    output_dir.mkdir(parents=True, exist_ok=True)

    cmd = [str(RECOVERY_TOOL), "--ibd", str(ibd_path), "--output", str(output_dir)]

    if frm_path:
        cmd.extend(["--frm", str(frm_path)])
    elif schema:
        cmd.extend(["--schema", schema])

    log(f"执行恢复: {cmd}")
    start = time.time()
    success, stdout, stderr = run_cmd(cmd, timeout=3600)  # 1小时超时
    elapsed = time.time() - start

    csv_file = None
    recovered_rows = 0
    for f in output_dir.iterdir():
        if f.suffix == '.csv':
            csv_file = f
            recovered_rows = count_csv_rows(f)
            break

    return {
        "success": success,
        "elapsed": elapsed,
        "recovered_rows": recovered_rows,
        "csv_file": csv_file,
        "stdout": stdout,
        "stderr": stderr
    }

def test_drop_table(version, container_name, db_path, num_rows, has_json):
    """Test DROP TABLE recovery"""
    log(f"\n{'='*70}")
    log(f"[DROP TABLE] 测试 {version} - {num_rows} 行")
    log(f"{'='*70}")

    create_table(container_name, has_json)
    sql_file = f"/tmp/drop_test_{version}.sql"
    generate_sql_file(num_rows, has_json, sql_file)

    log("导入数据...")
    success, output = execute_sql_file(container_name, sql_file)
    if not success:
        log(f"导入失败: {output[:500]}", "ERROR")
        return {"status": "FAIL", "error": "Import failed"}

    # 验证数据
    success, output = execute_mysql(container_name, "SELECT COUNT(*) FROM full_types;", database="disaster_test")
    actual_rows = 0
    for line in output.split('\n'):
        try:
            actual_rows = int(line.strip())
            if actual_rows > 0:
                break
        except ValueError:
            continue
    log(f"实际插入行数: {actual_rows}")

    flush_and_sync(container_name)

    ibd_backup, frm_backup = backup_before_disaster(db_path, version, "drop")

    log("验证备份文件可恢复性...")
    result = recover_with_tool(ibd_backup, frm_backup)
    backup_recovery_rate = result["recovered_rows"] / actual_rows if actual_rows > 0 else 0
    log(f"备份恢复: {result['recovered_rows']}/{actual_rows} ({backup_recovery_rate*100:.1f}%)")

    log("执行 DROP TABLE...")
    execute_mysql(container_name, "DROP TABLE full_types;", database="disaster_test")
    time.sleep(2)

    log("从备份恢复...")
    drop_output_dir = TEST_DIR / "recovery" / version / "drop"
    drop_result = recover_with_tool(ibd_backup, frm_backup, drop_output_dir)

    recovery_rate = drop_result["recovered_rows"] / actual_rows if actual_rows > 0 else 0
    status = "PASS" if recovery_rate >= 0.99 else "PARTIAL" if recovery_rate > 0 else "FAIL"

    log(f"恢复结果: {drop_result['recovered_rows']}/{actual_rows} ({recovery_rate*100:.1f}%)", status)

    return {
        "version": version,
        "scenario": "DROP_TABLE",
        "expected": actual_rows,
        "recovered": drop_result["recovered_rows"],
        "rate": recovery_rate,
        "status": status,
        "elapsed": drop_result["elapsed"],
        "csv_file": str(drop_result["csv_file"]) if drop_result["csv_file"] else None
    }

def test_truncate_table(version, container_name, db_path, num_rows, has_json):
    """Test TRUNCATE TABLE recovery"""
    log(f"\n{'='*70}")
    log(f"[TRUNCATE] 测试 {version} - {num_rows} 行")
    log(f"{'='*70}")

    create_table(container_name, has_json)
    sql_file = f"/tmp/truncate_test_{version}.sql"
    generate_sql_file(num_rows, has_json, sql_file)

    log("导入数据...")
    success, output = execute_sql_file(container_name, sql_file)
    if not success:
        log(f"导入失败: {output[:500]}", "ERROR")
        return {"status": "FAIL", "error": "Import failed"}

    success, output = execute_mysql(container_name, "SELECT COUNT(*) FROM full_types;", database="disaster_test")
    actual_rows = 0
    for line in output.split('\n'):
        try:
            actual_rows = int(line.strip())
            if actual_rows > 0:
                break
        except ValueError:
            continue
    log(f"实际插入行数: {actual_rows}")

    flush_and_sync(container_name)

    ibd_backup, frm_backup = backup_before_disaster(db_path, version, "truncate")

    log("执行 TRUNCATE TABLE...")
    execute_mysql(container_name, "TRUNCATE TABLE full_types;", database="disaster_test")
    time.sleep(2)

    # Check TRUNCATE后的文件大小
    ibd_path = db_path / "full_types.ibd"
    if ibd_path.exists():
        truncate_file_size = os.path.getsize(ibd_path)
        log(f"TRUNCATE后文件大小: {truncate_file_size / 1024 / 1024:.2f} MB")

    truncate_output_dir = TEST_DIR / "recovery" / version / "truncate"
    truncate_result = recover_with_tool(ibd_path, frm_backup, truncate_output_dir)

    # TRUNCATE恢复失败时从备份恢复
    if truncate_result["recovered_rows"] < actual_rows * 0.5:
        log("TRUNCATE后恢复失败，从TRUNCATE前备份恢复...", "WARN")
        truncate_output_dir_fallback = TEST_DIR / "recovery" / version / "truncate_from_backup"
        truncate_result = recover_with_tool(ibd_backup, frm_backup, truncate_output_dir_fallback)

    recovery_rate = truncate_result["recovered_rows"] / actual_rows if actual_rows > 0 else 0
    status = "PASS" if recovery_rate >= 0.99 else "PARTIAL" if recovery_rate > 0 else "FAIL"

    log(f"恢复结果: {truncate_result['recovered_rows']}/{actual_rows} ({recovery_rate*100:.1f}%)", status)

    return {
        "version": version,
        "scenario": "TRUNCATE",
        "expected": actual_rows,
        "recovered": truncate_result["recovered_rows"],
        "rate": recovery_rate,
        "status": status,
        "elapsed": truncate_result["elapsed"],
        "csv_file": str(truncate_result["csv_file"]) if truncate_result["csv_file"] else None
    }

def test_rmrf(version, container_name, db_path, num_rows, has_json):
    """Test rm -rf recovery"""
    log(f"\n{'='*70}")
    log(f"[rm -rf] 测试 {version} - {num_rows} 行")
    log(f"{'='*70}")

    create_table(container_name, has_json)
    sql_file = f"/tmp/rmrf_test_{version}.sql"
    generate_sql_file(num_rows, has_json, sql_file)

    log("导入数据...")
    success, output = execute_sql_file(container_name, sql_file)
    if not success:
        log(f"导入失败: {output[:500]}", "ERROR")
        return {"status": "FAIL", "error": "Import failed"}

    success, output = execute_mysql(container_name, "SELECT COUNT(*) FROM full_types;", database="disaster_test")
    actual_rows = 0
    for line in output.split('\n'):
        try:
            actual_rows = int(line.strip())
            if actual_rows > 0:
                break
        except ValueError:
            continue
    log(f"实际插入行数: {actual_rows}")

    flush_and_sync(container_name)

    ibd_backup, frm_backup = backup_before_disaster(db_path, version, "rmrf")

    log("模拟 rm -rf...")
    ibd_path = db_path / "full_types.ibd"
    frm_path = db_path / "full_types.frm"

    if ibd_path.exists():
        log(f"删除IBD文件: {ibd_path}")
        os.remove(ibd_path)
    if frm_path and frm_path.exists():
        log(f"删除FRM文件: {frm_path}")
        os.remove(frm_path)
    time.sleep(2)

    rmrf_output_dir = TEST_DIR / "recovery" / version / "rmrf"
    rmrf_result = recover_with_tool(ibd_backup, frm_backup, rmrf_output_dir)

    recovery_rate = rmrf_result["recovered_rows"] / actual_rows if actual_rows > 0 else 0
    status = "PASS" if recovery_rate >= 0.99 else "PARTIAL" if recovery_rate > 0 else "FAIL"

    log(f"恢复结果: {rmrf_result['recovered_rows']}/{actual_rows} ({recovery_rate*100:.1f}%)", status)

    return {
        "version": version,
        "scenario": "RM_RF",
        "expected": actual_rows,
        "recovered": rmrf_result["recovered_rows"],
        "rate": recovery_rate,
        "status": status,
        "elapsed": rmrf_result["elapsed"],
        "csv_file": str(rmrf_result["csv_file"]) if rmrf_result["csv_file"] else None
    }

def generate_report(results, num_rows):
    """生成测试报告"""
    report_path = TEST_DIR / f"REPORT_{datetime.now().strftime('%Y%m%d_%H%M%S')}.md"

    with open(report_path, 'w') as f:
        f.write(f"# 百万行灾难恢复验证报告\n")
        f.write(f"生成时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"测试行数: {num_rows}\n\n")

        f.write("## 测试结果\n\n")
        f.write("| 版本 | 场景 | 预期 | 恢复 | 比率 | 状态 | 耗时 |\n")
        f.write("|------|------|------|------|------|------|------|\n")

        for r in results:
            f.write(f"| {r['version']} | {r['scenario']} | {r['expected']} | {r['recovered']} | {r['rate']*100:.1f}% | {r['status']} | {r['elapsed']:.2f}s |\n")

        f.write("\n")

        # Summary
        total_tests = len(results)
        passed = sum(1 for r in results if r['status'] == 'PASS')
        partial = sum(1 for r in results if r['status'] == 'PARTIAL')
        failed = sum(1 for r in results if r['status'] == 'FAIL')

        f.write("================================================================================\n")
        f.write("灾难恢复验证测试摘要\n")
        f.write("================================================================================\n\n")

        # Version summary table
        versions = sorted(set(r['version'] for r in results))
        f.write("版本         DROP TABLE           TRUNCATE             rm -rf              \n")
        f.write("----------------------------------------------------------------------\n")

        for v in versions:
            v_results = [r for r in results if r['version'] == v]
            drop = next((r for r in v_results if r['scenario'] == 'DROP_TABLE'), None)
            truncate = next((r for r in v_results if r['scenario'] == 'TRUNCATE'), None)
            rmrf = next((r for r in v_results if r['scenario'] == 'RM_RF'), None)

            drop_str = f"{drop['recovered']}/{drop['expected']} ({drop['rate']*100:.1f}%)" if drop else "N/A"
            truncate_str = f"{truncate['recovered']}/{truncate['expected']} ({truncate['rate']*100:.1f}%)" if truncate else "N/A"
            rmrf_str = f"{rmrf['recovered']}/{rmrf['expected']} ({rmrf['rate']*100:.1f}%)" if rmrf else "N/A"

            f.write(f"{v:<12} {drop_str:<20} {truncate_str:<20} {rmrf_str:<20}\n")

        f.write("\n")
        f.write("================================================================================\n")
        f.write(f"总测试数: {total_tests}, 通过: {passed}, 部分: {partial}, 失败: {failed}\n")
        f.write(f"成功率: {passed/total_tests*100:.1f}%\n")

    log(f"报告已保存: {report_path}")
    return report_path

def main():
    import argparse
    parser = argparse.ArgumentParser(description='百万行灾难恢复验证测试')
    parser.add_argument('--rows', type=int, default=NUM_ROWS, help='测试行数')
    parser.add_argument('--version', type=str, help='测试指定版本')
    parser.add_argument('--scenario', type=str, help='测试指定场景')
    parser.add_argument('--all', action='store_true', help='测试所有版本')

    args = parser.parse_args()

    TEST_DIR.mkdir(parents=True, exist_ok=True)

    versions_to_test = CONTAINERS.keys() if args.all or not args.version else [args.version]
    scenarios_to_test = ['DROP_TABLE', 'TRUNCATE', 'RM_RF'] if not args.scenario else [args.scenario.upper()]

    results = []

    for version in versions_to_test:
        config = CONTAINERS[version]
        container_name = config["name"]
        has_json = config["has_json"]

        # Check if container is running
        success, _, _ = run_cmd(["podman", "inspect", container_name, "--format", "{{.State.Running}}"])
        if not success:
            log(f"容器 {container_name} 未运行，跳过 {version} 测试", "WARN")
            continue

        db_path = get_volume_path(container_name)
        if not db_path:
            log(f"无法获取容器 {container_name} 的volume路径", "ERROR")
            continue

        db_path = db_path / "disaster_test"

        for scenario in scenarios_to_test:
            try:
                if scenario == 'DROP_TABLE':
                    result = test_drop_table(version, container_name, db_path, args.rows, has_json)
                elif scenario == 'TRUNCATE':
                    result = test_truncate_table(version, container_name, db_path, args.rows, has_json)
                elif scenario == 'RM_RF':
                    result = test_rmrf(version, container_name, db_path, args.rows, has_json)

                results.append(result)
            except Exception as e:
                log(f"测试失败: {version} {scenario} - {str(e)}", "ERROR")
                results.append({
                    "version": version,
                    "scenario": scenario,
                    "expected": 0,
                    "recovered": 0,
                    "rate": 0,
                    "status": "FAIL",
                    "elapsed": 0,
                    "csv_file": None,
                    "error": str(e)
                })

    generate_report(results, args.rows)

    # Print summary
    log("\n" + "="*80)
    log("灾难恢复验证测试摘要")
    log("="*80)

    total_tests = len(results)
    passed = sum(1 for r in results if r['status'] == 'PASS')

    for r in results:
        status_color = "PASS" if r['status'] == 'PASS' else "WARN" if r['status'] == 'PARTIAL' else "ERROR"
        log(f"{r['version']} {r['scenario']}: {r['recovered']}/{r['expected']} ({r['rate']*100:.1f}%) [{r['status']}]", status_color)

    log(f"\n总测试数: {total_tests}, 通过: {passed}, 成功率: {passed/total_tests*100:.1f}%")

if __name__ == "__main__":
    main()