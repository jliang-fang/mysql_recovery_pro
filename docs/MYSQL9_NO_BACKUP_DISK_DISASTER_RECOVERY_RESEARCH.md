# MySQL9.0无备份磁盘恢复研究

**作者：** 方加俩

**单位：** 华北电力大学，广东省，510000

## MySQL9.0 Disk Recovery Without Backup: A Study

**Author:** jialiang.fang

**Affiliation:** North China Electric Power University, Guangdong Province, 510000, China

## 摘要

针对MySQL 9.0在无逻辑备份、无物理备份或原始实例无法启动条件下的数据抢救问题，提出一种只读、分层、可审计的磁盘级恢复逻辑。方法以磁盘镜像保护为入口，依次完成故障分级、InnoDB页面身份校验、SDI或外部schema解析、B+Tree记录提取、有限Undo/Redo证据关联及一致性检查，并按结构化结果、片段和原始证据分级输出。针对Redo损坏、局部数据页损坏和独立数据字典损坏三类故障，分别设计处理分支和失败转移机制。现有MySQL 9.0系列工具侧fixture记录了rm-rf、schema-aware和deep carve路径各48行输出；本轮进一步封存了一个局部数据页损坏的Level II同输入实验，但原生查询和离线输出均未达到可比较条件。因此，本文不以候选数量或测试通过率替代恢复率，强调物理证据、schema可信度和ground truth比较的约束作用。结果表明，该方法适合作为应急抢救和取证辅助方案，不能替代备份、binlog及容灾体系。

**关键词：** MySQL 9.0；磁盘级恢复；InnoDB；证据分级

## Abstract

To recover data from MySQL 9.0 when logical and physical backups are unavailable or the original instance cannot start, this paper proposes a read-only, layered, and auditable disk-level recovery method. The method protects the source evidence first, classifies damage, validates InnoDB page identity, extracts SDI or externally constrained schema information, parses B+Tree records, associates limited Undo/Redo evidence, and performs consistency checks before producing tiered results. Three branches are defined for damaged Redo logs, partially damaged data pages, and damaged independent data-dictionary tablespaces. Existing MySQL 9.0-series tool fixtures recorded 48 output rows for each of rm-rf recovery, schema-aware recovery, and deep carving, but no disaster experiment has yet provided a fair comparison with native recovery under identical inputs and ground truth. Therefore, candidate counts and test pass rates are not treated as recovery rates. The method is positioned as an emergency salvage and forensic aid rather than a replacement for backups, binlogs, or disaster-recovery systems.

**Key words:** MySQL 9.0; disk-level recovery; InnoDB; evidence grading

## 1 引言

数据库误删除、表空间损坏、文件系统异常和存储设备故障可能同时破坏目录、表空间、日志及数据字典。传统恢复以物理备份、逻辑备份、binlog或副本为基础；当这些材料不存在或不可用时，磁盘中仍可能残留可读取的`.ibd`页、共享表空间页、Undo/Redo片段、已解除链接文件和文件系统空闲块。此时恢复目标不是模拟数据库启动，而是在不修改原始证据的前提下，尽可能保存具有物理身份、字段边界和来源记录的数据证据。

无备份恢复的主要困难有三点：一是文件路径和目录元数据可能消失，不能仅依赖文件名发现数据；二是InnoDB记录同时包含页头、记录头、隐藏事务列、变长字段和外部BLOB指针，局部损坏会改变字段边界；三是启发式发现容易被误写成结构化结果。对灾难取证而言，错误导出可能比少导出更危险，因为未经证实的数据会污染后续数据库。为此，本文以“候选发现可以启发式，结构化导出必须确定性”为基本原则，研究MySQL 9.0无备份条件下的离线恢复逻辑。

MySQL 5.6/5.7通常需要结合FRM提取表结构；MySQL 8.0及以后主要依赖数据字典和InnoDB表空间中的SDI。因而版本识别不仅是兼容性步骤，也是输出授权条件。本文重点讨论三级故障模型、四个核心处理模块、证据分级策略及现有工具侧验证结果，并对尚未完成的真实灾难实验保持保守表述。

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

