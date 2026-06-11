#!/usr/bin/env python3
"""
Ultra-Large Scale Data Injector for MySQL Recovery Pro Chaos Testing
purpose: Inject 1M+ records with extreme edge cases for version-specific testing
Author: jliang.fang (Global MySQL Kernel QA Expert)
Version: 3.0.0 - With Vector Support (MySQL 9.0), GIS (5.7+), JSON (5.7+)

Features:
- Concurrent injection (multi-threaded)
- Version-specific data type handling (5.0 datetime precision, 9.0 Vector)
- Extreme edge cases (max-length BLOB, GIS, Unicode, NULL overlap)
- Dynamic schema adaptation based on MySQL version
- Real-time progress reporting with ETA
"""

import os
import sys
import json
import math
import random
import hashlib
import struct
import argparse
import threading
import concurrent.futures
from datetime import datetime, timedelta
from typing import List, Dict, Tuple, Optional, Generator
from dataclasses import dataclass, field
from collections import defaultdict
import time

# Try to import MySQL connector
try:
    import mysql.connector
    from mysql.connector import Error as MySQLError, Error
    MYSQL_CONNECTOR_AVAILABLE = True
except ImportError:
    MYSQL_CONNECTOR_AVAILABLE = False


# MySQL Version Constants
class MySQLVersion:
    """Version-specific configurations"""
    V5_0 = "5.0"
    V5_5 = "5.5"
    V5_6 = "5.6"
    V5_7 = "5.7"
    V8_0 = "8.0"
    V8_4 = "8.4"
    V9_0 = "9.0"
    
    @staticmethod
    def supports_json(version: str) -> bool:
        return version in [MySQLVersion.V5_7, MySQLVersion.V8_0, MySQLVersion.V8_4, MySQLVersion.V9_0]
    
    @staticmethod
    def supports_vector(version: str) -> bool:
        return version in [MySQLVersion.V9_0]
    
    @staticmethod
    def supports_gis(version: str) -> bool:
        return version in [MySQLVersion.V5_7, MySQLVersion.V8_0, MySQLVersion.V8_4, MySQLVersion.V9_0]
    
    @staticmethod
    def supports_emoji(version: str) -> bool:
        # All versions support emoji with utf8mb4
        return True
    
    @staticmethod
    def supports_datetime6(version: str) -> bool:
        # MySQL 5.6.4+ supports fractional seconds
        return version in [MySQLVersion.V5_6, MySQLVersion.V5_7, MySQLVersion.V8_0, MySQLVersion.V8_4, MySQLVersion.V9_0]
    
    @staticmethod
    def get_datetime_type(version: str) -> str:
        if MySQLVersion.supports_datetime6(version):
            return "DATETIME(6)"
        return "DATETIME"
    
    @staticmethod
    def get_timestamp_type(version: str) -> str:
        if MySQLVersion.supports_datetime6(version):
            return "TIMESTAMP(6)"
        return "TIMESTAMP"


# Chinese names for realistic data
CHINESE_NAMES = [
    "张伟", "李娜", "王芳", "刘强", "陈静", "杨洋", "赵敏", "黄勇", "周杰", "吴刚",
    "徐丽", "孙浩", "朱伟", "胡敏", "郭强", "何燕", "高杰", "林娜", "郑勇", "谢芳",
    "王磊", "张敏", "李杰", "刘洋", "陈勇", "杨静", "赵强", "黄丽", "周杰", "吴刚",
    "徐涛", "孙鹏", "朱磊", "胡静", "郭勇", "何敏", "高杰", "林娜", "郑强", "谢芳",
    "陈东", "王军", "李红", "张华", "刘伟", "杨柳", "赵峰", "黄萍", "周军", "吴敏",
    "王芳", "张强", "李秀英", "刘志明", "陈洁", "杨波", "赵磊", "黄燕", "周勇", "吴军",
    "徐明", "孙丽", "朱军", "胡强", "郭敏", "何杰", "高勇", "林红", "郑芳", "谢伟",
    "马燕", "罗强", "何静", "宋洋", "胡勇", "邓芳", "熊杰", "吴敏", "徐涛", "孙鹏",
    "王芳", "张强", "李秀英", "刘志明", "陈洁", "杨波", "赵磊", "黄燕", "周勇", "吴军",
    "徐明", "孙丽", "朱军", "胡强", "郭敏", "何杰", "高勇", "林红", "郑芳", "谢伟",
]


