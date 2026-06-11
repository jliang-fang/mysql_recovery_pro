#!/usr/bin/env python3
"""
Golden Hash Base Generator - SHA-256 Verification System
Purpose: Generate and store不可篡改的 SHA-256 hashes for data verification
Author: jliang.fang (Global MySQL Kernel QA Expert)
Version: 2.0.0 - Enhanced with version tracking and detailed metadata
"""

import os
import sys
import json
import hashlib
import argparse
from datetime import datetime
from typing import List, Dict, Tuple, Optional
from pathlib import Path
import time

try:
    import mysql.connector
    from mysql.connector import Error as MySQLError
    MYSQL_CONNECTOR_AVAILABLE = True
except ImportError:
    MYSQL_CONNECTOR_AVAILABLE = False


class HashDatabase:
    """Database for storing and verifying hash baselines"""
    
    def __init__(self, db_path: str = "./golden-hashes/"):
        self.db_path = Path(db_path)
        self.db_path.mkdir(parents=True, exist_ok=True)
        self.db_file = self.db_path / "hash_database.json"
        self._database = self._load_database()
    
    def _load_database(self) -> Dict:
        """Load hash database from file"""
        if self.db_file.exists():
            try:
                with open(self.db_file, 'r') as f:
                    return json.load(f)
            except json.JSONDecodeError:
                return {"hashes": {}, "metadata": {}}
        return {"hashes": {}, "metadata": {}}
    
    def _save_database(self):
        """Save hash database to file"""
        with open(self.db_file, 'w') as f:
            json.dump(self._database, f, indent=2)
    
    def add_hash(self, table: str, version: str, sha256: str, 
                 row_count: int, metadata: Dict = None) -> bool:
        """Add a new hash entry"""
        key = f"{table}_{version}"
        self._database["hashes"][key] = {
            "table": table,
            "version": version,
            "sha256": sha256,
            "row_count": row_count,
            "timestamp": datetime.now().isoformat(),
            "metadata": metadata or {}
        }
        self._save_database()
        return True
    
    def get_hash(self, table: str, version: str) -> Optional[Dict]:
        """Get hash entry for table/version"""
        key = f"{table}_{version}"
        return self._database["hashes"].get(key)
    
    def verify_hash(self, table: str, version: str, sha256: str,
                   row_count: Optional[int] = None) -> Tuple[bool, str]:
        """Verify a hash against baseline"""
        entry = self.get_hash(table, version)
        
        if not entry:
            return False, f"No baseline found for {table} (version: {version})"
        
        if entry["sha256"] != sha256:
            return False, f"Hash mismatch for {table} (version: {version})"
        
        if row_count is not None and entry["row_count"] != row_count:
            return False, f"Row count mismatch: expected {entry['row_count']}, got {row_count}"
        
        return True, "Hash verification passed"
    
    def list_all(self) -> List[Dict]:
        """List all hash entries"""
        return list(self._database["hashes"].values())
    
    def clear_version(self, version: str) -> int:
        """Clear all hashes for a version"""
        keys_to_remove = [k for k in self._database["hashes"].keys() 
                         if f"_{version}" in k]
        for key in keys_to_remove:
            del self._database["hashes"][key]
        self._save_database()
        return len(keys_to_remove)


