# MySQL9.0无备份磁盘恢复研究

**作者：** 方加俩

**单位：** 华北电力大学，广东省，510000

## MySQL9.0 Disk Recovery Without Backup: A Study

**Author:** jialiang.fang

**Affiliation:** North China Electric Power University, Guangdong Province, 510000, China

## 摘要

针对MySQL 9.0在无逻辑备份、无物理备份或原始实例无法启动条件下的数据抢救问题，提出一种只读、分层、可审计的磁盘级恢复逻辑。方法以磁盘镜像保护为入口，依次完成故障分级、InnoDB页面身份校验、SDI或外部schema解析、B+Tree记录提取、有限Undo/Redo证据关联及一致性检查，并按结构化结果、片段和原始证据分级输出。针对Redo损坏、局部数据页损坏和独立数据字典损坏三类故障，分别设计处理分支和失败转移机制。现有MySQL 9.0系列工具侧fixture记录了rm-rf、schema-aware和deep carve路径各48行输出；历史实验封存了一个局部数据页损坏的Level II同输入试验（预期5行，原生与离线均未达到可比较条件，比较状态均为NOT_COMPUTABLE）。2026年9月8日进一步采集并固化了MySQL 9.0.1全字段基线镜像与规范化ground truth（7页、114688字节，3行），并完成了隔离的Level I Redo损坏、Level II局部数据页损坏和Level III独立数据字典缺失矩阵：仅Level I在force recovery=6形成可比较原生导出且2/3行逐字段匹配；Level II和Level III的离线结果均因schema可信度不足降为EVIDENCE_ONLY并阻断CSV导出，比较状态为NOT_COMPUTABLE。在外部注入可信schema约束（--schema-file）的消融对比中，Level II和Level III离线分支转为允许结构化导出，分别输出1行（坏页乱码残片）和2行（主键2、3字段部分损坏），严格比较仍为NOT_COMPUTABLE，定量表明外部schema虽能解除证据阻断，但单靠物理偏移行走无法消除底层破坏导致的乱码假阳性。另以表级SDI payload局部清零做了消融对照：原生3组严格匹配3/3行，离线同因schema证据失效而fail-closed阻断导出。2026年9月12日在/dev/nvme0n2（200 GiB，ext4）真实块设备上建立了严格前置授权保护下的实测矩阵（300行混合类型ground truth）：修复虚拟NVMe Identify零载荷退避缺陷后安全门严格放行；rm-rf、TRUNCATE和DROP三场景的离线disk-carving均实现300/300主键逐字段精确吻合（precision=recall=1.0），但分别残留552、978、1404条精确重复行，故按“无重复”严格口径strict_pass=false。局部页损坏场景的审计暴露了一处方法缺陷：首轮登记的“page 4”变异因其页体本已全零而为空操作（pre/post SHA-256相同），该输入实为pristine副本，其300/300结果不构成有效损坏场景观测；对2026年9月10日更早一次输入的事后审计显示其灾后工件缺失且恢复产物中无任何ground truth标记，0/300与300/300观测均不进入比较；修正后的变异定位到实际承载251行的page 5并局部清零200..4000字节，实测摧毁63行的payload，而雕刻仍以全部300个主键输出（被毁的63行来自过期数据页），覆盖率口径recall=237/300=0.79，同时工具内部仍对312万条尝试记录报告100.00%恢复率——量化表明候选数量与内部恢复率均不能替代经校验的覆盖率。本文不以候选数量、启动就绪或测试通过率替代恢复率，强调物理证据、schema可信度和严格ground truth比较的约束作用。结果表明，该方法适合作为应急抢救和取证辅助方案，不能替代备份、binlog及容灾体系。

**关键词：** MySQL 9.0；磁盘级恢复；InnoDB；证据分级

## Abstract

To address data recovery challenges for MySQL 9.0 when logical and physical backups are unavailable or the original instance cannot start, this paper proposes a read-only, tiered, and auditable disk-level recovery methodology. The approach enforces physical evidence protection as its primary gate, followed by damage classification, InnoDB page identity validation, Serialized Dictionary Information (SDI) or external schema parsing, B+Tree record extraction, bounded Undo/Redo evidence correlation, and consistency verification, outputting tiered results across structured records, fragments, and raw forensic evidence. Dedicated branches and fail-closed mechanisms are designed for Redo corruption, partial data-page corruption, and independent data-dictionary destruction. Beyond existing MySQL 9.0 tool fixtures, a controlled MySQL 9.0.1 full-type baseline (7 pages, 114,688 bytes, 3 rows) completed Level I (Redo), Level II (partial page), and Level III (dictionary loss) matrices: only Level I achieved a comparable native export under force-recovery=6 (2/3 rows matching field-by-field), while Level II and Level III offline outputs were downgraded to EVIDENCE_ONLY and blocked from CSV export due to insufficient schema confidence (yielding NOT_COMPUTABLE comparisons). Ablation experiments with externally injected schema constraints (--schema-file) lifted export barriers for Level II and Level III, yielding 1 corrupted remnant row and 2 partially damaged rows respectively, with comparisons remaining NOT_COMPUTABLE, quantitatively demonstrating that external schemas alone cannot eliminate false-positive garbage induced by physical sector corruption. An SDI payload zeroing ablation further revealed that native recovery leverages server data dictionaries to maintain 3/3 strict row recovery, whereas offline parsing strictly adheres to fail-closed export blocking upon SDI invalidation. On 2026-09-12, a controlled matrix was executed on a real 200 GiB block device (/dev/nvme0n2, ext4, 300 rows) under strict pre-flight authorization: after repairing a virtualized-NVMe Identify zero-payload fallback defect, the safety gate admitted the device, and offline disk carving achieved exact field-by-field recovery of all 300/300 primary keys in the rm-rf, TRUNCATE, and DROP scenarios (precision=recall=1.0); however, 552, 978, and 1404 exact duplicate rows respectively remained, so the "no duplicates" strict criterion yields strict_pass=false. An audit of the partial-page-corruption scenario exposed a methodological defect: the originally registered "page 4" mutation was a no-op because that page's interior is already all-zero (identical pre/post SHA-256), so that input was a pristine copy and its 300/300 result does not constitute a valid damaged-scenario observation; a post-hoc audit of the earlier 2026-09-10 input found its post-disaster artifacts missing and no ground-truth markers anywhere in its recovery output, so both the 0/300 and 300/300 observations are excluded from comparison. The corrected mutation targeted page 5, which actually carries 251 rows, and zeroed bytes 200..4000, destroying the payloads of 63 rows; carving nonetheless emitted all 300 primary keys (the 63 destroyed rows resurfacing from stale data pages), giving a coverage-aware recall of 237/300=0.79 while the tool's internal report still claimed a 100.00% recovery rate over 3.12 million attempted records — quantitatively demonstrating that neither candidate counts nor internal recovery rates substitute for validated coverage. Candidate counts, readiness checks, and exit codes are strictly precluded from proxying recovery rates. The method serves as an emergency salvage and forensic aid rather than a substitute for enterprise backup and replication architectures.

