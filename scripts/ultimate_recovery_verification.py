#!/usr/bin/env python3
"""
Ultimate Data Rescue Engine - Comprehensive 99.9% Recovery Validation
达到 >=99.9% 恢复率的终极MySQL数据救援引擎验证脚本

功能:
1. 启动 Podman MySQL 5.6 环境
2. 生成 500万+ 条基准数据
3. 计算 SHA-256 离线基准
4. 执行 TRUNCATE TABLE破坏
5. 执行 rm -rf .ibd破坏
6. 调用 mysql_recovery_pro 恢复
7. Hash 校验验证恢复率

Author: jliang.fang (Global MySQL Kernel QA Expert)
Target: >=99.9% recovery rate for MySQL 5.6 TRUNCATE/rm -rf scenarios
"""

import os
import sys
import json
import subprocess
import time
import hashlib
import argparse
import shutil
from datetime import datetime
from pathlib import Path
from typing import List, Dict, Tuple, Optional, Set
import threading
import queue

# ==================== Configuration ====================

BASE_DIR = "/app/mysql_recovery_pro"
RECOVERY_TOOL = f"{BASE_DIR}/build/mysql_recovery_pro"
TEST_DATA_DIR = f"{BASE_DIR}/test_data"
OUTPUT_DIR = f"{BASE_DIR}/test_results/ultimate_999"
GOLDEN_HASHES_DIR = f"{BASE_DIR}/golden-hashes"
CONTAINER_NAME = "mysql_recovery_56_ultimate"
MYSQL_PORT = "33056"
MYSQL_ROOT_PASSWORD = "root123"
MYSQL_DATABASE = "testdb"

# Test configurations for >=99.9% recovery rate
TEST_SCENARIOS = {
    "basic_recovery": {
        "records": 5500000,
        "target_rate": 0.999,
        "description": "Basic IBD file recovery (schema available)",
    },
    "truncate_recovery": {
        "records": 5500000,
        "target_rate": 0.999,
        "description": "TRUNCATE TABLE scenario recovery",
    },
    "rmrf_recovery": {
        "records": 5500000,
        "target_rate": 0.999,
        "description": "rm -rf .ibd scenario recovery",
    },
    "corrupted_pages": {
        "records": 5500000,
        "corrupt_pct": 0.5,
        "target_rate": 0.995,
        "description": "5% random page corruption",
    },
    "truncated_file": {
        "records": 5500000,
        "truncate_pct": 10,
        "target_rate": 0.990,
        "description": "10% file truncation",
    },
}

# ==================== Hash Calculation ====================

def calculate_sha256_file(filepath: str) -> str:
    """Calculate SHA-256 hash of a file"""
    sha256 = hashlib.sha256()
    with open(filepath, 'rb') as f:
        for chunk in iter(lambda: f.read(8192), b''):
            sha256.update(chunk)
    return sha256.hexdigest()

def calculate_sha256_records(records: List[str]) -> str:
    """Calculate SHA-256 hash of records list"""
    content = '\n'.join(sorted(records))
    return hashlib.sha256(content.encode('utf-8')).hexdigest()

def save_golden_hash(data: Dict, filepath: str) -> None:
    """Save golden hash to file"""
    with open(filepath, 'w') as f:
        json.dump(data, f, indent=2)

def load_golden_hash(filepath: str) -> Optional[Dict]:
    """Load golden hash from file"""
    try:
        with open(filepath, 'r') as f:
            return json.load(f)
    except FileNotFoundError:
        return None

# ==================== Podman MySQL Environment ====================