class MySQLHashGenerator:
    """Generate ground truth SHA-256 hashes from MySQL database"""
    
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
                collation='utf8mb4_unicode_ci',
                connection_timeout=30
            )
        except MySQLError as e:
            print(f"Error connecting to MySQL: {e}")
            return None
    
    def get_mysql_version(self, conn) -> str:
        """Get MySQL version string"""
        cursor = conn.cursor()
        try:
            cursor.execute("SELECT VERSION()")
            return cursor.fetchone()[0].split('-')[0]
        finally:
            cursor.close()
    
    def get_table_columns(self, conn, table: str) -> List[str]:
        """Get column names for a table"""
        cursor = conn.cursor()
        try:
            cursor.execute(f"DESCRIBE {table}")
            return [row[0] for row in cursor.fetchall()]
        finally:
            cursor.close()
    
    def generate_table_hash(self, conn, table: str, output_file: str = None,
                           chunk_size: int = 10000) -> Tuple[str, int]:
        """Generate SHA-256 hash for table data with chunked processing"""
        cursor = conn.cursor()
        
        try:
            # Get column information
            columns = self.get_table_columns(conn, table)
            columns_str = ", ".join(columns)
            
            # Get total count
            cursor.execute(f"SELECT COUNT(*) FROM {table}")
            total_count = cursor.fetchone()[0]
            
            # Process in chunks to handle large datasets
            all_hashes = []
            processed = 0
            
            print(f"Processing {table}: {total_count:,} records...")
            
            # Get all data
            cursor.execute(f"SELECT {columns_str} FROM {table} ORDER BY id")
            
            while True:
                rows = cursor.fetchmany(chunk_size)
                if not rows:
                    break
                
                # Generate hash for each row
                for row in rows:
                    record = dict(zip(columns, row))
                    # Convert bytes to hex
                    for key, value in record.items():
                        if isinstance(value, bytes):
                            record[key] = value.hex()
                    # Generate row hash
                    row_json = json.dumps(record, sort_keys=True, ensure_ascii=False, 
                                         separators=(',', ':'))
                    all_hashes.append(hashlib.sha256(row_json.encode()).hexdigest())
                    processed += 1
                
                # Progress
                if processed % (chunk_size * 10) == 0:
                    print(f"  Processed {processed:,}/{total_count:,} records")
            
            # Generate final hash by combining all row hashes
            all_hashes.sort()  # Ensure deterministic ordering
            combined = "\n".join(all_hashes)
            final_hash = hashlib.sha256(combined.encode()).hexdigest()
            
            # Save to file
            if output_file:
                metadata = {
                    "table": table,
                    "generated_at": datetime.now().isoformat(),
                    "database": self.database,
                    "mysql_version": self.get_mysql_version(conn),
                    "row_count": total_count,
                    "column_count": len(columns),
                    "columns": columns,
                    "sha256": final_hash,
                    "row_hashes_count": len(all_hashes),
                    "checksum_method": "sha256_of_sorted_row_hashes"
                }
                
                with open(output_file, 'w') as f:
                    json.dump(metadata, f, indent=2, ensure_ascii=False)
                
                print(f"  ✓ Hash saved to: {output_file}")
            
            return final_hash, total_count
            
        except Exception as e:
            print(f"Error generating hash: {e}")
            return "", 0
        finally:
            cursor.close()
    
    def generate_all_hashes(self, output_dir: str = "./golden-hashes",
                           tables: List[str] = None, chunk_size: int = 10000) -> Dict[str, Dict]:
        """Generate ground truth hashes for all tables"""
        os.makedirs(output_dir, exist_ok=True)
        
        conn = self.create_connection()
        if not conn:
            return {}
        
        try:
            cursor = conn.cursor()
            
            # Get table list
            cursor.execute("SHOW TABLES")
            all_tables = [row[0] for row in cursor.fetchall()]
            
            # Filter tables if specified
            tables_to_process = tables or all_tables
            tables_to_process = [t for t in tables_to_process if t in all_tables]
            
            results = {}
            
            for table in tables_to_process:
                print(f"\nProcessing table: {table}")
                output_file = os.path.join(output_dir, f"hashes_{table}.json")
                
                sha256, count = self.generate_table_hash(
                    conn, table, output_file=output_file, chunk_size=chunk_size
                )
                
                if sha256:
                    version = self.get_mysql_version(conn)
                    results[table] = {
                        "sha256": sha256,
                        "row_count": count,
                        "output_file": output_file,
                        "version": version
                    }
            
            # Generate summary file
            summary = {
                "generated_at": datetime.now().isoformat(),
                "database": self.database,
                "tables": len(results),
                "hashes": results
            }
            
            summary_file = os.path.join(output_dir, "summary.json")
            with open(summary_file, 'w') as f:
                json.dump(summary, f, indent=2, ensure_ascii=False)
            
            print(f"\n✓ Summary saved to: {summary_file}")
            
            return results
            
        finally:
            conn.close()
    
    def verify_all_hashes(self, hash_dir: str = "./golden-hashes") -> Dict[str, Dict]:
        """Verify all hashes against baseline"""
        conn = self.create_connection()
        if not conn:
            return {}
        
        try:
            summary_file = os.path.join(hash_dir, "summary.json")
            
            if not os.path.exists(summary_file):
                print(f"No summary file found: {summary_file}")
                return {}
            
            with open(summary_file, 'r') as f:
                summary = json.load(f)
            
            results = {}
            
            for table, expected in summary.get("hashes", {}).items():
                sha256, count = self.generate_table_hash(conn, table)
                
                status = "PASS"
                messages = []
                
                if sha256 != expected["sha256"]:
                    status = "FAIL"
                    messages.append(f"Hash mismatch")
                
                if count != expected["row_count"]:
                    status = "FAIL"
                    messages.append(f"Row count mismatch")
                
                results[table] = {
                    "status": status,
                    "expected_sha256": expected["sha256"][:32] + "...",
                    "actual_sha256": sha256[:32] + "..." if sha256 else "N/A",
                    "expected_count": expected["row_count"],
                    "actual_count": count,
                    "messages": messages
                }
            
            return results
            
        finally:
            conn.close()


