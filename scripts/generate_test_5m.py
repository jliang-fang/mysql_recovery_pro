#!/bin/bash
# 更高效的批量数据生成脚本 - 使用 Python 直连

DB_HOST="localhost"
DB_PORT="3306"
DB_USER="root"
DB_NAME="recovery_test"
TABLE_NAME="test_recovery_big"
TOTAL_RECORDS=5200000
BATCH_SIZE=10000

echo "开始生成 ${TOTAL_RECORDS} 条测试数据..."

podman exec mysql56_recovery_test python3 - << 'PYTHON_SCRIPT'
import MySQLdb
import random
import sys
import time

# 连接到容器内的 MySQL (使用 socket)
conn = MySQLdb.connect(
    user='root',
    db='recovery_test',
    unix_socket='/var/lib/mysql/mysql.sock'
)
cursor = conn.cursor()

# 设置 SQL 模式
cursor.execute("SET SQL_MODE='';")
cursor.execute("SET autocommit=0;")

total_records = 5200000
batch_size = 10000
start_time = time.time()

for batch in range(0, total_records, batch_size):
    end = min(batch + batch_size, total_records)
    
    values = []
    for i in range(batch, end):
        user_name = f"user_{i:08d}"
        email = f"user_{i:08d}@test.com"
        age = random.randint(18, 97)
        year = 2020 + random.randint(0, 3)
        month = random.randint(1, 12)
        day = random.randint(1, 28)
        created_at = f"{year}-{month:02d}-{day:02d} 12:00:00"
        status = random.randint(0, 2)
        data = f"data_{random.randint(1000000000, 9999999999)}"
        values.append((user_name, email, age, created_at, status, data))
    
    sql = f"INSERT INTO test_recovery_big (user_name, email, age, created_at, status, data) VALUES {','.join(['%s']*len(values))}"
    
    # 重新格式化 SQL
    placeholders = ','.join(['(%s,%s,%s,%s,%s,%s)'] * len(values))
    sql = f"INSERT INTO test_recovery_big (user_name, email, age, created_at, status, data) VALUES {placeholders}"
    
    flat_values = []
    for v in values:
        flat_values.extend(v)
    
    cursor.execute(sql, flat_values)
    conn.commit()
    
    elapsed = time.time() - start_time
    progress = (end / total_records) * 100
    print(f"插入 {end:,} / {total_records:,} 条 ({progress:.1f}%) - 耗时 {elapsed:.1f}s", flush=True)

# 最终统计
cursor.execute("SELECT COUNT(*) FROM test_recovery_big")
count = cursor.fetchone()[0]
print(f"\n总计插入: {count:,} 条记录")

conn.close()
PYTHON_SCRIPT