**Key words:** MySQL 9.0; disk-level recovery; InnoDB; evidence grading

## 1 引言

数据库误删除、表空间损坏、文件系统异常和存储设备故障可能同时破坏目录、表空间、日志及数据字典。传统恢复以物理备份、逻辑备份、binlog或副本为基础；当这些材料不存在或不可用时，磁盘中仍可能残留可读取的`.ibd`页、共享表空间页、Undo/Redo片段、已解除链接文件和文件系统空闲块。此时恢复目标不是模拟数据库启动，而是在不修改原始证据的前提下，尽可能保存具有物理身份、字段边界和来源记录的数据证据。

无备份恢复的主要困难有三点：一是文件路径和目录元数据可能消失，不能仅依赖文件名发现数据；二是InnoDB记录同时包含页头、记录头、隐藏事务列、变长字段和外部BLOB指针，局部损坏会改变字段边界；三是启发式发现容易被误写成结构化结果。对灾难取证而言，错误导出可能比少导出更危险，因为未经证实的数据会污染后续数据库。为此，本文以“候选发现可以启发式，结构化导出必须确定性”为基本原则，研究MySQL 9.0无备份条件下的离线恢复逻辑。

MySQL 5.6/5.7通常需要结合FRM提取表结构；MySQL 8.0及以后主要依赖数据字典和InnoDB表空间中的SDI。因而版本识别不仅是兼容性步骤，也是输出授权条件。本文重点讨论三级故障模型、四个核心处理模块、证据分级策略及已封存的三级灾难矩阵结果，并对尚待扩展验证的场景保持保守表述。

## 2 方法与算法设计

### 2.1 问题定义与三级故障模型

设输入证据集合为

\[
E=\{F,D,P,U,R,S\}
\]

其中，`F`为文件系统和文件残留，`D`为数据字典或其残片，`P`为InnoDB页面，`U`为Undo证据，`R`为Redo证据，`S`为外部schema或人工约束。输出集合为`O={Q,V,H}`，分别表示结构化结果、证据片段和审计记录。只有当页面身份、schema、记录边界、字段值和一致性条件均得到验证时，候选记录才可以进入`Q`。

本文将故障划分为三级。Ⅰ级故障为Redo日志损坏、缺失、截断或校验失败，但目标表空间和独立数据字典主要页面仍可读取；此时页面和字典是主要依据，Redo只用于inventory、白名单MLOG审计和有限关联。Ⅱ级故障为部分数据页损坏而独立数据字典完好；健康页进入常规解析，局部可解释页只能在明确边界内提取片段。Ⅲ级故障为MySQL 9.0独立数据字典表空间损坏或缺失，但目标`.ibd`仍存在；系统依次尝试ibd内SDI、关联字典残片、版本匹配的FRM、外部schema和物理布局约束，不能将推断schema等同于完整数据字典。

### 2.2 总体流程与核心模块

恢复流程为“证据保护—故障分级—分支处理—页面校验—元数据提取—数据提取—一致性校验—结果输出”。状态机可表示为：

```text
INIT
  → IMAGE_PROTECTED → DAMAGE_CLASSIFIED → BRANCH_SELECTED
  → PAGES_VALIDATED → METADATA_EXTRACTED → DATA_EXTRACTED
  → CONSISTENCY_CHECKED → RESULT_EMITTED
```

发生不可恢复异常时，状态转移到`PROTECTION_FAILED`、`CLASSIFICATION_UNCERTAIN`、`NO_VALID_PAGE`、`METADATA_EVIDENCE_ONLY`、`UNSUPPORTED_ENCRYPTION`、`UNSUPPORTED_LAYOUT`或`CONSISTENCY_FAILED`，不得通过跳过校验直接输出结构化文件。

**图1 无备份磁盘级灾难恢复总体流程**

```text
原始介质、镜像或文件集合
          ↓
只读保护、范围登记、分段摘要和输入manifest
          ↓
可读性、页身份、字典和日志证据检查
          ↓
Ⅰ级Redo分支 / Ⅱ级数据页分支 / Ⅲ级字典分支
          ↓
FIL Header、page number、space ID、page type、LSN和checksum校验
          ↓
FRM、SDI、字典残片或外部schema解析
          ↓
INDEX叶页记录链、字段边界、BLOB指针及有限Undo证据提取
          ↓
主键、重复性、索引关联、跨表空间关系和ground truth比较
          ↓
L1结构化结果、L2片段、L3字符串证据或L4原始证据
```

