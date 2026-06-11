# MySQL 数据恢复工具评估与验证平台设计文档

## 1. 整体架构设计

### 1.1 模块划分

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        测试平台主控层 (Test Orchestrator)                │
│  - 配置解析 (YAML)                                                      │
│  - 用例编排与调度                                                        │
│  - 断点续跑 & 结果持久化                                                 │
└─────────────────────────────────────────────────────────────────────────┘
                                     │
         ┌───────────────────────────┼───────────────────────────┐
         ▼                           ▼                           ▼
┌─────────────────┐     ┌─────────────────────┐     ┌─────────────────────┐
│ 容器管理模块    │     │ 破坏与恢复模块       │     │ 验证与报告模块       │
│ (ContainerMgr) │     │ (RecoveryTester)    │     │ (VerificationMgr)   │
│                 │     │                      │     │                      │
│ - Podman 管理   │     │ - 破坏场景执行器     │     │ - 基线采集           │
│ - MySQL 实例    │     │ - 恢复工具调用       │     │ - 恢复率计算         │
│ - 生命周期管理  │     │ - 性能指标采集       │     │ - 报告生成           │
└─────────────────┘     └─────────────────────┘     └─────────────────────┘
         │                           │                           │
         └───────────────────────────┼───────────────────────────┘
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                          数据层 (Data Layer)                            │
│  - 测试配置 (YAML)  - 基线数据 (SHA256)  - 恢复结果 (JSON/CSV/SQL)     │
│  - 性能指标 (JSON)  - 测试报告 (MD/HTML)  - 日志归档                    │
└─────────────────────────────────────────────────────────────────────────┘
```

### 1.2 数据流

```
[配置加载] → [容器启动] → [数据注入] → [基线采集] → [破坏执行]
    │                                        │
    ▼                                        ▼
[断点检查] ←─────────────────────────── [恢复执行]
    │                                        │
    ▼                                        ▼
[结果汇总] ← [恢复率计算] ← [数据导入临时库] ← [工具输出]
    │
    ▼
[报告生成] → [Markdown/HTML] + [JSON明细]
```

### 1.3 核心组件

| 组件 | 职责 | 关键接口 |
|------|------|----------|
| `TestOrchestrator` | 主控调度 | `run_matrix()`, `resume_from_checkpoint()` |
| `ContainerManager` | Podman容器管理 | `start_mysql()`, `stop_mysql()`, `get_datadir()` |
| `DataGenerator` | 测试数据生成 | `generate_table()`, `compute_baseline()` |
| `DestructionEngine` | 破坏场景执行 | `truncate_table()`, `rm_ibd()`, `inject_badblocks()` |
| `RecoveryRunner` | 恢复工具调用 | `run_recovery()`, `collect_metrics()` |
| `VerificationEngine` | 结果验证 | `compute_recovery_rate()`, `compare_hashes()` |
| `ReportGenerator` | 报告生成 | `generate_matrix_report()`, `export_html()` |

---

## 2. 核心代码骨架

### 2.1 项目结构

```
recovery_test_platform/
├── config/
│   ├── test_matrix.yaml          # 测试矩阵配置
│   └── mysql_versions.yaml       # MySQL版本镜像映射
├── src/
│   ├── __init__.py
│   ├── orchestrator.py           # 主控调度
│   ├── container_manager.py      # Podman容器管理
│   ├── data_generator.py         # 数据生成器
│   ├── destruction_engine.py     # 破坏执行器
│   ├── recovery_runner.py        # 恢复工具调用
│   ├── verification_engine.py    # 验证引擎
│   ├── report_generator.py       # 报告生成
│   └── models.py                 # 数据模型定义
├── scripts/
│   ├── init_mysql_schema.sql     # 表结构模板
│   └── inject_data.py            # 数据注入脚本
├── templates/
│   ├── report.md.j2              # Markdown报告模板
│   └── report.html.j2            # HTML报告模板
├── results/                      # 测试结果输出
│   ├── baselines/                # 基线数据
│   ├── recovered/                # 恢复数据
│   └── reports/                  # 报告输出
├── main.py                       # 入口脚本
└── requirements.txt
```

### 2.2 数据模型 (models.py)

```python
#!/usr/bin/env python3
"""数据模型定义"""
from dataclasses import dataclass, field
from enum import Enum
from typing import Dict, List, Optional
from datetime import datetime
import hashlib
import json

class MySQLVersion(Enum):
    V56 = "5.6"
    V57 = "5.7"
    V80 = "8.0"
    V84 = "8.4"
    V90 = "9.0"

class DestructionType(Enum):
    TRUNCATE = "truncate"
    DROP_TABLE = "drop_table"
    DROP_TABLE_PURGE = "drop_table_purge"
    BAD_BLOCKS = "bad_blocks"
    RM_SINGLE_IBD = "rm_single_ibd"
    RM_IBD_FRM = "rm_ibd_frm"
    RM_MULTIPLE_TABLES = "rm_multiple_tables"
    RM_DATADIR = "rm_datadir"

class DataScale(Enum):
    S5K = "5k"      # 5,000 rows
    S10K = "10k"    # 10,000 rows
    S1GB = "1gb"    # ~1M rows (1KB/row)
    S10GB = "10gb"  # ~10M rows
    S20GB = "20gb"  # ~20M rows

@dataclass
class TableSchema:
    """表结构定义"""
    name: str
    columns: List[Dict[str, str]]  # [{name, type, nullable, is_pk}, ...]
    row_format: str = "COMPACT"
    
    def to_create_sql(self) -> str:
        cols = []
        pk_cols = []
        for col in self.columns:
            col_def = f"`{col['name']}` {col['type']}"
            if col.get('nullable', True):
                col_def += " NULL"
            else:
                col_def += " NOT NULL"
            if col.get('is_pk', False):
                pk_cols.append(col['name'])
            cols.append(col_def)
        
        sql = f"CREATE TABLE `{self.name}` (\n  "
        sql += ",\n  ".join(cols)
        if pk_cols:
            sql += f",\n  PRIMARY KEY ({', '.join(pk_cols)})"
        sql += f"\n) ENGINE=InnoDB ROW_FORMAT={self.row_format};"
        return sql

@dataclass
class Baseline:
    """基线数据"""
    table_name: str
    row_count: int
    data_size_bytes: int
    sha256_hash: str
    checksum_value: Optional[int] = None
    schema_sql: str = ""
    sample_data_path: str = ""
    created_at: datetime = field(default_factory=datetime.now)

@dataclass
class RecoveryMetrics:
    """恢复性能指标"""
    total_duration_sec: float
    scan_duration_sec: float
    parse_duration_sec: float
    export_duration_sec: float
    peak_memory_gb: float
    disk_throughput_mbps: float
    recovered_size_bytes: int
    temp_file_size_bytes: int