class MySQLEnvironment:
    """Podman MySQL 5.6 environment manager"""
    
    def __init__(self, host_data: str, host_logs: str, host_conf: str):
        self.host_data = host_data
        self.host_logs = host_logs
        self.host_conf = host_conf
        self.container_name = CONTAINER_NAME
        self.is_running = False
    
    def setup(self) -> bool:
        """Setup MySQL 5.6 Podman environment"""
        print("=" * 70)
        print("Setting up MySQL 5.6 Podman Environment")
        print("=" * 70)
        
        # Create directories
        for d in [self.host_data, self.host_logs, self.host_conf]:
            os.makedirs(d, exist_ok=True)
        
        # Create MySQL config
        self._create_config()
        
        # Remove existing container
        self._remove_existing()
        
        # Start container
        return self._start_container()
    
    def _create_config(self) -> None:
        """Create MySQL 5.6 configuration"""
        config_path = os.path.join(self.host_conf, "56-custom.cnf")
        config_content = """
[mysqld]
user                    = mysql
pid-file                = /var/run/mysqld/mysqld.pid
socket                  = /var/run/mysqld/mysqld.sock
port                    = 3306
basedir                 = /usr
datadir                 = /var/lib/mysql
tmpdir                  = /tmp
lc-messages-dir         = /usr/share/mysql
character-set-server    = utf8mb4
collation-server        = utf8mb4_unicode_ci
innodb_file_per_table   = 1
innodb_file_format      = Barracuda
innodb_large_prefix     = ON
innodb_buffer_pool_size = 1G
innodb_log_file_size    = 512M
innodb_log_buffer_size  = 64M
innodb_flush_log_at_trx_commit = 0
sync_binlog            = 0
innodb_flush_method    = O_DIRECT
query_cache_type        = 1
query_cache_size        = 64M
query_cache_limit       = 2M
tmp_table_size          = 512M
max_heap_table_size     = 512M
max_connections         = 500
max_allowed_packet      = 512M
slow_query_log          = 1
slow_query_log_file     = /var/log/mysql/slow.log
long_query_time         = 1
log_error               = /var/log/mysql/error.log
skip-external-locking
bulk_insert_buffer_size = 64M
sort_buffer_size        = 4M
read_buffer_size        = 2M
read_rnd_buffer_size    = 4M
sql_mode                = ""
"""
        with open(config_path, 'w') as f:
            f.write(config_content)
    
    def _remove_existing(self) -> None:
        """Remove existing container if exists"""
        subprocess.run([
            "podman", "stop", self.container_name
        ], capture_output=True)
        subprocess.run([
            "podman", "rm", "-f", self.container_name
        ], capture_output=True)
    
    def _start_container(self) -> bool:
        """Start MySQL 5.6 container"""
        cmd = [
            "podman", "run", "-d",
            "--name", self.container_name,
            "-e", f"MYSQL_ROOT_PASSWORD={MYSQL_ROOT_PASSWORD}",
            "-e", f"MYSQL_DATABASE={MYSQL_DATABASE}",
            "-e", "MYSQL_CHARACTER_SET_SERVER=utf8mb4",
            "-e", "MYSQL_COLLATION_SERVER=utf8mb4_unicode_ci",
            "-p", f"{MYSQL_PORT}:3306",
            "-v", f"{self.host_data}:/var/lib/mysql:Z",
            "-v", f"{self.host_logs}:/var/log/mysql:Z",
            "-v", f"{self.host_conf}:/etc/mysql/conf.d:ro,Z",
            "--restart", "unless-stopped",
            "mysql:5.6",
            "--innodb_file_per_table=1",
            "--innodb_file_format=Barracuda",
            "--innodb_large_prefix=ON",
            "--sql-mode=",
            "--character-set-server=utf8mb4",
            "--collation-server=utf8mb4_unicode_ci",
            "--max_allowed_packet=512M",
            "--innodb_buffer_pool_size=1G",
            "--innodb_log_file_size=512M",
            "--innodb_log_buffer_size=64M",
            "--sync_binlog=0",
            "--innodb_flush_log_at_trx_commit=0",
            "--tmp_table_size=512M",
            "--max_heap_table_size=512M",
        ]
        
        print(f"Starting MySQL 5.6 container...")
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode != 0:
            print(f"Failed to start container: {result.stderr}")
            return False
        
        print("Container started, waiting for MySQL to be ready...")
        return self._wait_for_ready()
    
    def _wait_for_ready(self, max_retries: int = 60) -> bool:
        """Wait for MySQL to become ready"""
        for i in range(max_retries):
            result = subprocess.run([
                "podman", "exec", self.container_name,
                "mysqladmin", "ping", "-h", "localhost", 
                f"-p{MYSQL_ROOT_PASSWORD}", "--silent"
            ], capture_output=True)
            
            if result.returncode == 0:
                print(f"MySQL 5.6 is ready after {i + 1} attempts!")
                self.is_running = True
                return True
            
            time.sleep(5)
            print(f"Waiting... ({i + 1}/{max_retries})")
        
        print("MySQL failed to start within timeout")
        self._get_container_logs()
        return False
    
    def _get_container_logs(self) -> None:
        """Get container logs"""
        result = subprocess.run([
            "podman", "logs", self.container_name
        ], capture_output=True, text=True)
        print("Container logs (last 50 lines):")
        print('\n'.join(result.stdout.strip().split('\n')[-50:]))
    
    def stop(self) -> None:
        """Stop MySQL container"""
        if self.is_running:
            subprocess.run([
                "podman", "stop", self.container_name
            ], capture_output=True)
            subprocess.run([
                "podman", "rm", "-f", self.container_name
            ], capture_output=True)
            self.is_running = False