图1展示从物理证据到恢复结果的单向约束关系。首先固定输入范围和摘要，防止分析过程覆盖或替换原始证据；随后根据字典、表空间、页和日志的可用性选择故障分支。页面校验并非单一魔数匹配，而是综合页边界、页号与偏移、space ID、页类型、LSN、页链和checksum。通过验证的页才可进入记录层。元数据解析按照SDI、关联字典残片、兼容FRM、外部schema和物理布局推断的顺序进行。最终输出由身份、字段边界、主键、BLOB链和严格比较结果共同决定，任何低等级证据均不得绕过输出策略生成CSV或SQL。

四个核心模块如下。

（1）**磁盘镜像保护模块。** 为设备镜像、普通文件、表空间和日志建立唯一标识，记录大小、声明范围、分段摘要、读取状态及不可读取区间。不可读取区间不能用零值替代，也不能自动成为有效空页。模块只回答“输入证据是什么、哪些区间可读取”，不判断数据是否属于目标表。

（2）**数据页校验模块。** 在声明范围内读取候选页，解析FIL Header和FIL Trailer，检查页号、物理偏移、space ID、页类型、LSN、前后页指针和checksum。页状态划分为`VALID`、`VALID_WITH_DAMAGE`、`IDENTITY_ONLY`和`REJECTED`。压缩页和加密页必须先确认参数或密钥条件；同一space ID与page number的多个候选需保留冲突，不能静默覆盖。

（3）**元数据解析模块。** MySQL 5.6/5.7优先验证FRM；MySQL 8.0、8.4和9.0优先验证SDI及表空间字典。SDI解压必须检查声明长度、压缩流完整结束、尾随输入和JSON结构。模块输出schema来源、适用范围、fingerprint、可信度及未解析项。`COMPLETE_VERIFIED`、`PARTIAL_VERIFIED`和`EXTERNAL_CONSTRAINED`可在满足字段完整性时参与结构化解析；`INFERRED`和`UNRESOLVED`默认只能形成证据。

（4）**数据提取与一致性校验模块。** 对INDEX页区分内部节点与叶页，内部节点只用于子页引用，叶页利用INFIMUM、SUPREMUM、页目录、记录头和`REC_NEXT`遍历用户记录。字段边界由NULL bitmap、变长长度、物理列顺序、隐藏事务列、行格式和schema共同确定。主键必须可验证且无冲突；BLOB指针要检查space ID、页号、偏移、长度、边界和循环；二级索引只能辅助关联主键和索引列，不能代替聚簇记录缺失字段。

### 2.3 分支处理与伪代码

Ⅰ级分支先建立页面和字典集合，再生成Redo inventory，记录日志块范围、LSN区间、MLOG类型、目标space ID、页号和校验状态。仅对目标页身份明确、LSN顺序可解释且前置条件满足的记录建立有限关联。Ⅱ级分支将页面划分为健康、可解释损坏和不可解释三类；记录链断裂时分别保存断裂两侧内容，不能静默连接。Ⅲ级分支则保留多个schema候选及其冲突，只有完整外部schema与页证据共同成立时才可能形成结构化结果。

总体算法伪代码如下：

```text
Algorithm RecoverWithoutBackup(source, scope, schema?, ground_truth?)
1  manifest ← ProtectEvidence(source, scope)
2  if manifest.failed: return EvidenceOrFailure(PROTECTION_FAILED)
3  damage ← ClassifyDamage(manifest)
4  if damage.uncertain: return EvidenceOrFailure(CLASSIFICATION_UNCERTAIN)
5  branch_evidence ← ProcessBranch(damage.branch, manifest)
6  pages ← ValidatePages(branch_evidence.pages, manifest)
7  if pages.has_no_usable_identity: return EvidenceOrFailure(NO_VALID_PAGE)
8  metadata ← ExtractMetadata(branch_evidence, pages, schema?)
9  if metadata.unsupported: return EvidenceOrFailure(metadata.status)
10 candidates ← ExtractData(pages, metadata, branch_evidence)
11 checked ← CheckConsistency(candidates, metadata, ground_truth?)
12 result ← ClassifyOutput(checked, metadata)
13 result ← EnforceEvidenceOutputPolicy(result)
14 return Emit(result, MergeAudit(manifest, damage, pages, metadata, checked))
```

一致性校验至少包括记录物理身份、页号与space ID、记录长度、NULL bitmap、变长字段、主键唯一性、BLOB链、索引关联、schema完整性、重复页及跨表空间冲突。存在canonical ground truth时，还需比较schema fingerprint、主键集合、行数和全部字段值。缺少ground truth时，只能报告候选集合、来源偏移和验证状态。

**图2 证据等级与结构化输出约束**

```text
L1 STRUCTURED  ← schema、页面、记录和字段均通过验证
L2 FRAGMENT    ← 来源明确，但存在缺失字段或局部损坏
L3 STRING_ONLY ← 仅确认字符串或有限编码片段
L4 RAW         ← 仅确认原始字节、偏移或页面候选
```

图2说明证据等级不是概率意义上的恢复率。L1在条件满足时才允许结构化输出；L2需根据字段完整性和输出语义进一步判断；L3和L4只能写入`evidence_fragments`，固定证据置信度分别为0.4和0.3，禁止生成CSV或SQL。若结构化记录与低等级证据无法安全分离，则整体采用fail-closed策略。该设计避免内部的“可导入”标记替代物理身份和逐字段验证。

