# 🔥 Chaos Testing Pipeline - Quick Reference Card

## 📋 文件清单

| 文件 | 大小 | 用途 |
|------|------|------|
| `docker/docker-compose.multiversions.yml` | 15KB | 多版本 MySQL 编排 |
| `docker/build_mysql_images.sh` | 6KB | Docker 镜像构建 |
| `docker/dockerfiles/Dockerfile.mysql-(5.0-9.0)` | 8 × 1KB | 版本特定 Dockerfile |
| `scripts/ultra_data_injector.py` | ~50KB | 1M+ 并发数据注入 |
| `scripts/golden_hash_base.py` | ~17KB | SHA-256 基准生成 |
| `scripts/advanced_chaos_monkey.py` | ~17KB | 15 种破坏场景 |
| `scripts/self_healing_loop.py` | ~23KB | GDB + AI 补丁生成 |
| `scripts/generate_final_report.py` | ~11KB | 报告生成器 |
| `scripts/run_chaos_test_suite.sh` | ~13KB | 主调度器 |
| `scripts/run_pipeline.py` | ~11KB | Python 管道 |
| `scripts/verify_setup.sh` | ~3KB | 设置验证 |
| `docs/CHAOS_TESTING.md` | ~10KB | 完整文档 |
| `docs/DELIVERY_SUMMARY.md` | ~8KB | 交付摘要 |

---

## 🚀 3 步启动

### 1️⃣ 验证设置
```bash
cd scripts
./verify_setup.sh
```

### 2️⃣ 运行快速测试 (本地 MySQL)
```bash
python3 run_pipeline.py --quick
```

### 3️⃣ 运行完整测试 (Docker)
```bash
cd docker && docker-compose -f docker-compose.multiversions.yml up -d
cd ../scripts && ./run_chaos_test_suite.sh --all
```

---

## 🔧 常用命令

| 命令 | 描述 |
|------|------|
| `python3 ultra_data_injector.py --num-records 100000 --version 8.0` | 注入 10W 记录 |
| `python3 advanced_chaos_monkey.py --input f.ibd --scenario swiss_cheese` | 应用瑞士奶酪破坏 |
| `python3 self_healing_loop.py --input f_corrupted.ibd --max-retries 5` | 自愈循环 |
| `python3 golden_hash_base.py --version 8.0` | 生成哈希基线 |
| `python3 generate_final_report.py` | 生成最终报告 |

---

## 🎯 破坏场景速查

| 场景 | 命令示例 |
|------|----------|
| Torn Page | `--scenario torn_page --corruption-percent 80` |
| Metadata Erasure | `--scenario metadata_annihilation` |
| Purge Race | `--scenario purge_race --num-corruptions 5` |
| Swiss Cheese | `--scenario swiss_cheese --corruption-ratio 0.20` |
| Grid | `--scenario grid_corruption --grid-spacing 10` |
| CRC Sabotage | `--scenario crc_sabotage --num-corruptions 10` |
| Type Confusion | `--scenario type_confusion --num-corruptions 5` |
| LSN Corruption | `--scenario lsn_corruption --num-corruptions 10` |
| Undo Destroy | `--scenario undo_destroy` |
| Blob Attack | `--scenario blob_attack` |
| Truncation | `--scenario page_truncation --num-pages 2` |
| All Scenarios | `--scenario all_scenarios` |

---

## 📊 版本特性矩阵

| 版本 | JSON | Vector | GIS | DATETIME(6) | SDI | File Format |
|------|------|--------|-----|-------------|-----|-------------|
| 5.0 | ✗ | ✗ | ✗ | ✗ | ✗ | Antelope |
| 5.5 | ✗ | ✗ | ✗ | ✗ | ✗ | Barracuda |
| 5.6 | ✗ | ✗ | ✗ | ✓ | ✗ | Barracuda |
| 5.7 | ✓ | ✗ | ✓ | ✓ | ✗ | Barracuda |
| 8.0 | ✓ | ✗ | ✓ | ✓ | ✓ | Barracuda |
| 8.4 | ✓ | ✗ | ✓ | ✓ | ✓ | Barracuda |
| 9.0 | ✓ | ✓ | ✓ | ✓ | ✓ | Barracuda |

---

## 🎓 恢复评分等级

| 分数 | 等级 | 描述 |
|------|------|------|
| 100 | 100% EXACT | 所有记录 SHA-256 匹配 |
| 90-99.9 | 90% HEURISTIC | >90% 提取, 稍微重构 |
| 50-89.9 | 50% PARTIAL | 50-89% 提取 |
| 0-49.9 | CRITICAL | <50% 或崩溃 |
| 0 | TOTAL | 0% 提取 |

---

## 📁 输出目录

```
golden-hashes/              # SHA-256 基线
chaos-test-volumes/        # 破坏的 .ibd 测试文件
recovery-output/           # 恢复结果
  └── mysql-*/             # 每版本输出
      ├── results.json
      └── recovery_*/      # 单场景恢复
diagnosis/                 # 自愈诊断
  └── healing_*.json
```

---

## 💡 技术亮点

1. **15 种破坏场景** - 模拟真实世界灾难
2. **7 版本覆盖** - 5.0 到 9.0
3. **并发注入** - 多线程 1M+ 记录
4. **ASAN/Valgrind** - 内存错误检测
5. **GDB Core 分析** - 自动崩溃诊断
6. **AI 补丁生成** - 统一 Diff 补丁
7. **自愈循环** - 最多 5 次自动修复

---

## 🔍 验证命令

```bash
cd scripts
python3 quick_verify.py  # 快速验证所有模块
```

输出:
```
✓ Import verification complete
✓ Data Generator class available:
  - Version: 8.0
  - Supports JSON: True
  - Supports Vector: True
  - Supports GIS: True
✓ Corruption Types available (15 total)
✓ Golden Hash Generator ready
✓ Chaos Monkey scenarios available
✓ Self-Healing Loop configured
```

---

## 📞 作者

**jliang.fang**  
Global MySQL Kernel QA Expert & Automation Test Architect  
Version: 2.1.0 | 2026-04-04

---

## 📚 后续步骤

1. 查看 `docs/CHAOS_TESTING.md` - 完整技术文档
2. 运行 `./verify_setup.sh` - 验证环境
3. 运行 `python3 run_pipeline.py --quick` - 快速测试
4. 运行 `./run_chaos_test_suite.sh --all` - 完整管道

---

**Once you have the MySQL recovery engine built and the Docker environment ready, you're ready to start the chaos testing pipeline!**
