#!/usr/bin/env python3
"""从宿主机插入测试数据到 Podman MySQL 容器"""
import mysql.connector
from mysql.connector import Error
import sys

def insert_test_data():
    try:
        conn = mysql.connector.connect(
            host='localhost',
            port=3306,
            user='root',
            database='recovery_test',
            password=''
        )
        cursor = conn.cursor()
        
        cursor.execute("SET SQL_MODE='NO_ENGINE_SUBSTITUTION'")
        cursor.execute("SET autocommit=0")
        conn.commit()
        
        start_id = 1
        total_records = 5200000
        batch_size = 100000
        
        for batch in range(52):
            batch_start = start_id + batch * batch_size
            batch_end = min(batch_start + batch_size - 1, total_records)
            
            if batch_start > total_records:
                break
            
            values = []
            for i in range(batch_start, batch_end + 1):
                year = 2020 + (i % 4)
                month = 1 + (i % 12)
                day = 1 + (i % 28)
                
                user_name = f"user_{i:08d}"
                email = f"user_{i:08d}@test.com"
                age = 18 + (i % 80)
                created_at = f"{year}-{month:02d}-{day:02d} 12:00:00"
                status = i % 3
                data = f"data_{i * 12345 + 67890}"
                
                values.append((i, user_name, email, age, created_at, status, data))
            
            placeholders = ','.join(['(%s,%s,%s,%s,%s,%s,%s)'] * len(values))
            sql = f"INSERT INTO test_recovery_big (id, user_name, email, age, created_at, status, data) VALUES {placeholders}"
            
            flat_values = []
            for v in values:
                flat_values.extend(v)
            
            cursor.execute(sql, flat_values)
            conn.commit()
            
            progress = (batch_end / total_records) * 100
            print(f"Batch {batch + 1}: Inserted {batch_end:,} / {total_records:,} ({progress:.1f}%)")
        
        cursor.execute("SELECT COUNT(*) FROM test_recovery_big")
        count = cursor.fetchone()[0]
        print(f"\nTotal records inserted: {count:,}")
        
    except Error as e:
        print(f"Error: {e}")
    finally:
        if conn.is_connected():
            cursor.close()
            conn.close()

if __name__ == "__main__":
   insert_test_data()