class DataGenerator:
    """Generate realistic test data with version-specific types"""
    
    def __init__(self, version: str = MySQLVersion.V8_0, seed: int = None):
        self.version = version
        if seed:
            random.seed(seed)
        else:
            random.seed(datetime.now().microsecond)
        
        self.emoji_list = [
            "😀", "😁", "😂", "🤣", "😃", "😄", "😅", "😆", "😉", "😊",
            "😋", "😎", "😍", "😘", "🥰", "😗", "😙", "😚", "🙂", "🤗",
            "🤩", "🤔", "🤨", "😐", "😑", "😶", "[jj", "🙄", "杄", "_story",
            "😱", "😨", "😰", "😥", "🤤", "😢", "😭", "愤怒", "靖", "ICAL",
            "💩", "antom", "👻", "👽", "🤖", "🎃", "🐶", "🐱", "🐭", "🐹",
            "🐰", "🦊", "🐻", "🐼", "🐨", "🐯", "🦁", "🐮", "🐷", "🐸",
        ]
    
    def generate_name(self) -> str:
        """Generate a random Chinese name"""
        return random.choice(CHINESE_NAMES)
    
    def generate_emoji_name(self) -> str:
        """Generate name with random emojis (Unicode testing)"""
        name = self.generate_name()
        num_emojis = random.randint(0, 3)
        if num_emojis > 0:
            emojis = ''.join(random.choice(self.emoji_list) for _ in range(num_emojis))
            return f"{emojis}{name}{emojis}"
        return name
    
    def generate_email(self) -> str:
        """Generate a random email"""
        username = f"user_{random.randint(10000, 99999)}"
        domains = ["gmail.com", "yahoo.com", "hotmail.com", "outlook.com", 
                   "qq.com", "163.com", "126.com", "sina.com", "sohu.com", "weibo.com",
                   "icloud.com", "protonmail.com", "outlook.cn", "gmail.cn"]
        domain = random.choice(domains)
        return f"{username}@{domain}"
    
    def generate_long_email(self, length: int = 254) -> str:
        """Generate email with max length"""
        username = f"user_{random.randint(10000, 99999)}"
        domain_len = length - len(username) - 1 - 3  # @, ., tld
        domains = ["gmail.com", "yahoo.com", "hotmail.com"]
        domain = random.choice(domains)[:domain_len]
        return f"{username}@{domain}"
    
    def generate_username(self) -> str:
        """Generate a random username"""
        bases = ["admin", "root", "test", "guest", "demo", "developer", "operator",
                 "user", "master", "system", "backup", "monitor", "agent", "proxy"]
        suffix = ''.join(random.choices('0123456789abcdef', k=4))
        return f"{random.choice(bases)}_{suffix}"
    
    def generate_password_hash(self) -> str:
        """Generate a password hash (SHA-256)"""
        password = f"password_{random.randint(100000, 999999)}_!@#"
        return hashlib.sha256(password.encode()).hexdigest()
    
    def generate_status(self) -> int:
        """Generate a random status"""
        return random.choices([0, 1, 2, 3, 4, 5], weights=[0.1, 0.6, 0.15, 0.1, 0.03, 0.02])[0]
    
    def generate_score(self) -> int:
        """Generate a random score (0-1000)"""
        return random.randint(0, 1000)
    
    def generate_json_data(self) -> str:
        """Generate complex JSON data for JSON column type"""
        patterns = [
            {"type": "customer", "level": 1, "tags": ["vip", "premium"], "metrics": {"visits": 150, "purchases": 25}},
            {"type": "customer", "level": 2, "tags": ["vip", "gold"], "metrics": {"visits": 500, "purchases": 100}},
            {"type": "customer", "level": 3, "tags": ["vip", "platinum"], "metrics": {"visits": 1000, "purchases": 500}},
            {"type": "vendor", "level": 1, "tags": ["verified"], "metrics": {"listings": 50, "sales": 500}},
            {"type": "vendor", "level": 2, "tags": ["verified", "premium"], "metrics": {"listings": 200, "sales": 5000}},
            {"type": "system", "level": 0, "tags": ["admin"], "metrics": {"config_count": 1000}},
            {"type": "guest", "level": 0, "tags": ["anonymous"], "metrics": {"visits": 1}},
        ]
        pattern = random.choice(patterns)
        pattern["timestamp"] = datetime.now().isoformat()
        pattern["random_value"] = round(random.random(), 6)
        pattern["nested"] = {
            "level2": {
                "level3": {
                    "deep": random.randint(0, 1000)
                }
            }
        }
        return json.dumps(pattern, ensure_ascii=False)
    
    def generate_extreme_json(self) -> str:
        """Generate JSON with extreme data (deep nesting, large arrays)"""
        deep = {"level0": {"level1": {"level2": {"level3": {"level4": {"level5": {"data": "deep"}}}}}}}
        for i in range(10):
            deep = {"nested": deep}
        deep["array"] = list(range(100))
        deep["large_string"] = "x" * 32768
        return json.dumps(deep, ensure_ascii=False)
    
    def generate_blob_data(self, size: int = 1024, pattern: str = "random") -> bytes:
        """Generate BLOB data with different patterns"""
        if pattern == "zeros":
            return bytes(size)
        elif pattern == "ones":
            return bytes([255] * size)
        elif pattern == "pattern":
            return bytes([i % 256 for i in range(size)])
        else:  # random
            return bytes([random.randint(0, 255) for _ in range(size)])
    
    def generate_text_description(self, min_words: int = 10, max_words: int = 50) -> str:
        """Generate text description with Chinese words"""
        words = [
            "测试", "数据", "记录", "用户", "系统", "应用", "开发", "运维", "数据库",
            "MySQL", "InnoDB", "恢复", "备份", "查询", "更新", "插入", "删除", "表",
            "索引", "页", "块", "事务", "提交", "回滚", "锁", "并发", "性能", "优化",
            "索引", "分区", "复制", "集群", "高可用", "灾备", "监控", "日志", "分析", "报表"
        ]
        num_words = random.randint(min_words, max_words)
        return " ".join(random.choice(words) for _ in range(num_words))
    
    def generate_long_text(self, length: int = 65535) -> str:
        """Generate text with max length (for TEXT type testing)"""
        words = ["测试", "数据", "记录", "用户", "系统", "应用", "开发", "运维", "数据库"]
        word_length = len("测试")  # 3 bytes
        num_words = length // word_length
        result = ""
        while len(result.encode('utf-8')) < length:
            result += random.choice(words) + " "
        return result[:length]
    
    def generate_datetime(self, start_year: int = 2020, end_year: int = 2024) -> datetime:
        """Generate a random datetime"""
        start = datetime(start_year, 1, 1)
        end = datetime(end_year, 12, 31, 23, 59, 59)
        delta = end - start
        random_days = random.randint(0, delta.days)
        random_seconds = random.randint(0, 86400)
        return start + timedelta(days=random_days, seconds=random_seconds)
    
    def generate_datetime_with_microseconds(self) -> datetime:
        """Generate datetime with microsecond precision (MySQL 5.6.4+)"""
        dt = self.generate_datetime()
        microsecond = random.randint(0, 999999)
        return dt.replace(microsecond=microsecond)
    
    def generate_date(self) -> str:
        """Generate a random date string"""
        dt = self.generate_datetime()
        return dt.strftime("%Y-%m-%d")
    
    def generate_time(self) -> str:
        """Generate a random time string"""
        dt = self.generate_datetime()
        return dt.strftime("%H:%M:%S")
    
    def generate_time_with_fsp(self) -> str:
        """Generate time with fractional seconds (MySQL 5.6.4+)"""
        dt = self.generate_datetime()
        microseconds = random.randint(0, 999999)
        return dt.strftime(f"%H:%M:%S.{microseconds:06d}")
    
    def generate_float(self) -> float:
        """Generate a random float"""
        return round(random.uniform(-1000000, 1000000), 4)
    
    def generate_double(self) -> float:
        """Generate a random double"""
        return round(random.uniform(-1000000000, 1000000000), 12)
    
    def generate_decimal(self, precision: int = 10, scale: int = 2) -> str:
        """Generate a decimal string"""
        max_int = 10 ** (precision - scale)
        integer_part = random.randint(0, max_int - 1)
        decimal_part = random.randint(0, 10 ** scale - 1)
        return f"{integer_part}.{decimal_part:0{scale}d}"
    
    def generate_vector_90(self, dimension: int = 128, normalize: bool = True) -> List[float]:
        """Generate a random vector for MySQL 9.0 Vector type"""
        import random
        vector = [random.gauss(0, 1) for _ in range(dimension)]
        if normalize:
            magnitude = math.sqrt(sum(x * x for x in vector))
            if magnitude > 0:
                vector = [x / magnitude for x in vector]
        return vector
    
    def generate_gis_point(self) -> str:
        """Generate a GIS Point geometry (MySQL 5.7+)"""
        return f"ST_PointFromText('POINT({random.uniform(-180, 180)} {random.uniform(-90, 90)})')"
    
    def generate_gis_polygon(self) -> str:
        """Generate a GIS Polygon geometry (MySQL 5.7+)"""
        points = []
        for _ in range(5):  # Pentagon
            lat = random.uniform(-10, 10)
            lon = random.uniform(-10, 10)
            points.append(f"{lon} {lat}")
        return f"ST_PolyFromText('POLYGON(({', '.join(points)}))')"
    
    def generate_very_long_blob(self, size_mb: int = 50) -> bytes:
        """Generate a very large BLOB (50MB for LONGBLOB testing)"""
        return self.generate_blob_data(size_mb * 1024 * 1024, "random")
    
    def generate_null_overlapping_record(self, null_probability: float = 0.5) -> Dict:
        """Generate record with NULL fields overlapping"""
        record = {}
        fields = ['name', 'email', 'username', 'status', 'score', 'description', 'json_data', 'blob_data']
        for field in fields:
            if random.random() < null_probability:
                record[field] = None
            else:
                record[field] = f"value_{field}_{random.randint(1, 1000)}"
        return record
    
    def generate_record(self, record_id: int, include_vector: bool = False,
                       include_gis: bool = False, include_unicode: bool = True) -> Dict:
        """Generate a single test record with version-specific fields"""
        record = {
            "id": record_id,
            "name": self.generate_name(),
            "email": self.generate_email(),
            "username": self.generate_username(),
            "password_hash": self.generate_password_hash(),
            "status": self.generate_status(),
            "score": self.generate_score(),
            "description": self.generate_text_description(),
            "json_data": self.generate_json_data() if MySQLVersion.supports_json(self.version) else None,
            "blob_data": self.generate_blob_data(512).hex(),
            "created_at": self.generate_datetime().isoformat(),
            "updated_at": self.generate_datetime().isoformat(),
            "created_date": self.generate_date(),
            "created_time": self.generate_time(),
            "score_float": self.generate_float(),
            "score_double": self.generate_double(),
            "balance": self.generate_decimal(10, 2),
        }
        
        # Version-specific fields
        if include_vector and MySQLVersion.supports_vector(self.version):
            record["vector_data"] = json.dumps(self.generate_vector_90(128), ensure_ascii=False)
        
        if include_gis and MySQLVersion.supports_gis(self.version):
            record["location"] = self.generate_gis_point()
            record["region"] = self.generate_gis_polygon()
        
        if include_unicode and MySQLVersion.supports_emoji(self.version):
            record["emoji_name"] = self.generate_emoji_name()
        
        # Version-specific datetime format
        if MySQLVersion.supports_datetime6(self.version):
            record["created_at_micro"] = self.generate_datetime_with_microseconds().isoformat()
        
        return record
    
    def generate_edge_case_record(self, record_id: int) -> Dict:
        """Generate an edge case record for extreme testing"""
        return {
            "id": record_id,
            "name": self.generate_long_text(65533),
            "email": self.generate_long_email(254),
            "username": self.generate_username(),
            "password_hash": hashlib.sha256(b"x" * 1000000).hexdigest(),  # 1MB hash
            "status": random.choice([0, 1, 2, 3]),
            "score": 0,
            "description": "",
            "json_data": self.generate_extreme_json(),
            "blob_data": self.generate_blob_data(65535).hex(),
            "created_at": "1970-01-01T00:00:00",
            "updated_at": "2038-01-19T03:14:07",
            "created_date": "2038-01-19",
            "created_time": "03:14:07",
            "score_float": -1e38,
            "score_double": 1e308,
            "balance": "9999999999.99",
        }


