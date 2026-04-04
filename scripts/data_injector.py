#!/usr/bin/env python3
"""
Data Injection Script for MySQL Recovery Pro
Generates 100,000 records with complex data types for testing
"""

import os
import sys
import json
import random
import hashlib
import argparse
from datetime import datetime, timedelta
from typing import List, Dict, Tuple
from pathlib import Path


class DataGenerator:
    """Generate test data with various complex types"""
    
    # Common Chinese names for realistic data
    CHINESE_NAMES = [
        "张伟", "李娜", "王芳", "刘强", "陈静", "杨洋", "赵敏", "黄勇", "周杰", "吴刚",
        "徐丽", "孙浩", "朱伟", "胡敏", "郭强", "何燕", "高杰", "林娜", "郑勇", "谢芳",
        "王磊", "张敏", "李杰", "刘洋", "陈勇", "杨静", "赵强", "黄丽", "周杰", "吴刚",
        "徐涛", "孙鹏", "朱磊", "胡静", "郭勇", "何敏", "高杰", "林娜", "郑强", "谢芳"
    ]
    
    # Common email domains
    EMAIL_DOMAINS = ["gmail.com", "yahoo.com", "hotmail.com", "outlook.com", "qq.com", 
                    "163.com", "126.com", "sina.com", "sohu.com", "weibo.com"]
    
    # Usernames
    USERNAMES = [
        "user_001", "user_002", "user_003", "user_004", "user_005",
        "admin", "root", "test", "guest", "demo", "developer", "operator"
    ]
    
    # Status values
    STATUSES = [0, 1, 2, 3]  # inactive, active, pending, banned
    
    # Complex data patterns
    JSON_PATTERNS = [
        {"type": "customer", "level": 1, "tags": ["vip", "premium"]},
        {"type": "customer", "level": 2, "tags": ["vip", "gold"]},
        {"type": "customer", "level": 3, "tags": ["vip", "platinum"]},
        {"type": "vendor", "level": 1, "tags": ["verified"]},
        {"type": "vendor", "level": 2, "tags": ["verified", "premium"]},
        {"type": "system", "level": 0, "tags": ["admin"]},
        {"type": "guest", "level": 0, "tags": ["anonymous"]},
    ]
    
    def __init__(self, seed: int = None):
        if seed:
            random.seed(seed)
        else:
            random.seed(datetime.now().microsecond)
    
    def generate_name(self) -> str:
        """Generate a random Chinese name"""
        return random.choice(self.CHINESE_NAMES)
    
    def generate_email(self) -> str:
        """Generate a random email"""
        username = f"user_{random.randint(10000, 99999)}"
        domain = random.choice(self.EMAIL_DOMAINS)
        return f"{username}@{domain}"
    
    def generate_username(self) -> str:
        """Generate a random username"""
        return random.choice(self.USERNAMES)
    
    def generate_password_hash(self) -> str:
        """Generate a password hash (SHA-256)"""
        password = f"password_{random.randint(100000, 999999)}"
        return hashlib.sha256(password.encode()).hexdigest()
    
    def generate_status(self) -> int:
        """Generate a random status"""
        return random.choice(self.STATUSES)
    
    def generate_score(self) -> int:
        """Generate a random score (0-1000)"""
        return random.randint(0, 1000)
    
    def generate_json_data(self) -> str:
        """Generate random JSON data"""
        pattern = random.choice(self.JSON_PATTERNS)
        pattern["timestamp"] = datetime.now().isoformat()
        pattern["random_value"] = random.random()
        return json.dumps(pattern, ensure_ascii=False)
    
    def generate_blob_data(self, size: int = 1024) -> bytes:
        """Generate random BLOB data"""
        return bytes([random.randint(0, 255) for _ in range(size)])
    
    def generate_text_description(self, min_words: int = 10, max_words: int = 50) -> str:
        """Generate a random text description"""
        words = [
            "测试", "数据", "记录", "用户", "系统", "应用", "开发", "运维", "数据库",
            "MySQL", "InnoDB", "恢复", "备份", "查询", "更新", "插入", "删除", "表",
            "索引", "页", "块", "事务", "提交", "回滚", "锁", "并发", "性能", "优化"
        ]
        num_words = random.randint(min_words, max_words)
        return " ".join(random.choice(words) for _ in range(num_words))
    
    def generate_datetime(self, start_year: int = 2020, end_year: int = 2024) -> datetime:
        """Generate a random datetime"""
        start = datetime(start_year, 1, 1)
        end = datetime(end_year, 12, 31)
        delta = end - start
        random_days = random.randint(0, delta.days)
        random_seconds = random.randint(0, 86400)
        return start + timedelta(days=random_days, seconds=random_seconds)
    
    def generate_date(self) -> str:
        """Generate a random date string"""
        dt = self.generate_datetime()
        return dt.strftime("%Y-%m-%d")
    
    def generate_time(self) -> str:
        """Generate a random time string"""
        dt = self.generate_datetime()
        return dt.strftime("%H:%M:%S")
    
    def generate_vector_90(self, dimension: int = 128) -> str:
        """Generate a random vector for MySQL 9.0 (JSON format)"""
        vector = [round(random.gauss(0, 1), 6) for _ in range(dimension)]
        return json.dumps(vector, ensure_ascii=False)
    
    def generate_record(self, record_id: int, include_vector: bool = False) -> Dict:
        """Generate a single test record"""
        record = {
            "id": record_id,
            "name": self.generate_name(),
            "email": self.generate_email(),
            "username": self.generate_username(),
            "password_hash": self.generate_password_hash(),
            "status": self.generate_status(),
            "score": self.generate_score(),
            "description": self.generate_text_description(),
            "json_data": self.generate_json_data(),
            "blob_data": self.generate_blob_data().hex(),  # Store as hex string
            "created_at": self.generate_datetime().isoformat(),
            "updated_at": self.generate_datetime().isoformat(),
            "created_date": self.generate_date(),
            "created_time": self.generate_time(),
        }
        
        if include_vector:
            record["vector_data"] = self.generate_vector_90()
        
        return record