### 2.4 方法边界

本方法是数据库系统上层的离线解析与证据抢救逻辑，不实现MySQL内核的checkpoint选择、Redo完整重放、mini-transaction语义、事务可见性重建、purge协调、锁系统恢复、数据字典事务提交或Atomic DDL状态机。Redo inventory和有限页面关联不等于MySQL 9.0崩溃恢复；Undo链分析不等于事务回滚或完整MVCC重建。

当目标扇区被覆盖、清零或不可读取，页面身份与记录结构同时丢失，SDI和外部schema均不可用，加密密钥缺失，压缩参数超出支持范围，Redo链无法确定页面版本，Undo已purge或断裂，BLOB链跨越缺失页，或者多个schema版本无法区分时，方法只能输出证据或拒绝结构化恢复。未知字段不得以NULL、默认值、空字符串或猜测类型填充。

## 3 实验与结果

### 3.1 实验层次与数据集

实验按证据强度分为四层：单元、回归和模糊测试用于验证边界及内存安全；合成fixture用于验证页、记录和字段解析；工具侧场景矩阵用于记录指定输入下的输出数量和耗时；真实灾难实验则必须封存灾难前ground truth、灾难后manifest、破坏范围、镜像摘要、运行环境和输出文件。四层证据不能互相替代。

现有MySQL 9.0系列fixture为`testdb.all_types_test`，页大小16384字节，文件大小114688字节，共7页、4行，DYNAMIC行格式，覆盖整数、浮点、DECIMAL、日期时间、字符、TEXT、BLOB、ENUM、SET、BIT和JSON等类型。fixture记录的`server_version`为9.0.1，镜像字段为未锁定的`mysql:9.0`；容器编排使用`mysql:9.0.0`。因此本文将其称为“MySQL 9.0系列fixture”，不把这些版本标识视为同一锁定环境。

2026年9月8日进一步采集并固化了独立的MySQL 9.0.1全字段基线：`testdb.all_types_test`，页大小16384字节，文件大小114688字节，共7页、3行，DYNAMIC行格式，覆盖bigint、varchar(32)、decimal(12,4)、blob、datetime(6)、json、enum、set和bit(8)等类型。该基线镜像digest为`sha256:92dc869678019f65d761155dacac660a904f6245bfe1b7997da0a73b2bfc68c9`，schema fingerprint为`8e3b759d8105b15e716a074a81af46a91faba80e115d1479e6616fef9bccdcd6`，规范化ground truth dataset SHA-256为`bfa7586b36ee8aa46c53d2d9604c794f4e8c6a6cdaf404892a81f78635c4b2ff`。该3行独立数据集与仓库既有4行fixture属于不同采集，二者不得混淆。

配置材料明确给出`innodb_file_per_table=1`、utf8mb4、3 GB buffer pool、2 GB redo log、128 MB log buffer、6 GB内存限制和3.0 CPU限制，同时使用`innodb_flush_log_at_trx_commit=0`、`sync_binlog=0`及ROW格式binlog。仓库没有提供宿主机CPU型号、物理内存、磁盘型号、挂载参数、虚拟化平台和块设备状态，故这些信息不作为实验结论。

### 3.2 工具侧验证结果

本轮验证于2026年9月5日在现有构建环境中完成，采用只读输入和受控测试输出目录，不运行Docker或Podman，不访问、挂载或修改块设备，也不对真实fixture实施删除、截断、覆盖或位翻转。测试结果按证据类型分组记录如下。

首先，页面身份、校验、扫描、损坏输入处理和文件系统辅助模块共303项测试全部通过，耗时约8.92 s；覆盖FIL/page header、checksum、页扫描、损坏页状态、极端输入、文件签名、XFS extent、ext4 block bitmap及io_uring scanner等边界。其次，元数据与结构解析模块共434项测试全部通过，耗时约11.85 s；覆盖SDI/FRM及版本证据、schema推断、B+Tree和记录链、BLOB指针与链、分区表、共享表空间、二级索引、DDL、压缩表和相关解析模糊测试。再次，Redo、Undo、Flashback、TDE/keyring、安全门、Salvage、导出隔离和幂等性模块共521项测试全部通过，耗时约241.72 s；这些结果支持解析边界、拒绝策略、只读安全约束及重复执行稳定性，不表示对应灾难场景已被恢复。

测试过程中曾出现两个`RawDiskCarverTest`安全门测试失败，分别为`SafetyGateRequiresAllMandatoryFacts`和`SafetyGateDiagnosticsDoNotAuthorize`。定向重建`test_raw_disk_carver`后两项通过，进一步核查确认失败原因是早于当前源码的陈旧构建产物；`SafetyGate::permits_read_only_raw_access`仍要求设备身份已验证、只读已确认、独占访问已确认且设备未挂载或状态允许四项强制事实同时成立，未修改安全门逻辑、测试断言或授权条件。此前导出隔离测试缺少`DBREC_TEST_TMPDIR`的问题则通过CTest注册注入构建目录下的受控临时目录解决；相关测试仍在环境变量缺失时拒绝运行，未削弱写盘隔离和fail-closed约束。

已有Phase 6工具侧矩阵报告59/59个场景通过、979行输出；其中MySQL 9.0系列fixture的rm-rf recovery、schema-aware recovery和deep carve分别记录48行，平均耗时分别为332 ms、298 ms和303 ms，10个场景总计约1394 ms。上述数字属于指定fixture和脚本口径，只能说明相应工具路径产生了这些输出，不能证明三类真实灾难恢复率、字段逐值正确性或优于原生恢复。另一份历史最终验证报告记录1270/1270通过、0失败及1项跳过，也不能与本轮1536项测试结果合并解释为恢复率。