class IBDHashGenerator:
    """Generate hashes directly from .ibd files without MySQL connection"""
    
    def __init__(self, file_path: str):
        self.file_path = file_path
        self.page_size = 16384
    
    def detect_page_size(self) -> int:
        """Detect page size from file"""
        with open(self.file_path, 'rb') as f:
            data = f.read(self.page_size * 2)
        
        # Try different page sizes
        for ps in [4096, 8192, 16384, 32768, 65536]:
            if len(data) >= ps and ps <= len(data):
                # Check if this looks like a valid page
                page_type = int.from_bytes(data[16:18], 'little')
                if page_type in [0x26F1, 0x26F3]:  # INDEX or SDI
                    return ps
        return 16384
    
    def calculate_file_hash(self) -> str:
        """Calculate SHA-256 of entire file"""
        sha256 = hashlib.sha256()
        
        with open(self.file_path, 'rb') as f:
            while True:
                data = f.read(65536)  # 64KB chunks
                if not data:
                    break
                sha256.update(data)
        
        return sha256.hexdigest()
    
    def calculate_page_hashes(self, output_dir: str = None) -> Dict[int, str]:
        """Calculate hashes for each page"""
        self.page_size = self.detect_page_size()
        
        hashes = {}
        
        with open(self.file_path, 'rb') as f:
            page_no = 0
            while True:
                data = f.read(self.page_size)
                if not data:
                    break
                
                page_hash = hashlib.sha256(data).hexdigest()
                hashes[page_no] = page_hash
                
                if output_dir:
                    page_file = os.path.join(output_dir, f"page_{page_no:06d}.hash")
                    with open(page_file, 'w') as f:
                        json.dump({
                            "page_no": page_no,
                            "sha256": page_hash,
                            "timestamp": datetime.now().isoformat()
                        }, f, indent=2)
                
                page_no += 1
                
                if page_no % 100 == 0:
                    sys.stdout.write(f"\rProcessed {page_no} pages")
                    sys.stdout.flush()
        
        return hashes
    
    def generate_page_hash_database(self, output_file: str):
        """Generate page-level hash database"""
        hashes = self.calculate_page_hashes()
        
        db = {
            "file_path": self.file_path,
            "file_size": os.path.getsize(self.file_path),
            "page_size": self.page_size,
            "total_pages": len(hashes),
            "generated_at": datetime.now().isoformat(),
            "page_hashes": hashes,
            "file_hash": self.calculate_file_hash()
        }
        
        with open(output_file, 'w') as f:
            json.dump(db, f, indent=2)
        
        print(f"\n✓ Page hash database saved to: {output_file}")


def compare_hashes(hash1: str, hash2: str) -> Tuple[bool, float]:
    """Compare two hashes and return match status + similarity"""
    if hash1 == hash2:
        return True, 100.0
    
    # Compare character by character
    matches = sum(1 for a, b in zip(hash1, hash2) if a == b)
    similarity = (matches / max(len(hash1), len(hash2))) * 100
    
    return False, similarity