class DataInjector:
    """Inject test data into MySQL database"""
    
    def __init__(self, host: str = "localhost", port: int = 3306,
                 user: str = "root", password: str = "test123",
                 database: str = "testdb"):
        self.host = host
        self.port = port
        self.user = user
        self.password = password
        self.database = database
        self.generator = DataGenerator()

    def create_connection(self):
        """Create MySQL connection"""
        try:
            import mysql.connector
            from mysql.connector import Error as MySQLError

            connection = mysql.connector.connect(
                host=self.host,
                port=self.port,
                user=self.user,
                password=self.password,
                database=self.database
            )
            return connection
        except MySQLError as e:
            print(f"Error connecting to MySQL: {e}")
            return None
        except Exception as e:
            print(f"Error connecting to MySQL: {e}")
            return None
    
    def create_table(self, connection, include_vector: bool = False):
        """Create test table"""
        cursor = connection.cursor()
        
        table_sql = """
        CREATE TABLE IF NOT EXISTS test_users (
            id BIGINT PRIMARY KEY AUTO_INCREMENT,
            name VARCHAR(255),
            email VARCHAR(255),
            username VARCHAR(255),
            password_hash CHAR(64),
            status TINYINT DEFAULT 1,
            score INT,
            description TEXT,
            json_data JSON,
            blob_data LONGBLOB,
            created_at DATETIME,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
            created_date DATE,
            created_time TIME
        """
        
        if include_vector:
            table_sql += ",\n    vector_data VECTOR(128) COMMENT 'vector'"
        
        table_sql += "\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;"
        
        try:
            cursor.execute(table_sql)
            connection.commit()
            print("Table created successfully")
        except Error as e:
            print(f"Error creating table: {e}")
        finally:
            cursor.close()
    
    def insert_batch(self, connection, records: List[Dict], batch_size: int = 1000):
        """Insert records in batches"""
        cursor = connection.cursor()
        
        # Build INSERT statement
        columns = [
            "name", "email", "username", "password_hash", "status",
            "score", "description", "json_data", "blob_data",
            "created_at", "updated_at", "created_date", "created_time"
        ]
        
        if "vector_data" in records[0]:
            columns.append("vector_data")
        
        placeholders = ", ".join(["%s"] * len(columns))
        columns_str = ", ".join(columns)
        
        insert_sql = f"INSERT INTO test_users ({columns_str}) VALUES ({placeholders})"
        
        total = len(records)
        inserted = 0
        
        for i in range(0, total, batch_size):
            batch = records[i:i + batch_size]
            
            # Prepare data
            data = []
            for record in batch:
                row = [
                    record["name"], record["email"], record["username"],
                    record["password_hash"], record["status"], record["score"],
                    record["description"], record["json_data"], 
                    bytes.fromhex(record["blob_data"]),
                    record["created_at"], record["updated_at"],
                    record["created_date"], record["created_time"]
                ]
                if "vector_data" in record:
                    row.append(record["vector_data"])
                data.append(row)
            
            try:
                cursor.executemany(insert_sql, data)
                connection.commit()
                inserted += len(batch)
                
                if inserted % 10000 == 0:
                    print(f"  Inserted {inserted}/{total} records...")
            except Error as e:
                print(f"Error inserting batch: {e}")
                connection.rollback()
        
        cursor.close()
        return inserted
    
    def inject_data(self, num_records: int = 100000, batch_size: int = 1000,
                   include_vector: bool = False) -> int:
        """Inject test data into database"""
        print(f"Injecting {num_records} records into MySQL...")
        
        # Generate all records
        records = []
        for i in range(1, num_records + 1):
            record = self.generator.generate_record(i, include_vector=include_vector)
            records.append(record)
        
        # Connect to MySQL
        connection = self.create_connection()
        if not connection:
            return 0
        
        try:
            # Create table
            self.create_table(connection, include_vector=include_vector)
            
            # Insert data
            inserted = self.insert_batch(connection, records, batch_size)
            
            print(f"Successfully inserted {inserted} records")
            return inserted
            
        finally:
            if connection.is_connected():
                connection.close()
    
    def verify_data(self, connection) -> Tuple[int, int]:
        """Verify injected data"""
        cursor = connection.cursor()
        
        try:
            cursor.execute("SELECT COUNT(*) FROM test_users")
            count = cursor.fetchone()[0]
            return count, count
        finally:
            cursor.close()


