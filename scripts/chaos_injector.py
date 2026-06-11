# Chaos Injector Script - Populate MySQL Database with Test Data
# Author: jliang.fang (Global MySQL Kernel QA Expert)
# Purpose: Generate test data for MySQL Recovery Pro validation

import sys
import os
import json
import hashlib
import random
import argparse
from datetime import datetime, timedelta
from typing import List, Dict, Optional

try:
    import mysql.connector
    from mysql.connector import Error
except ImportError:
    print("Please install mysql-connector-python: pip3 install mysql-connector-python")
    sys.exit(1)


class DataInjector:
    """Inject test data into MySQL database"""
    
    CHINESE_NAMES = [
        "张伟", "李娜", "王芳", "刘强", "陈静", "杨洋", "赵敏", "黄勇", "周杰", "吴刚",
        "徐丽", "孙浩", "朱伟", "胡敏", "郭强", "何燕", "高杰", "林娜", "郑勇", "谢芳",
        "王磊", "张敏", "李杰", "刘洋", "陈勇", "杨静", "赵强", "黄丽", "周杰", "吴刚",
        "徐涛", "孙鹏", "朱磊", "胡静", "郭勇", "何敏", "高杰", "林娜", "郑强", "谢芳",
        "陈东", "王军", "李红", "张华", "刘伟", "杨柳", "赵峰", "黄萍", "周军", "吴敏",
    ]
    
    EMAIL_DOMAINS = ["gmail.com", "yahoo.com", "hotmail.com", "outlook.com", "qq.com",
                    "163.com", "126.com", "sina.com", "sohu.com", "weibo.com"]
    
    USERNAMES = [
        "user_001", "user_002", "user_003", "user_004", "user_005",
        "admin", "root", "test", "guest", "demo", "developer", "operator"
    ]
    
    STATUSES = [0, 1, 2, 3]
    CATEGORIES = ['A', 'B', 'C', 'D', 'E']
    
    def __init__(self, seed: int = 42):
        self.seed = seed
        random.seed(seed)
        self.insert_count = 0
        self.blobs_inserted = 0
    
    def generate_name(self) -> str:
        return random.choice(self.CHINESE_NAMES)
    
    def generate_email(self) -> str:
        username = f"user_{random.randint(10000, 99999)}"
        domain = random.choice(self.EMAIL_DOMAINS)
        return f"{username}@{domain}"
    
    def generate_username(self) -> str:
        return random.choice(self.USERNAMES)
    
    def generate_password_hash(self) -> str:
        password = f"password_{random.randint(100000, 999999)}"
        return hashlib.sha256(password.encode()).hexdigest()
    
    def generate_status(self) -> int:
        return random.choice(self.STATUSES)
    
    def generate_score(self) -> int:
        return random.randint(0, 1000)
    
    def generate_json_data(self) -> str:
        patterns = [
            {"type": "customer", "level": 1, "tags": ["vip", "premium"]},
            {"type": "customer", "level": 2, "tags": ["vip", "gold"]},
            {"type": "customer", "level": 3, "tags": ["vip", "platinum"]},
            {"type": "vendor", "level": 1, "tags": ["verified"]},
            {"type": "vendor", "level": 2, "tags": ["verified", "premium"]},
            {"type": "system", "level": 0, "tags": ["admin"]},
            {"type": "guest", "level": 0, "tags": ["anonymous"]},
        ]
        pattern = random.choice(patterns)
        pattern["timestamp"] = datetime.now().isoformat()
        pattern["random_value"] = round(random.random(), 6)
        pattern["score"] = self.generate_score()
        return json.dumps(pattern, ensure_ascii=False)
    
    def generate_blob_data(self, size: int = 1024) -> bytes:
        return bytes([random.randint(0, 255) for _ in range(size)])
    
    def generate_text_description(self, min_words: int = 10, max_words: int = 50) -> str:
        words = [
            "测试", "数据", "记录", "用户", "系统", "应用", "开发", "运维", "数据库",
            "MySQL", "InnoDB", "恢复", "备份", "查询", "更新", "插入", "删除", "表",
            "索引", "页", "块", "事务", "提交", "回滚", "锁", "并发", "性能", "优化"
        ]
        num_words = random.randint(min_words, max_words)
        return " ".join(random.choice(words) for _ in range(num_words))
    
    def generate_datetime(self, start_year: int = 2020, end_year: int = 2024) -> datetime:
        start = datetime(start_year, 1, 1)
        end = datetime(end_year, 12, 31)
        delta = end - start
        random_days = random.randint(0, delta.days)
        random_seconds = random.randint(0, 86400)
        return start + timedelta(days=random_days, seconds=random_seconds)
    
    def generate_records(self, num_records: int, blob_size_kb: int = 1) -> List[Dict]:
        """Generate test records"""
        records = []
        blob_size = blob_size_kb * 1024
        
        for i in range(num_records):
            record = {
                'id': i + 1,
                'name': self.generate_name(),
                'email': self.generate_email(),
                'username': self.generate_username(),
                'password_hash': self.generate_password_hash(),
                'status': self.generate_status(),
                'score': self.generate_score(),
                'description': self.generate_text_description(),
                'json_data': self.generate_json_data(),
                'blob_data': self.generate_blob_data(blob_size),
                'created_at': self.generate_datetime(),
                'updated_at': self.generate_datetime(),
                'created_date': self.generate_datetime().date(),
                'created_time': self.generate_datetime().time(),
                'score_float': round(random.uniform(0, 1000), 2),
                'score_double': round(random.uniform(0, 1000000), 6),
                'balance': round(random.uniform(0, 100000), 2),
            }
            records.append(record)
        
        return records
    
    def insert_records(self, conn: mysql.connector.connection.MySQLConnection, 
                       records: List[Dict]) -> int:
        """Insert records into database"""
        cursor = conn.cursor()
        
        insert_sql = """
        INSERT INTO test_table 
        (id, name, email, username, password_hash, status, score, description, 
         json_data, blob_data, created_at, updated_at, created_date, created_time,
         score_float, score_double, balance)
        VALUES (%(id)s, %(name)s, %(email)s, %(username)s, %(password_hash)s, 
                %(status)s, %(score)s, %(description)s, 
                %(json_data)s, %(blob_data)s, %(created_at)s, %(updated_at)s, 
                %(created_date)s, %(created_time)s,
                %(score_float)s, %(score_double)s, %(balance)s)
        ON DUPLICATE KEY UPDATE
            name = VALUES(name),
            updated_at = VALUES(updated_at)
        """
        
        batch_size = 1000
        total_inserted = 0
        
        for i in range(0, len(records), batch_size):
            batch = records[i:i + batch_size]
            
            try:
                cursor.executemany(insert_sql, batch)
                conn.commit()
                total_inserted += len(batch)
                self.insert_count += len(batch)
                
                if total_inserted % 10000 == 0:
                    print(f"  Inserted {total_inserted} records...")
                    
            except Error as e:
                print(f"  Error inserting batch: {e}")
                conn.rollback()
        
        cursor.close()
        return total_inserted
    
    def insert_varchar_records(self, conn: mysql.connector.connection.MySQLConnection, 
                                num_records: int) -> int:
        """Insert records into test_varchar table"""
        cursor = conn.cursor()
        
        insert_sql = """
        INSERT INTO test_varchar (id, short_varchar, medium_varchar, long_varchar)
        VALUES (%s, %s, %s, %s)
        ON DUPLICATE KEY UPDATE
            medium_varchar = VALUES(medium_varchar)
        """
        
        records = []
        for i in range(num_records):
            records.append((
                i + 1,
                ''.join(random.choices('abcdefghijklmnopqrstuvwxyz', k=50)),
                self.generate_text_description(100, 200),
                self.generate_text_description(500, 1000)
            ))
        
        batch_size = 1000
        for i in range(0, len(records), batch_size):
            batch = records[i:i + batch_size]
            cursor.executemany(insert_sql, batch)
            conn.commit()
        
        cursor.close()
        return len(records)
    
    def insert_datetime_records(self, conn: mysql.connector.connection.MySQLConnection, 
                                 num_records: int) -> int:
        """Insert records into test_datetime table"""
        cursor = conn.cursor()
        
        insert_sql = """
        INSERT INTO test_datetime (id, dt, ts, tm, yd)
        VALUES (%s, %s, %s, %s, %s)
        ON DUPLICATE KEY UPDATE
            dt = VALUES(dt)
        """
        
        records = []
        for i in range(num_records):
            dt = self.generate_datetime()
            tm = dt.time()
            records.append((
                i + 1,
                dt,
                dt,
                tm,
                dt.year
            ))
        
        batch_size = 1000
        for i in range(0, len(records), batch_size):
            batch = records[i:i + batch_size]
            cursor.executemany(insert_sql, batch)
            conn.commit()
        
        cursor.close()
        return len(records)
    
    def insert_numeric_records(self, conn: mysql.connector.connection.MySQLConnection, 
                                num_records: int) -> int:
        """Insert records into test_numeric table"""
        cursor = conn.cursor()
        
        insert_sql = """
        INSERT INTO test_numeric 
        (id, bi, si, ti, mi, ii, flt, dbl, dec_val, bit_val, alloc_date, alloc_time)
        VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
        ON DUPLICATE KEY UPDATE
            bi = VALUES(bi)
        """
        
        records = []
        for i in range(num_records):
            records.append((
                i + 1,
                random.randint(-9223372036854775808, 9223372036854775807),
                random.randint(-32768, 32767),
                random.randint(-128, 127),
                random.randint(-8388608, 8388607),
                random.randint(-2147483648, 2147483647),
                round(random.uniform(-3.4e38, 3.4e38), 6),
                round(random.uniform(-1.7e308, 1.7e308), 15),
                round(random.uniform(-1e30, 1e30), 10),
                random.randint(0, 2**64 - 1),
                self.generate_datetime().date(),
                self.generate_datetime().time()
            ))
        
        batch_size = 1000
        for i in range(0, len(records), batch_size):
            batch = records[i:i + batch_size]
            cursor.executemany(insert_sql, batch)
            conn.commit()
        
        cursor.close()
        return len(records)
    
    def insert_blob_records(self, conn: mysql.connector.connection.MySQLConnection, 
                            num_records: int, blob_size_kb: int) -> int:
        """Insert records into test_blob table"""
        cursor = conn.cursor()
        
        insert_sql = """
        INSERT INTO test_blob (id, tiny_blob, normal_blob, medium_blob, long_blob)
        VALUES (%s, %s, %s, %s, %s)
        ON DUPLICATE KEY UPDATE
            normal_blob = VALUES(normal_blob)
        """
        
        blob_size = blob_size_kb * 1024
        records = []
        for i in range(num_records):
            # BLOB sizes: TINY (255), normal (64KB), medium (16MB), long (4GB)
            records.append((
                i + 1,
                os.urandom(min(255, blob_size)),
                os.urandom(min(65535, blob_size)),
                os.urandom(min(16777215, blob_size)),
                os.urandom(blob_size)
            ))
        
        batch_size = 100
        for i in range(0, len(records), batch_size):
            batch = records[i:i + batch_size]
            cursor.executemany(insert_sql, batch)
            conn.commit()
        
        cursor.close()
        self.blobs_inserted += num_records
        return len(records)