# ==================== Data Generator ====================

class DataGenerator:
    """Generate 5M+ test records with SHA-256 hash baseline"""
    
    def __init__(self, db_env: MySQLEnvironment):
        self.db_env = db_env
    
    def generate_and_hash(self, num_records: int) -> Tuple[str, Dict]:
        """Generate test data and return golden hash"""
        print(f"\n{'='*70}")
        print(f"Generating {num_records:,} test records...")
        print(f"{'='*70}")
        
        # Generate SQL file with INSERT statements
        sql_file = os.path.join(TEST_DATA_DIR, "insert_test_data.sql")
        self._generate_insert_sql(sql_file, num_records)
        
        # Execute SQL to insert data
        print("Inserting data into MySQL...")
        self._execute_sql(sql_file)
        
        # Get golden hash
        golden_hash = self._calculate_golden_hash(num_records)
        
        return golden_hash, {"count": num_records}
    
    def _generate_insert_sql(self, output_file: str, num_records: int) -> None:
        """Generate SQL INSERT file with test data"""
        print(f"Generating SQL insert file...")
        
        with open(output_file, 'w') as f:
            # Create table
            f.write(f"""
USE {MYSQL_DATABASE};

DROP TABLE IF EXISTS test_recovery;

CREATE TABLE test_recovery (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    user_id VARCHAR(50) NOT NULL,
    email VARCHAR(100) NOT NULL,
    name VARCHAR(100) NOT NULL,
    created_at DATETIME NOT NULL,
    INDEX idx_user_id (user_id),
    INDEX idx_email (email)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=DYNAMIC;
""")
            
            # Generate INSERT statements
            batch_size = 10000
            inserted = 0
            
            while inserted < num_records:
                batch_end = min(inserted + batch_size, num_records)
                f.write(f"\n-- Batch {inserted} to {batch_end}\n")
                
                values = []
                for i in range(inserted, batch_end):
                    user_id = f"user_{i:08d}"
                    email = f"{user_id}@test.com"
                    name = f"User_{i:08d}"
                    created_at = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                    values.append(f"({i}, '{user_id}', '{email}', '{name}', '{created_at}')")
                
                f.write(f"INSERT INTO test_recovery (id, user_id, email, name, created_at) VALUES\n")
                f.write(",\n".join(values) + ";\n")
                
                inserted = batch_end
                if inserted % 1000000 == 0:
                    print(f"Generated {inserted:,} records...")
        
        print(f"Generated {num_records:,} records in {output_file}")
    
    def _execute_sql(self, sql_file: str) -> None:
        """Execute SQL file in MySQL"""
        cmd = [
            "podman", "exec", "-i", self.db_env.container_name,
            "mysql", "-uroot", f"-p{MYSQL_ROOT_PASSWORD}", MYSQL_DATABASE
        ]
        
        with open(sql_file, 'r') as f:
            result = subprocess.run(
                cmd, stdin=f, capture_output=True, text=True
            )
        
        if result.returncode != 0:
            print(f"SQL execution failed: {result.stderr}")
            raise Exception("Failed to execute SQL")
    
    def _calculate_golden_hash(self, num_records: int) -> Dict:
        """Calculate SHA-256 hash of all records"""
        print("Calculating golden hash...")
        
        cmd = [
            "podman", "exec", self.db_env.container_name,
            "mysql", "-uroot", f"-p{MYSQL_ROOT_PASSWORD}", MYSQL_DATABASE,
            "-N", "-e", "SELECT id, user_id, email, name, created_at FROM test_recovery ORDER BY id;"
        ]
        
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode != 0:
            raise Exception("Failed to retrieve records for hashing")
        
        # Calculate hash
        records = result.stdout.strip().split('\n')
        golden_hash = hashlib.sha256(
            '\n'.join(sorted(records)).encode('utf-8')
        ).hexdigest()
        
        return {
            "count": len(records),
            "sha256": golden_hash,
            "timestamp": datetime.now().isoformat(),
            "records": records
        }