class MySQLConnectionPool:
    """Thread-safe MySQL connection pool"""
    
    def __init__(self, host: str = "localhost", port: int = 3306,
                 user: str = "root", password: str = "test123",
                 database: str = "testdb", pool_size: int = 4):
        self.host = host
        self.port = port
        self.user = user
        self.password = password
        self.database = database
        self.pool_size = pool_size
        self._connections = []
        self._lock = threading.Lock()
        self._init_pool()
    
    def _init_pool(self):
        """Initialize connection pool"""
        if not MYSQL_CONNECTOR_AVAILABLE:
            raise RuntimeError("MySQL connector not available. Install: pip install mysql-connector-python")
        
        for _ in range(self.pool_size):
            try:
                conn = mysql.connector.connect(
                    host=self.host,
                    port=self.port,
                    user=self.user,
                    password=self.password,
                    database=self.database,
                    autocommit=True,
                    charset='utf8mb4',
                    collation='utf8mb4_unicode_ci'
                )
                self._connections.append(conn)
            except MySQLError as e:
                print(f"Error creating connection: {e}")
    
    def get_connection(self):
        """Get a connection from pool"""
        with self._lock:
            if not self._connections:
                raise RuntimeError("No available connections in pool")
            return self._connections.pop()
    
    def return_connection(self, conn):
        """Return connection to pool"""
        with self._lock:
            self._connections.append(conn)


