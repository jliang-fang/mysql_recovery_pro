# MySQL9.0无备份磁盘恢复研究

**作者：** 方加俩

**单位：** 华北电力大学，广东省，510000

## MySQL9.0 Disk Recovery Without Backup: A Study

**Author:** jialiang.fang

**Affiliation:** North China Electric Power University, Guangdong Province, 510000, China

## 摘要

针对MySQL 9.0在无逻辑备份、无物理备份或原始实例无法启动条件下的数据抢救问题，提出一种只读、分层、可审计的磁盘级恢复逻辑。方法以磁盘镜像保护为入口，依次完成故障分级、InnoDB页面身份校验、SDI或外部schema解析、B+Tree记录提取、有限Undo/Redo证据关联及一致性检查，并按结构化结果、片段和原始证据分级输出。针对Redo损坏、局部数据页损坏和独立数据字典损坏三类故障，分别设计处理分支和失败转移机制。现有MySQL 9.0系列工具侧fixture记录了rm-rf、schema-aware和deep carve路径各48行输出，但尚无与原生恢复机制同输入、同基准的真实灾难对照实验。因此，本文不以候选数量或测试通过率替代恢复率，强调物理证据、schema可信度和ground truth比较的约束作用。结果表明，该方法适合作为应急抢救和取证辅助方案，不能替代备份、binlog及容灾体系。

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

Phase 6工具侧矩阵报告59/59个场景通过，覆盖MySQL 5.6至9.0并报告979行输出。针对MySQL 9.0，rm-rf recovery记录48行、平均332 ms；schema-aware recovery记录48行、平均298 ms；deep carve记录48行、平均303 ms；10个场景总计约1394 ms。上述数字属于指定fixture和脚本口径，只能说明相应工具路径产生了这些输出，不能证明三类真实灾难恢复率、字段逐值正确性或优于原生恢复。

同一报告中，corrupted page、raw disk carving、auto-salvage、Undo和flashback相关场景部分记录为0行。报告说明，Undo场景受clean shutdown后日志purge影响，损坏页fixture则被刻意构造为难以解析。因而0行主要表明程序未崩溃、空槽被过滤或不可靠内容被拒绝，不能解释为完成了对应数据恢复。另一份最终验证报告记录1270/1270通过、0失败及1项跳过，其中包括58项P0回归、34项P1格式模糊测试、19项P5灾难矩阵、306项Redo/Flashback/Undo测试和853项其他单元测试。这些结果支持实现稳定性与构造测试覆盖，不等价于真实介质恢复率。另一次REAL_MYSQL标签测试中，压缩表fixture、真实fixture和真实恢复流水线三个测试目标均通过，覆盖压缩表布局、真实MySQL页面结构、manifest完整性以及分区、共享表空间、二级索引、DDL和Redo文件基本结构验证；这些测试不包含三级灾难破坏后的同输入恢复对照，也不能替代逐字段ground truth比较。

此前默认非REAL_MYSQL测试套件曾记录2个SafetyGate测试失败和9个导出隔离测试失败，已于2026-09-05定位并处置。2个SafetyGate失败在定向重建测试目标后通过；复核确认`SafetyGate::permits_read_only_raw_access`的四项强制事实要求与测试预期一致，失败源于早于源码提交的陈旧构建产物，未修改源码即解决。9个导出隔离测试失败源于早期提交引入的测试写盘约束：测试辅助函数要求在`DBREC_TEST_TMPDIR`指定的受控目录内写文件、未设置该变量时抛出异常，而CTest注册未提供该变量；修复方式是在CMake测试注册处为相关套件注入受控临时目录环境变量，测试源码中的fail-closed约束未削弱。修复后默认套件重跑结果为1536/1536通过、0失败，总耗时约261.3秒；REAL_MYSQL标签的3个测试目标在同一环境亦全部通过。上述数字仍属构造测试与fixture口径的通过率，不构成恢复率证据。

### 3.3 三级真实灾难对照设计及完成状态

方案A为MySQL 9原生`innodb_force_recovery`基线，方案B为本文的离线证据解析路径。两者应使用同一灾难后输入、同一灾难前ground truth、同一schema约束和相同资源限制。原生基线拟在灾难后副本上按`innodb_force_recovery=0`至`6`逐级测试，仅记录实例启动、表可见性、查询可读性、逻辑导出范围、错误信息和耗时；任何级别均不得直接作用于原始证据。当前尚未完成该基线测试，因此不报告原生恢复率、可启动级别或与本文方法的性能优劣。

