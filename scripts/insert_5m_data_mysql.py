#!/usr/bin/env python3
# =============================================================================
# MySQL 5.6 5M+ Data Insertion Script
# Author: Qwen Code
# Purpose: Insert 5,000,000 records into MySQL 5.6 for recovery testing
# =============================================================================

import os
import sys
import argparse
import mysql.connector
from mysql.connector import Error
from datetime import datetime, timedelta
import random
import hashlib
import json
import time

# Test data patterns
NAMES = [
    "张伟", "李娜", "王芳", "刘强", "陈静", "杨洋", "赵敏", "黄勇", "周杰", "吴刚",
    "徐丽", "孙浩", "朱伟", "胡敏", "郭强", "何燕", "高杰", "林娜", "郑勇", "谢芳",
    "王磊", "张敏", "李杰", "刘洋", "陈勇", "杨静", "赵强", "黄丽", "周军", "吴敏",
    "徐涛", "孙鹏", "朱磊", "胡静", "郭勇", "何敏", "高洁", "林敏", "郑强", "谢芳",
    "陈东", "王军", "李红", "张华", "刘伟", "杨柳", "赵峰", "黄萍", "周军", "吴敏",
]

EMAIL_DOMAINS = ["gmail.com", "yahoo.com", "hotmail.com", "outlook.com",
                "qq.com", "163.com", "126.com", "sina.com", "sohu.com", "weibo.com"]

USERNAMES = [f"user_{i:06d}" for i in range(10000)]

STATUSES = [0, 1, 2, 3]


def create_connection(host, port, user, password, database):
    """Create MySQL connection"""
    try:
        connection = mysql.connector.connect(
            host=host,
            port=port,
            user=user,
            password=password,
            database=database,
            autocommit=True,
            connection_timeout=300
        )
        print(f"Connected to MySQL {host}:{port}")
        return connection
    except Error as e:
        print(f"Error connecting to MySQL: {e}")
        return None


def generate_record(record_id, seed=42):
    """Generate a single deterministic test record"""
    random.seed(seed + record_id)
    
    username = USERNAMES[record_id % len(USERNAMES)]
    
    record = {
        'id': record_id + 1,
        'name': random.choice(NAMES),
        'email': f"{username}@{random.choice(EMAIL_DOMAINS)}",
        'username': username,
        'password_hash': hashlib.sha256(f"password_{record_id}".encode()).hexdigest(),
        'status': random.choice(STATUSES),
        'age': random.randint(18, 80),
        'score': random.randint(0, 1000),
        'description': f"测试数据记录_{record_id:06d}_{''.join(random.choices('abcdefghijklmnopqrstuvwxyz', k=50))}",
        'score_float': round(random.uniform(0, 1000), 2),
        'score_double': round(random.uniform(0, 1000000), 6),
        'balance': round(random.uniform(0, 100000), 2),
        'created_at': (datetime(2020, 1, 1) + timedelta(days=random.randint(0, 1460))).strftime('%Y-%m-%d %H:%M:%S'),
        'updated_at': (datetime(2020, 1, 1) + timedelta(days=random.randint(0, 1460))).strftime('%Y-%m-%d %H:%M:%S'),
    }
    
    return record


def get_insert_statement():
    """Return the INSERT SQL statement"""
    return """
        INSERT INTO users (username, email, age, created_at, status, bio, score, data)
        VALUES (%s, %s, %s, %s, %s, %s, %s, %s)
    """


def insert_batch(connection, records):
    """Insert a batch of records"""
    try:
        cursor = connection.cursor()
        data = [
            (
                r['username'],
                r['email'],
                r['age'],
                r['created_at'],
                r['status'],
                r['description'],
                r['score_float'],
                r['password_hash']  # Using password_hash as BLOB data
            )
            for r in records
        ]
        cursor.executemany(get_insert_statement(), data)
        connection.commit()
        cursor.close()
        return True
    except Error as e:
        print(f"Error inserting batch: {e}")
        return False


def get_table_info(connection, database, table):
    """Get table information"""
    try:
        cursor = connection.cursor(dictionary=True)
        cursor.execute(f"""
            SELECT 
                table_rows as rows,
                data_length,
                index_length,
                round((data_length + index_length) / 1024 / 1024, 2) as size_mb
            FROM information_schema.tables 
            WHERE table_schema = %s AND table_name = %s
        """, (database, table))
        info = cursor.fetchone()
        cursor.close()
        return info
    except Error as e:
        print(f"Error getting table info: {e}")
        return None


def generate_golden_hashes(records):
    """Generate golden hashes for validation"""
    hashes = {}
    for record in records:
        key = record['username']  # Using username as unique key
        # Create a hash of the record data
        hash_input = f"{record['username']}|{record['email']}|{record['age']}|{record['score_float']}"
        hashes[key] = hashlib.md5(hash_input.encode()).hexdigest()
    return hashes