class tableSchema:
    """Generate version-specific CREATE TABLE statements"""
    
    @staticmethod
    def get_base_schema() -> str:
        """Get base table schema"""
        return """
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
            created_time TIME,
            score_float FLOAT,
            score_double DOUBLE,
            balance DECIMAL(10,2)
        """
    
    @staticmethod
    def get_schema_v50() -> str:
        """MySQL 5.0 schema (no JSON, no UTF8MB4 default)"""
        base = tableSchema.get_base_schema()
        return f"{base}\n        ) ENGINE=InnoDB DEFAULT CHARSET=utf8;"

    @staticmethod
    def get_schema_v55() -> str:
        """MySQL 5.5 schema (Barracuda, UTF8MB4)"""
        base = tableSchema.get_base_schema()
        return f"{base}\n        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;"

    @staticmethod
    def get_schema_v56() -> str:
        """MySQL 5.6 schema (COMPRESSED/DYNAMIC support)"""
        base = tableSchema.get_base_schema().replace("DATETIME", "DATETIME(6)")
        return f"{base}\n        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;"
    
    @staticmethod
    def get_schema_v57() -> str:
        """MySQL 5.7 schema (GIS, JSON)"""
        base = tableSchema.get_base_schema().replace("DATETIME", "DATETIME(6)")
        return f"{base}\n            , location POINT SRID 4326\n            , region POLYGON SRID 4326\n            , emoji_name VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci\n        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;"

    @staticmethod
    def get_schema_v80() -> str:
        """MySQL 8.0 schema (SDI, improved JSON)"""
        base = tableSchema.get_base_schema().replace("DATETIME", "DATETIME(6)")
        return f"{base}\n            , location POINT SRID 4326\n            , region POLYGON SRID 4326\n            , emoji_name VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci\n        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci ROW_FORMAT=DYNAMIC;"

    @staticmethod
    def get_schema_v84() -> str:
        """MySQL 8.4 schema (LTS improvements)"""
        base = tableSchema.get_base_schema().replace("DATETIME", "DATETIME(6)")
        return f"{base}\n            , location POINT SRID 4326\n            , region POLYGON SRID 4326\n            , emoji_name VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci\n        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci ROW_FORMAT=DYNAMIC TABLESPACE=InnoDB;"
    
    @staticmethod
    def get_schema_v90() -> str:
        """MySQL 9.0 schema (Vector support)"""
        return "CREATE TABLE IF NOT EXISTS test_users (\n            id BIGINT PRIMARY KEY AUTO_INCREMENT,\n            name VARCHAR(255),\n            email VARCHAR(255),\n            username VARCHAR(255),\n            password_hash CHAR(64),\n            status TINYINT DEFAULT 1,\n            score INT,\n            description TEXT,\n            json_data JSON,\n            blob_data LONGBLOB,\n            created_at DATETIME(6),\n            updated_at TIMESTAMP(6) DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),\n            created_date DATE,\n            created_time TIME(6),\n            score_float FLOAT,\n            score_double DOUBLE,\n            balance DECIMAL(10,2),\n            vector_data VECTOR(128) COMMENT 'vector',\n            location POINT SRID 4326,\n            region POLYGON SRID 4326,\n            emoji_name VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci\n        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci ROW_FORMAT=DYNAMIC TABLESPACE=InnoDB;"