配置材料明确给出`innodb_file_per_table=1`、utf8mb4、3 GB buffer pool、2 GB redo log、128 MB log buffer、6 GB内存限制和3.0 CPU限制，同时使用`innodb_flush_log_at_trx_commit=0`、`sync_binlog=0`及ROW格式binlog。仓库没有提供宿主机CPU型号、物理内存、磁盘型号、挂载参数、虚拟化平台和块设备状态，故这些信息不作为实验结论。

### 3.2 工具侧验证结果

本轮验证于2026年9月5日在现有构建环境中完成，采用只读输入和受控测试输出目录，不运行Docker或Podman，不访问、挂载或修改块设备，也不对真实fixture实施删除、截断、覆盖或位翻转。测试结果按证据类型分组记录如下。

首先，页面身份、校验、扫描、损坏输入处理和文件系统辅助模块共303项测试全部通过，耗时约8.92 s；覆盖FIL/page header、checksum、页扫描、损坏页状态、极端输入、文件签名、XFS extent、ext4 block bitmap及io_uring scanner等边界。其次，元数据与结构解析模块共434项测试全部通过，耗时约11.85 s；覆盖SDI/FRM及版本证据、schema推断、B+Tree和记录链、BLOB指针与链、分区表、共享表空间、二级索引、DDL、压缩表和相关解析模糊测试。再次，Redo、Undo、Flashback、TDE/keyring、安全门、Salvage、导出隔离和幂等性模块共521项测试全部通过，耗时约241.72 s；这些结果支持解析边界、拒绝策略、只读安全约束及重复执行稳定性，不表示对应灾难场景已被恢复。

测试过程中曾出现两个`RawDiskCarverTest`安全门测试失败，分别为`SafetyGateRequiresAllMandatoryFacts`和`SafetyGateDiagnosticsDoNotAuthorize`。定向重建`test_raw_disk_carver`后两项通过，进一步核查确认失败原因是早于当前源码的陈旧构建产物；`SafetyGate::permits_read_only_raw_access`仍要求设备身份已验证、只读已确认、独占访问已确认且设备未挂载或状态允许四项强制事实同时成立，未修改安全门逻辑、测试断言或授权条件。此前导出隔离测试缺少`DBREC_TEST_TMPDIR`的问题则通过CTest注册注入构建目录下的受控临时目录解决；相关测试仍在环境变量缺失时拒绝运行，未削弱写盘隔离和fail-closed约束。

已有Phase 6工具侧矩阵报告59/59个场景通过、979行输出；其中MySQL 9.0系列fixture的rm-rf recovery、schema-aware recovery和deep carve分别记录48行，平均耗时分别为332 ms、298 ms和303 ms，10个场景总计约1394 ms。上述数字属于指定fixture和脚本口径，只能说明相应工具路径产生了这些输出，不能证明三类真实灾难恢复率、字段逐值正确性或优于原生恢复。另一份历史最终验证报告记录1270/1270通过、0失败及1项跳过，也不能与本轮1536项测试结果合并解释为恢复率。

本轮默认全量CTest共1536项通过、0项失败，总耗时约266.95 s；其中包含3个标记为`REAL_MYSQL`的真实fixture测试，因为命令中的`-E REAL_MYSQL`按测试名称过滤而不是按CTest label排除。为单独核对真实fixture，另以`DBREC_REAL_FIXTURES=1 ctest -L REAL_MYSQL`运行`test_compressed_table_fixture`、`test_real_fixture`和`test_real_recovery_pipeline`，3/3通过，耗时约0.34 s。该组测试使用已有MySQL 9.0系列及其他版本fixture，验证压缩表布局、页面结构、SDI/manifest完整性以及分区、共享表空间、二级索引、DDL和Redo文件的基本流水线行为；现有测试不包含灾难破坏后的同输入逐行逐字段恢复率对照。