def create_connection(host: str, port: int, user: str, password: str) -> Optional[mysql.connector.connection.MySQLConnection]:
    """Create MySQL connection"""
    try:
        conn = mysql.connector.connect(
            host=host,
            port=port,
            user=user,
            password=password,
            database='testdb',
            connection_timeout=60
        )
        if conn.is_connected():
            print(f"  Connected to MySQL at {host}:{port}")
            return conn
    except Error as e:
        print(f"  Error connecting to MySQL: {e}")
    return None


def main():
    parser = argparse.ArgumentParser(
        description="Chaos Data Injector for MySQL Recovery Pro",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Usage Examples:
  python3 chaos_injector.py --host localhost --port 3306 --num-records 100000 --output /tmp/hashes.json
  python3 chaos_injector.py --host mysql-8.0 --port 3306 --num-records 100000 --blob-size 32 --output /tmp/hashes.json
        """
    )
    
    parser.add_argument('--host', type=str, default='localhost',
                       help='MySQL host')
    parser.add_argument('--port', type=int, default=3306,
                       help='MySQL port')
    parser.add_argument('--user', type=str, default='root',
                       help='MySQL user')
    parser.add_argument('--password', type=str, default='root123',
                       help='MySQL password')
    parser.add_argument('--database', type=str, default='testdb',
                       help='Database name')
    parser.add_argument('--num-records', type=int, default=100000,
                       help='Number of records to generate')
    parser.add_argument('--blob-size', type=int, default=1,
                       help='BLOB size in KB')
    parser.add_argument('--seed', type=int, default=42,
                       help='Random seed for reproducibility')
    parser.add_argument('--output', type=str, default=None,
                       help='Output file for hash verification')
    
    args = parser.parse_args()
    
    print("=" * 60)
    print("MySQL Chaos Data Injector")
    print("=" * 60)
    print(f"Host: {args.host}:{args.port}")
    print(f"Records: {args.num_records}")
    print(f"BLOB Size: {args.blob_size}KB")
    print(f"Seed: {args.seed}")
    print()
    
    # Connect to MySQL
    conn = create_connection(args.host, args.port, args.user, args.password)
    if not conn:
        print("Failed to connect to MySQL. Exiting.")
        sys.exit(1)
    
    try:
        # Check if table exists
        cursor = conn.cursor()
        cursor.execute("SHOW TABLES LIKE 'test_table'")
        table_exists = cursor.fetchone() is not None
        
        if not table_exists:
            print(f"Table test_table does not exist in database {args.database}")
            cursor.close()
            conn.close()
            print("Please run schema script first. Exiting.")
            sys.exit(1)
        
        cursor.close()
        
        # Generate data
        injector = DataInjector(seed=args.seed)
        print(f"Generating {args.num_records} records...")
        records = injector.generate_records(args.num_records, args.blob_size)
        print(f"Generated {len(records)} records")
        
        # Insert records
        print(f"Inserting records into database...")
        inserted = injector.insert_records(conn, records)
        print(f"Inserted {inserted} records")
        
        # Insert additional table data
        print(f"Inserting additional table data...")
        injector.insert_varchar_records(conn, 10000)
        injector.insert_datetime_records(conn, 10000)
        injector.insert_numeric_records(conn, 10000)
        injector.insert_blob_records(conn, 100, args.blob_size)
        
        # Generate output hash file if specified
        if args.output:
            print(f"Generating hash verification file...")
            hash_data = {
                "generated_at": datetime.now().isoformat(),
                "seed": args.seed,
                "total_records": args.num_records,
                "blobs_inserted": injector.blobs_inserted,
                "verification": {
                    "sample_hash": hashlib.sha256(
                        json.dumps(records[:100], default=str).encode()
                    ).hexdigest()
                }
            }
            
            os.makedirs(os.path.dirname(args.output), exist_ok=True)
            with open(args.output, 'w') as f:
                json.dump(hash_data, f, indent=2)
            
            print(f"Hash file saved to {args.output}")
        
        # Print summary
        print()
        print("=" * 60)
        print("Injection Summary")
        print("=" * 60)
        print(f"Total Records Inserted: {injector.insert_count}")
        print(f"BLOB Records Inserted: {injector.blobs_inserted}")
        print(f"Data Size: {args.num_records * args.blob_size / 1024:.2f} MB")
        print()
        print("Next Step: Run chaos tests...")
        
    finally:
        if conn.is_connected():
            conn.close()
            print("MySQL connection closed")


if __name__ == "__main__":
    main()