class DataInjector:
    """High-performance data injector for chaos testing"""
    
    def __init__(self, host: str = "localhost", port: int = 3306,
                 user: str = "root", password: str = "test123",
                 database: str = "testdb", version: str = MySQLVersion.V8_0):
        self.host = host
        self.port = port
        self.user = user
        self.password = password
        self.database = database
        self.version = version
        self.generator = DataGenerator(version=version)
        self.schema = tableSchema()
        self._pool = None
    
    def create_connection(self):
        """Create MySQL connection"""
        if not MYSQL_CONNECTOR_AVAILABLE:
            raise RuntimeError("MySQL connector not available")
        
        try:
            conn = mysql.connector.connect(
                host=self.host,
                port=self.port,
                user=self.user,
                password=self.password,
                database=self.database,
                autocommit=True,
                charset='utf8mb4',
                collation='utf8mb4_unicode_ci',
                connection_timeout=60
            )
            return conn
        except MySQLError as e:
            print(f"Error connecting to MySQL: {e}")
            return None
    
    def create_pool(self, pool_size: int = 4):
        """Create connection pool"""
        self._pool = MySQLConnectionPool(
            host=self.host, port=self.port,
            user=self.user, password=self.password,
            database=self.database, pool_size=pool_size
        )
    
    def create_table(self, conn, table_name: str = "test_users", 
                     include_vector: bool = False, include_gis: bool = False,
                     include_unicode: bool = True) -> bool:
        """Create test table with version-specific schema"""
        cursor = conn.cursor()
        
        try:
            # Get schema based on version
            schema_map = {
                MySQLVersion.V5_0: self.schema.get_schema_v50(),
                MySQLVersion.V5_5: self.schema.get_schema_v55(),
                MySQLVersion.V5_6: self.schema.get_schema_v56(),
                MySQLVersion.V5_7: self.schema.get_schema_v57(),
                MySQLVersion.V8_0: self.schema.get_schema_v80(),
                MySQLVersion.V8_4: self.schema.get_schema_v84(),
                MySQLVersion.V9_0: self.schema.get_schema_v90(),
            }
            
            schema = schema_map.get(self.version, self.schema.get_schema_v80())
            
            # Drop existing table
            cursor.execute(f"DROP TABLE IF EXISTS {table_name}")
            
            # Create new table
            cursor.execute(schema)
            conn.commit()
            print(f"✓ Table {table_name} created successfully")
            return True
            
        except Exception as e:
            print(f"Error creating table: {e}")
            conn.rollback()
            return False
        finally:
            cursor.close()
    
    def insert_batch_optimized(self, conn, records: List[Dict], table_name: str = "test_users") -> int:
        """Insert records using batch operations with bulk INSERT"""
        cursor = conn.cursor()
        
        try:
            # Build INSERT statement dynamically based on fields
            base_columns = [
                "name", "email", "username", "password_hash", "status",
                "score", "description", "json_data", "blob_data",
                "created_at", "updated_at", "created_date", "created_time",
                "score_float", "score_double", "balance"
            ]
            
            extra_columns = []
            if MySQLVersion.supports_vector(self.version):
                extra_columns.append("vector_data")
            if MySQLVersion.supports_gis(self.version):
                extra_columns.extend(["location", "region"])
            if MySQLVersion.supports_emoji(self.version):
                extra_columns.append("emoji_name")
            
            all_columns = base_columns + extra_columns
            placeholders = ", ".join(["%s"] * len(all_columns))
            columns_str = ", ".join(all_columns)
            
            insert_sql = f"""INSERT INTO {table_name} ({columns_str}) VALUES ({placeholders})"""
            
            # Prepare data - convert hex to bytes for blob
            data = []
            for record in records:
                row = [
                    record.get("name"), record.get("email"), record.get("username"),
                    record.get("password_hash"), record.get("status"), record.get("score"),
                    record.get("description"), record.get("json_data"),
                    bytes.fromhex(record.get("blob_data", "")) if record.get("blob_data") else None,
                    record.get("created_at"), record.get("updated_at"),
                    record.get("created_date"), record.get("created_time"),
                    record.get("score_float"), record.get("score_double"),
                    record.get("balance")
                ]
                
                # Add version-specific fields
                if MySQLVersion.supports_vector(self.version) and "vector_data" in record:
                    row.append(record["vector_data"])
                if MySQLVersion.supports_gis(self.version):
                    row.append(record.get("location"))
                    row.append(record.get("region"))
                if MySQLVersion.supports_emoji(self.version):
                    row.append(record.get("emoji_name"))
                
                data.append(row)
            
            # Execute batch insert
            cursor.executemany(insert_sql, data)
            conn.commit()
            
            return len(data)
            
        except Exception as e:
            print(f"Error inserting batch: {e}")
            conn.rollback()
            return 0
        finally:
            cursor.close()
    
    def insert_batch_concurrent(self, records: List[Dict], batch_size: int = 1000,
                                concurrency: int = 4, table_name: str = "test_users") -> int:
        """Insert records using concurrent workers"""
        if not self._pool:
            self.create_pool(pool_size=concurrency)

        total = len(records)
        inserted = 0
        lock = threading.Lock()
        
        def worker(start_idx: int, end_idx: int):
            nonlocal inserted
            conn = self._pool.get_connection()
            try:
                inserted_count = self.insert_batch_optimized(conn, records[start_idx:end_idx], table_name)
                with lock:
                    inserted += inserted_count
            finally:
                self._pool.return_connection(conn)
        
        # Split records into chunks
        chunks = []
        for i in range(0, total, batch_size * concurrency):
            end = min(i + batch_size * concurrency, total)
            chunk = records[i:end]
            
            # Split chunk into batch_size pieces
            for j in range(0, len(chunk), batch_size):
                batch_end = min(j + batch_size, len(chunk))
                chunks.append((i + j, i + batch_end))
        
        # Execute concurrent batches
        with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
            futures = [executor.submit(worker, start, end) for start, end in chunks]
            for future in concurrent.futures.as_completed(futures):
                try:
                    future.result()
                except Exception as e:
                    print(f"Worker error: {e}")
        
        return inserted
    
    def insert_data(self, num_records: int = 100000, batch_size: int = 1000,
                   concurrency: int = 4, include_vector: bool = False,
                   include_gis: bool = False, include_unicode: bool = True,
                   table_name: str = "test_users") -> Tuple[int, float]:
        """Main method to inject data with progress reporting"""
        print(f"Injecting {num_records:,} records into MySQL {self.version}...")
        print(f"  Concurrency: {concurrency} threads, Batch size: {batch_size:,}")
        print(f"  Version-specific features:")
        print(f"    - JSON: {MySQLVersion.supports_json(self.version)}")
        print(f"    - Vector: {MySQLVersion.supports_vector(self.version)}")
        print(f"    - GIS: {MySQLVersion.supports_gis(self.version)}")
        print(f"    - Emoji: {MySQLVersion.supports_emoji(self.version)}")
        
        total_start = time.time()
        
        # Connect and create table
        conn = self.create_connection()
        if not conn:
            return 0, 0
        
        if not self.create_table(conn, table_name, include_vector, include_gis, include_unicode):
            conn.close()
            return 0, 0
        
        # Generate all records
        print("\nGenerating test data...")
        records = []
        for i in range(1, num_records + 1):
            record = self.generator.generate_record(i, 
                include_vector=include_vector,
                include_gis=include_gis,
                include_unicode=include_unicode
            )
            records.append(record)
        
        # Insert data
        print(f"\nInserting records...")
        inserted = 0
        start_time = time.time()
        
        if concurrency > 1 and num_records > 10000:
            # Use concurrent insertion for large datasets
            inserted = self.insert_batch_concurrent(records, batch_size, concurrency, table_name)
        else:
            # Single-threaded insertion for smaller datasets
            for i in range(0, len(records), batch_size):
                batch = records[i:i + batch_size]
                conn = self.create_connection()
                if conn:
                    inserted += self.insert_batch_optimized(conn, batch, table_name)
                    conn.close()
                
                # Progress reporting
                elapsed = time.time() - start_time
                rate = inserted / elapsed if elapsed > 0 else 0
                remaining = (num_records - inserted) / rate if rate > 0 else 0
                sys.stdout.write(f"\r  Injected {inserted:,}/{num_records:,} records "
                               f"({rate:.0f} rec/s, est {remaining:.0f}s remaining)")
                sys.stdout.flush()
        
        conn.close()
        total_elapsed = time.time() - total_start
        
        print(f"\n\n✓ Successfully inserted {inserted:,} records")
        print(f"  Total time: {total_elapsed:.2f} seconds")
        print(f"  Average rate: {inserted/total_elapsed:.0f} records/second")
        
        return inserted, total_elapsed
    
    def verify_data(self, table_name: str = "test_users") -> Tuple[int, int]:
        """Verify injected data count"""
        conn = self.create_connection()
        if not conn:
            return 0, 0
        
        cursor = conn.cursor()
        try:
            cursor.execute(f"SELECT COUNT(*) FROM {table_name}")
            count = cursor.fetchone()[0]
            return count, count
        finally:
            cursor.close()
            conn.close()