本轮默认全量CTest共1536项通过、0项失败；其中包含3个标记为`REAL_MYSQL`的真实fixture测试，因为命令中的`-E REAL_MYSQL`按测试名称过滤而不是按CTest label排除，正确排除/选择命令应为`-LE REAL_MYSQL`/`-L REAL_MYSQL`。真实fixture测试的实际门控是fixture路径是否存在：路径缺失时在测试内部调用`GTEST_SKIP()`，因此缺失fixture记为跳过而不是通过或失败；仓库中`DBREC_REAL_FIXTURES`仅出现在注释中，没有任何测试读取该变量，早期文档将其描述为开关并不准确。在本机fixture齐备条件下，以`ctest -L REAL_MYSQL`运行`test_compressed_table_fixture`、`test_real_fixture`和`test_real_recovery_pipeline`三个可执行文件，共29个gtest用例全部通过（12+8+9），覆盖压缩表布局、页面结构、SDI/manifest完整性以及分区、共享表空间、二级索引、DDL和Redo文件的基本流水线行为；现有测试不包含灾难破坏后的同输入逐行逐字段恢复率对照。报告与对照工具的Python测试另行记录：`tests/test_phase6_report.py`共10项通过，覆盖静态库检查、缺矩阵和缺测试结果时整体状态为`FAIL`、`NOT_COMPUTABLE`/`EVIDENCE_ONLY`/`strict_pass=false`/空结构化导出不得渲染为PASS；`test/test_disaster_harness.py`共35项通过，覆盖typed ground truth、SET规范化、schema fingerprint、typed encoding版本门、输入身份和seal等strict comparator行为。这些Python用例验证拒绝与渲染策略，不产生恢复率。

本节所有测试通过数、输出行数和耗时均是软件验证或指定fixture运行指标，不是恢复记录数、precision、recall或有效恢复率。报告中出现的0行结果表示候选被过滤、输入不可解释或程序安全拒绝，不能单独解释为数据已恢复或数据不存在。

### 3.3 三级真实灾难对照设计及完成状态

方案A为MySQL 9原生`innodb_force_recovery`基线，方案B为本文的离线证据解析路径。理想对照应使用同一灾难后输入、同一灾难前ground truth、同一schema约束和相同资源限制。原生基线在灾难后副本上按`innodb_force_recovery=0`至`6`逐级测试，仅记录实例启动、表可见性、查询可读性、逻辑导出范围、错误信息和耗时；任何级别均不得直接作用于原始证据。历史Level II实验显示，所有级别均能启动并通过就绪检查，但访问受损表时查询返回137；日志记录space=2、page number=4的数据库页损坏，离线结果因schema可信度不足被阻断。该历史结果只能分类为“原生查询触发的损坏处理失败/非可比较”，不能视为启动成功或恢复成功。

三级场景的统一实验设计如下：Ⅰ级在保留表空间和字典证据的条件下，构造Redo缺失、截断或校验失败输入，比较原生启动边界、离线页面证据和有限Redo inventory；Ⅱ级在字典基本完好的条件下，对预先登记的一个或多个数据页实施可追溯的局部损坏，比较健康页、受损页和拒绝结果；Ⅲ级在保留目标`.ibd`的条件下，使MySQL 9.0数据字典相关证据不可用或不完整，比较原生路径的字典依赖与离线路径的SDI、外部schema及物理布局约束。每个场景均须封存灾难前ground truth、灾难后manifest、损坏偏移、镜像摘要、版本digest、配置、资源限制和输出文件。上述实验不得以构造页、单元测试或工具退出码替代真实损坏输入。2026年9月8日基于同一3行、7页MySQL 9.0.1基线完成了三组隔离矩阵；各级别使用独立副本，输入和运行结果均由manifest及seal封存。