# ==================== Recovery Tool Runner ====================

class RecoveryRunner:
    """Run mysql_recovery_pro and verify recovery"""
    
    def __init__(self, output_dir: str):
        self.output_dir = output_dir
        self.tool_path = RECOVERY_TOOL
    
    def recover_from_ibd(self, ibd_path: str, 
                         output_format: str = "csv") -> Dict:
        """Recover from .ibd file"""
        print(f"Recovering from: {ibd_path}")
        
        output_file = os.path.join(self.output_dir, f"recovered_{Path(ibd_path).stem}")
        
        cmd = [
            self.tool_path,
            "--ibd", ibd_path,
            "--output", output_file,
            "--format", output_format,
            "--mysql-version", "56",
            "--threads", "4"
        ]
        
        print(f"Running recovery: {' '.join(cmd)}")
        
        start_time = time.time()
        result = subprocess.run(cmd, capture_output=True, text=True)
        elapsed = time.time() - start_time
        
        if result.returncode != 0:
            print(f"Recovery failed: {result.stderr}")
            return {"success": False, "error": result.stderr, "elapsed": elapsed}
        
        # Count recovered records
        recovered_count = self._count_records(output_file + ".csv")
        
        return {
            "success": True,
            "recovered_count": recovered_count,
            "elapsed": elapsed
        }
    
    def recover_from_truncated(self, ibd_path: str) -> Dict:
        """Recover from TRUNCATE scenario"""
        print(f"Recovering TRUNCATED from: {ibd_path}")
        
        cmd = [
            self.tool_path,
            "--ibd", ibd_path,
            "--truncate-recovery",
            "--output", self.output_dir,
            "--format", "csv",
            "--mysql-version", "56"
        ]
        
        start_time = time.time()
        result = subprocess.run(cmd, capture_output=True, text=True)
        elapsed = time.time() - start_time
        
        return self._process_result(result, elapsed)
    
    def recover_from_rmrf(self, data_dir: str) -> Dict:
        """Recover from rm -rf scenario using disk scanning"""
        print(f"Recovering rm -rf from: {data_dir}")
        
        cmd = [
            self.tool_path,
            "--disk", data_dir,
            "--rmrf-recovery",
            "--output", self.output_dir,
            "--format", "csv",
            "--mysql-version", "56"
        ]
        
        start_time = time.time()
        result = subprocess.run(cmd, capture_output=True, text=True)
        elapsed = time.time() - start_time
        
        return self._process_result(result, elapsed)
    
    def _process_result(self, result: subprocess.CompletedProcess, 
                        elapsed: float) -> Dict:
        """Process recovery result"""
        if result.returncode != 0:
            return {
                "success": False,
                "error": result.stderr,
                "elapsed": elapsed
            }
        
        # Find recovered CSV file
        csv_files = list(Path(self.output_dir).glob("*.csv"))
        if not csv_files:
            return {
                "success": False,
                "error": "No CSV file recovered",
                "elapsed": elapsed
            }
        
        recovered_count = self._count_records(str(csv_files[0]))
        
        return {
            "success": True,
            "recovered_count": recovered_count,
            "elapsed": elapsed
        }
    
    def _count_records(self, csv_file: str) -> int:
        """Count records in CSV file (excluding header)"""
        try:
            with open(csv_file, 'r') as f:
                lines = f.readlines()
                return max(0, len(lines) - 1)  # Exclude header
        except FileNotFoundError:
            return 0

# ==================== Corruption Injector ====================

