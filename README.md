# MySQL Recovery Pro - Ultimate Recovery Engine

## 项目结构

```
mysql_recovery_pro/
├── CMakeLists.txt          # CMake 构建配置
├── README.md               # 本文件
│
├── include/                # 头文件目录
│   └── dbrecovery/
│       ├── engine/         # 引擎模块
│       │   ├── raw_disk_engine.hpp              # 基础磁盘 I/O
│       │   ├── enhanced_raw_disk_engine.hpp     # 增强磁盘 I/O (io_uring)
│       │   ├── advanced_signature_scraper.hpp   # 签名扫描器
│       │   └── signature_scraper.hpp             # 基础签名扫描
│       │
│       ├── recovery/      # 恢复模块
│       │   ├── engine.hpp                       # 恢复引擎
│       │   ├── rmrf_recovery.hpp                 # rm -rf 恢复
│       │   ├── heuristic_reconstructor.hpp      # 启发式重组器
│       │   └── enhanced_recovery.hpp             # 增强恢复
│       │
│       ├── innodb/        # InnoDB 解析模块
│       │   ├── page_defs.hpp                    # 页定义
│       │   ├── page_reader.hpp                   # 页读取器
│       │   ├── btree.hpp                          # B+Tree 解析
│       │   ├── heuristic_parser.hpp              # 启发式解析器
│       │   ├── schema_aware_parser.hpp           # 结构感知解析器
│       │   ├── schema_inferrer.hpp               # 结构推断器
│       │   └── blob_parser.hpp                   # BLOB 解析
│       │
│       ├── catalog/       # 目录模块
│       │   └── schema_defs.hpp                   # 结构定义
│       │
│       └── repair/        # 修复模块
│           ├── corruption_detector.hpp          # 损坏检测
│           └── header_repairer.hpp               # 头修复
│
├── src/                    # 源文件目录
│   ├── engine/             # 引擎实现
│   ├── recovery/          # 恢复实现
│   ├── innodb/            # InnoDB 实现
│   ├── repair/            # 修复实现
│   ├── page/              # 页解析实现
│   ├── export/            # 导出实现
│   ├── utils/             # 工具实现
│   ├── platform/          # 平台特定实现
│   └── main.cpp           # 主程序入口
│
├── tests/                  # 单元测试
│   ├── test_extreme_recovery_validation.cpp    # 极端恢复验证测试
│   ├── test_extreme_scenarios.cpp               # 极端场景测试
│   ├── test_page_scanner.cpp                    # 页扫描测试
│   ├── test_btree_traversal.cpp                  # B+Tree 遍历测试
│   ├── test_heuristic_inference.cpp              # 启发式推断测试
│   └── ...                                       # 其他测试
│
├── tools/                  # 工具脚本
│   ├── extreme_recovery_test_framework.py       # 极端恢复测试框架
│   ├── mysql56_container_setup.py               # MySQL 5.6 容器设置
│   └── quick_validation_test.py                  # 快速验证测试
│
├── scripts/                # 构建和测试脚本
│   ├── build_linux.sh     # Linux 构建
│   ├── build_macos.sh     # macOS 构建
│   └── run_tests.py       # 测试运行器
│
├── docs/                   # 文档
│   ├── en/                 # 英文文档
│   └── zh/                 # 中文文档
│
├── examples/              # 示例代码
│
├── golden-hashes/         # Golden Hash 基线
│
└── build/                  # 构建输出目录
    ├── mysql_recovery_pro                      # 主可执行文件
    └── tests/                                   # 测试可执行文件
```

## 核心模块

### 1. Enhanced Raw Disk I/O Engine
- io_uring 批量异步读取
- 内存高效流式扫描
- Pinned buffer pool（取证安全）
- 支持 TB 级磁盘扫描

### 2. Signature-Based Scraper (Carver)
- InnoDB 页签名库（MySQL 5.6/5.7/8.0）
- PhotoRec 风格双层索引
- CRC32C + INNODB 双校验和验证
- 页聚合为文件

### 3. Heuristic Reconstructor
- LSN 版本选择（TRUNCATE 恢复）
- 页冲突解决
- B+Tree 链重建
- 缺失页推断

### 4. Schema-Aware Parser
- MySQL 5.6 COMPACT 行格式解析
- 模板匹配验证
- 删除记录提取
- 多格式支持

### 5. Automated Validation Pipeline
- Podman MySQL 5.6 容器管理
- 500 万行测试数据生成
- SHA-256 Hash 基线验证
- 性能度量

## 构建说明

```bash
# 配置
cmake -B build -S .

# 编译
cmake --build build -j$(nproc)

# 运行测试
cd build/tests && ctest --output-on-failure
```

## 使用说明

```bash
# 扫描磁盘
./build/mysql_recovery_pro scan -i /dev/sdb -o ./output

# 恢复 TRUNCATE 表
./build/mysql_recovery_pro recover -i table.ibd -s schema.json -o ./recovered

# 极端恢复模式
./build/mysql_recovery_pro extreme-recovery -d /dev/sdb -o ./output -t 4
```

## 测试验证

```bash
# 快速验证
python3 tools/quick_validation_test.py

# 完整测试（需要 Podman）
python3 tools/extreme_recovery_test_framework.py --test all

# 单元测试
cd build/tests && ./test_extreme_scenarios
```

## 性能目标

- 扫描速度: ≥ 100 MB/s
- 内存使用: < 4 GB（100GB 磁盘）
- 恢复率: ≥ 99.9%

## 版本信息

- 版本: 2.2.0
- 支持 MySQL 版本: 5.6, 5.7, 8.0, 8.4, 9.0
- 支持 InnoDB 行格式: REDUNDANT, COMPACT, DYNAMIC, COMPRESSED
- 支持 InnoDB 页大小: 4K, 8K, 16K, 32K, 64K