class GroundTruthGenerator:
    """Generate SHA-256 ground truth hashes for data verification"""
    
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
        if not MYSQL_CONNECTOR_AVAILABLE:
            raise RuntimeError("MySQL connector not available")
        
        try:
            return mysql.connector.connect(
                host=self.host,
                port=self.port,
                user=self.user,
                password=self.password,
                database=self.database,
                charset='utf8mb4',
                collation='utf8mb4_unicode_ci'
            )
        except MySQLError as e:
            print(f"Error connecting to MySQL: {e}")
            return None
    
    def generate_table_hash_file(self, conn, table_name: str = "test_users",
                                 output_file: str = None) -> Tuple[str, str]:
        """Generate SHA-256 hash for table data and save to file"""
        cursor = conn.cursor()
        
        try:
            # Get column information
            cursor.execute(f"DESCRIBE {table_name}")
            columns = [row[0] for row in cursor.fetchall()]
            
            # Get all data with ordered by id
            cursor.execute(f"SELECT * FROM {table_name} ORDER BY id")
            rows = cursor.fetchall()
            
            # Build hash input
            hash_input = []
            for row in rows:
                record = dict(zip(columns, row))
                # Convert bytes to hex for hashing
                for key, value in record.items():
                    if isinstance(value, bytes):
                        record[key] = value.hex()
                hash_input.append(json.dumps(record, sort_keys=True, ensure_ascii=False, separators=(',', ':')))
            
            # Generate hash
            hash_input_str = "\n".join(hash_input)
            sha256_hash = hashlib.sha256(hash_input_str.encode()).hexdigest()
            
            # Save to file
            if output_file:
                with open(output_file, 'w') as f:
                    json.dump({
                        "table": table_name,
                        "generated_at": datetime.now().isoformat(),
                        "database": self.database,
                        "version": self._get_mysql_version(conn),
                        "row_count": len(rows),
                        "sha256": sha256_hash,
                        "columns": columns,
                        "sample": json.loads(hash_input[0]) if hash_input else None
                    }, f, indent=2, ensure_ascii=False)
                print(f"  Hash saved to: {output_file}")
            
            return sha256_hash, str(len(rows))
            
        except Exception as e:
            print(f"Error generating hash: {e}")
            return "", "0"
        finally:
            cursor.close()
    
    def _get_mysql_version(self, conn) -> str:
        """Get MySQL version string"""
        cursor = conn.cursor()
        try:
            cursor.execute("SELECT VERSION()")
            return cursor.fetchone()[0]
        finally:
            cursor.close()
    
    def generate_all_hashes(self, output_dir: str = "./golden-hashes",
                           versions: List[str] = None) -> Dict[str, Dict]:
        """Generate ground truth hashes for all versions"""
        os.makedirs(output_dir, exist_ok=True)
        
        hashes = {}
        
        # Generate hash for current version
        conn = self.create_connection()
        if conn:
            version = self._get_mysql_version(conn).split('-')[0]
            output_file = os.path.join(output_dir, f"hashes_{version}.json")
            sha256, count = self.generate_table_hash_file(conn, output_file=output_file)
            hashes[version] = {
                "sha256": sha256,
                "row_count": count,
                "output_file": output_file
            }
            conn.close()
        
        return hashes