@dataclass
class TestCase:
    """测试用例"""
    id: str
    mysql_version: MySQLVersion
    destruction_type: DestructionType
    data_scale: DataScale
    table_schema: TableSchema
    
    # 执行状态
    status: str = "pending"  # pending, running, passed, failed, skipped
    error_message: str = ""
    
    # 基线与结果
    baseline: Optional[Baseline] = None
    recovered_row_count: int = 0
    matched_row_count: int = 0
    recovery_rate: float = 0.0
    content_match_rate: float = 0.0
    
    # 性能指标
    metrics: Optional[RecoveryMetrics] = None
    
    # 时间戳
    started_at: Optional[datetime] = None
    finished_at: Optional[datetime] = None

@dataclass
class TestResult:
    """测试结果"""
    test_case_id: str
    passed: bool
    recovery_rate: float
    content_match_rate: float
    duration_sec: float
    details: Dict = field(default_factory=dict)
    
    def to_dict(self) -> dict:
        return {
            "test_case_id": self.test_case_id,
            "passed": self.passed,
            "recovery_rate": self.recovery_rate,
            "content_match_rate": self.content_match_rate,
            "duration_sec": self.duration_sec,
            "details": self.details
        }

@dataclass
class TestMatrix:
    """测试矩阵"""
    versions: List[MySQLVersion]
    destruction_types: List[DestructionType]
    scales: List[DataScale]
    
    def generate_cases(self, schema: TableSchema) -> List[TestCase]:
        cases = []
        case_id = 0
        for version in self.versions:
            for dtype in self.destruction_types:
                for scale in self.scales:
                    case_id += 1
                    cases.append(TestCase(
                        id=f"TC-{case_id:04d}",
                        mysql_version=version,
                        destruction_type=dtype,
                        data_scale=scale,
                        table_schema=schema
                    ))
        return cases
```

### 2.3 容器管理模块 (container_manager.py)

```python
#!/usr/bin/env python3
"""Podman容器管理模块"""
import subprocess
import time
import json
import shutil
from pathlib import Path
from typing import Dict, Optional, Tuple
from dataclasses import dataclass
import logging

logger = logging.getLogger(__name__)

# MySQL版本到镜像映射
MYSQL_IMAGES = {
    "5.6": "mysql:5.6",
    "5.7": "mysql:5.7",
    "8.0": "mysql:8.0",
    "8.4": "mysql:8.4",
    "9.0": "mysql:9.0",
}

CONTAINER_PORTS = {
    "5.6": 3306,
    "5.7": 3307,
    "8.0": 3308,
    "8.4": 3309,
    "9.0": 3310,
}

@dataclass
class MySQLInstance:
    container_id: str
    container_name: str
    version: str
    datadir_host: Path
    port: int
    root_password: str = "test123456"