本节所有测试通过数、输出行数和耗时均是软件验证或指定fixture运行指标，不是恢复记录数、precision、recall或有效恢复率。报告中出现的0行结果表示候选被过滤、输入不可解释或程序安全拒绝，不能单独解释为数据已恢复或数据不存在。

### 3.3 三级真实灾难对照设计及完成状态

方案A为MySQL 9原生`innodb_force_recovery`基线，方案B为本文的离线证据解析路径。两者应使用同一灾难后输入、同一灾难前ground truth、同一schema约束和相同资源限制。原生基线在灾难后副本上按`innodb_force_recovery=0`至`6`逐级测试，仅记录实例启动、表可见性、查询可读性、逻辑导出范围、错误信息和耗时；任何级别均不得直接作用于原始证据。Level II实验显示，所有级别均能启动并通过就绪检查，但访问受损表时查询返回137；日志记录space=2、page number=4的数据库页损坏，Level 1至6还出现随后的SIGSEGV。该结果只能分类为“原生查询触发的损坏处理失败/非可比较”，不能视为启动成功或恢复成功。离线结果包含1条结构化证据行，但schema来源和可信度均为`EVIDENCE_ONLY`，导出因`schema_trust_insufficient`被阻断，故不能与typed ground truth进行精确比较。

三级场景的统一实验设计如下：Ⅰ级在保留表空间和字典证据的条件下，构造Redo缺失、截断或校验失败输入，比较原生启动边界、离线页面证据和有限Redo inventory；Ⅱ级在字典基本完好的条件下，对预先登记的一个或多个数据页实施可追溯的局部损坏，比较健康页、受损页和拒绝结果；Ⅲ级在保留目标`.ibd`的条件下，使MySQL 9.0数据字典相关证据不可用或不完整，比较原生路径的字典依赖与离线路径的SDI、外部schema及物理布局约束。每个场景均须封存灾难前ground truth、灾难后manifest、损坏偏移、镜像摘要、版本digest、配置、资源限制和输出文件。上述实验不得以构造页、单元测试或工具退出码替代真实损坏输入。

| 故障等级 | 方案A：原生`innodb_force_recovery` | 方案B：离线恢复 | 当前状态 |
|---|---|---|---|
| Ⅰ级：Redo损坏，字典和表空间主要完好 | 待实测启动、读取、导出边界及错误信息 | 待实测页面身份、Redo inventory、有限关联和结构化结果 | 本轮未执行，暂无同输入对照 |
| Ⅱ级：局部数据页损坏，字典基本完好 | 7个独立副本均启动和就绪检查成功；访问受损表时查询均返回137，日志指向space=2/page=4并记录损坏处理失败；不形成可信导出 | 离线命令正常退出，但仅产生1条`EVIDENCE_ONLY`结构化证据；schema可信度不足，CSV为空，导出被阻断 | 已封存：`/tmp/dbrec_ws_l2_valid/experiment`；manifest seal=`8a541f1252d0ab038f76445463932892f8b0d0a4bfc193a30ca08b38c7730078`；损坏输入SHA-256=`8ecfc6cf364c18afe87b3dd733585108b7f185079751f135e1602fe2354f1c3e`；7个比较均为`NOT_COMPUTABLE` |
| Ⅲ级：数据字典相关证据损坏，`.ibd`尚存 | 待实测字典损坏后的启动、表识别和查询边界 | 待实测SDI、外部schema、页证据和冲突处理 | 本轮未执行，暂无同输入对照 |

截至本文定稿，Level II已封存同输入灾难镜像、独立原生副本、离线运行结果和比较报告；Level I、Level III仍未执行，且Level II尚未形成可信灾难后逐行逐字段输出。因此，本文的Level II结果是可审计的非可比较实验结果，不是恢复率结论；Level I和Level III仍属于可复现实验方案。当前已有的REAL_MYSQL测试只读取既有fixture并验证结构和流水线行为，不等同于对Redo或数据字典实施破坏后的恢复实验。

### 3.4 评价指标与讨论