def main():
    parser = argparse.ArgumentParser(
        description='Golden Hash Base Generator for MySQL Recovery Pro'
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
    parser.add_argument('--output-dir', default='./golden-hashes',
                       help='Output directory for hash files')
    parser.add_argument('--table', help='Specific table to process')
    parser.add_argument('--version', default='8.0',
                       choices=['5.0', '5.5', '5.6', '5.7', '8.0', '8.4', '9.0'],
                       help='MySQL version for storage')
    parser.add_argument('--chunk-size', type=int, default=10000,
                       help='Chunk size for processing')
    parser.add_argument('--ibd', help='IBD file path for direct hashing')
    parser.add_argument('--generate-page-hashes', action='store_true',
                       help='Generate page-level hashes')
    parser.add_argument('--verify', action='store_true',
                       help='Verify hashes against baseline')
    parser.add_argument('--list', action='store_true',
                       help='List all stored hashes')
    
    args = parser.parse_args()
    
    if args.list:
        # List all stored hashes
        db = HashDatabase(args.output_dir)
        hashes = db.list_all()
        
        print(f"\nStored Hashes ({len(hashes)} entries):")
        print("-" * 80)
        for h in hashes:
            print(f"  {h['table']} ({h['version']}): {h['sha256'][:32]}...")
            print(f"    Rows: {h['row_count']:,} | Generated: {h['timestamp']}")
        
        sys.exit(0)
    
    if args.ibd:
        # IBD file hashing mode
        generator = IBDHashGenerator(args.ibd)
        
        if args.generate_page_hashes:
            output_file = os.path.join(args.output_dir, 
                f"page_hashes_{os.path.basename(args.ibd)}.json")
            generator.generate_page_hash_database(output_file)
        else:
            file_hash = generator.calculate_file_hash()
            print(f"File: {args.ibd}")
            print(f"SHA-256: {file_hash}")
            print(f"Page Size: {generator.page_size}")
            print(f"File Size: {os.path.getsize(args.ibd):,} bytes")
        
        sys.exit(0)
    
    if args.verify:
        # Verify mode
        generator = MySQLHashGenerator(
            host=args.host,
            port=args.port,
            user=args.user,
            password=args.password,
            database=args.database
        )
        
        results = generator.verify_all_hashes(args.output_dir)
        
        print(f"\nHash Verification Results:")
        print("-" * 80)
        for table, result in results.items():
            symbol = "✓" if result["status"] == "PASS" else "✗"
            print(f"{symbol} {table}: {result['status']}")
            if result["messages"]:
                for msg in result["messages"]:
                    print(f"    {msg}")
        
        sys.exit(0)
    
    # Generate mode
    if not MYSQL_CONNECTOR_AVAILABLE:
        print("Error: MySQL connector not installed")
        print("Install with: pip install mysql-connector-python")
        sys.exit(1)
    
    generator = MySQLHashGenerator(
        host=args.host,
        port=args.port,
        user=args.user,
        password=args.password,
        database=args.database
    )
    
    results = generator.generate_all_hashes(
        output_dir=args.output_dir,
        tables=[args.table] if args.table else None,
        chunk_size=args.chunk_size
    )
    
    # Store in hash database
    db = HashDatabase(args.output_dir)
    for table, info in results.items():
        db.add_hash(
            table=table,
            version=args.version,
            sha256=info["sha256"],
            row_count=info["row_count"],
            metadata={"output_file": info["output_file"]}
        )
    
    # Print summary
    print(f"\n{'='*60}")
    print(f"Hash Generation Summary")
    print(f"{'='*60}")
    print(f"Tables Processed: {len(results)}")
    print(f"Output Directory: {args.output_dir}")
    print(f"Version: {args.version}")
    
    for table, info in results.items():
        print(f"\n  {table}:")
        print(f"    SHA-256: {info['sha256'][:32]}...")
        print(f"    Rows: {info['row_count']:,}")
        print(f"    File: {info['output_file']}")


if __name__ == '__main__':
    main()