| 故障等级 | 方案A：原生`innodb_force_recovery` | 方案B：离线恢复 | 当前状态 |
|---|---|---|---|
| Ⅰ级：Redo损坏，字典和表空间主要完好 | Level 0..5未通过readiness；Level 6可查询并导出3行，其中2行逐字段匹配，precision=2/3、recall=2/3，主键1的`flags`字段不匹配，`strict_pass=false` | 本轮未提供离线输入，比较为`NOT_COMPUTABLE` | 新一轮封存工件：`/tmp/dbrec_mysql9_disaster_20260908/workspace/level1_redo/experiment`；manifest seal=`cf15833a2daeb87f54335201e5cd354a632d6d1fb1850c3eb6c2e821e75c4c71`；Redo变异为`#innodb_redo/#ib_redo9`偏移2048至2559写入`0xff`；损坏输入SHA-256=`0a02305f7a03fcdccdfdaa1d2bc4213c6031e64bf97197ffd7a235edf8017976`；Level 0..5不可比较，Level 6为可比较但非严格通过结果） |
| Ⅱ级：局部数据页损坏，字典基本完好 | 7个独立副本均通过就绪检查；Level 0、1访问受损表时查询超时，Level 2..6导出进程返回137，日志指向space=2/page=4；不形成可信导出 | 离线命令正常退出，识别2条结构化候选但trust=`EVIDENCE_ONLY`、classification=`forensic_evidence`，因`schema_trust_insufficient`阻断导出，CSV为空；全部比较为`NOT_COMPUTABLE` | 新一轮封存工件：`/tmp/dbrec_mysql9_disaster_20260908/workspace/level2_page/experiment`；manifest seal=`cdf54efbe2dd7d34e4e6ca1d7f40a4759897a0329bf77dea6c82dbf15fa1cf0b`；page 4偏移`4×16384+200`至`4×16384+399`局部清零；损坏输入SHA-256=`f9f1271f16a627272ce953db903cb2fade0e1edcb0935941d3ae5f79cbd52f4a`；7组比较均为`NOT_COMPUTABLE` |
| Ⅲ级：数据字典相关证据损坏，`.ibd`尚存 | 删除`mysql.ibd`后Level 0..6容器均无法保持运行，全部未通过readiness；原生比较均为`NOT_COMPUTABLE` | 对pristine目标IBD执行离线解析，识别3条结构化候选但trust=`EVIDENCE_ONLY`、classification=`forensic_evidence`，因`schema_trust_insufficient`阻断导出，CSV为空；全部比较为`NOT_COMPUTABLE` | 新一轮封存工件：`/tmp/dbrec_mysql9_disaster_20260908/workspace/level3_dd/experiment`；manifest seal=`49091612e3595b0fb36ac14d47cf5080071ddd3a89481da4278fc09e735cde51`；变异为删除`mysql.ibd`，删除前SHA-256=`ba6bf4dfd881428d6eba61e3fcef07e8b688eb98930fc2aad2234414e862891d`；离线输入为pristine目标IBD，故不构成与删除数据字典后的完整datadir相同输入对照 |
| Ⅲ-b级：表级SDI payload局部清零，server dictionary与`.ibd`主要完好 | Level 0、3、6均通过readiness并可查询导出，3/3行与ground truth逐字段匹配（precision=recall=1.0，`strict_pass=true`） | 离线识别3条候选但trust=`EVIDENCE_ONLY`、classification=`forensic_evidence`，因`no_structured_rows`阻断CSV导出，CSV为0行；全部比较为`NOT_COMPUTABLE` | 新一轮封存工件：`/tmp/dbrec_sdi_exp/workspace4/experiment`；manifest seal=`ff9f3ac7024ec33ca7d1a25a516587362eb022b4b1e222fe8d99d51f2aed0993`；变异为`testdb/all_types_test.ibd`页3（SDI页）偏移100至299共200字节清零；损坏输入SHA-256=`bbb9e9366e59bb313a8b1ee5fc5bb67ac94d29c0e6611f1e68e03bffca2b0860`；原生3组与ground truth比较均为`COMPARABLE`且`strict_pass=true` |

截至本文定稿，历史Level II实验和2026年9月8日新一轮三场景矩阵均已封存。新一轮Level I工件位于`/tmp/dbrec_mysql9_disaster_20260908/workspace/level1_redo/experiment`，Level II工件位于`/tmp/dbrec_mysql9_disaster_20260908/workspace/level2_page/experiment`，Level III工件位于`/tmp/dbrec_mysql9_disaster_20260908/workspace/level3_dd/experiment`；各自的manifest seal已写入表格。Level I在最高级别force recovery=6形成了可比较native export，但仅2/3行逐字段匹配，不能视为严格恢复成功；Level II和Level III均保留了证据级候选但拒绝结构化导出，比较状态均为`NOT_COMPUTABLE`。Level III的offline input是pristine目标IBD，而非删除server dictionary后的完整datadir，因此其结果仅证明缺失可信server dictionary时的目标IBD证据解析能力，不是同一灾难后输入的native/offline恢复率对照。

在三场景之外，2026年9月11日补充了一组同输入、同ground truth的表级SDI子实验（Ⅲ-b级），作为Ⅲ级故障的消融对照：在server dictionary（`mysql.ibd`）与目标`.ibd`其余部分完好的条件下，仅将目标表`testdb/all_types_test.ibd`页3（SDI页）偏移100至299共200字节清零，随后在同一3行、7页基线上对Level 0、3、6分别执行原生路径与离线路径。结果是：原生3组均通过readiness并导出3行，与ground truth的typed比较达到precision=recall=1.0、`strict_pass=true`；离线路径命令正常退出（returncode=0），但在SDI payload失效、无法确立schema provenance时只保留3条`EVIDENCE_ONLY`/`forensic_evidence`片段，以`no_structured_rows`阻断结构化CSV导出（`recovered.csv`为0行）。因此`ground_truth_vs_offline`与`native_vs_offline`的状态均为`NOT_COMPUTABLE`，该组比较不产生恢复率。此结果说明：运行中的MySQL 9.0.1可从server数据字典继续提供表元数据，表内嵌SDI冗余缺失并不妨碍其查询；而离线路径唯一可用的schema证据正是该内嵌SDI，一旦其payload被破坏即按fail-closed策略拒绝结构化导出。该子实验只隔离了SDI依赖，不代表server dictionary同时丢失时的完整datadir灾难（Ⅲ级）已可恢复。

### 3.4 评价指标与讨论

设ground truth记录集合为`G`，结构化恢复集合为`R`，其中逐行逐字段正确匹配集合为`C`，则

\[
precision=|C|/|R|,\qquad recall=|C|/|G|
\]

并记录误报、漏报、目标表数、目标记录数、候选记录数、去重记录数、正确结构化记录数、扫描页数、有效页数、损坏页数、耗时、schema fingerprint、BLOB完整性和证据等级。严格通过要求schema及fingerprint一致、主键集合一致、全部字段值一致、无重复、无误报且无漏报。没有canonical ground truth或比较前置条件不满足时，不报告精确precision、recall、零误报、零漏报或“有效恢复率”。`NOT_COMPUTABLE`表示比较条件不足，不表示ground-truth记录已被确认丢失。

本文方案的主要短板包括：schema缺失时结构化能力受限，FRM不能作为8.0以后SDI的等价替代；Redo仍是受约束的inventory和页面证据关联，不是完整重放；clean shutdown会削弱Undo和闪回实验；TDE、加密双写页、压缩页和BLOB链尚需真实损坏条件下的验证；共享表空间、分区表、二级索引、延迟分配及历史残留可能造成归属歧义；版本标签未固定digest也会降低实验可复现性。

