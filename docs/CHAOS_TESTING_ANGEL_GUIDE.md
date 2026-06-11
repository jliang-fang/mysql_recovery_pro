# 🛡️ MySQL Recovery Pro - 混沌测试天使长手册 (Chaos Testing Guardian Angel)

> **Author**: `jliang.fang` (Global MySQL Kernel QA & Chaos Engineering)
> **Purpose**: 驱动 C++ 引擎的 "破坏->验证->自愈代码" 无限演进闭环
> **Version**: 1.0 (Bullet-proof Level)

---

## 📚 目录 (Table of Contents)

1. [架构概览](#-architecture-overview)
2. [多版本 Docker 失败矩阵](#-multi-version-docker-failure-matrix)
3. [A/B/C/D 功能破坏矩阵](#-abcd-feature-to-chaos-mapping)
4. [极端数据注入脚本](#-chaos-data-injection-scripts)
5. [自动编排与自愈驱动](#-auto-orchestration--self-healing-driver)
6. [裂缝诊断引擎](#-core-dump-analysis--patch-generator)
7. [外部编排器](#-external-orchestration)
8. [测试报告格式](#-test-report-format)
9. [如何启动](#-how-to-start)

---

## 🏗️ Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                     Chaos Testing Infrastructure                      │
└─────────────────────────────────────────────────────────────────────┘
                                │
        ┌───────────────────────┼───────────────────────┐
        ▼                       ▼                       ▼
┌────────────────┐  ┌──────────────────┐  ┌──────────────────────┐
│  Docker-Compose  │  │  Chaos Orchestrator │  │  Self-Healing Driver │
│  Multi-Vers      │  │  (Python)          │  │  (GDB Analysis)     │
│  MySQL 5.0-9.x   │  │  ── Test Cycles    │  │  ── Error Mining    │
└────────────────┘  │  ── Report Gen       │  │  ── Patch Generation │
                    └──────────────────┘  └──────────────────────┘
                                │
                                ▼
                    ┌───────────────────────┐
                    │   Chaos Injection     │
                    │   (Python mmap/dd)    │
                    └───────────────────────┘
                                │
        ┌───────────────────────┼───────────────────────┐
        ▼                       ▼                       ▼
┌────────────────┐  ┌──────────────────┐  ┌──────────────────────┐
│   A: Parser    │  │   B: Recovery    │  │   C: Discovery       │
│   - Page Size  │  │   - Truncate     │  │   - SDI Corruption   │
│   - Format     │  │   - Rm-rf        │  │   - B+Tree Chain     │
│   - Torn Page  │  │   - BLOB Chain   │  │   - Deep Directory   │
└────────────────┘  └──────────────────┘  └──────────────────────┘
        ▼                       ▼                       ▼
┌────────────────┐  ┌──────────────────┐  ┌──────────────────────┐
│   D: Detection │  │   Core Dumps     │  │   Auto-Hotfix        │
│   - Checksum   │  │   (GDB/ASAN)     │  │   (Unified Diff)     │
│   - Batch      │  │   Analysis       │  │   (C++ patch)        │
│   - DeadTuple  │  │   -> PatchGen    │  │   -> Rebuild         │
└────────────────┘  └──────────────────┘  └──────────────────────┘
                                │
                                ▼
                    ┌───────────────────────┐
                    │  QA Synthesis Report  │
                    │  (JSON + Markdown)    │
                    └───────────────────────┘
```

---

## 🐳 Multi-Version Docker Failure Matrix

### 文件位置: `docker/docker-compose.chaos.yml`

| MySQL Version | Container Name | Port | Row Format | File Format | SDI Page |
|---------------|----------------|------|------------|-------------|----------|
| MySQL 5.0 | `chaos_mysql50` | 33060 | Compact/Redundant | Antelope | ❌ |
| MySQL 5.6 | `chaos_mysql56` | 33061 | Dynamic | Barracuda | ❌ |
| MySQL 5.7 | `chaos_mysql57` | 33062 | Dynamic | Barracuda | ❌ |
| MySQL 8.0 | `chaos_mysql80` | 33063 | Dynamic | Barracuda | ✅ (Page 3) |
| MySQL 8.4 | `chaos_mysql84` | 33064 | Dynamic | Thomas | ✅ |
| MySQL 9.0 | `chaos_mysql90` | 33065 | Dynamic | Puhui | ✅ |

### 启动命令

```bash
# 启动所有 MySQL 容器
docker-compose -f docker/docker-compose.chaos.yml up -d

# 查看容器状态
docker-compose -f docker/docker-compose.chaos.yml ps

# 查看日志
docker-compose -f docker/docker-compose.chaos.yml logs -f

# 停止所有容器
docker-compose -f docker/docker-compose.chaos.yml down
```

---

## 🗡️ A/B/C/D Feature-to-Chaos Mapping

### A. 基础解析层 (Basic Parser Layer)

| Scenario | Script | Destructive Action | Defense Pattern |
|----------|--------|-------------------|-----------------|
| A1. Page Size Extreme | `chaos_injector.py --corrupt-page-header` | 覆写 Page Header | `--page-size` validation |
| A2. Format Converter | `chaos_injector.py --tear-page` | 半页断裂 (Torn Page) | 块级校验 |
| A3. Header Corrupt | `chaos_injector.py --corrupt-sdi` | SDI Page 破坏 | 元数据冗余 |
| A4. Torn Page Injection | `chaos_injector.py --tear-page 0 --ratio 0.5` | Page 尾部清零 | 检查 Page Trailer |

### B. 灾难恢复模式 (Disaster Recovery Modes)

| Scenario | Script | Destructive Action | Recovery Mode |
|----------|--------|-------------------|---------------|
| B1. Truncate Table | `chaos_injector.py --truncate-record` | setPageDirection=0xFFFF | `--truncate-recovery` |
| B2. Rm-rf Fragment | `chaos_injector.py --fragmented-rmrf` | dd 碎片化文件 | `--rmrf-recovery` |
| B3. Header Repair | `chaos_injector.py --corrupt-page-header` | Page Type 随机化 | `--header-repair` |
| B4. BLOB Deep Rescue | `chaos_injector.py --corrupt-blob` | BLOB Chain 断裂 | `--blob-recovery` |

### C. 扫描与发现模块 (Scan & Discovery)

| Scenario | Script | Destructive Action | Defense Pattern |
|----------|--------|-------------------|-----------------|
| C1. SDI Nuke | `chaos_injector.py --corrupt-sdi` | Page 3 全零化 | 启发式重构 |
| C2. B+Tree Deletion | `chaos_injector.py --delete-random-nodes` | 中间层节点 Next 破坏 | `--rebuild-chain` |
| C3. Directory Depth Bomb | `chaos_injector.py --depth=100` | 递归 100 层目录 | 堆栈边界检查 |

### D. 异常检测与批处理 (Error Detection & Batch)

| Scenario | Script | Destructive Action | Detection Mode |
|----------|--------|-------------------|----------------|
| D1. Silent Corruption | `chaos_injector.py --corrupt-all-checksums` | Page Trailer 随机化 | `--check-corruption` |
| D2. Batch Concurrent | `bulk_loader.py` | 10,000 个文件并发打开 | 资源泄露检测 |
| D3. Dead Tuple Extraction | `chaos_injector.py --drop-table-kill-9` | 所有记录标记为 deleted | Dead Tuple 恢复 |

---

## 🚨 Chaos Data Injection Scripts

### 核心脚本: `scripts/chaos_injection/chaos_injector.py`

#### 使用示例

```bash
# Corrupt page header (Header Repair 测试)
python3 scripts/chaos_injection/chaos_injector.py \
    --ibd test_data/test.ibd \
    --chaos corrupt-page-header \
    --page-num 0

# Tear page (Torn Page 测试)
python3 scripts/chaos_injection/chaos_injector.py \
    --ibd test_data/test.ibd \
    --chaos tear-page \
    --page-num 0

# Corrupt SDI Page (SDI 核爆测试)
python3 scripts/chaos_injection/chaos_injector.py \
    --ibd test_data/test.ibd \
    --chaos corrupt-sdi

# Corrupt all checksums (静默损坏测试)
python3 scripts/chaos_injection/chaos_injector.py \
    --ibd test_data/test.ibd \
    --chaos all

# Run bulk chaos suite on directory
python3 scripts/chaos_injection/chaos_injector.py \
    --ibd-dir test_data \
    --batch
```

#### API Reference

```python
from scripts.chaos_injection.chaos_injector import ChaosInjector

# Initialize
injector = ChaosInjector("test.ibd", page_size=16384)

# A类破坏
injector.corrupt_page_header(page_num=0, corrupt_type="all")
injector.tear_page(page_num=0, tear_ratio=0.5)
injector.corrupt_page_trailer(page_num=0)

# B类破坏
injector.truncate_record(page_num=0, record_offset=0)
injector.corrupt_blob_chain(blob_page_nums=[1, 2, 3], break_ratio=0.5)
injector.drop_table_kill_9()

# C类破坏
injector.corrupt_sdi_page()
injector.delete_random_nodes(degree=100)

# D类破坏
injector.corrupt_all_checksums()
injector.corrupt_directory_headers(depth=100)
```

---

## ⚙️ Auto-Orchestration & Self-Healing Driver

### 核心脚本: `scripts/chaos_orchestrator.py`

#### 使用示例

```bash
# Run full chaos suite
python3 scripts/chaos_orchestrator.py \
    --engine build/mysql_recovery_pro \
    --test-data test_data \
    --results chaos_test_results \
    --asan

# Enable debug mode
python3 scripts/chaos_orchestrator.py \
    --engine build/mysql_recovery_pro \
    --test-data test_data \
    --results chaos_test_results \
    --asan \
    --debug
```

#### 流程图

```
┌─────────────────────────────────────────────────────────┐
│  1. Load IBD Files (test_data/*.ibd)                   │
└─────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────┐
│  2. Iterate over Categories (A/B/C/D)                  │
│     └─ For each scenario:                              │
│         ├─ Inject Chaos (chaos_injector.py)           │
│         ├─ Run Recovery (mysql_recovery_pro)          │
│         ├─ Capture SegFault → Core Dump               │
│         └─ Analyze (GDB → Backtrace)                  │
└─────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────┐
│  3. Auto-Heal (if Core Dump present)                   │
│     ├─ Pattern Match (buffer_overflow, null_ptr...)   │
│     ├─ Extract Source Context                         │
│     ├─ Generate Unified Diff Patch                    │
│     └─ Apply Patch → Rebuild → Retest (max 5x)        │
└─────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────┐
│  4. Generate Report (chaos_test_report.json)           │
└─────────────────────────────────────────────────────────┘
```

#### TestReport 数据结构

```json
{
  "test_id": "category_a_corrupt_page_header_0",
  "category": "category_a",
  "scenario": "corrupt_page_header",
  "ibd_file": "test_data/test.ibd",
  "page_size": 16384,
  "native_binary": "build/mysql_recovery_pro",
  "recovery_rate": 1.0,
  "errors": [],
  "core_dump_path": "chaos_test_results/core_dumps/core_20260405.bin",
  "analysis_result": "#0 0x... in parse_page_header()",
  "passed": true,
  "execution_time": 2.5
}
```

---

## 🔍 Core Dump Analysis & Patch Generator

### 核心脚本: `scripts/self_healing/bug_to_patch.py`

#### 使用示例

```bash
# Process GDB output
python3 scripts/self_healing/bug_to_patch.py \
    --gdb core_dump_analysis.txt \
    --output patches/bug_fix.diff

# Process ASAN report
python3 scripts/self_healing/bug_to_patch.py \
    --asan asan_report.txt \
    --output patches/asan_fix.diff
```

#### Bug 识别模式

| Bug Type | GDB Pattern | Fix Strategy |
|----------|-------------|--------------|
| Buffer Overflow | `heap-buffer-overflow` | Bounds Check |
| Null Pointer | `segfault at 0x0` | Null Check Before Access |
| Use-After-Free | `heap-use-after-free` | Smart Pointer + Tag |
| Memory Leak | `definitely lost: X bytes` | RAII + Cleanup |
| Double Free | `double free` | Free + Set Null |

#### 统一补丁模板 (Unified Diff)

```diff
--- a/src/recovery_engine.cpp
+++ b/src/recovery_engine.cpp
@@ -123,7 +123,10 @@ int IBDParser::parse_page_header(uint8_t* data) {
     uint16_t page_type = read_uint16(data + 12);
     
     // BUGFIX: Buffer overflow in page type check
-    if (page_type > PAGE_TYPE_MAX) {
+    if (page_type > PAGE_TYPE_MAX || page_type < PAGE_TYPE_MIN) {
+        // AUTO-FIX: Patch_generated_20260405
         return ERROR_INVALID_PAGE_TYPE;
     }
```

---

## 🚀 External Orchestration

### Terraform (Infrastructure as Code)

```bash
# 初始化 Terraform
cd terraform
terraform init

# 预览变更
terraform plan \
    -var="docker_host=unix:///var/run/docker.sock" \
    -var="chaos_volumes_dir=./chaos-test-volumes"

# 部署
terraform apply -auto-approve

# 清理
terraform destroy -auto-approve
```

### Bash 入口 (docker/entrypoint.sh)

```bash
# 直接运行混沌测试
docker run -it --rm \
    -v $(pwd):/app/mysql_recovery_pro \
    mysql_recovery_pro:latest \
    /app/mysql_recovery_pro/docker/entrypoint.sh

# 启用 ASAN 运行
docker run -it --rm \
    -e USE_ASAN=true \
    -e DEBUG=true \
    -v $(pwd):/app/mysql_recovery_pro \
    mysql_recovery_pro:latest \
    /app/mysql_recovery_pro/docker/entrypoint.sh

# 无限循环模式 (用于 CI/CD)
docker run -d \
    --name chaos_orchestrator \
    -e INFINITE_CYCLE=true \
    -e CYCLE_INTERVAL=300 \
    mysql_recovery_pro:latest
```

---

## 📊 Test Report Format

### chaos_test_report.json 结构

```json
{
  "timestamp": "2026-04-05T10:30:00Z",
  "summary": {
    "total_tests": 125,
    "passed": 118,
    "failed": 7,
    "pass_rate": 0.944,
    "average_recovery_rate": 0.967
  },
  "category_stats": {
    "category_a": { "total": 25, "passed": 24 },
    "category_b": { "total": 30, "passed": 28 },
    "category_c": { "total": 35, "passed": 33 },
    "category_d": { "total": 35, "passed": 33 }
  },
  "category_recovery": {
    "category_a": 0.97,
    "category_b": 0.94,
    "category_c": 0.95,
    "category_d": 0.96
  },
  "self_heal": {
    "patches_generated": 2,
    "patches_applied": 2,
    "bugs_fixed": {
      "buffer_overflow": 1,
      "null_pointer": 1
    }
  },
  "_patches": [
    {
      "id": "BUG_001_ABCD1234",
      "category": "truncate_recovery",
      "type": "buffer_overflow",
      "description": "Buffer overflow in IBD page header parsing",
      "file": "src/recovery_engine.cpp",
      "fixed": true
    }
  ],
  "test_details": [ /* 详细测试报告 */ ]
}
```

---

## 🛠️ How to Start

### Step 1: 初始化工程

```bash
cd /app/mysql_recovery_pro

# 确保 CMakeLists.txt 正确
cmake -B build -S .

# 构建引擎 (非 ASAN)
make -C build -j$(nproc)
```

### Step 2: 准备测试数据

```bash
# 生成测试 IBD 文件
python3 scripts/generate_test_data/generate_test_data.py \
    --output test_data/test.ibd \
    --type "all" \
    --page-size 16384
```

### Step 3: 运行混沌测试循环

```bash
# 启动 Docker 容器
docker-compose -f docker/docker-compose.chaos.yml up -d

# 等待 MySQL 准备就绪
sleep 30

# 运行混沌编排器 (ASAN 模式)
python3 scripts/chaos_orchestrator.py \
    --engine build/mysql_recovery_pro \
    --test-data test_data \
    --results chaos_test_results \
    --asan \
    --debug

# 或直接运行入口脚本
docker exec -it chaos_mysql80 \
    /app/mysql_recovery_pro/docker/entrypoint.sh
```

### Step 4: 进入循环模式 (无限演进)

```bash
# 在容器内
docker exec -it chaos_orchestrator \
    /bin/bash -c "INFINITE_CYCLE=true /app/mysql_recovery_pro/docker/entrypoint.sh"
```

---

## 🔥 解决方案亮点 (Solutions Highlights)

### 1️⃣ 4-Minute Complete Loop

```
Corrupt → Inject → Trigger → SegFault → GDB → Extract → Patch → Rebuild → Retest
```

### 2️⃣ Auto-Healing with Pattern Matching

- **Buffer Overflow**: → `if (offset + len > page_size) return ERROR;`
- **Null Pointer**: → `if (!ptr) return NULL;`
- **Double Free**: → `free(ptr); ptr = NULL;`

### 3️⃣ Multi-Version Compatibility

| MySQL 5.0 | MySQL 8.0 | MySQL 9.0 |
|-----------|-----------|-----------|
| Antelope | Barracuda | Thomas/Puhui |
| Compact | Dynamic | Dynamic+ |

### 4️⃣ Unified Diff Output

- Automatically generate patch that can be `git apply`-ed
- Version-controlled via Chaos Git

---

## 📜 License

MIT License - See LICENSE file for details.

---

** Bởi vì sự hoàn hảo không trong khả năng của con người, nhưng trong giới hạn của kỹ thuật**

**-- jliang.fang (Global MySQL Kernel QA & Chaos Engineering)**

---

### 📞 Support

- **Email**: `jliang.fang@mysql-recovery-pro.internal`
- **Documentation**: `docs/CHAOS_TESTING_README.md`
- **Bug Tracker**: GitHub Issues with label `[Chaos]`

---

### 🎯 最终目标 (Final Goal)

> **100% 恢复率**: 通过无限循环的破坏→验证→自愈，直到引擎达到展示所有崩溃点的 100% 容错能力

> **Bullet-proof**: 任何 InnoDB Page Layout 的破坏，引擎都能优雅处理或报告清晰错误

> **Zero-Learning-Cycle**: 自动化自愈代码生成，无需工程师手动分析每个崩溃

---

**🚀 Start the Infinite Chaos Engine Now!**