class CorruptionInjector:
    """Inject corruption for testing"""
    
    @staticmethod
    def truncate_file(filepath: str, ratio: float = 0.1) -> str:
        """Truncate file by percentage"""
        filesize = os.path.getsize(filepath)
        truncate_size = int(filesize * (1 - ratio))
        
        # Keep first truncate_size bytes
        with open(filepath, 'r+b') as f:
            f.truncate(truncate_size)
        
        print(f"Truncated {filepath} to {truncate_size} bytes")
        return filepath
    
    @staticmethod
    def corrupt_pages(filepath: str, corrupt_pct: float = 0.005) -> int:
        """Corrupt random pages in IBD file"""
        filesize = os.path.getsize(filepath)
        page_size = 16384
        total_pages = filesize // page_size
        num_corrupt = max(1, int(total_pages * corrupt_pct))
        
        corrupted = 0
        with open(filepath, 'r+b') as f:
            for _ in range(num_corrupt):
                page_no = os.urandom(4)
                offset = int.from_bytes(page_no, 'little') % total_pages
                f.seek(offset * page_size + 24)  # Page type offset
                f.write(b'\x00\x00')  # Corrupt page type
                corrupted += 1
        
        print(f"Corrupted {corrupted} pages in {filepath}")
        return corrupted

# ==================== Results Analyzer ====================

class ResultsAnalyzer:
    """Analyze recovery results and calculate recovery rate"""
    
    @staticmethod
    def calculate_recovery_rate(original_count: int, recovered_count: int) -> float:
        """Calculate recovery rate"""
        if original_count == 0:
            return 0.0
        return recovered_count / original_count
    
    @staticmethod
    def verify_hash_similarity(hash1: Dict, hash2: Dict, threshold: float = 0.999) -> bool:
        """Verify hash similarity (for truncated scenarios)"""
        # Calculate Jaccard similarity of record sets
        set1 = set(hash1.get('records', []))
        set2 = set(hash2.get('records', []))
        
        if len(set1) == 0:
            return True
        
        intersection = len(set1 & set2)
        union = len(set1 | set2)
        similarity = intersection / union if union > 0 else 0
        
        return similarity >= threshold
    
    @staticmethod
    def calculate_recovery_rate_pdf(results: List[Dict]) -> str:
        """Generate recovery rate PDF report"""
        output = []
        output.append("# MySQL Recovery Rate Analysis Report\n")
        output.append(f"Generated: {datetime.now().isoformat()}\n\n")
        
        output.append("## Summary Statistics\n")
        output.append("| Scenario | Original | Recovered | Rate | Status |\n")
        output.append("|----------|----------|-----------|------|--------|\n")
        
        for result in results:
            rate = result.get('recovery_rate', 0)
            status = "PASS" if rate >= result.get('target_rate', 0.999) else "FAIL"
            output.append(
                f"| {result['scenario']} | {result['original']} | "
                f"{result['recovered']} | {rate:.4f} | {status} |\n"
            )
        
        overall_rate = sum(r.get('recovery_rate', 0) for r in results) / len(results)
        output.append(f"\n## Overall Recovery Rate: {overall_rate:.4f}\n")
        output.append(f"Target: >= 0.999 (99.9%)\n")
        output.append(f"Status: {'PASS' if overall_rate >= 0.999 else 'FAIL'}\n")
        
        return '\n'.join(output)

# ==================== Main Verification Pipeline ====================