class ContainerManager:
    """Podman容器管理器"""
    
    def __init__(self, base_dir: Path = Path("/tmp/mysql_test")):
        self.base_dir = base_dir
        self.base_dir.mkdir(parents=True, exist_ok=True)
        self.containers: Dict[str, MySQLInstance] = {}
        
    def _run_podman(self, args: list, check: bool = True) -> Tuple[bool, str, str]:
        """执行podman命令"""
        cmd = ["podman"] + args
        logger.debug(f"Running: {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True)
        if check and result.returncode != 0:
            raise RuntimeError(f"Podman command failed: {result.stderr}")
        return result.returncode == 0, result.stdout, result.stderr
    
    def pull_image(self, version: str) -> bool:
        """拉取MySQL镜像"""
        image = MYSQL_IMAGES.get(version)
        if not image:
            raise ValueError(f"Unsupported MySQL version: {version}")
        
        logger.info(f"Pulling image: {image}")
        success, _, err = self._run_podman(["pull", image], check=False)
        return success
    
    def create_mysql_instance(
        self, 
        version: str,
        port_override: Optional[int] = None
    ) -> MySQLInstance:
        """创建MySQL容器实例"""
        image = MYSQL_IMAGES[version]
        port = port_override or CONTAINER_PORTS[version]
        container_name = f"mysql_test_{version.replace('.', '_')}"
        datadir = self.base_dir / version / "datadir"
        datadir.mkdir(parents=True, exist_ok=True)
        
        # 检查容器是否已存在
        success, stdout, _ = self._run_podman(
            ["ps", "-a", "--filter", f"name={container_name}", "--format", "{{.ID}}"],
            check=False
        )
        existing_id = stdout.strip()
        if existing_id:
            logger.info(f"Container {container_name} exists, removing...")
            self._run_podman(["rm", "-f", existing_id], check=False)
        
        # 创建新容器
        args = [
            "run", "-d",
            "--name", container_name,
            "-e", "MYSQL_ROOT_PASSWORD=test123456",
            "-e", "MYSQL_DATABASE=testdb",
            "-e", "INNODB_FILE_PER_TABLE=ON",
            "-p", f"{port}:3306",
            "-v", f"{datadir}:/var/lib/mysql",
            image
        ]
        
        # MySQL 8.0+ 需要额外参数
        if version >= "8.0":
            args.insert(-1, "--character-set-server=utf8mb4")
            args.insert(-1, "--collation-server=utf8mb4_unicode_ci")
        
        success, stdout, err = self._run_podman(args, check=False)
        if not success:
            raise RuntimeError(f"Failed to create container: {err}")
        
        container_id = stdout.strip()[:12]
        
        instance = MySQLInstance(
            container_id=container_id,
            container_name=container_name,
            version=version,
            datadir_host=datadir,
            port=port
        )
        self.containers[version] = instance
        
        # 等待MySQL就绪
        self._wait_for_mysql(instance)
        
        return instance
    
    def _wait_for_mysql(self, instance: MySQLInstance, timeout: int = 120) -> bool:
        """等待MySQL就绪"""
        logger.info(f"Waiting for MySQL {instance.version} to be ready...")
        start_time = time.time()
        
        while time.time() - start_time < timeout:
            try:
                result = subprocess.run(
                    [
                        "podman", "exec", instance.container_name,
                        "mysql", "-uroot", f"-ptest123456", "-e", "SELECT 1"
                    ],
                    capture_output=True,
                    text=True,
                    timeout=5
                )
                if result.returncode == 0:
                    logger.info(f"MySQL {instance.version} is ready")
                    return True
            except subprocess.TimeoutExpired:
                pass
            time.sleep(2)
        
        raise TimeoutError(f"MySQL {instance.version} failed to start within {timeout}s")
    
    def execute_sql(
        self, 
        instance: MySQLInstance, 
        sql: str,
        database: str = "testdb"
    ) -> Tuple[bool, str, str]:
        """在容器内执行SQL"""
        cmd = [
            "podman", "exec", instance.container_name,
            "mysql", "-uroot", "-ptest123456", database, "-e", sql
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result.returncode == 0, result.stdout, result.stderr
    
    def execute_sql_file(
        self,
        instance: MySQLInstance,
        sql_file: Path,
        database: str = "testdb"
    ) -> Tuple[bool, str, str]:
        """执行SQL文件"""
        cmd = [
            "podman", "exec", "-i", instance.container_name,
            "mysql", "-uroot", "-ptest123456", database
        ]
        with open(sql_file) as f:
            result = subprocess.run(cmd, stdin=f, capture_output=True, text=True)
        return result.returncode == 0, result.stdout, result.stderr
    
    def stop_instance(self, instance: MySQLInstance):
        """停止容器"""
        self._run_podman(["stop", instance.container_name], check=False)
    
    def start_instance(self, instance: MySQLInstance):
        """启动已停止的容器"""
        self._run_podman(["start", instance.container_name])
        self._wait_for_mysql(instance)
    
    def remove_instance(self, instance: MySQLInstance):
        """删除容器"""
        self._run_podman(["rm", "-f", instance.container_name], check=False)
        if instance.version in self.containers:
            del self.containers[instance.version]
    
    def get_datadir(self, instance: MySQLInstance) -> Path:
        """获取数据目录路径（宿主机）"""
        return instance.datadir_host
    
    def copy_file_to_container(
        self, 
        instance: MySQLInstance, 
        src: Path, 
        dest: str
    ):
        """复制文件到容器"""
        self._run_podman([
            "cp", str(src), f"{instance.container_name}:{dest}"
        ])
    
    def copy_file_from_container(
        self,
        instance: MySQLInstance,
        src: str,
        dest: Path
    ):
        """从容器复制文件"""
        self._run_podman([
            "cp", f"{instance.container_name}:{src}", str(dest)
        ])
```

### 2.4 数据生成器 (data_generator.py)

```python
#!/usr/bin/env python3
"""测试数据生成器"""
import random
import string
import hashlib
import subprocess
from pathlib import Path
from typing import Tuple, Optional
from datetime import datetime, timedelta
import logging

from .models import TableSchema, Baseline, DataScale
from .container_manager import MySQLInstance

logger = logging.getLogger(__name__)

class DataGenerator:
    """测试数据生成器"""
    
    # 默认测试表结构
    DEFAULT_SCHEMA = TableSchema(
        name="test_table",
        columns=[
            {"name": "id", "type": "BIGINT", "nullable": False, "is_pk": True},
            {"name": "name", "type": "VARCHAR(255)", "nullable": False},
            {"name": "email", "type": "VARCHAR(255)", "nullable": True},
            {"name": "age", "type": "INT", "nullable": True},
            {"name": "salary", "type": "DECIMAL(10,2)", "nullable": True},
            {"name": "bio", "type": "TEXT", "nullable": True},
            {"name": "avatar", "type": "BLOB", "nullable": True},
            {"name": "created_at", "type": "DATETIME", "nullable": False},
        ],
        row_format="COMPACT"
    )
    
    # 宽表结构（用于大数据量测试）
    WIDE_SCHEMA = TableSchema(
        name="wide_table",
        columns=[
            {"name": "id", "type": "BIGINT", "nullable": False, "is_pk": True},
            {"name": f"col_{i}", "type": "VARCHAR(100)", "nullable": True}
            for i in range(50)
        ] + [
            {"name": "created_at", "type": "DATETIME", "nullable": False}
        ],
        row_format="DYNAMIC"
    )
    
    def __init__(self, output_dir: Path = Path("/tmp/mysql_test/baselines")):
        self.output_dir = output_dir
        self.output_dir.mkdir(parents=True, exist_ok=True)
    
    def get_row_count_for_scale(self, scale: DataScale, schema: TableSchema) -> int:
        """根据规模计算行数"""
        scale_map = {
            DataScale.S5K: 5_000,
            DataScale.S10K: 10_000,
            DataScale.S1GB: 1_000_000,    # 约1KB/行
            DataScale.S10GB: 10_000_000,
            DataScale.S20GB: 20_000_000,
        }
        return scale_map.get(scale, 10_000)
    
    def generate_value(self, col_type: str) -> str:
        """根据列类型生成随机值"""
        if "INT" in col_type:
            return str(random.randint(1, 10_000_000))
        elif "VARCHAR" in col_type:
            length = int(col_type.split("(")[1].split(")")[0])
            length = min(length, 50)  # 限制生成长度
            return "'" + "".join(random.choices(string.ascii_letters, k=length)) + "'"
        elif "TEXT" in col_type:
            return "'" + "".join(random.choices(string.ascii_letters + " ", k=200)) + "'"
        elif "BLOB" in col_type:
            # 生成随机字节并转义
            data = bytes(random.choices(range(256), k=100))
            return "0x" + data.hex()
        elif "DECIMAL" in col_type:
            return f"{random.uniform(1000, 100000):.2f}"
        elif "DATETIME" in col_type:
            dt = datetime.now() - timedelta(days=random.randint(0, 365))
            return f"'{dt.strftime('%Y-%m-%d %H:%M:%S')}'"
        return "NULL"
    
    def create_table_and_insert(
        self,
        instance: MySQLInstance,
        schema: TableSchema,
        row_count: int,
        batch_size: int = 1000
    ) -> Tuple[bool, str]:
        """创建表并插入数据"""
        from .container_manager import ContainerManager
        
        mgr = ContainerManager()
        
        # 创建表
        create_sql = schema.to_create_sql()
        success, _, err = mgr.execute_sql(instance, create_sql)
        if not success:
            return False, f"Failed to create table: {err}"
        
        # 批量插入数据
        logger.info(f"Inserting {row_count} rows into {schema.name}...")
        
        for offset in range(0, row_count, batch_size):
            values_list = []
            for i in range(offset, min(offset + batch_size, row_count)):
                values = []
                for col in schema.columns:
                    if col["name"] == "id":
                        values.append(str(i + 1))
                    elif col.get("is_pk") or not col.get("nullable", True) or random.random() > 0.1:
                        values.append(self.generate_value(col["type"]))
                    else:
                        values.append("NULL")
                values_list.append(f"({', '.join(values)})")
            
            insert_sql = f"INSERT INTO `{schema.name}` VALUES {', '.join(values_list)}"
            success, _, err = mgr.execute_sql(instance, insert_sql)
            if not success:
                logger.warning(f"Batch insert failed at row {offset}: {err}")
        
        # 验证插入行数
        success, stdout, _ = mgr.execute_sql(
            instance, f"SELECT COUNT(*) FROM `{schema.name}`"
        )
        actual_count = int(stdout.split()[1]) if success else 0
        
        return True, f"Inserted {actual_count} rows"
    
    def compute_baseline(
        self,
        instance: MySQLInstance,
        schema: TableSchema,
        scale: DataScale
    ) -> Optional[Baseline]:
        """计算基线数据"""
        from .container_manager import ContainerManager
        
        mgr = ContainerManager()
        
        # 1. 获取行数
        success, stdout, _ = mgr.execute_sql(
            instance, f"SELECT COUNT(*) FROM `{schema.name}`"
        )
        if not success:
            return None
        row_count = int(stdout.split()[1])
        
        # 2. 获取数据文件大小
        datadir = instance.datadir_host
        ibd_file = datadir / "testdb" / f"{schema.name}.ibd"
        data_size = ibd_file.stat().st_size if ibd_file.exists() else 0
        
        # 3. 导出数据并计算SHA256
        export_path = self.output_dir / f"{schema.name}_{scale.value}_baseline.csv"
        
        # 使用SELECT INTO OUTFILE或mysqldump
        success, stdout, _ = mgr.execute_sql(
            instance,
            f"SELECT * FROM `{schema.name}` ORDER BY id INTO OUTFILE '/tmp/baseline.csv' "
            f"FIELDS TERMINATED BY ',' LINES TERMINATED BY '\\n'"
        )
        
        if success:
            # 从容器复制文件
            mgr.copy_file_from_container(instance, "/tmp/baseline.csv", export_path)
        else:
            # 使用mysqldump作为备选
            export_path = self._export_via_dump(instance, schema, export_path)
        
        # 计算哈希
        sha256_hash = self._compute_file_hash(export_path) if export_path.exists() else ""
        
        # 4. 获取CHECKSUM
        success, stdout, _ = mgr.execute_sql(
            instance, f"CHECKSUM TABLE `{schema.name}`"
        )
        checksum = int(stdout.split()[1]) if success else None
        
        return Baseline(
            table_name=schema.name,
            row_count=row_count,
            data_size_bytes=data_size,
            sha256_hash=sha256_hash,
            checksum_value=checksum,
            schema_sql=schema.to_create_sql(),
            sample_data_path=str(export_path)
        )
    
    def _export_via_dump(
        self, 
        instance: MySQLInstance, 
        schema: TableSchema,
        export_path: Path
    ) -> Path:
        """通过mysqldump导出数据"""
        cmd = [
            "podman", "exec", instance.container_name,
            "mysqldump", "-uroot", "-ptest123456",
            "--no-create-info", "--skip-triggers",
            "--order-by-primary", "--compact",
            "testdb", schema.name
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode == 0:
            with open(export_path, "w") as f:
                f.write(result.stdout)
        return export_path
    
    def _compute_file_hash(self, file_path: Path) -> str:
        """计算文件SHA256哈希"""
        sha256 = hashlib.sha256()
        with open(file_path, "rb") as f:
            for chunk in iter(lambda: f.read(8192), b""):
                sha256.update(chunk)
        return sha256.hexdigest()
```

### 2.5 破坏执行器 (destruction_engine.py)

```python
#!/usr/bin/env python3
"""破坏场景执行器"""
import os
import random
import shutil
import subprocess
from pathlib import Path
from typing import List, Tuple
import logging

from .models import DestructionType, TableSchema
from .container_manager import MySQLInstance, ContainerManager

logger = logging.getLogger(__name__)

class DestructionEngine:
    """破坏执行引擎"""
    
    def __init__(self):
        self.container_mgr = ContainerManager()
    
    def execute(
        self,
        instance: MySQLInstance,
        destruction_type: DestructionType,
        schema: TableSchema,
        additional_tables: List[str] = None
    ) -> Tuple[bool, str]:
        """执行破坏操作"""
        
        handlers = {
            DestructionType.TRUNCATE: self._truncate_table,
            DestructionType.DROP_TABLE: lambda i, s, t: self._drop_table(i, s, purge=False),
            DestructionType.DROP_TABLE_PURGE: lambda i, s, t: self._drop_table(i, s, purge=True),
            DestructionType.BAD_BLOCKS: self._inject_bad_blocks,
            DestructionType.RM_SINGLE_IBD: self._rm_single_ibd,
            DestructionType.RM_IBD_FRM: self._rm_ibd_frm,
            DestructionType.RM_MULTIPLE_TABLES: lambda i, s, t: self._rm_multiple_tables(i, t or [s.name]),
            DestructionType.RM_DATADIR: lambda i, s, t: self._rm_datadir(i),
        }
        
        handler = handlers.get(destruction_type)
        if not handler:
            return False, f"Unknown destruction type: {destruction_type}"
        
        return handler(instance, schema, additional_tables)
    
    def _truncate_table(
        self, 
        instance: MySQLInstance, 
        schema: TableSchema,
        _
    ) -> Tuple[bool, str]:
        """TRUNCATE TABLE"""
        logger.info(f"Truncating table: {schema.name}")
        success, _, err = self.container_mgr.execute_sql(
            instance, f"TRUNCATE TABLE `{schema.name}`"
        )
        return success, f"Truncated {schema.name}" if success else err
    
    def _drop_table(
        self, 
        instance: MySQLInstance, 
        schema: TableSchema,
        purge: bool = False
    ) -> Tuple[bool, str]:
        """DROP TABLE"""
        sql = f"DROP TABLE `{schema.name}`"
        if purge:
            sql += " PURGE"  # MySQL 8.0.22+ 支持
        
        logger.info(f"Dropping table: {schema.name} (purge={purge})")
        success, _, err = self.container_mgr.execute_sql(instance, sql)
        return success, f"Dropped {schema.name}" if success else err
    
    def _inject_bad_blocks(
        self, 
        instance: MySQLInstance, 
        schema: TableSchema,
        _,
        corruption_ratio: float = 0.01
    ) -> Tuple[bool, str]:
        """注入坏块"""
        # 停止MySQL以确保文件不被访问
        self.container_mgr.stop_instance(instance)
        
        ibd_path = instance.datadir_host / "testdb" / f"{schema.name}.ibd"
        
        if not ibd_path.exists():
            self.container_mgr.start_instance(instance)
            return False, f"IBD file not found: {ibd_path}"
        
        file_size = ibd_path.stat().st_size
        page_size = 16384  # 默认页大小
        total_pages = file_size // page_size
        corrupt_pages = max(1, int(total_pages * corruption_ratio))
        
        logger.info(f"Injecting bad blocks into {corrupt_pages} pages")
        
        with open(ibd_path, "r+b") as f:
            for _ in range(corrupt_pages):
                page_num = random.randint(0, total_pages - 1)
                offset = page_num * page_size
                
                # 随机覆盖页的一部分
                corrupt_offset = offset + random.randint(0, page_size - 100)
                corrupt_size = random.randint(50, 100)
                
                f.seek(corrupt_offset)
                f.write(os.urandom(corrupt_size))
        
        # 重启MySQL
        self.container_mgr.start_instance(instance)
        
        return True, f"Corrupted {corrupt_pages} pages"
    
    def _rm_single_ibd(
        self, 
        instance: MySQLInstance, 
        schema: TableSchema,
        _
    ) -> Tuple[bool, str]:
        """删除单个.ibd文件"""
        # 停止MySQL
        self.container_mgr.stop_instance(instance)
        
        ibd_path = instance.datadir_host / "testdb" / f"{schema.name}.ibd"
        
        if ibd_path.exists():
            # 备份到临时位置以便后续验证
            backup_path = ibd_path.with_suffix(".ibd.deleted")
            shutil.move(str(ibd_path), str(backup_path))
            logger.info(f"Moved {ibd_path} to {backup_path}")
            return True, f"Removed {ibd_path}"
        
        self.container_mgr.start_instance(instance)
        return False, f"IBD file not found: {ibd_path}"
    
    def _rm_ibd_frm(
        self, 
        instance: MySQLInstance, 
        schema: TableSchema,
        _
    ) -> Tuple[bool, str]:
        """删除.ibd和.frm文件"""
        self.container_mgr.stop_instance(instance)
        
        datadir = instance.datadir_host / "testdb"
        ibd_path = datadir / f"{schema.name}.ibd"
        frm_path = datadir / f"{schema.name}.frm"
        
        removed = []
        for path in [ibd_path, frm_path]:
            if path.exists():
                backup = path.with_suffix(path.suffix + ".deleted")
                shutil.move(str(path), str(backup))
                removed.append(str(path))
        
        self.container_mgr.start_instance(instance)
        return True, f"Removed: {', '.join(removed)}"
    
    def _rm_multiple_tables(
        self, 
        instance: MySQLInstance,
        table_names: List[str]
    ) -> Tuple[bool, str]:
        """删除多个表的文件"""
        self.container_mgr.stop_instance(instance)
        
        datadir = instance.datadir_host / "testdb"
        removed = []
        
        for table_name in table_names:
            for ext in [".ibd", ".frm"]:
                path = datadir / f"{table_name}{ext}"
                if path.exists():
                    backup = path.with_suffix(ext + ".deleted")
                    shutil.move(str(path), str(backup))
                    removed.append(str(path))
        
        self.container_mgr.start_instance(instance)
        return True, f"Removed {len(removed)} files"
    
    def _rm_datadir(
        self, 
        instance: MySQLInstance
    ) -> Tuple[bool, str]:
        """删除整个数据目录"""
        self.container_mgr.stop_instance(instance)
        
        datadir = instance.datadir_host
        
        # 备份整个目录
        backup_dir = datadir.parent / f"{datadir.name}.deleted"
        if datadir.exists():
            shutil.move(str(datadir), str(backup_dir))
            datadir.mkdir(parents=True, exist_ok=True)
        
        return True, f"Moved datadir to {backup_dir}"
    
    def get_deleted_ibd_path(
        self, 
        instance: MySQLInstance, 
        schema: TableSchema
    ) -> Path:
        """获取被删除的IBD文件备份路径"""
        return instance.datadir_host / "testdb" / f"{schema.name}.ibd.deleted"
```

### 2.6 恢复工具调用 (recovery_runner.py)

```python
#!/usr/bin/env python3
"""恢复工具调用模块"""
import subprocess
import time
import re
import os
from pathlib import Path
from typing import Dict, Optional, Tuple
from dataclasses import dataclass
import logging

from .models import TableSchema, DestructionType, RecoveryMetrics

logger = logging.getLogger(__name__)

@dataclass
class RecoveryConfig:
    """恢复工具配置"""
    binary_path: Path = Path("./build/mysql_recovery_pro")
    default_output_dir: Path = Path("/tmp/recovered")
    timeout_seconds: int = 3600  # 1小时超时

class RecoveryRunner:
    """恢复工具运行器"""
    
    def __init__(self, config: RecoveryConfig = None):
        self.config = config or RecoveryConfig()
        self.config.binary_path = self._resolve_binary_path()
    
    def _resolve_binary_path(self) -> Path:
        """解析恢复工具路径"""
        # 尝试多个可能的位置
        candidates = [
            Path("./build/mysql_recovery_pro"),
            Path("../build/mysql_recovery_pro"),
            Path("/app/mysql_recovery_pro/build/mysql_recovery_pro"),
            Path.cwd() / "build" / "mysql_recovery_pro",
        ]
        
        for path in candidates:
            if path.exists() and os.access(path, os.X_OK):
                return path.resolve()
        
        raise FileNotFoundError(
            f"Recovery tool not found. Searched: {[str(p) for p in candidates]}"
        )
    
    def run_recovery(
        self,
        ibd_path: Path,
        output_dir: Path,
        schema: Optional[TableSchema] = None,
        destruction_type: DestructionType = DestructionType.RM_SINGLE_IBD,
        additional_args: Dict[str, str] = None
    ) -> Tuple[bool, RecoveryMetrics, str]:
        """
        运行恢复工具
        
        Returns:
            (success, metrics, output_message)
        """
        output_dir.mkdir(parents=True, exist_ok=True)
        
        # 构建命令行参数
        args = [
            str(self.config.binary_path),
            f"--ibd={ibd_path}",
            f"--output={output_dir}",
            "--format=sql",
        ]
        
        # 根据破坏类型添加恢复模式
        mode_flags = {
            DestructionType.TRUNCATE: "--truncate-recovery",
            DestructionType.DROP_TABLE: "--drop-recovery",
            DestructionType.DROP_TABLE_PURGE: "--drop-recovery",
            DestructionType.BAD_BLOCKS: "--check-corruption",
            DestructionType.RM_SINGLE_IBD: "--rmrf-recovery",
            DestructionType.RM_IBD_FRM: "--rmrf-recovery",
            DestructionType.RM_MULTIPLE_TABLES: "--rmrf-recovery",
            DestructionType.RM_DATADIR: "--raw-scan",
        }
        
        if destruction_type in mode_flags:
            args.append(mode_flags[destruction_type])
        
        # 添加schema定义
        if schema:
            schema_def = self._build_schema_def(schema)
            args.append(f"--schema={schema_def}")
        
        # 添加额外参数
        if additional_args:
            for key, value in additional_args.items():
                args.append(f"--{key}={value}")
        
        logger.info(f"Running recovery: {' '.join(args)}")
        
        # 执行并计时
        start_time = time.time()
        memory_before = self._get_process_memory()
        
        try:
            result = subprocess.run(
                args,
                capture_output=True,
                text=True,
                timeout=self.config.timeout_seconds
            )
            success = result.returncode == 0
            output = result.stdout + "\n" + result.stderr
        except subprocess.TimeoutExpired:
            success = False
            output = f"Recovery timed out after {self.config.timeout_seconds}s"
        except Exception as e:
            success = False
            output = str(e)
        
        end_time = time.time()
        memory_after = self._get_process_memory()
        
        # 解析输出文件
        output_files = list(output_dir.glob("*.sql")) + list(output_dir.glob("*.csv"))
        recovered_size = sum(f.stat().st_size for f in output_files) if output_files else 0
        
        # 解析性能指标（从输出中提取）
        metrics = self._parse_metrics(output, end_time - start_time, recovered_size)
        
        return success, metrics, output
    
    def _build_schema_def(self, schema: TableSchema) -> str:
        """构建schema定义字符串"""
        parts = []
        for col in schema.columns:
            col_def = f"{col['name']}:{col['type']}"
            if col.get('is_pk'):
                col_def += ":pk"
            elif not col.get('nullable', True):
                col_def += ":notnull"
            parts.append(col_def)
        return ",".join(parts)
    
    def _parse_metrics(
        self, 
        output: str, 
        total_duration: float,
        recovered_size: int
    ) -> RecoveryMetrics:
        """从输出中解析性能指标"""
        metrics = RecoveryMetrics(
            total_duration_sec=total_duration,
            scan_duration_sec=0.0,
            parse_duration_sec=0.0,
            export_duration_sec=0.0,
            peak_memory_gb=0.0,
            disk_throughput_mbps=0.0,
            recovered_size_bytes=recovered_size,
            temp_file_size_bytes=0
        )
        
        # 尝试解析时间统计（如果工具输出包含）
        patterns = {
            "scan": r"Scan duration:\s*([\d.]+)\s*s",
            "parse": r"Parse duration:\s*([\d.]+)\s*s",
            "export": r"Export duration:\s*([\d.]+)\s*s",
            "memory": r"Peak memory:\s*([\d.]+)\s*(GB|MB)",
            "throughput": r"Throughput:\s*([\d.]+)\s*MB/s",
        }
        
        for key, pattern in patterns.items():
            match = re.search(pattern, output, re.IGNORECASE)
            if match:
                value = float(match.group(1))
                if key == "scan":
                    metrics.scan_duration_sec = value
                elif key == "parse":
                    metrics.parse_duration_sec = value
                elif key == "export":
                    metrics.export_duration_sec = value
                elif key == "memory":
                    if match.group(2) == "MB":
                        value /= 1024
                    metrics.peak_memory_gb = value
                elif key == "throughput":
                    metrics.disk_throughput_mbps = value
        
        return metrics
    
    def _get_process_memory(self) -> float:
        """获取当前进程内存使用（简化版）"""
        try:
            with open("/proc/self/status") as f:
                for line in f:
                    if "VmRSS" in line:
                        return float(line.split()[1]) / (1024 * 1024)  # Convert to GB
        except:
            pass
        return 0.0
```

### 2.7 验证引擎 (verification_engine.py)

```python
#!/usr/bin/env python3
"""恢复结果验证引擎"""
import hashlib
import subprocess
from pathlib import Path
from typing import Tuple, Optional
import logging

from .models import Baseline, TableSchema, RecoveryMetrics
from .container_manager import MySQLInstance, ContainerManager

logger = logging.getLogger(__name__)

class VerificationEngine:
    """验证引擎"""
    
    def __init__(self):
        self.container_mgr = ContainerManager()
    
    def verify_recovery(
        self,
        recovered_sql_path: Path,
        baseline: Baseline,
        schema: TableSchema,
        verification_instance: MySQLInstance
    ) -> Tuple[float, float, int, int]:
        """
        验证恢复结果
        
        Returns:
            (recovery_rate, content_match_rate, recovered_rows, matched_rows)
        """
        if not recovered_sql_path.exists():
            logger.error(f"Recovered SQL file not found: {recovered_sql_path}")
            return 0.0, 0.0, 0, 0
        
        # 1. 导入恢复数据到临时表
        temp_table = f"{schema.name}_recovered"
        
        # 创建临时表结构
        create_sql = schema.to_create_sql().replace(schema.name, temp_table)
        self.container_mgr.execute_sql(verification_instance, create_sql)
        
        # 导入数据
        self._import_sql_file(verification_instance, recovered_sql_path, temp_table)
        
        # 2. 获取恢复行数
        success, stdout, _ = self.container_mgr.execute_sql(
            verification_instance, f"SELECT COUNT(*) FROM `{temp_table}`"
        )
        recovered_rows = int(stdout.split()[1]) if success else 0
        
        # 3. 计算恢复率
        recovery_rate = recovered_rows / baseline.row_count if baseline.row_count > 0 else 0.0
        
        # 4. 计算内容匹配率
        # 导出恢复数据并排序
        recovered_export = Path(f"/tmp/{temp_table}_export.csv")
        self._export_table_data(verification_instance, temp_table, recovered_export)
        
        # 比较哈希
        if recovered_export.exists() and Path(baseline.sample_data_path).exists():
            # 对两个文件排序后比较
            content_match_rate = self._compare_sorted_files(
                Path(baseline.sample_data_path), 
                recovered_export,
                baseline.row_count
            )
        else:
            content_match_rate = 0.0
        
        matched_rows = int(recovered_rows * content_match_rate)
        
        # 清理临时表
        self.container_mgr.execute_sql(
            verification_instance, f"DROP TABLE IF EXISTS `{temp_table}`"
        )
        
        return recovery_rate, content_match_rate, recovered_rows, matched_rows
    
    def _import_sql_file(
        self, 
        instance: MySQLInstance, 
        sql_path: Path, 
        table_name: str
    ) -> bool:
        """导入SQL文件"""
        # 预处理SQL（替换表名）
        with open(sql_path) as f:
            sql_content = f.read()
        
        # 替换INSERT语句中的表名
        # 这里简化处理，实际可能需要更复杂的SQL解析
        sql_content = sql_content.replace(f"`{table_name.replace('_recovered', '')}`", f"`{table_name}`")
        
        temp_sql = Path(f"/tmp/import_{table_name}.sql")
        with open(temp_sql, "w") as f:
            f.write(sql_content)
        
        return self.container_mgr.execute_sql_file(instance, temp_sql)[0]
    
    def _export_table_data(
        self, 
        instance: MySQLInstance, 
        table_name: str, 
        output_path: Path
    ) -> bool:
        """导出表数据"""
        success, _, _ = self.container_mgr.execute_sql(
            instance,
            f"SELECT * FROM `{table_name}` ORDER BY 1 "
            f"INTO OUTFILE '/tmp/{table_name}.csv' "
            f"FIELDS TERMINATED BY ',' LINES TERMINATED BY '\\n'"
        )
        
        if success:
            self.container_mgr.copy_file_from_container(
                instance, f"/tmp/{table_name}.csv", output_path
            )
        
        return output_path.exists()
    
    def _compare_sorted_files(
        self, 
        baseline_file: Path, 
        recovered_file: Path,
        total_rows: int
    ) -> float:
        """比较两个排序后的文件，计算匹配率"""
        # 简化版：逐行比较
        # 实际生产中应使用更高效的方法（如哈希对比）
        
        try:
            with open(baseline_file) as f1, open(recovered_file) as f2:
                baseline_lines = sorted(f1.readlines())
                recovered_lines = sorted(f2.readlines())
            
            if not baseline_lines:
                return 0.0
            
            # 使用集合进行差异比较
            baseline_set = set(line.strip() for line in baseline_lines)
            recovered_set = set(line.strip() for line in recovered_lines)
            
            matched = len(baseline_set & recovered_set)
            return matched / len(baseline_set)
            
        except Exception as e:
            logger.error(f"File comparison failed: {e}")
            return 0.0
    
    def compute_content_hash(self, file_path: Path) -> str:
        """计算文件内容哈希"""
        sha256 = hashlib.sha256()
        with open(file_path, "rb") as f:
            for chunk in iter(lambda: f.read(8192), b""):
                sha256.update(chunk)
        return sha256.hexdigest()
```

### 2.8 主控调度 (orchestrator.py)

```python
#!/usr/bin/env python3
"""测试平台主控调度"""
import json
import yaml
import time
from pathlib import Path
from datetime import datetime
from typing import Dict, List, Optional
from dataclasses import asdict
import logging

from .models import (
    TestCase, TestResult, TestMatrix, TableSchema,
    MySQLVersion, DestructionType, DataScale
)
from .container_manager import ContainerManager, MySQLInstance
from .data_generator import DataGenerator
from .destruction_engine import DestructionEngine
from .recovery_runner import RecoveryRunner, RecoveryConfig
from .verification_engine import VerificationEngine

logger = logging.getLogger(__name__)

class TestOrchestrator:
    """测试主控调度器"""
    
    def __init__(
        self,
        config_path: Path = Path("config/test_matrix.yaml"),
        results_dir: Path = Path("results"),
        recovery_tool_path: Path = None
    ):
        self.config = self._load_config(config_path)
        self.results_dir = results_dir
        self.results_dir.mkdir(parents=True, exist_ok=True)
        
        # 初始化各模块
        self.container_mgr = ContainerManager(results_dir / "containers")
        self.data_generator = DataGenerator(results_dir / "baselines")
        self.destruction_engine = DestructionEngine()
        self.verification_engine = VerificationEngine()
        
        recovery_config = RecoveryConfig()
        if recovery_tool_path:
            recovery_config.binary_path = recovery_tool_path
        self.recovery_runner = RecoveryRunner(recovery_config)
        
        # 状态持久化
        self.checkpoint_file = results_dir / "checkpoint.json"
        self.results_file = results_dir / "test_results.json"
        self.completed_cases: Dict[str, TestResult] = self._load_checkpoint()
    
    def _load_config(self, config_path: Path) -> dict:
        """加载YAML配置"""
        with open(config_path) as f:
            return yaml.safe_load(f)
    
    def _load_checkpoint(self) -> Dict[str, TestResult]:
        """加载断点数据"""
        if self.checkpoint_file.exists():
            with open(self.checkpoint_file) as f:
                data = json.load(f)
                return {k: TestResult(**v) for k, v in data.items()}
        return {}
    
    def _save_checkpoint(self):
        """保存断点"""
        with open(self.checkpoint_file, "w") as f:
            json.dump({k: asdict(v) for k, v in self.completed_cases.items()}, f, indent=2)
    
    def run_matrix(
        self,
        versions: List[str] = None,
        destruction_types: List[str] = None,
        scales: List[str] = None,
        resume: bool = True
    ) -> Dict[str, TestResult]:
        """运行测试矩阵"""
        
        # 从配置或参数获取测试范围
        versions = versions or self.config.get("mysql_versions", ["8.0"])
        destruction_types = destruction_types or self.config.get(
            "destruction_types", ["truncate", "rm_single_ibd"]
        )
        scales = scales or self.config.get("data_scales", ["10k"])
        
        # 构建测试矩阵
        matrix = TestMatrix(
            versions=[MySQLVersion(f"V{v.replace('.', '')}") for v in versions],
            destruction_types=[DestructionType(dt) for dt in destruction_types],
            scales=[DataScale(f"S{s}") for s in scales]
        )
        
        schema = DataGenerator.DEFAULT_SCHEMA
        test_cases = matrix.generate_cases(schema)
        
        logger.info(f"Generated {len(test_cases)} test cases")
        
        # 执行测试
        for case in test_cases:
            # 跳过已完成的用例
            if resume and case.id in self.completed_cases:
                logger.info(f"Skipping completed case: {case.id}")
                continue
            
            result = self._run_single_case(case)
            self.completed_cases[case.id] = result
            self._save_checkpoint()
        
        return self.completed_cases
    
    def _run_single_case(self, case: TestCase) -> TestResult:
        """执行单个测试用例"""
        start_time = time.time()
        logger.info(f"Running test case: {case.id}")
        
        try:
            # 1. 启动MySQL容器
            instance = self.container_mgr.create_mysql_instance(
                case.mysql_version.value
            )
            
            # 2. 生成测试数据
            row_count = self.data_generator.get_row_count_for_scale(
                case.data_scale, case.table_schema
            )
            self.data_generator.create_table_and_insert(
                instance, case.table_schema, row_count
            )
            
            # 3. 计算基线
            baseline = self.data_generator.compute_baseline(
                instance, case.table_schema, case.data_scale
            )
            case.baseline = baseline
            
            # 4. 执行破坏
            success, msg = self.destruction_engine.execute(
                instance, case.destruction_type, case.table_schema
            )
            if not success:
                raise RuntimeError(f"Destruction failed: {msg}")
            
            # 5. 获取被删除的IBD路径（针对文件删除场景）
            if case.destruction_type in [
                DestructionType.RM_SINGLE_IBD, 
                DestructionType.RM_IBD_FRM
            ]:
                ibd_path = self.destruction_engine.get_deleted_ibd_path(
                    instance, case.table_schema
                )
            else:
                ibd_path = instance.datadir_host / "testdb" / f"{case.table_schema.name}.ibd"
            
            # 6. 执行恢复
            output_dir = self.results_dir / "recovered" / case.id
            success, metrics, output = self.recovery_runner.run_recovery(
                ibd_path=ibd_path,
                output_dir=output_dir,
                schema=case.table_schema,
                destruction_type=case.destruction_type
            )
            case.metrics = metrics
            
            if not success:
                raise RuntimeError(f"Recovery failed: {output}")
            
            # 7. 验证恢复结果
            recovered_sql = output_dir / "recovered.sql"
            
            # 创建验证用的临时MySQL实例
            verify_instance = self.container_mgr.create_mysql_instance(
                case.mysql_version.value,
                port_override=self.container_mgr.CONTAINER_PORTS.get(
                    case.mysql_version.value, 3400
                ) + 100
            )
            
            recovery_rate, match_rate, recovered_rows, matched_rows = \
                self.verification_engine.verify_recovery(
                    recovered_sql, baseline, case.table_schema, verify_instance
                )
            
            case.recovery_rate = recovery_rate
            case.content_match_rate = match_rate
            case.recovered_row_count = recovered_rows
            
            # 清理
            self.container_mgr.remove_instance(verify_instance)
            self.container_mgr.remove_instance(instance)
            
            passed = recovery_rate >= 0.99 and match_rate >= 0.99
            
            return TestResult(
                test_case_id=case.id,
                passed=passed,
                recovery_rate=recovery_rate,
                content_match_rate=match_rate,
                duration_sec=time.time() - start_time,
                details={
                    "baseline_rows": baseline.row_count,
                    "recovered_rows": recovered_rows,
                    "matched_rows": matched_rows,
                    "metrics": asdict(metrics) if metrics else None
                }
            )
            
        except Exception as e:
            logger.error(f"Test case {case.id} failed: {e}")
            return TestResult(
                test_case_id=case.id,
                passed=False,
                recovery_rate=0.0,
                content_match_rate=0.0,
                duration_sec=time.time() - start_time,
                details={"error": str(e)}
            )
    
    def generate_report(self) -> Path:
        """生成测试报告"""
        from .report_generator import ReportGenerator
        
        report_gen = ReportGenerator(self.results_dir)
        return report_gen.generate_matrix_report(self.completed_cases)
```

---

## 3. 配置文件格式 (test_matrix.yaml)

```yaml
# MySQL数据恢复工具测试矩阵配置

# MySQL版本列表
mysql_versions:
  - "5.6"
  - "5.7"
  - "8.0"
  - "8.4"
  - "9.0"

# 破坏场景列表
destruction_types:
  - truncate              # TRUNCATE TABLE
  - drop_table            # DROP TABLE
  - drop_table_purge      # DROP TABLE PURGE
  - bad_blocks            # 坏块注入
  - rm_single_ibd         # 删除单个.ibd文件
  - rm_ibd_frm            # 删除.ibd和.frm文件
  - rm_multiple_tables    # 删除多个表文件
  - rm_datadir            # 删除整个数据目录

# 数据规模列表
data_scales:
  - "5k"    # 5,000行
  - "10k"   # 10,000行
  - "1gb"   # 约1GB数据
  - "10gb"  # 约10GB数据
  - "20gb"  # 约20GB数据

# 恢复工具配置
recovery_tool:
  binary_path: "./build/mysql_recovery_pro"
  timeout_seconds: 3600
  default_output_dir: "/tmp/recovered"

# 容器配置
container:
  runtime: "podman"
  base_dir: "/tmp/mysql_test"
  network: "bridge"

# 表结构模板
table_schemas:
  default:
    name: "test_table"
    row_format: "COMPACT"
    columns:
      - name: "id"
        type: "BIGINT"
        nullable: false
        is_pk: true
      - name: "name"
        type: "VARCHAR(255)"
        nullable: false
      - name: "email"
        type: "VARCHAR(255)"
        nullable: true
      - name: "age"
        type: "INT"
        nullable: true
      - name: "salary"
        type: "DECIMAL(10,2)"
        nullable: true
      - name: "bio"
        type: "TEXT"
        nullable: true
      - name: "avatar"
        type: "BLOB"
        nullable: true
      - name: "created_at"
        type: "DATETIME"
        nullable: false

  wide_table:
    name: "wide_table"
    row_format: "DYNAMIC"
    # 50列的宽表，用于大数据量测试

# 验证配置
verification:
  recovery_rate_threshold: 0.999  # 99.9%
  content_match_threshold: 0.999
  timeout_seconds: 600

# 报告配置
report:
  output_dir: "results/reports"
  formats:
    - markdown
    - html
    - json
```

---

## 4. MVP原型实现计划

### 4.1 MVP范围

**MySQL版本**: 仅 8.0
**破坏场景**: TRUNCATE + rm单个.ibd
**数据规模**: 10k行

### 4.2 MVP文件结构

```
recovery_test_platform/
├── config/
│   └── test_matrix.yaml
├── src/
│   ├── __init__.py
│   ├── models.py
│   ├── container_manager.py
│   ├── data_generator.py
│   ├── destruction_engine.py
│   ├── recovery_runner.py
│   ├── verification_engine.py
│   ├── orchestrator.py
│   └── report_generator.py
├── main.py
└── requirements.txt
```

### 4.3 MVP实现步骤

```
Week 1: 基础框架
├── Day 1-2: 数据模型 + 容器管理模块
├── Day 3-4: 数据生成器 + 基线采集
└── Day 5: 破坏执行器（TRUNCATE + rm）

Week 2: 恢复验证
├── Day 1-2: 恢复工具调用 + 性能采集
├── Day 3-4: 验证引擎 + 恢复率计算
└── Day 5: 主控调度 + 断点续跑

Week 3: 报告与扩展
├── Day 1-2: 报告生成（Markdown/HTML）
├── Day 3-4: 测试运行 + 调试
└── Day 5: 文档 + 扩展性设计验证
```

### 4.4 MVP运行命令

```bash
# 安装依赖
pip install pyyaml jinja2

# 运行MVP测试
python main.py --versions 8.0 --scenarios truncate,rm_single_ibd --scales 10k

# 查看报告
cat results/reports/test_report.md
```

### 4.5 预期输出

```
results/
├── checkpoint.json           # 断点数据
├── test_results.json         # 测试结果JSON
├── baselines/
│   └── test_table_10k_baseline.csv
├── recovered/
│   ├── TC-0001/
│   │   └── recovered.sql
│   └── TC-0002/
│       └── recovered.sql
└── reports/
    ├── test_report.md
    ├── test_report.html
    └── test_report.json
```

---

## 5. 扩展性设计

### 5.1 恢复工具抽象接口

```python
from abc import ABC, abstractmethod

class RecoveryToolInterface(ABC):
    """恢复工具抽象接口"""
    
    @abstractmethod
    def recover(
        self, 
        ibd_path: Path, 
        output_dir: Path, 
        schema: Optional[TableSchema] = None
    ) -> Tuple[bool, RecoveryMetrics, Path]:
        """执行恢复"""
        pass
    
    @abstractmethod
    def get_name(self) -> str:
        """获取工具名称"""
        pass

class MySQLRecoveryPro(RecoveryToolInterface):
    """mysql_recovery_pro 实现"""
    ...

class Ibd2Sql(RecoveryToolInterface):
    """ibd2sql 工具实现"""
    ...

class TestDiskRecovery(RecoveryToolInterface):
    """TestDisk/PhotoRec 实现"""
    ...

# 工具注册表
RECOVERY_TOOLS = {
    "mysql_recovery_pro": MySQLRecoveryPro,
    "ibd2sql": Ibd2Sql,
    "testdisk": TestDiskRecovery,
}
```

### 5.2 破坏场景插件

```python
class DestructionPlugin(ABC):
    """破坏场景插件接口"""
    
    @abstractmethod
    def execute(self, instance: MySQLInstance, schema: TableSchema) -> Tuple[bool, str]:
        pass
    
    @abstractmethod
    def get_name(self) -> str:
        pass
    
    @abstractmethod
    def cleanup(self, instance: MySQLInstance):
        """清理恢复环境"""
        pass

# 自定义破坏场景示例
class RedoLogDeletion(DestructionPlugin):
    """删除redo log"""
    def get_name(self) -> str:
        return "redo_log_deletion"
    
    def execute(self, instance, schema):
        # 实现删除redo log的逻辑
        ...

# 注册插件
DESTRUCTION_PLUGINS = {
    "redo_log_deletion": RedoLogDeletion,
    "metadata_page_corruption": MetadataPageCorruption,
}
```

### 5.3 表结构模板配置

```yaml
# 可配置的表结构模板
table_templates:
  narrow_table:
    columns:
      - {name: id, type: BIGINT, is_pk: true}
      - {name: data, type: VARCHAR(100)}
  
  wide_table:
    columns:
      - {name: id, type: BIGINT, is_pk: true}
      # 动态生成50列
      - {name: col_1, type: VARCHAR(100)}
      - ...
  
  blob_heavy:
    columns:
      - {name: id, type: BIGINT, is_pk: true}
      - {name: blob_data, type: LONGBLOB}
      - {name: text_data, type: LONGTEXT}
```