def main():
    parser = argparse.ArgumentParser(description='Insert 5M+ records into MySQL 5.6')
    parser.add_argument('--host', type=str, default='localhost',
                       help='MySQL host (default: localhost)')
    parser.add_argument('--port', type=int, default=33056,
                       help='MySQL port (default: 33056)')
    parser.add_argument('--user', type=str, default='root',
                       help='MySQL user (default: root)')
    parser.add_argument('--password', type=str, default='root123',
                       help='MySQL password (default: root123)')
    parser.add_argument('--database', type=str, default='testdb',
                       help='MySQL database (default: testdb)')
    parser.add_argument('--table', type=str, default='users',
                       help='MySQL table (default: users)')
    parser.add_argument('--records', type=int, default=5000000,
                       help='Number of records to insert (default: 5000000)')
    parser.add_argument('--batch-size', type=int, default=10000,
                       help='Batch size for insertions (default: 10000)')
    parser.add_argument('--output-dir', type=str, default='./golden-hashes',
                       help='Output directory for golden hashes')
    
    args = parser.parse_args()
    
    print(f"Starting data insertion...")
    print(f"Host: {args.host}:{args.port}")
    print(f"Database: {args.database}")
    print(f"Records: {args.records:,}")
    print(f"Batch size: {args.batch_size:,}")
    print()
    
    # Create connection
    connection = create_connection(args.host, args.port, args.user, args.password, args.database)
    if not connection:
        return 1
    
    try:
        cursor = connection.cursor()
        
        # Create table if not exists (simplified schema for MySQL 5.6)
        cursor.execute(f"""
            CREATE TABLE IF NOT EXISTS {args.table} (
                id BIGINT PRIMARY KEY AUTO_INCREMENT,
                username VARCHAR(50) NOT NULL,
                email VARCHAR(100) NOT NULL,
                age INT,
                created_at DATETIME,
                status TINYINT DEFAULT 1,
                bio TEXT,
                score DECIMAL(10,2),
                data BLOB,
                INDEX idx_username (username),
                INDEX idx_email (email)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=DYNAMIC
        """)
        print(f"Table '{args.table}' ready")
        cursor.close()
        
        # Clear existing data
        cursor = connection.cursor()
        cursor.execute(f"TRUNCATE TABLE {args.table}")
        print(f"Table '{args.table}' cleared")
        cursor.close()
        
        # Insert data
        total_records = args.records
        inserted = 0
        start_time = time.time()
        
        for batch_num in range(0, total_records, args.batch_size):
            batch_records = []
            for i in range(batch_num, min(batch_num + args.batch_size, total_records)):
                record = generate_record(i, seed=42)
                batch_records.append(record)
            
            if insert_batch(connection, batch_records):
                inserted += len(batch_records)
                
                # Progress report every 500k records
                if inserted % 500000 == 0:
                    elapsed = time.time() - start_time
                    rate = inserted / elapsed if elapsed > 0 else 0
                    print(f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] Inserted {inserted:,}/{total_records:,} records "
                          f"({rate:.0f} records/sec)")
            
            # Commit periodically
            connection.commit()
            
        elapsed = time.time() - start_time
        rate = inserted / elapsed if elapsed > 0 else 0
        
        print(f"\n{'='*60}")
        print(f"Insertion Complete!")
        print(f"{'='*60}")
        print(f"Total Records Inserted: {inserted:,}")
        print(f"Elapsed Time: {elapsed:.2f} seconds")
        print(f"Average Rate: {rate:.0f} records/sec")
        
        # Get table info
        info = get_table_info(connection, args.database, args.table)
        if info:
            print(f"Table Size: {info['size_mb']:.2f} MB")
            print(f"Rows: {info['rows']:,}")
        
        # Generate golden hashes for last batch
        final_batch = []
        for i in range(inserted - min(1000, inserted), inserted):
            record = generate_record(i, seed=42)
            final_batch.append(record)
        
        golden_hashes = generate_golden_hashes(final_batch)
        
        # Save golden hashes
        os.makedirs(args.output_dir, exist_ok=True)
        hash_file = os.path.join(args.output_dir, 'golden_hashes_5m.json')
        with open(hash_file, 'w', encoding='utf-8') as f:
            json.dump(golden_hashes, f, indent=2, ensure_ascii=False)
        print(f"Golden hashes saved to: {hash_file}")
        
        cursor.close()
        connection.close()
        
        return 0
        
    except Error as e:
        print(f"Error: {e}")
        return 1


if __name__ == '__main__':
    sys.exit(main())