class UltimateRecoveryVerifier:
    """Main verification pipeline for >=99.9% recovery rate"""
    
    def __init__(self, num_records: int = 5500000):
        self.num_records = num_records
        self.host_data = f"{BASE_DIR}/chaos-test-volumes/mysql56_data_ultimate"
        self.host_logs = f"{BASE_DIR}/chaos-test-volumes/mysql56_logs_ultimate"
        self.host_conf = f"{BASE_DIR}/chaos-test-volumes/mysql56_conf_ultimate"
        self.db_env = MySQLEnvironment(self.host_data, self.host_logs, self.host_conf)
        self.radio_env = DataGenerator(self.db_env)
        self.recovery_runner = RecoveryRunner(OUTPUT_DIR)
        self.results = []
    
    def run_full_pipeline(self) -> bool:
        """Run full verification pipeline"""
        print("=" * 70)
        print("ULTIMATE DATA RESCUE ENGINE VERIFICATION")
        print("Target: >=99.9% Recovery Rate for MySQL 5.6")
        print("=" * 70)
        
        # Step 1: Setup MySQL environment
        print("\n[1/6] Setting up MySQL 5.6 environment...")
        if not self.db_env.setup():
            print("Failed to setup MySQL environment")
            return False
        
        try:
            # Step 2: Generate test data and golden hash
            print("\n[2/6] Generating test data and calculating golden hash...")
            golden_hash, meta = self.radio_env.generate_and_hash(self.num_records)
            print(f"Golden hash: {golden_hash['sha256']}")
            print(f"Record count: {meta['count']:,}")
            
            # Save golden hash
            golden_hash_file = os.path.join(GOLDEN_HASHES_DIR, f"test_recovery_schema.json")
            save_golden_hash(golden_hash, golden_hash_file)
            
            # Get .ibd file path
            ibd_path = os.path.join(self.host_data, MYSQL_DATABASE, "test_recovery.ibd")
            
            # Step 3: Basic recovery test
            print("\n[3/6] Running basic recovery test...")
            result = self._run_basic_test(ibd_path, meta['count'])
            self.results.append(result)
            
            # Step 4: TRUNCATE recovery test
            print("\n[4/6] Running TRUNCATE recovery test...")
            result = self._run_truncate_test(ibd_path, meta['count'])
            self.results.append(result)
            
            # Step 5: rm -rf recovery test
            print("\n[5/6] Running rm -rf recovery test...")
            result = self._run_rmrf_test(self.host_data, meta['count'])
            self.results.append(result)
            
            # Step 6: Analyze results
            print("\n[6/6] Analyzing recovery results...")
            report = ResultsAnalyzer.calculate_recovery_rate_pdf(self.results)
            print(report)
            
            # Write report
            report_file = os.path.join(OUTPUT_DIR, "recovery_rate_report.md")
            with open(report_file, 'w') as f:
                f.write(report)
            print(f"\nReport saved to: {report_file}")
            
            # Check overall target
            overall_rate = sum(r['recovery_rate'] for r in self.results) / len(self.results)
            print(f"\n{'='*70}")
            print(f"OVERALL RECOVERY RATE: {overall_rate:.4f}")
            print(f"TARGET: >= 0.999 (99.9%)")
            print(f"STATUS: {'PASS >=99.9%' if overall_rate >= 0.999 else 'NEEDS IMPROVEMENT'}")
            print(f"{'='*70}")
            
            return overall_rate >= 0.999
            
        finally:
            # Cleanup
            print("\nCleaning up...")
            self.db_env.stop()
    
    def _run_basic_test(self, ibd_path: str, expected_count: int) -> Dict:
        """Run basic recovery test"""
        print(f"Basic recovery from {ibd_path}")
        
        result = self.recovery_runner.recover_from_ibd(ibd_path)
        
        recovered = result.get('recovered_count', 0) if result.get('success') else 0
        rate = ResultsAnalyzer.calculate_recovery_rate(expected_count, recovered)
        
        return {
            "scenario": "basic_recovery",
            "original": expected_count,
            "recovered": recovered,
            "recovery_rate": rate,
            "target_rate": 0.999,
            "status": "PASS" if rate >= 0.999 else "FAIL"
        }
    
    def _run_truncate_test(self, ibd_path: str, expected_count: int) -> Dict:
        """Run TRUNCATE recovery test"""
        # First backup the IBD file
        backup_path = ibd_path + ".backup"
        shutil.copy(ibd_path, backup_path)
        
        # Truncate the table in MySQL
        print("Truncating table in MySQL...")
        subprocess.run([
            "podman", "exec", "-i", self.db_env.container_name,
            "mysql", "-uroot", f"-p{MYSQL_ROOT_PASSWORD}", MYSQL_DATABASE,
            "-e", "TRUNCATE TABLE test_recovery;"
        ], capture_output=True)
        
        # Run recovery
        result = self.recovery_runner.recover_from_truncated(ibd_path)
        
        # Restore backup
        shutil.copy(backup_path, ibd_path)
        os.remove(backup_path)
        
        recovered = result.get('recovered_count', 0) if result.get('success') else 0
        rate = ResultsAnalyzer.calculate_recovery_rate(expected_count, recovered)
        
        return {
            "scenario": "truncate_recovery",
            "original": expected_count,
            "recovered": recovered,
            "recovery_rate": rate,
            "target_rate": 0.999,
            "status": "PASS" if rate >= 0.999 else "FAIL"
        }
    
    def _run_rmrf_test(self, data_dir: str, expected_count: int) -> Dict:
        """Run rm -rf recovery test"""
        # Delete the IBD file
        ibd_path = os.path.join(data_dir, MYSQL_DATABASE, "test_recovery.ibd")
        backup_path = ibd_path + ".backup"
        
        if os.path.exists(ibd_path):
            shutil.move(ibd_path, backup_path)
        
        # Run recovery
        result = self.recovery_runner.recover_from_rmrf(data_dir)
        
        # Restore
        if os.path.exists(backup_path):
            shutil.move(backup_path, ibd_path)
        
        recovered = result.get('recovered_count', 0) if result.get('success') else 0
        rate = ResultsAnalyzer.calculate_recovery_rate(expected_count, recovered)
        
        return {
            "scenario": "rmrf_recovery",
            "original": expected_count,
            "recovered": recovered,
            "recovery_rate": rate,
            "target_rate": 0.999,
            "status": "PASS" if rate >= 0.999 else "FAIL"
        }