class GroundTruthGenerator:
    """Generate ground truth hashes for verification"""
    
    def __init__(self, host: str = "localhost", port: int = 3306,
                 user: str = "root", password: str = "test123",
                 database: str = "testdb"):
        self.host = host
        self.port = port
        self.user = user
        self.password = password
        self.database = database
    
    def create_connection(self):
        """Create MySQL connection"""
        try:
            import mysql.connector
            from mysql.connector import Error
            
            connection = mysql.connector.connect(
                host=self.host,
                port=self.port,
                user=self.user,
                password=self.password,
                database=self.database
            )
            return connection
        except Error as e:
            print(f"Error connecting to MySQL: {e}")
            return None
    
    def generate_table_hash(self, connection, table_name: str = "test_users") -> str:
        """Generate SHA-256 hash of table data"""
        cursor = connection.cursor()
        
        try:
            # Export data as CSV-like format
            cursor.execute(f"SELECT * FROM {table_name} ORDER BY id")
            rows = cursor.fetchall()
            
            # Get column names
            cursor.execute(f"DESCRIBE {table_name}")
            columns = [row[0] for row in cursor.fetchall()]
            
            # Build hash input
            hash_input = []
            for row in rows:
                record = dict(zip(columns, row))
                # Convert bytes to hex for hashing
                for key, value in record.items():
                    if isinstance(value, bytes):
                        record[key] = value.hex()
                hash_input.append(json.dumps(record, sort_keys=True, ensure_ascii=False))
            
            # Generate hash
            hash_input_str = "\n".join(hash_input)
            return hashlib.sha256(hash_input_str.encode()).hexdigest()
            
        finally:
            cursor.close()
    
    def generate_all_hashes(self, output_file: str = "ground_truth.json"):
        """Generate and save all ground truth hashes"""
        print("Generating ground truth hashes...")
        
        connection = self.create_connection()
        if not connection:
            return None
        
        try:
            cursor = connection.cursor()
            
            # Get all tables
            cursor.execute("SHOW TABLES")
            tables = [row[0] for row in cursor.fetchall()]
            
            # Generate hashes for each table
            hashes = {}
            for table in tables:
                hash_value = self.generate_table_hash(connection, table)
                hashes[table] = hash_value
                print(f"  {table}: {hash_value}")
            
            # Save to file
            with open(output_file, 'w') as f:
                json.dump({
                    "generated_at": datetime.now().isoformat(),
                    "database": self.database,
                    "hashes": hashes
                }, f, indent=2)
            
            print(f"Ground truth saved to {output_file}")
            return hashes
            
        finally:
            if connection.is_connected():
                connection.close()


def main():
    parser = argparse.ArgumentParser(
        description='Data Injection Script for MySQL Recovery Pro'
    )
    parser.add_argument('--host', default='localhost',
                       help='MySQL host')
    parser.add_argument('--port', type=int, default=3306,
                       help='MySQL port')
    parser.add_argument('--user', default='root',
                       help='MySQL user')
    parser.add_argument('--password', default='test123',
                       help='MySQL password')
    parser.add_argument('--database', default='testdb',
                       help='Database name')
    parser.add_argument('--num-records', type=int, default=100000,
                       help='Number of records to inject')
    parser.add_argument('--batch-size', type=int, default=1000,
                       help='Batch size for insertion')
    parser.add_argument('--vector', action='store_true',
                       help='Include vector column (MySQL 9.0+)')
    parser.add_argument('--verify', action='store_true',
                       help='Verify injected data')
    parser.add_argument('--hash', action='store_true',
                       help='Generate ground truth hash')
    parser.add_argument('--output', default='ground_truth.json',
                       help='Output file for ground truth')
    
    args = parser.parse_args()
    
    # Inject data
    injector = DataInjector(
        host=args.host,
        port=args.port,
        user=args.user,
        password=args.password,
        database=args.database
    )
    
    inserted = injector.inject_data(
        num_records=args.num_records,
        batch_size=args.batch_size,
        include_vector=args.vector
    )
    
    if args.verify:
        connection = injector.create_connection()
        if connection:
            count, expected = injector.verify_data(connection)
            print(f"Verification: {count}/{expected} records")
            connection.close()
    
    if args.hash:
        generator = GroundTruthGenerator(
            host=args.host,
            port=args.port,
            user=args.user,
            password=args.password,
            database=args.database
        )
        generator.generate_all_hashes(output_file=args.output)


if __name__ == '__main__':
    main()
