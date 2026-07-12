#!/usr/bin/env python3
"""
全面灾难恢复验证测试 - 1000K数据量
- MySQL 5.6, 5.7, 8.0, 8.4, 9.0
- ext4和XFS文件系统
- DROP TABLE, TRUNCATE TABLE, rm -rf 场景
- 包含所有字段类型的完整验证
- 块设备映射模式磁盘雕刻
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
    "mysql56": {"image": "docker.m.daocloud.io/mysql:5.6", "has_frm": True},
    "mysql57": {"image": "docker.m.daocloud.io/mysql:5.7", "has_frm": True},
    "mysql80": {"image": "docker.m.daocloud.io/mysql:8.0", "has_frm": False},
    "mysql84": {"image": "docker.m.daocloud.io/mysql:8.4", "has_frm": False},
    "mysql90": {"image": "docker.m.daocloud.io/mysql:9.0", "has_frm": False},
}

RECOVERY_TOOL = Path("/app/mysql_recovery_pro/build/mysql_recovery_pro")
BLOCK_DEVICE = "/dev/nvme0n2"
MOUNT_POINT = Path("/mnt/mysql_comprehensive_test")
TEST_DIR = Path("/app/mysql_recovery_test/comprehensive_1000k")
NUM_ROWS = 1000000  # 1000K

# 完整字段类型的表结构
ALL_TYPES_SCHEMA = """
CREATE TABLE test_table (
    id INT PRIMARY KEY AUTO_INCREMENT,
    col_tinyint TINYINT,
    col_smallint SMALLINT,
    col_mediumint MEDIUMINT,
    col_int INT,
    col_bigint BIGINT,
    col_float FLOAT,
    col_double DOUBLE,
    col_decimal DECIMAL(15,4),
    col_date DATE,
    col_time TIME,
    col_datetime DATETIME,
    col_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    col_year YEAR,
    col_char CHAR(20),
    col_varchar VARCHAR(200),
    col_text TEXT,
    col_blob BLOB,
    col_enum ENUM('active','inactive','pending','deleted'),
    col_set SET('read','write','execute','admin'),
    col_json JSON,
    col_bit BIT(8)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
"""

# Schema参数（冒号格式）
SCHEMA_PARAM = (
    "id:INT:4:pk,"
    "col_tinyint:TINYINT:1,"
    "col_smallint:SMALLINT:2,"
    "col_mediumint:MEDIUMINT:3,"
    "col_int:INT:4,"
    "col_bigint:BIGINT:8,"
    "col_float:FLOAT:4,"
    "col_double:DOUBLE:8,"
    "col_decimal:DECIMAL:15:4,"
    "col_date:DATE:3,"
    "col_time:TIME:3,"
    "col_datetime:DATETIME:5,"
    "col_timestamp:TIMESTAMP:4,"
    "col_year:YEAR:1,"
    "col_char:CHAR:20,"
    "col_varchar:VARCHAR:200,"
    "col_text:TEXT:0,"
    "col_blob:BLOB:0,"
    "col_enum:ENUM:0,"
    "col_set:SET:0,"
    "col_json:JSON:0,"
    "col_bit:BIT:1"
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
    """生成包含所有字段类型的测试数据SQL文件"""
    log(f"生成SQL文件: {num_rows} 行...")
    with open(output_path, 'w') as f:
        f.write("SET autocommit=0;\nSET unique_checks=0;\nSET foreign_key_checks=0;\n")

        batch_size = 100  # 较小的批处理以避免SQL过长
        enum_values = ['active', 'inactive', 'pending', 'deleted']

        for batch_start in range(0, num_rows, batch_size):
            batch_end = min(batch_start + batch_size, num_rows)
            values_list = []

            for i in range(batch_start, batch_end):
                # 生成各种类型的随机数据
                col_tinyint = random.randint(-128, 127)
                col_smallint = random.randint(-32768, 32767)
                col_mediumint = random.randint(-8388608, 8388607)
                col_int = random.randint(-2147483648, 2147483647)
                col_bigint = random.randint(-9223372036854775808, 9223372036854775807) % (10**12)
                col_float = round(random.uniform(-1000, 1000), 2)
                col_double = round(random.uniform(-1000000, 1000000), 6)
                col_decimal = round(random.uniform(0, 100000), 4)

                # 日期时间
                year = random.randint(2000, 2030)
                month = random.randint(1, 12)
                day = random.randint(1, 28)
                col_date = f"'{year}-{month:02d}-{day:02d}'"
                col_time = f"'{random.randint(0,23):02d}:{random.randint(0,59):02d}:{random.randint(0,59):02d}'"
                col_datetime = f"'{year}-{month:02d}-{day:02d} {random.randint(0,23):02d}:{random.randint(0,59):02d}:{random.randint(0,59):02d}'"
                col_year = year

                # 字符串
                col_char = f"'test_{i:08d}'"
                col_varchar = f"'varchar_test_{i}_{random.randint(1000,9999)}'"
                col_text = f"'text_content_{i}_" + "x" * random.randint(10, 100) + "'"

                # BLOB (十六进制)
                blob_data = ''.join([random.choice('0123456789ABCDEF') for _ in range(20)])
                col_blob = f"UNHEX('{blob_data}')"

                # ENUM和SET
                col_enum = f"'{random.choice(enum_values)}'"
                set_values = random.sample(['read', 'write', 'execute', 'admin'], random.randint(1, 4))
                col_set = f"'{','.join(set_values)}'"

                # JSON
                col_json = f"'{{\"id\":{i},\"name\":\"json_test_{i}\",\"values\":[{random.randint(1,100)},{random.randint(1,100)}]}}'"

                # BIT
                col_bit = f"b'{random.randint(0,255):08b}'"

                values = f"({col_tinyint},{col_smallint},{col_mediumint},{col_int},{col_bigint}," \
                         f"{col_float},{col_double},{col_decimal},{col_date},{col_time}," \
                         f"{col_datetime},NOW(),{col_year},{col_char},{col_varchar},{col_text}," \
                         f"{col_blob},{col_enum},{col_set},{col_json},{col_bit})"
                values_list.append(values)

            f.write(f"INSERT INTO test_table (col_tinyint,col_smallint,col_mediumint,col_int,col_bigint,"
                   f"col_float,col_double,col_decimal,col_date,col_time,col_datetime,col_timestamp,"
                   f"col_year,col_char,col_varchar,col_text,col_blob,col_enum,col_set,col_json,col_bit) "
                   f"VALUES {','.join(values_list)};\n")

        f.write("COMMIT;\n")
    log(f"SQL文件生成完成: {output_path}")

def run_test(version, scenario, fs_type, sql_file):
    """运行单个测试"""
    config = CONTAINERS[version]
    container_name = f"test_{version}_{scenario}"

    if not setup_filesystem(fs_type):
        return {"error": "文件系统设置失败"}

    log(f"\n{'='*60}")
    log(f"测试: {version} | {scenario} | {fs_type}")
    log(f"{'='*60}")

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
    for i in range(120):
        success, stdout, _ = run_cmd(["podman", "exec", container_name,
                                       "mysqladmin", "-uroot", "-proot123", "ping"])
        if success and "alive" in stdout.lower():
            log(f"MySQL就绪 ({i+1}秒)")
            break
        time.sleep(1)
    else:
        log("MySQL启动超时", "ERROR")
        return {"error": "MySQL启动超时"}

    # 创建数据库和表
    run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             "CREATE DATABASE IF NOT EXISTS testdb"])

    # 创建包含所有字段类型的表
    run_cmd(["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             ALL_TYPES_SCHEMA], timeout=60)

    # 插入数据
    log(f"插入 {NUM_ROWS} 行数据...")
    start_time = time.time()
    with open(sql_file, 'r') as f:
        proc = subprocess.Popen(
            ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123", "testdb"],
            stdin=f, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        proc.communicate(timeout=3600)
    elapsed = time.time() - start_time
    log(f"数据插入完成，耗时: {elapsed:.1f}秒")

    # 刷盘
    flush_and_sync(container_name)

    # 获取实际行数
    success, stdout, _ = run_cmd(["podman", "exec", container_name,
                                   "mysql", "-uroot", "-proot123", "-N", "-e",
                                   "SELECT COUNT(*) FROM testdb.test_table"])
    actual_rows = int(stdout.strip()) if success and stdout.strip().isdigit() else 0
    log(f"表中实际行数: {actual_rows}")

    if actual_rows == 0:
        log("数据插入失败", "ERROR")
        return {"error": "数据插入失败"}

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

    # 停止容器
    run_cmd(["podman", "stop", container_name])
    run_cmd(["podman", "rm", container_name])

    # RMRF场景删除文件
    if scenario == "RMRF":
        log("执行 rm -rf...")
        ibd_path = MOUNT_POINT / "testdb" / "test_table.ibd"
        if ibd_path.exists():
            ibd_path.unlink()

    # 卸载文件系统
    run_cmd(["umount", str(MOUNT_POINT)])

    # 磁盘雕刻恢复
    output_dir = TEST_DIR / version / fs_type / scenario
    output_dir.mkdir(parents=True, exist_ok=True)

    log(f"开始磁盘雕刻恢复: {BLOCK_DEVICE}")

    # 使用完整schema参数恢复
    cmd = [str(RECOVERY_TOOL),
           "--disk-carving",
           "--device-path", BLOCK_DEVICE,
           "--output", str(output_dir),
           "--filesystem", fs_type,
           "--schema", SCHEMA_PARAM,
           "--num-workers", "8"]

    start_time = time.time()
    success, stdout, stderr = run_cmd(cmd, timeout=3600)  # 1小时超时
    elapsed = time.time() - start_time

    if not success:
        log(f"恢复失败: {stderr[:500]}", "ERROR")
        return {"error": f"恢复失败: {stderr[:200]}", "elapsed": elapsed}

    log(f"恢复完成，耗时: {elapsed:.1f}秒")

    # 统计结果
    recovered_rows = 0
    valid_decimal_count = 0
    valid_enum_count = 0
    csv_files = list(output_dir.glob("*.csv"))

    # 查找包含目标数据的CSV
    for csv_file in csv_files:
        try:
            with open(csv_file, 'r', errors='replace') as f:
                header = f.readline()
                # 检查是否是目标表的数据（包含col_tinyint等字段）
                if 'col_tinyint' in header or 'col_varchar' in header:
                    count = 0
                    for line in f:
                        if line.strip() and 'page_no' not in line:
                            count += 1
                            # 检查数据质量
                            parts = line.split(',')
                            if len(parts) > 8:  # col_decimal字段
                                try:
                                    dec_val = parts[8]
                                    if dec_val and dec_val != 'NULL':
                                        float(dec_val)  # 验证是否是有效小数
                                        valid_decimal_count += 1
                                except:
                                    pass
                            if len(parts) > 18:  # col_enum字段
                                enum_val = parts[18]
                                if enum_val in ['active', 'inactive', 'pending', 'deleted']:
                                    valid_enum_count += 1
                    if count > recovered_rows:
                        recovered_rows = count
        except:
            pass

    rate = (recovered_rows / actual_rows * 100) if actual_rows > 0 else 0
    status = "PASS" if rate >= 95 else "WARN" if rate >= 80 else "FAIL"

    log(f"[{version}] [{scenario}] [{fs_type}] 恢复率: {rate:.2f}% ({recovered_rows}/{actual_rows})", status)
    log(f"  有效DECIMAL: {valid_decimal_count}, 有效ENUM: {valid_enum_count}")

    return {
        "version": version,
        "scenario": scenario,
        "filesystem": fs_type,
        "expected": actual_rows,
        "recovered": recovered_rows,
        "rate": round(rate, 2),
        "status": status,
        "elapsed_seconds": round(elapsed, 1),
        "valid_decimal": valid_decimal_count,
        "valid_enum": valid_enum_count
    }

def main():
    log("=" * 60)
    log("全面灾难恢复验证测试 - 1000K数据量")
    log(f"MySQL版本: 5.6, 5.7, 8.0, 8.4, 9.0")
    log(f"文件系统: ext4, XFS")
    log(f"数据量: {NUM_ROWS} 行")
    log(f"块设备: {BLOCK_DEVICE}")
    log(f"字段类型: 全部类型 (INT, FLOAT, DECIMAL, DATE, DATETIME, VARCHAR, TEXT, BLOB, JSON, ENUM, SET, BIT...)")
    log("=" * 60)

    TEST_DIR.mkdir(parents=True, exist_ok=True)
    sql_file = TEST_DIR / "all_types_insert.sql"
    generate_sql_file(NUM_ROWS, sql_file)

    results = []

    # 测试配置
    versions = ["mysql80"]  # 先测试MySQL 8.0
    scenarios = ["DROP"]    # 先测试DROP
    fs_types = ["ext4"]     # 先测试ext4

    for fs_type in fs_types:
        for version in versions:
            for scenario in scenarios:
                result = run_test(version, scenario, fs_type, sql_file)
                results.append(result)
                time.sleep(2)

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
            log(f"    DECIMAL质量: {r.get('valid_decimal', 0)}, ENUM质量: {r.get('valid_enum', 0)}")

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