**图3 三级矩阵结果汇总**

| 级别 | 原生结果 | 离线结果 | 比较状态 |
|---|---|---|---|
| Ⅰ级 | 仅force recovery=6可比较，2/3行逐字段匹配（`flags`不匹配，`strict_pass=false`） | 无输入 | Level 6可比较非严格通过，其余`NOT_COMPUTABLE` |
| Ⅱ级 | Level 0、1查询超时，Level 2..6返回137，无可信导出 | 2条`EVIDENCE_ONLY`候选，导出被阻断 | 全部`NOT_COMPUTABLE` |
| Ⅲ级 | 删除`mysql.ibd`后Level 0..6均未通过readiness | pristine目标IBD识别3条`EVIDENCE_ONLY`候选，导出被阻断 | 全部`NOT_COMPUTABLE`；离线输入非同灾副本，不构成恢复率对照 |
| Ⅲ-b级 | server dictionary完好，仅表级SDI页清零；Level 0、3、6均可查询，3/3行`strict_pass=true` | 3条`EVIDENCE_ONLY`候选，因`no_structured_rows`阻断导出 | 原生3组`COMPARABLE`严格通过；离线全部`NOT_COMPUTABLE`（无语义化恢复率） |

因此三类场景均未证明严格、完整的无备份恢复率。表级SDI payload局部清零（Ⅲ-b级）已经测得但只作消融对照；仍待实测或验证的项目包括：不同Redo损坏位置和类型，更多局部数据页损坏分布，server dictionary删除与SDI的更多局部损坏形态，crash-preserved Undo与Flashback，真实BLOB链损坏，分区表和共享表空间的行级恢复，二级索引救援正确性，完整DDL变更链语义，TDE/keyring兼容性，以及封存ground truth的ext4/XFS设备级灾难实验。尤其在ext4和XFS场景中，flush时序、删除方式和延迟分配会直接影响磁盘残留，恢复行数超过目标行数时应优先排查重复页和历史记录混入。

截至2026年9月10日，本文能够复核的已完成实验分为两类。第一类是上述封存的MySQL 9.0.1灾难矩阵，其结果均以manifest seal和损坏输入SHA-256可追溯（见3.3节）。第二类是软件和fixture验证：默认全量CTest记录1536/1536通过（含3项`REAL_MYSQL`标签测试，因`-E`按名称过滤而被误包含；label级排除/选择应为`-LE`/`-L`），独立REAL_MYSQL标签运行共29个gtest用例全部通过（12+8+9），报告与对照工具的Python测试另为10/10和35/35通过；这些结果验证解析边界、拒绝策略、隔离和流水线稳定性，不改变三组灾难矩阵的行级结论。除上述封存材料外，本次补充审计尚未产生新的真实介质恢复证据，因此不将代码审计、测试通过数或候选输出追加为恢复率。随后（2026年9月12日）在真实块设备`/dev/nvme0n2`上完成的四场景灾难矩阵及其覆盖率口径分析见3.4节末尾。

**真实块设备矩阵（2026-09-12）。** 2026年9月10日获授权的`/dev/nvme0n2`首次尝试因MySQL容器认证失败（`ERROR 1045`）而未建立灾难前基线，安全门随后拒绝读取，五个场景退出码均为1且无输出，只能记为实验初始化失败。修正初始化与就绪判定后，2026年9月12日在同一200 GiB设备（ext4、4096字节块、300行混合类型ground truth）上完成了四场景隔离矩阵。每场景均重新`mkfs`并挂载，容器启动采用“`mysqld is alive`且`SELECT 1`成功”的双重就绪判定，装载后以`innodb_flush_log_at_trx_commit=1`、`sync_binlog=1`、`FLUSH TABLES`和`sync`落盘，停止容器后卸载并`blockdev --setro`，仅由工具安全门通过`BLKROGET`/`O_EXCL`复核只读与独占后执行离线雕刻。四场景共同使用SHA-256=`70943d528c89c5b8432b9e9dfb217cda79fdb64396994abf0d414cbf0d54ff72`的ground truth，恢复退出码均为0。

| 场景 | 变异方式 | 主键精确匹配 | 精确重复行 | 工具内部报告的恢复率 | 覆盖率口径recall | 恢复CSV SHA-256 |
|---|---|---|---|---|---|---|
| rm-rf | 删除`testdb/recovery_target.ibd` | 300/300 | 552 | 100.00% | 1.0000 | `a69817237bb508b98635260784ca2bb1a24aa9aca9506737e4e9478f40364e49` |
| TRUNCATE | 在线`TRUNCATE TABLE`后flush | 300/300 | 978 | 100.00% | 1.0000 | `87e1733705d92c1ffe60dce4cdac0765762edd8c1ec75a728111715b2ceaa14b` |
| DROP | 在线`DROP TABLE` | 300/300 | 1404 | 100.00% | 1.0000 | `7d83784571d551e67d962c6064d7368d29aaf7bcd46efa4c180d4223ee868892` |
| 局部页损坏 | page 5偏移200..4000清零（摧毁63行payload） | 300/300（含63行过期副本） | 1404 | 100.00% | **237/300=0.79** | `ec5bf04abdf69be7a1d3b14157a4e178bb4e9970db849f499595236189eb0ac7` |