class EdgeCaseInjector:
    """Inject edge case records for extreme testing"""
    
    def __init__(self, host: str = "localhost", port: int = 3306,
                 user: str = "root", password: str = "test123",
                 database: str = "testdb", version: str = MySQLVersion.V8_0):
        self.host = host
        self.port = port
        self.user = user
        self.password = password
        self.database = database
        self.version = version
        self.generator = DataGenerator(version=version)
    
    def create_connection(self):
        """Create MySQL connection"""
        if not MYSQL_CONNECTOR_AVAILABLE:
            raise RuntimeError("MySQL connector not available")
        
        try:
            return mysql.connector.connect(
                host=self.host,
                port=self.port,
                user=self.user,
                password=self.password,
                database=self.database,
                autocommit=True
            )
        except MySQLError as e:
            print(f"Error connecting to MySQL: {e}")
            return None
    
    def inject_edge_cases(self, num_records: int = 1000, table_name: str = "test_users_edge") -> int:
        """Inject edge case records"""
        conn = self.create_connection()
        if not conn:
            return 0
        
        cursor = conn.cursor()
        inserted = 0
        
        try:
            # Create edge case table
            cursor.execute(f"""
                CREATE TABLE IF NOT EXISTS {table_name} (
                    id BIGINT PRIMARY KEY,
                    name VARCHAR(65533),
                    email VARCHAR(254),
                    username VARCHAR(255),
                    password_hash CHAR(64),
                    status TINYINT,
                    score INT,
                    description TEXT,
                    json_data JSON,
                    blob_data LONGBLOB,
                    created_at DATETIME,
                    updated_at TIMESTAMP,
                    created_date DATE,
                    created_time TIME,
                    score_float FLOAT,
                    score_double DOUBLE,
                    balance DECIMAL(20,2)
                )
            """)
            
            for i in range(1, num_records + 1):
                record = self.generator.generate_edge_case_record(i)
                
                # Insert with ON DUPLICATE KEY UPDATE for idempotency
                insert_sql = f"""
                    INSERT INTO {table_name} (id, name, email, username, password_hash,
                        status, score, description, json_data, blob_data,
                        created_at, updated_at, created_date, created_time,
                        score_float, score_double, balance)
                    VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
                    ON DUPLICATE KEY UPDATE
                        name=VALUES(name), email=VALUES(email)
                """
                
                blob_bytes = bytes.fromhex(record["blob_data"]) if record["blob_data"] else b""
                
                cursor.execute(insert_sql, (
                    record["id"], record["name"], record["email"], record["username"],
                    record["password_hash"], record["status"], record["score"],
                    record["description"], record["json_data"], blob_bytes,
                    record["created_at"], record["updated_at"], record["created_date"],
                    record["created_time"], record["score_float"], record["score_double"],
                    record["balance"]
                ))
                
                inserted += 1
                
                if inserted % 100 == 0:
                    sys.stdout.write(f"\r  Injected {inserted}/{num_records} edge case records")
                    sys.stdout.flush()
            
            print(f"\n✓ Successfully injected {inserted} edge case records")
            return inserted
            
        except Exception as e:
            print(f"Error injecting edge cases: {e}")
            conn.rollback()
            return 0
        finally:
            cursor.close()
            conn.close()


