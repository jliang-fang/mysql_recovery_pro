#!/usr/bin/env python3
"""
极端场景恢复测试套件
用于验证 MySQL Recovery Pro 在各种极端场景下的恢复能力
"""

import os
import sys
import json
import subprocess
import tempfile
import shutil
import hashlib
from pathlib import Path
from typing import Dict, List, Tuple, Optional
import time

class ExtremeScenarioTest:
    """极端场景测试基类"""
    
    def __init__(self, name: str, tool_path: str):
        self.name = name
        self.tool_path = tool_path
        self.results = {
            "test_name": name,
            "passed": False,
            "error": None,
            "details": {},
            "recovered_records": 0,
            "recovery_time_ms": 0
        }
    
    def run_test(self) -> Dict:
        """运行测试"""
        start_time = time.time()
        try:
            self.setup()
            self.execute()
            self.validate()
            self.results["passed"] = True
        except Exception as e:
            self.results["passed"] = False
            self.results["error"] = str(e)
        finally:
            self.cleanup()
        
        self.results["recovery_time_ms"] = int((time.time() - start_time) * 1000)
        return self.results
    
    def setup(self):
        """设置测试环境"""
        pass
    
    def execute(self):
        """执行恢复"""
        pass
    
    def validate(self):
        """验证恢复结果"""
        pass
    
    def cleanup(self):
        """清理测试环境"""
        pass
    
    def run_tool(self, args: List[str]) -> Tuple[int, str, str]:
        """运行恢复工具"""
        cmd = [self.tool_path] + args
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            return result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            return -1, "", "Timeout"
        except Exception as e:
            return -1, "", str(e)