三级场景的统一实验设计如下：Ⅰ级在保留表空间和字典证据的条件下，构造Redo缺失、截断或校验失败输入，比较原生启动边界、离线页面证据和有限Redo inventory；Ⅱ级在字典基本完好的条件下，对预先登记的一个或多个数据页实施可追溯的局部损坏，比较健康页、受损页和拒绝结果；Ⅲ级在保留目标`.ibd`的条件下，使MySQL 9.0数据字典相关证据不可用或不完整，比较原生路径的字典依赖与离线路径的SDI、外部schema及物理布局约束。每个场景均须封存灾难前ground truth、灾难后manifest、损坏偏移、镜像摘要、版本digest、配置、资源限制和输出文件。上述实验不得以构造页、单元测试或工具退出码替代真实损坏输入。

| 故障等级 | 方案A：原生`innodb_force_recovery` | 方案B：离线恢复 | 当前状态 |
|---|---|---|---|
| Ⅰ级：Redo损坏，字典和表空间主要完好 | 待实测启动、读取、导出边界及错误信息 | 待实测页面身份、Redo inventory、有限关联和结构化结果 | 未实测，暂无同输入对照 |
| Ⅱ级：局部数据页损坏，字典基本完好 | 待实测可启动性、可读表范围和导出结果 | 待实测健康页解析、损坏页分级、记录拒绝和可恢复字段 | 未实测，暂无同输入对照 |
| Ⅲ级：数据字典相关证据损坏，`.ibd`尚存 | 待实测字典损坏后的启动、表识别和查询边界 | 待实测SDI、外部schema、页证据和冲突处理 | 未实测，暂无同输入对照 |

截至本文定稿，尚未封存上述三级场景的同输入灾难镜像，也未完成原生基线和离线路径的逐行逐字段比较。因此，本文的三级对照属于可复现实验方案，不属于已完成的真实灾难结果。

### 3.4 评价指标与讨论

设ground truth记录集合为`G`，结构化恢复集合为`R`，其中逐行逐字段正确匹配集合为`C`，则

\[
precision=|C|/|R|,\qquad recall=|C|/|G|
\]

并记录误报、漏报、目标表数、目标记录数、候选记录数、去重记录数、正确结构化记录数、扫描页数、有效页数、损坏页数、耗时、schema fingerprint、BLOB完整性和证据等级。严格通过要求schema及fingerprint一致、主键集合一致、全部字段值一致、无重复、无误报且无漏报。没有canonical ground truth时，不报告精确precision、recall、零误报、零漏报或“有效恢复率”。

本文方案的主要短板包括：schema缺失时结构化能力受限，FRM不能作为8.0以后SDI的等价替代；Redo仍是受约束的inventory和页面证据关联，不是完整重放；clean shutdown会削弱Undo和闪回实验；TDE、加密双写页、压缩页和BLOB链尚需真实损坏条件下的验证；共享表空间、分区表、二级索引、延迟分配及历史残留可能造成归属歧义；版本标签未固定digest也会降低实验可复现性。以下项目截至本文定稿仍为待实测或待验证：Redo丢失、截断和校验失败，局部数据页损坏，数据字典及SDI损坏，crash-preserved Undo与Flashback，真实BLOB链损坏，分区表和共享表空间的行级恢复，二级索引救援正确性，完整DDL变更链语义，TDE/keyring兼容性，以及封存ground truth的ext4/XFS设备级灾难实验。尤其在ext4和XFS场景中，flush时序、删除方式和延迟分配会直接影响磁盘残留，恢复行数超过目标行数时应优先排查重复页和历史记录混入。

## 4 结论

本文提出了面向MySQL 9.0无备份场景的磁盘级灾难恢复逻辑。该逻辑以只读证据保护为前提，以三级故障模型选择处理分支，以页面身份、schema来源、记录边界和一致性校验控制结构化输出，并将低可信内容降级为可审计证据。现有工具侧验证记录了MySQL 9.0系列fixture下三条路径各48行输出，但尚未建立三级真实灾难实验及原生`innodb_force_recovery`同条件基线，因此不能据此宣称确定恢复率或性能优势。

后续应固定镜像digest和配置，封存灾难前后manifest，分别构造Redo损坏、局部页损坏和独立字典损坏场景，并以schema fingerprint、主键集合和逐字段比较计算precision、recall、误报、漏报及耗时。在此之前，该方法应定位为应急抢救和取证辅助工具，不能替代备份、binlog、远程副本、容灾切换和恢复演练。

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