设ground truth记录集合为`G`，结构化恢复集合为`R`，其中逐行逐字段正确匹配集合为`C`，则

\[
precision=|C|/|R|,\qquad recall=|C|/|G|
\]

并记录误报、漏报、目标表数、目标记录数、候选记录数、去重记录数、正确结构化记录数、扫描页数、有效页数、损坏页数、耗时、schema fingerprint、BLOB完整性和证据等级。严格通过要求schema及fingerprint一致、主键集合一致、全部字段值一致、无重复、无误报且无漏报。没有canonical ground truth或比较前置条件不满足时，不报告精确precision、recall、零误报、零漏报或“有效恢复率”。本轮Level II实验虽封存了灾难前ground truth、损坏范围、同输入副本和原生七级运行记录，但原生查询未形成可信导出，离线结果只有`EVIDENCE_ONLY`证据且schema可信度不足；因此7个等级的三组比较均为`NOT_COMPUTABLE`，precision和recall均为null。该状态表示比较条件不足，不表示五条ground-truth记录已被确认丢失。

本文方案的主要短板包括：schema缺失时结构化能力受限，FRM不能作为8.0以后SDI的等价替代；Redo仍是受约束的inventory和页面证据关联，不是完整重放；clean shutdown会削弱Undo和闪回实验；TDE、加密双写页、压缩页和BLOB链尚需真实损坏条件下的验证；共享表空间、分区表、二级索引、延迟分配及历史残留可能造成归属歧义；版本标签未固定digest也会降低实验可复现性。Level II已完成一个受控局部数据页损坏实验，但该实验的原生路径在查询受损页时失败，离线路径因schema证据不足而拒绝导出，未产生可计算的恢复率。以下项目截至本文定稿仍为待实测或待验证：Redo丢失、截断和校验失败，更多局部数据页损坏分布，数据字典及SDI损坏，crash-preserved Undo与Flashback，真实BLOB链损坏，分区表和共享表空间的行级恢复，二级索引救援正确性，完整DDL变更链语义，TDE/keyring兼容性，以及封存ground truth的ext4/XFS设备级灾难实验。尤其在ext4和XFS场景中，flush时序、删除方式和延迟分配会直接影响磁盘残留，恢复行数超过目标行数时应优先排查重复页和历史记录混入。

## 4 结论

本文提出了面向MySQL 9.0无备份场景的磁盘级灾难恢复逻辑。该逻辑以只读证据保护为前提，以三级故障模型选择处理分支，以页面身份、schema来源、记录边界和一致性校验控制结构化输出，并将低可信内容降级为可审计证据。本轮验证在页面、元数据、结构解析、日志证据、安全门、导出隔离和幂等性等方面完成了分组测试及全量回归，既有真实MySQL系列fixture的结构和流水线测试也通过；这些结果证明实现具备相应的边界约束、稳定性和拒绝不可靠输出的能力，但不构成真实介质的恢复率证据。

截至本文定稿，Level II已完成一个封存的局部数据页损坏实验，并完成原生`innodb_force_recovery=0..6`的独立副本运行和离线同输入运行；Level I、Level III以及灾难前ground truth与可信灾难后输出之间的逐行逐字段比较仍未完成。Level II中原生实例均通过启动和就绪检查，但访问受损页时查询失败；离线结果被限制为`EVIDENCE_ONLY`，7个等级的比较均为`NOT_COMPUTABLE`。因此，本文不宣称离线方法优于MySQL原生恢复，也不将Level II的查询崩溃、离线证据行或空CSV解释为数据总量损失，更不将测试通过率、候选数量、输出行数或耗时外推为precision、recall或有效恢复率。后续应在隔离副本上固定镜像digest和配置，封存manifest与输入哈希，分别构造Redo损坏和独立字典损坏场景，并以schema fingerprint、主键集合和逐字段比较计算precision、recall、误报、漏报及耗时。在此之前，该方法应定位为应急抢救和取证辅助工具，不能替代备份、binlog、远程副本、容灾切换和恢复演练。

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