**变异有效性审计。** 局部页损坏场景首轮登记的变异目标为`testdb/recovery_target.ibd`中的page 4，但该页在该基线下是已分配但页体全零的空叶页，随之执行的“清零200..16320字节”为空操作，`pre_sha256`与`post_sha256`完全相同（`476b75765f759e3b5fc2389238509dc3a4b2e43d618f6540701559d8bcaf8d86`），即灾难输入实为pristine副本。该缺陷同时解释了两次自相矛盾的观测：空操作输入自然得到300/300精确匹配；而2026年9月10日的一次同源输入曾得到0/300并伴随大量乱码行，事后审计其封存目录发现：该目录缺少`ibd_after`、`page_mutation.txt`、`files_after.txt`等灾后工件，且其恢复产物中检索不到任何ground truth payload标记，说明该次运行的灾难基线本身未建立，其输出不构成有效恢复观测，也不应进入任何比较。修正后的脚本不再使用固定页号，而是扫描pristine`.ibd`定位实际承载ground truth行数最多的叶页（page 5，类型`0x45bf`，251行），清零其200..4000字节，并断言`pre_sha256 != post_sha256`（`0dbc6e1f63dd730cf9564340d878e3ddfdd42f22458d695edabba4443c9de1e6`→`d9b21adb9b17c8a55a0a2bab8297e1449c7e5a68323606cfc23048c21fa46265`）。该修正变异摧毁了主键2..64共63行的payload。

**覆盖率口径的必要性。** 修正后的局部页损坏场景中，离线雕刻仍输出全部300个主键且每个主键都存在逐字段精确行；但对照修复后的`.ibd`可知其中63个主键的payload已不存在于活跃数据页，其“精确匹配行”来自文件系统中尚未复用的过期页残留。以活跃文件内容为覆盖分母的覆盖率口径给出237/300=0.79，而以ground truth为分母的naive recall仍为1.0。与此同时，工具内部依据“尝试记录数=成功记录数”对约312万条记录报告100.00%恢复率，该数值与逐字段校验后的覆盖率相差21个百分点。这说明：在真实介质上，仅凭主键存在性、候选计数或工具自报恢复率都会系统性高估恢复能力；必须同时报告活跃证据覆盖与重复计数。三场景中552/978/1404条精确重复行同样表明，同一行会在多个未被复用的页副本中重复出现，去重是严格口径的必要条件。

## 4 结论

本文提出了面向MySQL 9.0无备份场景的磁盘级灾难恢复逻辑。该逻辑以只读证据保护为前提，以三级故障模型选择处理分支，以页面身份、schema来源、记录边界和一致性校验控制结构化输出，并将低可信内容降级为可审计证据。本轮验证在页面、元数据、结构解析、日志证据、安全门、导出隔离和幂等性等方面完成了分组测试及全量回归，既有真实MySQL系列fixture的结构和流水线测试也通过；这些结果证明实现具备相应的边界约束、稳定性和拒绝不可靠输出的能力，但不构成真实介质的恢复率证据。

截至本文定稿，2026年9月8日完成了Level I、Level II和Level III三组封存矩阵，逐级结果及封存工件已在3.3节完整记录，此处不再重述数字。2026年9月12日在200 GiB真实ext4块设备上完成的rm-rf/TRUNCATE/DROP/局部页损坏四场景矩阵（3.4节）进一步表明：即使离线雕刻在三个删除类场景中达成了300/300主键精确匹配，也伴随552至1404条精确重复行；在真实摧毁63行的页损坏场景中，naive recall为1.0而活跃证据覆盖仅为237/300=0.79，且工具自报恢复率（100.00%）与覆盖率相差21个百分点。总体而言：仅Level I在最高恢复级别形成了可比较但非严格通过的原生结果，离线分支在该场景没有输入；Level II和Level III中，离线路径能够在不满足可信结构化恢复条件时保留证据并拒绝导出，但Level III的离线输入不是删除server dictionary后的同一完整灾难副本，不能解释为同输入native/offline恢复率。三组矩阵均未证明严格、完整的行级恢复能力。补充的表级SDI清零消融实验（Ⅲ-b级）进一步显示：native可借server数据字典继续严格读出3/3行，而离线路径在唯一可用的内嵌SDI证据失效时按fail-closed策略拒绝结构化导出，二者的差异量化了offline对SDI的依赖，而非离线恢复率。本文不宣称离线方法优于MySQL原生恢复，也不将查询失败、证据候选、空CSV、测试通过率或候选数量解释为数据总量损失或恢复率；该方法仍应定位为应急抢救和取证辅助工具，不能替代备份、binlog、远程副本、容灾切换和恢复演练。

## 作者简介

方加俩（1995年8月—），男，汉族，广东省揭阳市人，本科学历，运维工程师，主要从事数据库研究。

## 参考文献

[1] Oracle Corporation. MySQL 9.0 Reference Manual[M]. Oracle Corporation, 2024.

[2] Oracle Corporation. MySQL 9.0 Reference Manual: The InnoDB Storage Engine[M]. Oracle Corporation, 2024.

[3] Oracle Corporation. MySQL 8.0 Reference Manual: Serialized Dictionary Information[M]. Oracle Corporation, 2024.

[4] Oracle Corporation. MySQL 5.7 Reference Manual: The .frm File[M]. Oracle Corporation, 2020.

[5] GRAY J, REUTER A. Transaction Processing: Concepts and Techniques[M]. San Francisco: Morgan Kaufmann, 1993.

[6] CARD R, TS'O T, TWEEDIE S. Design and implementation of the second extended filesystem[C]//Proceedings of the First Dutch International Symposium on Linux. Amsterdam: Linux International, 1994: 1-6.

[7] SWEENIE D. Scalability in the XFS file system[C]//Proceedings of the USENIX Annual Technical Conference. Berkeley: USENIX Association, 1996: 1-12.