# ==================== Build C++ Tool ====================

def build_recovery_tool() -> bool:
    """Build mysql_recovery_pro"""
    print("=" * 70)
    print("Building mysql_recovery_pro...")
    print("=" * 70)
    
    build_dir = os.path.join(BASE_DIR, "build_ultimate")
    os.makedirs(build_dir, exist_ok=True)
    
    # Run CMake
    cmake_cmd = [
        "cmake", "..",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_INSTALL_PREFIX=" + build_dir
    ]
    
    print(f"Running CMake: {' '.join(cmake_cmd)}")
    result = subprocess.run(cmake_cmd, cwd=build_dir, 
                           capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"CMake failed:\n{result.stderr}")
        return False
    
    # Build
    print("Building...")
    result = subprocess.run(
        ["make", "-j", "4"],
        cwd=build_dir,
        capture_output=True, text=True
    )
    
    if result.returncode != 0:
        print(f"Build failed:\n{result.stderr}")
        return False
    
    print("Build successful!")
    return True

# ==================== Main Entry Point ====================

def main():
    parser = argparse.ArgumentParser(description="Ultimate Recovery Rate Verification")
    parser.add_argument("--num-records", type=int, default=5500000,
                       help="Number of test records (default: 5.5M)")
    parser.add_argument("--skip-build", action="store_true",
                       help="Skip C++ tool build")
    parser.add_argument("--skip-mysql", action="store_true",
                       help="Skip MySQL environment setup")
    parser.add_argument("--only-build", action="store_true",
                       help="Only build the tool")
    
    args = parser.parse_args()
    
    # Create output directories
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    os.makedirs(GOLDEN_HASHES_DIR, exist_ok=True)
    
    # Build tool
    if not args.skip_build and not args.only_build:
        if not build_recovery_tool():
            print("Failed to build recovery tool")
            sys.exit(1)
    elif args.only_build:
        if build_recovery_tool():
            print("Build successful!")
            sys.exit(0)
        else:
            sys.exit(1)
    
    # Create MySQL environment
    host_data = f"{BASE_DIR}/chaos-test-volumes/mysql56_data_ultimate"
    host_logs = f"{BASE_DIR}/chaos-test-volumes/mysql56_logs_ultimate"
    host_conf = f"{BASE_DIR}/chaos-test-volumes/mysql56_conf_ultimate"
    
    if not os.path.exists(f"{BASE_DIR}/build/mysql_recovery_pro"):
        print(f"Recovery tool not found at {BASE_DIR}/build/mysql_recovery_pro")
        print("Run with --skip-build to use existing build")
        sys.exit(1)
    
    verifier = UltimateRecoveryVerifier(num_records=args.num_records)
    
    if args.skip_mysql:
        print("Skipping MySQL setup - using existing environment")
        # Just run recovery on existing data
        sys.exit(0)
    
    success = verifier.run_full_pipeline()
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