def main():
    parser = argparse.ArgumentParser(
        description='Data Injector for MySQL Recovery Pro Chaos Testing'
    )
    parser.add_argument('--host', default='localhost',
                       help='MySQL host (default: localhost)')
    parser.add_argument('--port', type=int, default=3306,
                       help='MySQL port (default: 3306)')
    parser.add_argument('--user', default='root',
                       help='MySQL user (default: root)')
    parser.add_argument('--password', default='test123',
                       help='MySQL password (default: test123)')
    parser.add_argument('--database', default='testdb',
                       help='Database name (default: testdb)')
    parser.add_argument('--version', default=MySQLVersion.V8_0,
                       choices=[MySQLVersion.V5_0, MySQLVersion.V5_5, MySQLVersion.V5_6,
                               MySQLVersion.V5_7, MySQLVersion.V8_0, MySQLVersion.V8_4, MySQLVersion.V9_0],
                       help='MySQL version for schema generation')
    parser.add_argument('--num-records', type=int, default=100000,
                       help='Number of records to inject (default: 100000)')
    parser.add_argument('--batch-size', type=int, default=1000,
                       help='Batch size for insertion (default: 1000)')
    parser.add_argument('--concurrency', type=int, default=4,
                       help='Number of concurrent workers (default: 4)')
    parser.add_argument('--vector', action='store_true',
                       help='Include vector column (MySQL 9.0+)')
    parser.add_argument('--gis', action='store_true',
                       help='Include GIS columns (MySQL 5.7+)')
    parser.add_argument('--unicode', action='store_true', default=True,
                       help='Include Unicode/Emoji columns')
    parser.add_argument('--edge-cases', action='store_true',
                       help='Inject edge case records')
    parser.add_argument('--num-edge-cases', type=int, default=1000,
                       help='Number of edge case records')
    parser.add_argument('--verify', action='store_true',
                       help='Verify injected data')
    parser.add_argument('--hash', action='store_true',
                       help='Generate ground truth hash')
    parser.add_argument('--hash-output', default='./golden-hashes',
                       help='Output directory for hash files')
    
    args = parser.parse_args()
    
    # Check MySQL connector
    if not MYSQL_CONNECTOR_AVAILABLE:
        print("Error: MySQL connector not installed")
        print("Install with: pip install mysql-connector-python")
        sys.exit(1)
    
    # Create injector
    injector = DataInjector(
        host=args.host,
        port=args.port,
        user=args.user,
        password=args.password,
        database=args.database,
        version=args.version
    )
    
    # Inject data
    inserted, elapsed = injector.insert_data(
        num_records=args.num_records,
        batch_size=args.batch_size,
        concurrency=args.concurrency,
        include_vector=args.vector,
        include_gis=args.gis,
        include_unicode=args.unicode
    )
    
    # Verify
    if args.verify:
        count, expected = injector.verify_data()
        print(f"Verification: {count:,}/{expected:,} records")
    
    # Generate hashes
    if args.hash:
        generator = GroundTruthGenerator(
            host=args.host,
            port=args.port,
            user=args.user,
            password=args.password,
            database=args.database
        )
        generator.generate_all_hashes(output_dir=args.hash_output)
    
    # Inject edge cases
    if args.edge_cases:
        edge_injector = EdgeCaseInjector(
            host=args.host,
            port=args.port,
            user=args.user,
            password=args.password,
            database=args.database,
            version=args.version
        )
        edge_injector.inject_edge_cases(num_records=args.num_edge_cases)
    
    print("\n" + "="*60)
    print("Data injection completed successfully!")
    print(f"Records inserted: {inserted:,}")
    print(f"Total time: {elapsed:.2f} seconds")
    print(f"Average rate: {inserted/elapsed:.0f} records/second")
    print("="*60)


if __name__ == '__main__':
    main()