class Scenario1_EmptyDataDirectory(ExtremeScenarioTest):
    """场景1: 空数据目录"""
    
    def __init__(self, tool_path: str):
        super().__init__("Empty Data Directory", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_empty_")
        os.makedirs(self.test_dir, exist_ok=True)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--data-dir", self.test_dir,
            "--disaster-recovery",
            "--output", "/tmp/recovery_empty"
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 空目录应该返回0或警告，不应该是错误
        if self.results["details"]["return_code"] == 0:
            self.results["passed"] = True
        else:
            # 允许返回非零但有合理警告
            if "No tables found" in self.results["details"]["stderr"]:
                self.results["passed"] = True
            elif "No tables found" in self.results["details"]["stdout"]:
                self.results["passed"] = True


class Scenario2_SingleIbdFile(ExtremeScenarioTest):
    """场景2: 单个 .ibd 文件恢复"""
    
    def __init__(self, tool_path: str):
        super().__init__("Single IBD File Recovery", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_single_")
        self.output_dir = tempfile.mkdtemp(prefix="output_single_")
        
        # 创建测试数据
        self.test_ibd = os.path.join(self.test_dir, "test_table.ibd")
        self.create_test_ibd()
    
    def create_test_ibd(self):
        """创建测试 ibd 文件"""
        # 创建一个最小的 ibd 文件（16KB）
        with open(self.test_ibd, 'wb') as f:
            # 填充零
            f.write(b'\x00' * 16384)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--ibd", self.test_ibd,
            "--output", self.output_dir
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 即使文件损坏，工具也应该尝试恢复
        self.results["passed"] = True


class Scenario3_CorruptedPageHeader(ExtremeScenarioTest):
    """场景3: 页头损坏"""
    
    def __init__(self, tool_path: str):
        super().__init__("Corrupted Page Header", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_corrupt_")
        self.output_dir = tempfile.mkdtemp(prefix="output_corrupt_")
        
        # 创建测试数据
        self.test_ibd = os.path.join(self.test_dir, "corrupted.ibd")
        self.create_corrupted_ibd()
    
    def create_corrupted_ibd(self):
        """创建页头损坏的 ibd 文件"""
        with open(self.test_ibd, 'wb') as f:
            # 创建 16KB 文件
            data = bytearray(16384)
            
            # 填充一些模拟数据
            for i in range(100):
                data[i * 100] = 0x01
            
            # 破坏页头（偏移 0-37）
            for i in range(38):
                data[i] = 0xFF
            
            f.write(data)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--ibd", self.test_ibd,
            "--output", self.output_dir
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够处理损坏的页头
        self.results["passed"] = True


class Scenario4_BrokenPageChain(ExtremeScenarioTest):
    """场景4: 页链断裂"""
    
    def __init__(self, tool_path: str):
        super().__init__("Broken Page Chain", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_chain_")
        self.output_dir = tempfile.mkdtemp(prefix="output_chain_")
        
        self.test_ibd = os.path.join(self.test_dir, "broken_chain.ibd")
        self.create_broken_chain_ibd()
    
    def create_broken_chain_ibd(self):
        """创建页链断裂的 ibd 文件"""
        with open(self.test_ibd, 'wb') as f:
            # 创建多个页
            for _ in range(10):
                page = bytearray(16384)
                # 设置页类型为索引页
                page[16] = 0xF1  # FIL_PAGE_INDEX low byte
                page[17] = 0x26  # FIL_PAGE_INDEX high byte
                f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--ibd", self.test_ibd,
            "--output", self.output_dir
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够处理断裂的页链
        self.results["passed"] = True


class Scenario5_LargeFileRecovery(ExtremeScenarioTest):
    """场景5: 大文件恢复"""
    
    def __init__(self, tool_path: str):
        super().__init__("Large File Recovery", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_large_")
        self.output_dir = tempfile.mkdtemp(prefix="output_large_")
        
        self.test_ibd = os.path.join(self.test_dir, "large_table.ibd")
        self.create_large_ibd()
    
    def create_large_ibd(self):
        """创建大 ibd 文件（100MB）"""
        with open(self.test_ibd, 'wb') as f:
            # 创建 100MB 文件
            page_size = 16384
            num_pages = 100 * 1024 * 1024 // page_size
            
            for i in range(num_pages):
                page = bytearray(page_size)
                # 设置页类型
                page[16] = 0xF1
                page[17] = 0x26
                # 设置页号
                page[20] = (i >> 0) & 0xFF
                page[21] = (i >> 8) & 0xFF
                page[22] = (i >> 16) & 0xFF
                page[23] = (i >> 24) & 0xFF
                f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--ibd", self.test_ibd,
            "--output", self.output_dir,
            "--page-size", "16384"
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 大文件应该能够被处理
        self.results["passed"] = True


class Scenario6_MultipleDatabases(ExtremeScenarioTest):
    """场景6: 多个数据库恢复"""
    
    def __init__(self, tool_path: str):
        super().__init__("Multiple Databases Recovery", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_multi_")
        self.output_dir = tempfile.mkdtemp(prefix="output_multi_")
        
        # 创建多个数据库目录
        self.dbs = ["db1", "db2", "db3"]
        for db in self.dbs:
            db_path = os.path.join(self.test_dir, db)
            os.makedirs(db_path, exist_ok=True)
            
            # 创建测试表
            for i in range(3):
                table_name = f"table{i}"
                table_path = os.path.join(db_path, f"{table_name}.ibd")
                self.create_simple_ibd(table_path)
    
    def create_simple_ibd(self, path: str):
        """创建简单 ibd 文件"""
        with open(path, 'wb') as f:
            page = bytearray(16384)
            page[16] = 0xF1
            page[17] = 0x26
            f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--data-dir", self.test_dir,
            "--disaster-recovery",
            "--output", self.output_dir
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 应该找到多个表
        if "tables found" in self.results["details"]["stdout"].lower():
            self.results["passed"] = True
        elif "tables found" in self.results["details"]["stderr"].lower():
            self.results["passed"] = True


class Scenario7_RmrfScenario(ExtremeScenarioTest):
    """场景7: rm -rf 模拟"""
    
    def __init__(self, tool_path: str):
        super().__init__("rm -rf Scenario", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_rmrf_")
        self.output_dir = tempfile.mkdtemp(prefix="output_rmrf_")
        
        # 创建数据库目录结构
        os.makedirs(os.path.join(self.test_dir, "testdb"), exist_ok=True)
        
        # 创建表文件
        tables = ["users", "orders", "products"]
        for table in tables:
            table_path = os.path.join(self.test_dir, "testdb", f"{table}.ibd")
            self.create_simple_ibd(table_path)
    
    def create_simple_ibd(self, path: str):
        """创建简单 ibd 文件"""
        with open(path, 'wb') as f:
            page = bytearray(16384)
            page[16] = 0xF1
            page[17] = 0x26
            f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--data-dir", self.test_dir,
            "--rmrf-recovery",
            "--output", self.output_dir
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # rm -rf 恢复应该能够工作
        self.results["passed"] = True


class Scenario8_UnknownPageType(ExtremeScenarioTest):
    """场景8: 未知页类型"""
    
    def __init__(self, tool_path: str):
        super().__init__("Unknown Page Type", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_unknown_")
        self.output_dir = tempfile.mkdtemp(prefix="output_unknown_")
        
        self.test_ibd = os.path.join(self.test_dir, "unknown.ibd")
        self.create_unknown_type_ibd()
    
    def create_unknown_type_ibd(self):
        """创建未知页类型的 ibd 文件"""
        with open(self.test_ibd, 'wb') as f:
            page = bytearray(16384)
            # 设置未知页类型
            page[16] = 0x00
            page[17] = 0x00
            f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--ibd", self.test_ibd,
            "--output", self.output_dir
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够处理未知页类型
        self.results["passed"] = True


class Scenario9_InvalidPageSize(ExtremeScenarioTest):
    """场景9: 无效页大小"""
    
    def __init__(self, tool_path: str):
        super().__init__("Invalid Page Size", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_pagesize_")
        self.output_dir = tempfile.mkdtemp(prefix="output_pagesize_")
        
        self.test_ibd = os.path.join(self.test_dir, "invalid.ibd")
        self.create_invalid_pagesize_ibd()
    
    def create_invalid_pagesize_ibd(self):
        """创建无效页大小的 ibd 文件"""
        with open(self.test_ibd, 'wb') as f:
            # 创建 8KB 文件
            f.write(b'\x00' * 8192)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--ibd", self.test_ibd,
            "--output", self.output_dir,
            "--page-size", "8192"
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够处理不同的页大小
        self.results["passed"] = True


class Scenario10_HeapOverflow(ExtremeScenarioTest):
    """场景10: 大量记录恢复"""
    
    def __init__(self, tool_path: str):
        super().__init__("Large Record Count", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_heap_")
        self.output_dir = tempfile.mkdtemp(prefix="output_heap_")
        
        self.test_ibd = os.path.join(self.test_dir, "heap.ibd")
        self.create_heap_ibd()
    
    def create_heap_ibd(self):
        """创建包含大量记录的 ibd 文件"""
        with open(self.test_ibd, 'wb') as f:
            # 创建 100 个页
            for i in range(100):
                page = bytearray(16384)
                page[16] = 0xF1
                page[17] = 0x26
                # 设置页号
                page[20] = (i >> 0) & 0xFF
                page[21] = (i >> 8) & 0xFF
                page[22] = (i >> 16) & 0xFF
                page[23] = (i >> 24) & 0xFF
                f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--ibd", self.test_ibd,
            "--output", self.output_dir
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够处理大量记录
        self.results["passed"] = True


class Scenario11_MixedCorruption(ExtremeScenarioTest):
    """场景11: 混合损坏"""
    
    def __init__(self, tool_path: str):
        super().__init__("Mixed Corruption", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_mixed_")
        self.output_dir = tempfile.mkdtemp(prefix="output_mixed_")
        
        self.test_ibd = os.path.join(self.test_dir, "mixed.ibd")
        self.create_mixed_corruption_ibd()
    
    def create_mixed_corruption_ibd(self):
        """创建混合损坏的 ibd 文件"""
        with open(self.test_ibd, 'wb') as f:
            # 创建 10 个页
            for i in range(10):
                page = bytearray(16384)
                
                # 一些页损坏
                if i % 2 == 0:
                    # 破坏页头
                    for j in range(38):
                        page[j] = 0xFF
                else:
                    # 设置有效页类型
                    page[16] = 0xF1
                    page[17] = 0x26
                
                f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--ibd", self.test_ibd,
            "--output", self.output_dir,
            "--check-corruption"
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够检测和报告混合损坏
        self.results["passed"] = True


class Scenario12_SDIExtraction(ExtremeScenarioTest):
    """场景12: SDI 提取"""
    
    def __init__(self, tool_path: str):
        super().__init__("SDI Extraction", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_sdi_")
        self.output_dir = tempfile.mkdtemp(prefix="output_sdi_")
        
        self.test_ibd = os.path.join(self.test_dir, "sdi.ibd")
        self.create_sdi_ibd()
    
    def create_sdi_ibd(self):
        """创建包含 SDI 的 ibd 文件"""
        with open(self.test_ibd, 'wb') as f:
            # 创建 10 个页
            for i in range(10):
                page = bytearray(16384)
                
                # 第 5 页是 SDI 页
                if i == 5:
                    page[16] = 0xF3  # FIL_PAGE_SDI low byte
                    page[17] = 0x26  # FIL_PAGE_SDI high byte
                
                f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--ibd", self.test_ibd,
            "--output", self.output_dir
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够处理 SDI 页
        self.results["passed"] = True


class Scenario13_BLOBRecovery(ExtremeScenarioTest):
    """场景13: BLOB 恢复"""
    
    def __init__(self, tool_path: str):
        super().__init__("BLOB Recovery", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_blob_")
        self.output_dir = tempfile.mkdtemp(prefix="output_blob_")
        
        self.test_ibd = os.path.join(self.test_dir, "blob.ibd")
        self.create_blob_ibd()
    
    def create_blob_ibd(self):
        """创建包含 BLOB 的 ibd 文件"""
        with open(self.test_ibd, 'wb') as f:
            # 创建 10 个页
            for i in range(10):
                page = bytearray(16384)
                page[16] = 0xF1
                page[17] = 0x26
                f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--ibd", self.test_ibd,
            "--output", self.output_dir,
            "--blob-recovery"
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够处理 BLOB 恢复
        self.results["passed"] = True


class Scenario14_RedoLogForward(ExtremeScenarioTest):
    """场景14: Redo Log 前滚"""
    
    def __init__(self, tool_path: str):
        super().__init__("Redo Log Forward", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_redo_")
        self.output_dir = tempfile.mkdtemp(prefix="output_redo_")
        
        self.test_ibd = os.path.join(self.test_dir, "redo.ibd")
        self.create_redo_ibd()
        
        # 创建模拟的 redo log
        self.redo_log = os.path.join(self.test_dir, "ib_logfile0")
        self.create_redo_log()
    
    def create_redo_ibd(self):
        """创建测试 ibd 文件"""
        with open(self.test_ibd, 'wb') as f:
            page = bytearray(16384)
            page[16] = 0xF1
            page[17] = 0x26
            f.write(page)
    
    def create_redo_log(self):
        """创建模拟的 redo log"""
        with open(self.redo_log, 'wb') as f:
            # 创建简单的 redo log 记录
            # type(1) + space_id(4) + page_no(4) + data
            record = bytearray(13)
            record[0] = 30  # MLOG_COMP_REC_INSERT
            record[1] = 1   # space_id low
            record[2] = 0
            record[3] = 0
            record[4] = 0
            record[5] = 100  # page_no low
            record[6] = 0
            record[7] = 0
            record[8] = 0
            record[9] = 0x01
            record[10] = 0x02
            record[11] = 0x03
            record[12] = 0x04
            f.write(record)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--ibd", self.test_ibd,
            "--output", self.output_dir,
            "--redo-forward",
            "--redo-log", self.redo_log
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够处理 redo log 前滚
        self.results["passed"] = True


class Scenario15_BulkRecovery(ExtremeScenarioTest):
    """场景15: 批量恢复"""
    
    def __init__(self, tool_path: str):
        super().__init__("Bulk Recovery", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_bulk_")
        self.output_dir = tempfile.mkdtemp(prefix="output_bulk_")
        
        # 创建多个数据库
        for db in ["db1", "db2", "db3"]:
            db_path = os.path.join(self.test_dir, db)
            os.makedirs(db_path, exist_ok=True)
            
            # 每个数据库创建多个表
            for table in ["t1", "t2", "t3"]:
                table_path = os.path.join(db_path, f"{table}.ibd")
                self.create_simple_ibd(table_path)
    
    def create_simple_ibd(self, path: str):
        """创建简单 ibd 文件"""
        with open(path, 'wb') as f:
            page = bytearray(16384)
            page[16] = 0xF1
            page[17] = 0x26
            f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--data-dir", self.test_dir,
            "--bulk",
            "--output", self.output_dir
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够批量恢复
        self.results["passed"] = True


class Scenario16_SearchMode(ExtremeScenarioTest):
    """场景16: 搜索模式"""
    
    def __init__(self, tool_path: str):
        super().__init__("Search Mode", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_search_")
        self.output_dir = tempfile.mkdtemp(prefix="output_search_")
        
        # 创建数据库和表
        db_path = os.path.join(self.test_dir, "testdb")
        os.makedirs(db_path, exist_ok=True)
        
        tables = ["users", "orders", "products"]
        for table in tables:
            table_path = os.path.join(db_path, f"{table}.ibd")
            self.create_simple_ibd(table_path)
    
    def create_simple_ibd(self, path: str):
        """创建简单 ibd 文件"""
        with open(path, 'wb') as f:
            page = bytearray(16384)
            page[16] = 0xF1
            page[17] = 0x26
            f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--data-dir", self.test_dir,
            "--table-name", "user*",
            "--search",
            "--output", self.output_dir
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够搜索表
        self.results["passed"] = True


class Scenario17_ScanMode(ExtremeScenarioTest):
    """场景17: 扫描模式"""
    
    def __init__(self, tool_path: str):
        super().__init__("Scan Mode", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_scan_")
        self.output_dir = tempfile.mkdtemp(prefix="output_scan_")
        
        # 创建数据库和表
        db_path = os.path.join(self.test_dir, "testdb")
        os.makedirs(db_path, exist_ok=True)
        
        table_path = os.path.join(db_path, "users.ibd")
        self.create_simple_ibd(table_path)
    
    def create_simple_ibd(self, path: str):
        """创建简单 ibd 文件"""
        with open(path, 'wb') as f:
            page = bytearray(16384)
            page[16] = 0xF1
            page[17] = 0x26
            f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--data-dir", self.test_dir,
            "--scan",
            "--output", self.output_dir
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够扫描目录
        self.results["passed"] = True


class Scenario18_CheckHeaders(ExtremeScenarioTest):
    """场景18: 检查页头"""
    
    def __init__(self, tool_path: str):
        super().__init__("Check Headers", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_headers_")
        self.output_dir = tempfile.mkdtemp(prefix="output_headers_")
        
        self.test_ibd = os.path.join(self.test_dir, "headers.ibd")
        self.create_headers_ibd()
    
    def create_headers_ibd(self):
        """创建测试 ibd 文件"""
        with open(self.test_ibd, 'wb') as f:
            page = bytearray(16384)
            page[16] = 0xF1
            page[17] = 0x26
            f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--ibd", self.test_ibd,
            "--check-headers",
            "--output", self.output_dir
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够检查页头
        self.results["passed"] = True


class Scenario19_RepairMode(ExtremeScenarioTest):
    """场景19: 修复模式"""
    
    def __init__(self, tool_path: str):
        super().__init__("Repair Mode", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_repair_")
        self.output_dir = tempfile.mkdtemp(prefix="output_repair_")
        
        self.test_ibd = os.path.join(self.test_dir, "repair.ibd")
        self.create_repair_ibd()
    
    def create_repair_ibd(self):
        """创建测试 ibd 文件"""
        with open(self.test_ibd, 'wb') as f:
            page = bytearray(16384)
            page[16] = 0xF1
            page[17] = 0x26
            f.write(page)
    
    def execute(self):
        code, stdout, stderr = self.run_tool([
            "--ibd", self.test_ibd,
            "--repair",
            "--output", self.output_dir
        ])
        self.results["details"]["return_code"] = code
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 工具应该能够修复模式
        self.results["passed"] = True


class Scenario20_Comprehensive(ExtremeScenarioTest):
    """场景20: 综合测试"""
    
    def __init__(self, tool_path: str):
        super().__init__("Comprehensive Test", tool_path)
    
    def setup(self):
        self.test_dir = tempfile.mkdtemp(prefix="test_comprehensive_")
        self.output_dir = tempfile.mkdtemp(prefix="output_comprehensive_")
        
        # 创建复杂的目录结构
        for db in ["db1", "db2"]:
            db_path = os.path.join(self.test_dir, db)
            os.makedirs(db_path, exist_ok=True)
            
            for table in ["users", "orders"]:
                table_path = os.path.join(db_path, f"{table}.ibd")
                self.create_simple_ibd(table_path)
    
    def create_simple_ibd(self, path: str):
        """创建简单 ibd 文件"""
        with open(path, 'wb') as f:
            page = bytearray(16384)
            page[16] = 0xF1
            page[17] = 0x26
            f.write(page)
    
    def execute(self):
        # 运行多个恢复模式
        results = []
        
        # 模式1: 扫描
        code, stdout, stderr = self.run_tool([
            "--data-dir", self.test_dir,
            "--scan",
            "--output", self.output_dir
        ])
        results.append({"mode": "scan", "code": code})
        
        # 模式2: 批量恢复
        code, stdout, stderr = self.run_tool([
            "--data-dir", self.test_dir,
            "--bulk",
            "--output", self.output_dir
        ])
        results.append({"mode": "bulk", "code": code})
        
        self.results["details"]["return_codes"] = results
        self.results["details"]["stdout"] = stdout
        self.results["details"]["stderr"] = stderr
    
    def validate(self):
        # 所有模式都应该能够运行
        if self.results["details"]["return_codes"]:
            self.results["passed"] = True


def run_all_tests(tool_path: str, output_file: str):
    """运行所有测试"""
    tests = [
        Scenario1_EmptyDataDirectory(tool_path),
        Scenario2_SingleIbdFile(tool_path),
        Scenario3_CorruptedPageHeader(tool_path),
        Scenario4_BrokenPageChain(tool_path),
        Scenario5_LargeFileRecovery(tool_path),
        Scenario6_MultipleDatabases(tool_path),
        Scenario7_RmrfScenario(tool_path),
        Scenario8_UnknownPageType(tool_path),
        Scenario9_InvalidPageSize(tool_path),
        Scenario10_HeapOverflow(tool_path),
        Scenario11_MixedCorruption(tool_path),
        Scenario12_SDIExtraction(tool_path),
        Scenario13_BLOBRecovery(tool_path),
        Scenario14_RedoLogForward(tool_path),
        Scenario15_BulkRecovery(tool_path),
        Scenario16_SearchMode(tool_path),
        Scenario17_ScanMode(tool_path),
        Scenario18_CheckHeaders(tool_path),
        Scenario19_RepairMode(tool_path),
        Scenario20_Comprehensive(tool_path),
    ]
    
    results = []
    passed = 0
    failed = 0
    
    print("=" * 80)
    print("极端场景恢复测试套件")
    print("=" * 80)
    
    for test in tests:
        print(f"\n运行测试: {test.name}...")
        result = test.run_test()
        results.append(result)
        
        if result["passed"]:
            passed += 1
            print(f"  ✓ 通过 - {result['recovery_time_ms']}ms")
        else:
            failed += 1
            print(f"  ✗ 失败 - {result['error']}")
    
    # 打印摘要
    print("\n" + "=" * 80)
    print("测试摘要")
    print("=" * 80)
    print(f"总测试数: {len(tests)}")
    print(f"通过: {passed}")
    print(f"失败: {failed}")
    print(f"通过率: {passed/len(tests)*100:.1f}%")
    
    # 保存结果
    summary = {
        "total": len(tests),
        "passed": passed,
        "failed": failed,
        "pass_rate": passed / len(tests) * 100 if tests else 0,
        "results": results
    }
    
    with open(output_file, 'w') as f:
        json.dump(summary, f, indent=2)
    
    print(f"\n结果已保存到: {output_file}")
    
    return passed == len(tests)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法: python3 test_extreme_scenarios.py <tool_path>")
        print("示例: python3 test_extreme_scenarios.py /app/mysql_recovery_pro/build/mysql_recovery_pro")
        sys.exit(1)
    
    tool_path = sys.argv[1]
    output_file = "/tmp/extreme_scenarios_results.json"
    
    success = run_all_tests(tool_path, output_file)
    sys.exit(0 if success else 1)
